#pragma once
#include "../../core/placement_geometry.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../core/shape_generation_stage.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../pipeline/stages/filter_by_layer_visibility_stage.hpp"
#include "../../pipeline/stages/filter_by_viewport_and_size_stage.hpp"
#include "../../pipeline/stages/generate_abstract_shapes_stage.hpp"
#include "../../pipeline/stages/generate_layout_shapes_stage.hpp"
#include "parallel_generate_layout_shapes_stage.hpp"
#include "../../render/draw_helpers.hpp"
#include "../../render/pixel_types.hpp"
#include "../../render/stages/build_layout_picture_stage.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <algorithm>
#include <optional>
#include <set>
#include <taskflow/taskflow.hpp>
#include <unordered_map>
#include <vector>

namespace le::flow
{
    /// @brief A one-shot, parallel alternative to
    /// src/instancing/InstanceRenderer's own build_layout_picture, built
    /// by reading that class as reference (not editing it) -
    /// TASKFLOW_EXPERIMENT.md's second pipeline, targeting a different
    /// real bottleneck than src/flow/pipeline/'s per-layer shape
    /// generation: InstanceRenderer's own single-threaded per-placement
    /// drawing loop at 1,000,000-instance scale (test_data/ispd19_test10:
    /// 899,404 placements, but only 70 distinct referenced designs -
    /// resolve_design_picture's own cache already means each distinct
    /// design's picture is built once regardless of placement count; the
    /// real cost is the placement-drawing loop itself, once per
    /// placement, into one shared SkCanvas).
    ///
    /// Deliberately reuses the real, unmodified
    /// GenerateAbstractShapesStage/GenerateLayoutShapesStage/
    /// FilterByViewportAndSizeStage/FilterByLayerVisibilityStage/
    /// BuildLayoutPictureStage/draw_helpers - none of that is
    /// src/instancing's own code, all of it is already Skia-correct and
    /// tested; only InstanceRenderer's own orchestration (the
    /// resolve/build recursion) is forked here.
    ///
    /// No persistent cache the way InstanceRenderer has one (Epoch,
    /// {DesignId,remaining_depth}-keyed maps, min_visible_instance_pixels
    /// as a settable knob) - this is a one-shot parallelism experiment,
    /// not a replacement for interactive use - see
    /// TASKFLOW_EXPERIMENT.md.
    ///
    /// Only the top-level run() call gets a two-phase parallel treatment
    /// (see its own comment) - a distinct design that itself resolves to
    /// a nested Layout is built by the same plain serial recursive
    /// helper every phase-1 task already uses for its own single
    /// assigned design, matching InstanceRenderer's own recursive shape
    /// but not further parallelized at that depth. ispd19_test10's own
    /// flat structure (70 leaf cell types, no macro-level hierarchy)
    /// never exercises this path in practice - a real, stated scope
    /// boundary, not a silent one.
    class ParallelLayoutPictureBuilder
    {
    public:
        sk_sp<SkPicture> run(const Root &root, LayoutId layout_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale, tf::Executor &executor) const
        {
            const std::vector<PlacementId> &placements = root.get_layout_placements(layout_id);

            // Real StageProfiler numbers against test_data/ispd19_test10
            // (see TASKFLOW_EXPERIMENT.md) showed the placement-resolve/
            // draw work (what this class originally set out to
            // parallelize) costs only ~90ms total, while a serial
            // "compose_final_picture" phase run AFTER it cost ~290ms
            // regardless of worker count, capping real speedup at ~1.2x.
            // The cause: generating the Layout's own direct content (die
            // area/blockages/routes/ports/rows/tracks/gcell grid/
            // regions) has NO dependency on placement resolution at all
            // - real, substantial work that was only serial because the
            // original 3-phase design put it last. One single Taskflow
            // below expresses that real independence directly:
            // `generate_own_shapes` has no precede edge to the
            // resolve/draw chain, so Taskflow's own scheduler runs it
            // concurrently with Phase 1/2 instead of serially after
            // them; only the final compose step needs both results.
            // `generate_own_shapes` is itself further split - via
            // ParallelGenerateLayoutShapesStage - into one dynamic
            // sub-task per object type (die area/blockages/routes/ports/
            // rows/tracks/gcell grid/regions), since none of those 8
            // sources read each other's output; whichever one actually
            // dominates on a given real design (found to vary - see
            // TASKFLOW_EXPERIMENT.md) gets parallelized against the
            // other 7 instead of being a single opaque serial call.
            ParallelGenerateLayoutShapesStage generate_stage; // kept alive at run()'s own scope (not the task's local scope) so compose_final_picture can still read its version() for FilterByViewportAndSizeStage's own cache key
            std::vector<RenderedShape> own_dbu_shapes;
            std::vector<ChunkResult> chunk_results;
            sk_sp<SkPicture> final_picture;

            tf::Taskflow taskflow;

            tf::Task generate_own_shapes_task = taskflow.emplace([&](tf::Subflow &sf)
                                                                   { own_dbu_shapes = generate_stage.run(root, layout_id, view_layers, sf); })
                                                     .name("generate_own_shapes");

            // Resolve every distinct referenced design, then draw every
            // placement chunk - both counts (distinct designs, chunks)
            // are only known once this task's own body runs, so this is
            // expressed as dynamic (Subflow-based) tasking rather than a
            // statically-built graph. A Subflow can only be joined once
            // (see TASKFLOW_EXPERIMENT.md's own earlier double-join
            // lesson), so the chunk-drawing round gets its own nested
            // dynamic task (`draw_all_chunks`, with its own fresh
            // Subflow) rather than reusing `sf` a second time.
            tf::Task resolve_and_draw_task = taskflow.emplace([&](tf::Subflow &sf)
                                                                {
                std::vector<DesignId> distinct_designs;
                {
                    std::set<DesignId> seen;
                    for (PlacementId placement_id : placements)
                    {
                        const PlacementData *placement = root.get_placement(placement_id);
                        if (!placement || !placement->location || !placement->reference_design.valid())
                            continue;
                        if (seen.insert(placement->reference_design).second)
                            distinct_designs.push_back(placement->reference_design);
                    }
                }

                // Phase 1: resolve every distinct design's own picture,
                // in parallel - one dynamic sub-task per distinct
                // design. Pre-sized (not incrementally grown) so every
                // task's captured slot reference stays valid regardless
                // of how many other tasks are added afterward.
                std::vector<sk_sp<SkPicture>> resolved_slots(distinct_designs.size());
                std::vector<tf::Task> resolve_tasks;
                resolve_tasks.reserve(distinct_designs.size());
                for (size_t i = 0; i < distinct_designs.size(); ++i)
                {
                    const DesignId design_id = distinct_designs[i];
                    sk_sp<SkPicture> &slot = resolved_slots[i];
                    resolve_tasks.push_back(sf.emplace([&, design_id]
                                                         { slot = build_design_picture_serial(root, design_id, remaining_depth, view_layers, scene, scale); })
                                                 .name("resolve_distinct_design"));
                }

                // Phase 2: chunk the placement list, draw each chunk
                // into its own sub-picture, in parallel - one more
                // dynamic sub-task of `sf`, preceded by every Phase-1
                // task (so it only starts once every distinct design is
                // resolved), owning its own nested Subflow for its own
                // per-chunk fan-out.
                tf::Task draw_all_chunks_task = sf.emplace([&](tf::Subflow &sf2)
                                                             {
                    std::unordered_map<DesignId, sk_sp<SkPicture>> resolved;
                    resolved.reserve(distinct_designs.size());
                    for (size_t i = 0; i < distinct_designs.size(); ++i)
                        resolved.emplace(distinct_designs[i], resolved_slots[i]);

                    // Below the threshold (not enough placements to give
                    // every worker a full chunk), or a single-worker
                    // executor, falls back to one chunk covering
                    // everything - same threshold shape as
                    // src/flow/pipeline/'s own parallel_chunks
                    // (now-removed but its lesson kept).
                    const size_t placement_count = placements.size();
                    const size_t num_workers = std::max<size_t>(1, sf2.executor().num_workers());
                    const size_t chunk_count = (num_workers <= 1 || placement_count < 2 * num_workers) ? size_t{1} : num_workers;

                    // Grouped BY REFERENCED DESIGN, not by a flat index
                    // range - real, confirmed reason (TASKFLOW_EXPERIMENT.md):
                    // draw_placement_chunk's own resolve_picture copies a
                    // shared sk_sp<SkPicture> per placement (one real
                    // atomic ref/unref pair, unavoidable given
                    // BuildLayoutPictureStage::ResolvedInstance's own real
                    // ownership contract), and ispd19_test10's 899,404
                    // placements reference only 70 distinct designs - a
                    // flat index-range split let MANY different worker
                    // threads hammer the SAME design's refcount cache
                    // line concurrently, confirmed (via a standalone
                    // synthetic probe) to be the actual cause of the
                    // negative scaling past 4 workers. Bin-packing whole
                    // per-design placement lists (greedy
                    // longest-processing-time-first, by placement count
                    // descending, into whichever chunk currently has the
                    // least work) guarantees a given design's own
                    // placements - and therefore its own refcount - are
                    // only ever touched by ONE thread, while still
                    // keeping chunks reasonably balanced.
                    std::unordered_map<DesignId, std::vector<PlacementId>> placements_by_design;
                    for (PlacementId placement_id : placements)
                    {
                        const PlacementData *placement = root.get_placement(placement_id);
                        placements_by_design[placement ? placement->reference_design : DesignId{}].push_back(placement_id);
                    }

                    std::vector<std::pair<DesignId, size_t>> bucket_sizes;
                    bucket_sizes.reserve(placements_by_design.size());
                    for (const auto &[design_id, list] : placements_by_design)
                        bucket_sizes.emplace_back(design_id, list.size());
                    std::sort(bucket_sizes.begin(), bucket_sizes.end(), [](const auto &a, const auto &b)
                              { return a.second > b.second; });

                    std::vector<std::vector<PlacementId>> chunk_placement_lists(chunk_count);
                    std::vector<size_t> chunk_loads(chunk_count, 0);
                    for (const auto &[design_id, size] : bucket_sizes)
                    {
                        const size_t target = static_cast<size_t>(std::min_element(chunk_loads.begin(), chunk_loads.end()) - chunk_loads.begin());
                        std::vector<PlacementId> &bucket = placements_by_design[design_id];
                        std::vector<PlacementId> &target_list = chunk_placement_lists[target];
                        target_list.insert(target_list.end(), bucket.begin(), bucket.end());
                        chunk_loads[target] += size;
                    }

                    chunk_results.reserve(chunk_count);
                    for (const std::vector<PlacementId> &chunk_placements : chunk_placement_lists)
                    {
                        if (chunk_placements.empty())
                            continue;
                        chunk_results.emplace_back();
                        ChunkResult &slot = chunk_results.back();
                        sf2.emplace([&]
                                    { slot = draw_placement_chunk(root, chunk_placements, remaining_depth, view_layers, scale, resolved); })
                            .name("draw_placement_chunk");
                    }
                    sf2.join(); })
                                                     .name("draw_all_chunks");

                for (tf::Task &t : resolve_tasks)
                    t.precede(draw_all_chunks_task);

                sf.join(); })
                                                  .name("resolve_and_draw");

            tf::Task compose_task = taskflow.emplace([&]
                                                       { final_picture = compose_final_picture(root, layout_id, view_layers, scene, scale, generate_stage, own_dbu_shapes, chunk_results); })
                                         .name("compose_final_picture");

            generate_own_shapes_task.precede(compose_task);
            resolve_and_draw_task.precede(compose_task);

            executor.run(taskflow).wait();
            return final_picture;
        }

        // Mirrors InstanceRenderer's own real
        // set_min_visible_instance_pixels() - same knob, same default
        // (100.0), for the same reason (a testing/tuning value, not a
        // fixed constant - see that method's own doc comment).
        void set_min_visible_instance_pixels(double pixels) { min_visible_instance_pixels_ = pixels; }

    private:
        double min_visible_instance_pixels_ = 100.0; // matches InstanceRenderer's own default

        // draw_placement_chunk's own result: the sub-picture plus its own
        // real content bbox (dbu space) - propagated back to Phase 3 so
        // the outer picture's own SkPictureRecorder bounds can be a real,
        // properly-unioned rect instead of an arbitrary/oversized one (a
        // picture's own bounds never clip recording, but a too-small one
        // risks a future caller's quickReject wrongly culling real
        // content - see BuildLayoutPictureStage's own comment).
        struct ChunkResult
        {
            sk_sp<SkPicture> picture;
            Rect bbox{};
            bool has_bbox = false;
        };

        sk_sp<SkPicture> build_design_picture_serial(const Root &root, DesignId design_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale) const
        {
            const LayoutId layout_id = root.get_design_layout(design_id);
            if (remaining_depth > 0 && layout_id.valid())
                return build_layout_picture_serial(root, layout_id, remaining_depth - 1, view_layers, scene, scale);

            const AbstractId abstract_id = root.get_design_abstract(design_id);
            if (abstract_id.valid())
                return build_abstract_picture_serial(root, abstract_id, view_layers, scene, scale);

            return nullptr; // unresolved - a real, narrower gap than InstanceRenderer's own (which logs once per DesignId); no persistent state to log against here in a one-shot tool.
        }

        sk_sp<SkPicture> build_abstract_picture_serial(const Root &root, AbstractId abstract_id, const ViewLayerSet &view_layers, const Scene &scene, double scale) const
        {
            // Fresh, local stage instances - never shared across
            // concurrently-running Phase-1 tasks (their own VersionedStage
            // single cache slot isn't thread-safe for concurrent hits),
            // and nothing here benefits from caching anyway - each
            // distinct design is resolved exactly once by construction
            // (see Phase 0's own dedup).
            GenerateAbstractShapesStage generate_stage;
            FilterByViewportAndSizeStage viewport_stage;
            FilterByLayerVisibilityStage layer_stage;

            const std::vector<RenderedShape> dbu_shapes = generate_stage.run(root, abstract_id, view_layers);
            return record_local_picture(generate_stage, viewport_stage, layer_stage, dbu_shapes, view_layers, scene, scale, {}, {}, abstract_declared_bbox(root, abstract_id));
        }

        sk_sp<SkPicture> build_layout_picture_serial(const Root &root, LayoutId layout_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale) const
        {
            GenerateLayoutShapesStage generate_stage;
            FilterByViewportAndSizeStage viewport_stage;
            FilterByLayerVisibilityStage layer_stage;

            const std::vector<RenderedShape> dbu_shapes = generate_stage.run(root, layout_id, view_layers);

            std::vector<BuildLayoutPictureStage::ResolvedInstance> instances;
            std::vector<PixelRect> tiny_instance_rects;
            Rect content_bbox = layout_declared_bbox(root, layout_id);
            bool have_content_bbox = content_bbox.ll.x != content_bbox.ur.x || content_bbox.ll.y != content_bbox.ur.y;

            auto resolve_picture = [&](DesignId design_id)
            { return build_design_picture_serial(root, design_id, remaining_depth, view_layers, scene, scale); };

            for (PlacementId placement_id : root.get_layout_placements(layout_id))
                process_placement(root, placement_id, remaining_depth, scale, resolve_picture, instances, tiny_instance_rects, content_bbox, have_content_bbox);

            return record_local_picture(generate_stage, viewport_stage, layer_stage, dbu_shapes, view_layers, scene, scale, instances, tiny_instance_rects, content_bbox);
        }

        // The Phase-2 counterpart of build_layout_picture_serial's own
        // per-placement loop - `resolve_picture` is a cheap map lookup
        // here (Phase 1 already resolved every distinct design) instead
        // of a real, possibly-recursive build call, but the bbox/
        // sub-pixel-threshold logic (process_placement) is identical
        // either way. No own_shapes drawn here at all - Phase 3 draws
        // the Layout's own direct content exactly once, separately.
        //
        // Takes an explicit placement list (not a [begin,end) index
        // range into the full placement list) - see run()'s own
        // "group_placements_by_design" comment for why: every placement
        // in `chunk_placements` is guaranteed to reference only designs
        // this one chunk owns exclusively, so every sk_sp<SkPicture>
        // copy `resolve_picture` below performs (BuildLayoutPictureStage::
        // ResolvedInstance::picture's own real, fixed ownership contract
        // requires one real atomic ref/unref pair per placement drawn -
        // see TASKFLOW_EXPERIMENT.md, that part can't be eliminated) only
        // ever touches a given design's refcount from THIS thread - no
        // other concurrently-running chunk task ever contends for the
        // same cache line.
        ChunkResult draw_placement_chunk(const Root &root, const std::vector<PlacementId> &chunk_placements, int remaining_depth, const ViewLayerSet &view_layers, double scale, const std::unordered_map<DesignId, sk_sp<SkPicture>> &resolved) const
        {
            std::vector<BuildLayoutPictureStage::ResolvedInstance> instances;
            std::vector<PixelRect> tiny_instance_rects;
            ChunkResult result;

            auto resolve_picture = [&](DesignId design_id) -> sk_sp<SkPicture>
            {
                const auto it = resolved.find(design_id);
                return it != resolved.end() ? it->second : nullptr;
            };

            for (PlacementId placement_id : chunk_placements)
                process_placement(root, placement_id, remaining_depth, scale, resolve_picture, instances, tiny_instance_rects, result.bbox, result.has_bbox);

            if (instances.empty() && tiny_instance_rects.empty())
                return result; // picture stays null, has_bbox stays false

            const SkRect bounds = SkRect::MakeLTRB(
                static_cast<SkScalar>(static_cast<double>(result.bbox.ll.x) * scale), static_cast<SkScalar>(static_cast<double>(result.bbox.ll.y) * scale),
                static_cast<SkScalar>(static_cast<double>(result.bbox.ur.x) * scale), static_cast<SkScalar>(static_cast<double>(result.bbox.ur.y) * scale));
            static const std::map<ViewLayerId, std::vector<PixelShape>> kNoOwnShapes;
            result.picture = BuildLayoutPictureStage::run(bounds, kNoOwnShapes, instances, tiny_instance_rects, view_layers);
            return result;
        }

        // Shared per-placement logic (bbox, sub-pixel-collapse threshold,
        // instance-transform) - ported from
        // InstanceRenderer::build_layout_picture_uncached's own loop body
        // - used by both build_layout_picture_serial (recursion, real
        // build via `resolve_picture`) and draw_placement_chunk
        // (top-level, cheap map-lookup `resolve_picture`), so the two
        // never drift apart on which placements get collapsed to a tiny
        // dot vs. drawn in full.
        template <typename ResolvePictureFn>
        void process_placement(const Root &root, PlacementId placement_id, int remaining_depth, double scale, ResolvePictureFn &&resolve_picture, std::vector<BuildLayoutPictureStage::ResolvedInstance> &instances, std::vector<PixelRect> &tiny_instance_rects, Rect &content_bbox, bool &have_content_bbox) const
        {
            const PlacementData *placement = root.get_placement(placement_id);
            if (!placement || !placement->location || !placement->reference_design.valid())
                return;

            auto expand = [&](Rect r)
            {
                if (!have_content_bbox)
                {
                    content_bbox = r;
                    have_content_bbox = true;
                    return;
                }
                content_bbox.ll.x = std::min(content_bbox.ll.x, r.ll.x);
                content_bbox.ll.y = std::min(content_bbox.ll.y, r.ll.y);
                content_bbox.ur.x = std::max(content_bbox.ur.x, r.ur.x);
                content_bbox.ur.y = std::max(content_bbox.ur.y, r.ur.y);
            };

            if (!design_is_resolvable(root, placement->reference_design, remaining_depth))
            {
                resolve_picture(placement->reference_design); // still visited (matches InstanceRenderer's own ordering), result discarded - nothing real to draw
                return;
            }

            const Rect child_local_bbox = resolved_local_bbox(root, placement->reference_design, remaining_depth);
            const Orientation orientation = placement->orientation.value_or(Orientation::N);
            const Geometry::InstanceTransform transform = Geometry::instance_transform(orientation, child_local_bbox, *placement->location);
            const Rect world_bbox = Geometry::transform_bbox(transform, child_local_bbox);
            expand(world_bbox);

            const double min_visible_dbu = min_visible_instance_pixels_ / scale;
            const double width = static_cast<double>(world_bbox.ur.x - world_bbox.ll.x);
            const double height = static_cast<double>(world_bbox.ur.y - world_bbox.ll.y);
            if (width < min_visible_dbu && height < min_visible_dbu)
            {
                tiny_instance_rects.push_back(PixelRect{
                    .ll = PixelPoint{.x = static_cast<double>(world_bbox.ll.x) * scale, .y = static_cast<double>(world_bbox.ll.y) * scale},
                    .ur = PixelPoint{.x = static_cast<double>(world_bbox.ur.x) * scale, .y = static_cast<double>(world_bbox.ur.y) * scale},
                });
                return;
            }

            sk_sp<SkPicture> child_picture = resolve_picture(placement->reference_design);
            if (!child_picture)
                return;

            instances.push_back(BuildLayoutPictureStage::ResolvedInstance{
                .transform = to_instance_matrix(transform, Point{0, 0}, scale),
                .picture = std::move(child_picture),
            });
        }

        // Shared core of build_abstract_picture_serial/
        // build_layout_picture_serial - ported from InstanceRenderer::
        // record_local_picture, always with local_origin={0,0} ("local
        // pixel space", InstanceRenderer's own normal - non-top-level -
        // convention; this class has no render_layout_frame-style
        // top-level real-Scene-pan case to support).
        sk_sp<SkPicture> record_local_picture(const ShapeGenerationStage &generate_stage, FilterByViewportAndSizeStage &viewport_stage, FilterByLayerVisibilityStage &layer_stage, const std::vector<RenderedShape> &dbu_shapes, const ViewLayerSet &view_layers, const Scene &scene, double scale, const std::vector<BuildLayoutPictureStage::ResolvedInstance> &instances, const std::vector<PixelRect> &tiny_instance_rects, Rect declared_bbox) const
        {
            Rect content_bbox = declared_bbox;
            for (const RenderedShape &rs : dbu_shapes)
            {
                const std::optional<Rect> shape_bbox = Geometry::bbox(rs.shape);
                if (!shape_bbox)
                    continue;
                content_bbox.ll.x = std::min(content_bbox.ll.x, shape_bbox->ll.x);
                content_bbox.ll.y = std::min(content_bbox.ll.y, shape_bbox->ll.y);
                content_bbox.ur.x = std::max(content_bbox.ur.x, shape_bbox->ur.x);
                content_bbox.ur.y = std::max(content_bbox.ur.y, shape_bbox->ur.y);
            }

            Scene cull_scene;
            cull_scene.set_scale(scale);
            cull_scene.set_pan(content_bbox.ll);
            const double width_dbu = static_cast<double>(content_bbox.ur.x - content_bbox.ll.x);
            const double height_dbu = static_cast<double>(content_bbox.ur.y - content_bbox.ll.y);
            cull_scene.set_viewport_size(static_cast<int>(width_dbu * scale) + 2, static_cast<int>(height_dbu * scale) + 2);

            const auto &viewport_filtered = viewport_stage.run(generate_stage, dbu_shapes, cull_scene);
            const auto &layer_filtered = layer_stage.run(viewport_stage, viewport_filtered, scene, view_layers);
            const auto pixel_shapes = transform_shapes_to_pixel_space(layer_filtered, Point{0, 0}, scale);

            const SkRect bounds = SkRect::MakeLTRB(
                static_cast<SkScalar>(static_cast<double>(content_bbox.ll.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ll.y) * scale),
                static_cast<SkScalar>(static_cast<double>(content_bbox.ur.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ur.y) * scale));

            return BuildLayoutPictureStage::run(bounds, pixel_shapes, instances, tiny_instance_rects, view_layers);
        }

        // Phase 3 - draws the top Layout's own direct content once, then
        // every chunk's already-recorded sub-picture on top. Takes
        // `own_dbu_shapes` as an already-computed input (produced by
        // run()'s own `generate_own_shapes` task, which runs concurrently
        // with placement resolution/drawing rather than serially after
        // it - see run()'s own comment) instead of calling
        // GenerateLayoutShapesStage itself. Unioned from the Layout's own
        // declared bbox, its own direct shapes' bbox, and every chunk's
        // own already-computed bbox (ChunkResult::bbox) - a real,
        // properly-tight bounds, not an arbitrary/oversized placeholder
        // (a picture's own bounds never clip recording, but a too-small
        // one risks a future caller's quickReject wrongly culling real
        // content - see BuildLayoutPictureStage's own comment).
        sk_sp<SkPicture> compose_final_picture(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, const Scene &scene, double scale, const ShapeGenerationStage &generate_stage, const std::vector<RenderedShape> &dbu_shapes, const std::vector<ChunkResult> &chunk_results) const
        {
            FilterByViewportAndSizeStage viewport_stage;
            FilterByLayerVisibilityStage layer_stage;

            Rect content_bbox = layout_declared_bbox(root, layout_id);
            bool have_content_bbox = content_bbox.ll.x != content_bbox.ur.x || content_bbox.ll.y != content_bbox.ur.y;
            auto expand = [&](Rect r)
            {
                if (!have_content_bbox)
                {
                    content_bbox = r;
                    have_content_bbox = true;
                    return;
                }
                content_bbox.ll.x = std::min(content_bbox.ll.x, r.ll.x);
                content_bbox.ll.y = std::min(content_bbox.ll.y, r.ll.y);
                content_bbox.ur.x = std::max(content_bbox.ur.x, r.ur.x);
                content_bbox.ur.y = std::max(content_bbox.ur.y, r.ur.y);
            };
            for (const RenderedShape &rs : dbu_shapes)
                if (const std::optional<Rect> shape_bbox = Geometry::bbox(rs.shape))
                    expand(*shape_bbox);
            for (const ChunkResult &chunk : chunk_results)
                if (chunk.has_bbox)
                    expand(chunk.bbox);

            Scene cull_scene;
            cull_scene.set_scale(scale);
            cull_scene.set_pan(content_bbox.ll);
            cull_scene.set_viewport_size(
                static_cast<int>(static_cast<double>(content_bbox.ur.x - content_bbox.ll.x) * scale) + 2,
                static_cast<int>(static_cast<double>(content_bbox.ur.y - content_bbox.ll.y) * scale) + 2);

            const auto &viewport_filtered = viewport_stage.run(generate_stage, dbu_shapes, cull_scene);
            const auto &layer_filtered = layer_stage.run(viewport_stage, viewport_filtered, scene, view_layers);
            const auto pixel_shapes = transform_shapes_to_pixel_space(layer_filtered, Point{0, 0}, scale);

            const SkRect bounds = SkRect::MakeLTRB(
                static_cast<SkScalar>(static_cast<double>(content_bbox.ll.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ll.y) * scale),
                static_cast<SkScalar>(static_cast<double>(content_bbox.ur.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ur.y) * scale));

            SkPictureRecorder recorder;
            SkCanvas *canvas = recorder.beginRecording(bounds);
            draw_shape_groups(*canvas, pixel_shapes, view_layers);
            for (const ChunkResult &chunk : chunk_results)
                if (chunk.picture)
                    canvas->drawPicture(chunk.picture);
            return recorder.finishRecordingAsPicture();
        }
    };
}
