#pragma once
#include "../core/placement_geometry.hpp"
#include "../core/versioned_stage.hpp"
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../render/draw_helpers.hpp"
#include "../render/stages/build_layout_picture_stage.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include "frame_render_pipeline.hpp"
#include "pipeline_options.hpp"
#include "stages/abstract_geometry_stage.hpp"
#include "stages/layer_visibility_filter_stage.hpp"
#include "stages/layout_geometry_stage.hpp"
#include "stages/mouse_overlay_stage.hpp"
#include "stages/ruler_overlay_stage.hpp"
#include "stages/selection_overlay_stage.hpp"
#include "stages/viewport_filter_stage.hpp"
#include "synchronous_stage_runner.hpp"
#include "tbb_core.hpp"
#include "version_utils.hpp"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <spdlog/spdlog.h>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief oneTBB-based replacement for InstanceRenderer's own
    /// Placement -> Design resolution (Phase 4, backend/ONETBB_INTEGRATION.md
    /// migration plan) - resolves cached, per-instance-transformable
    /// SkPictures the same way InstanceRenderer does today (same "local
    /// pixel space" convention, same {id, remaining_depth}-keyed caches,
    /// same whole-epoch invalidation) - see InstanceRenderer's own class
    /// comment for the parts of this design carried over unchanged.
    ///
    /// Deliberately NOT built from MemoizingStage/flow::graph nodes for the
    /// recursive walk itself, per the migration plan's decision 1: the
    /// recursion (which Placement recurses into which nested Layout
    /// depends on runtime database structure, decided fresh on every call)
    /// is a data-dependent tree walk, not a fixed graph topology, and doing
    /// it at up to 1,000,000-placement-per-frame granularity is a very
    /// different, far higher-frequency usage than "rebuild a graph's
    /// topology once when the frame's input set changes" (confirmed safe
    /// separately, in the sibling oneTBB_test project's own dynamic-topology
    /// experiment - see that project's src/main.cpp `sync_layer_chains`).
    /// What that same experimentation *does* license here: a stage's own
    /// compute() safely constructing and tearing down an independent nested
    /// tbb::flow::graph, even concurrently across sibling instances (see
    /// oneTBB_test's own AreaFilterStage nested-graph test) - so
    /// generate_abstract_stage_/generate_layout_stage_ below are the real
    /// MemoizingStage-based AbstractGeometryStage/LayoutGeometryStage
    /// (Phase 2/3), each wrapped in a tiny private graph + sink for
    /// synchronous invocation, not a parallel VersionedStage-based
    /// reimplementation just for this class's own use.
    ///
    /// Preserves both of InstanceRenderer's own load-bearing correctness
    /// fixes verbatim (see its own class comment for the original bugs
    /// these fixed):
    /// - build_abstract_picture/build_layout_picture_uncached construct a
    ///   fresh, call-local ViewportFilterStage/LayerVisibilityFilterStage
    ///   pair (each with its own tiny local graph) every call, never
    ///   shared members - a throwaway culling Scene's viewport_version()
    ///   is a proxy for "how many setters were called," not "what was
    ///   set," so a shared single-slot cache could otherwise reuse output
    ///   culled at the wrong scale.
    /// - generate_abstract_stage_/generate_layout_stage_ ARE persistent,
    ///   shared members, but every call site copies their own result into
    ///   a local std::vector<RenderedShape> immediately, before the
    ///   placement loop runs - a shared single-slot cache would otherwise
    ///   be overwritten by a nested recursive call for a different
    ///   LayoutId before the outer call's own reference to it was used.
    ///
    /// This iteration covers InstanceRenderer's own "Phase B" scope only
    /// (resolve_design_picture and its own recursive helpers) - Phase C
    /// (render_layout_frame, composing the final displayable Layout-view
    /// frame by sharing RasterizePictureStage/ComposeStage instances with
    /// the Abstract path) is a deliberate follow-up, once
    /// DesignRenderPipeline/SelectionGhostLayerPipeline grow the accessors
    /// it needs - the same Phase B/Phase C split the original Migration
    /// Step 3 used.
    class HierarchyResolver
    {
    public:
        /// @brief Resolves design_id's own current view - its Layout
        /// (recursed with remaining_depth - 1) if it has one AND
        /// remaining_depth > 0, otherwise its Abstract - into a cached
        /// local-pixel-space SkPicture. Returns an empty sk_sp if neither
        /// resolves (logged once per DesignId, not once per Placement).
        sk_sp<SkPicture> resolve_design_picture(const Root &root, DesignId design_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            ensure_epoch(root, view_layers, scene, scale);

            const auto key = std::tuple{design_id, remaining_depth};
            if (auto it = design_pictures_.find(key); it != design_pictures_.end())
                return it->second;

            sk_sp<SkPicture> picture = build_design_picture(root, design_id, remaining_depth, view_layers, scene, scale);
            design_pictures_.emplace(key, picture);
            return picture;
        }

        /// @brief Builds/returns layout_id's own fully-composited
        /// local-pixel-space picture: its own direct content
        /// (LayoutGeometryStage) plus, per Placement, its resolved
        /// reference_design's own picture (resolve_design_picture above)
        /// drawn through its own SkMatrix.
        sk_sp<SkPicture> build_layout_picture(const Root &root, LayoutId layout_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            ensure_epoch(root, view_layers, scene, scale);

            const auto key = std::tuple{layout_id, remaining_depth};
            if (auto it = layout_pictures_.find(key); it != layout_pictures_.end())
                return it->second;

            sk_sp<SkPicture> picture = build_layout_picture_uncached(root, layout_id, remaining_depth, view_layers, scene, scale);
            layout_pictures_.emplace(key, picture);
            return picture;
        }

        uint64_t design_picture_recompute_count() const { return recompute_count_; }

        double min_visible_instance_pixels() const { return min_visible_instance_pixels_; }
        void set_min_visible_instance_pixels(double pixels) { min_visible_instance_pixels_ = pixels; }

        /// @brief Renders the full displayable frame for a Layout view -
        /// mirrors InstanceRenderer::render_layout_frame's own role
        /// exactly (see that method's own doc comment for the full
        /// reasoning: `local_origin = scene.pan()` for this one top-level
        /// picture trades away its own pan-independence for correctness
        /// at high zoom on real absolute DBU coordinates; every nested
        /// Placement's own resolved picture keeps its usual
        /// local_origin={0,0} convention untouched).
        ///
        /// `frame` is the same FrameRenderPipeline instance `api.cpp`'s
        /// own Abstract-path rendering uses (mirrors
        /// InstanceRenderer::render_layout_frame taking a `Renderer&`) -
        /// its design/tiny-shapes/selection/ruler RasterizePictureStage
        /// instances and ComposeStage are shared directly (via
        /// run_design_rasterize/run_tiny_shapes_rasterize/
        /// run_selection_rasterize/run_ruler_rasterize/run_compose) rather
        /// than duplicated, since all are already fully generic over a
        /// caller-supplied version number. kLayoutVersionDomainTag keeps
        /// this domain's own version numbering disjoint from
        /// FrameRenderPipeline's own Abstract-path numbering - see the
        /// original class's own doc comment for the real bug (two
        /// unrelated pictures landing on the same small version number)
        /// this fixes. This class's own MouseOverlayStage/RulerOverlayStage/
        /// SelectionOverlayStage instances stay separately owned (not
        /// shared with `frame`'s own MouseTargetLayerPipeline/
        /// SelectionGhostLayerPipeline) - their content is Scene-only,
        /// never view-dependent, so duplicating them costs a little
        /// memory/CPU, not correctness, unlike the rasterize/compose
        /// stages above.
        const PixelBuffer &render_layout_frame(const Root &root, LayoutId layout_id, int hierarchy_depth, const ViewLayerSet &view_layers, const Scene &scene, FrameRenderPipeline &frame)
        {
            const int remaining_depth = std::max(0, hierarchy_depth - 1);
            ensure_epoch(root, view_layers, scene, scene.scale());

            const auto top_picture_key = std::tuple{layout_id, remaining_depth, root.mutation_version(), view_layers.generation(), scene.visibility_version(), scene.viewport_version()};
            const sk_sp<SkPicture> &local_picture = top_layout_picture_stage_.get(top_picture_key, [&]
                                                                                   { return build_layout_picture_uncached(root, layout_id, remaining_depth, view_layers, scene, scene.scale(), scene.pan()); });
            const uint64_t design_version = top_layout_picture_stage_.version();

            SkPictureRecorder recorder;
            SkCanvas *canvas = recorder.beginRecording(SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));
            draw_grid(*canvas, scene);
            if (local_picture)
                canvas->drawPicture(local_picture);
            const sk_sp<SkPicture> design_picture = recorder.finishRecordingAsPicture();

            PipelineOptions options;
            options.ctx.root = &root;
            options.ctx.view_layers = &view_layers;
            options.ctx.scene = &scene;
            options.epoch.root_mutation_version = root.mutation_version();
            options.viewport.viewport_version = scene.viewport_version();
            options.viewport.visibility_version = scene.visibility_version();
            options.interaction.mouse_version = scene.mouse_version();
            options.interaction.ruler_version = scene.ruler_version();
            options.interaction.selection_version = scene.selection_version();

            const sk_sp<SkPicture> &overlay_picture = build_overlay_picture_stage_.run(0, 0, options);
            const sk_sp<SkPicture> &ruler_overlay_picture = build_ruler_overlay_picture_stage_.run(0, 0, options);

            const SelectionOverlayRequest selection_request{.abstract_id = {}, .current_layout = layout_id, .remaining_depth = remaining_depth};
            const sk_sp<SkPicture> &selection_overlay_picture = build_selection_overlay_picture_stage_.run(selection_request, SelectionOverlayStage::data_version_for(selection_request, options), options);

            // Keeps this domain's own version numbering entirely disjoint
            // from FrameRenderPipeline's own Abstract-path one - see this
            // method's own doc comment.
            constexpr uint64_t kLayoutVersionDomainTag = uint64_t{1} << 63;
            static const sk_sp<SkPicture> kEmptyPicture; // no tiny-shapes content for a Layout view yet

            const RasterizedFrame &design_frame = frame.design_pipeline().run_design_rasterize(design_picture, kLayoutVersionDomainTag | design_version, options);
            const RasterizedFrame &tiny_frame = frame.design_pipeline().run_tiny_shapes_rasterize(kEmptyPicture, kLayoutVersionDomainTag, options);
            const RasterizedFrame &selection_frame = frame.selection_ghost_pipeline().run_selection_rasterize(selection_overlay_picture, kLayoutVersionDomainTag | build_selection_overlay_picture_stage_.last_version(), options);
            const RasterizedFrame &ruler_frame = frame.selection_ghost_pipeline().run_ruler_rasterize(ruler_overlay_picture, kLayoutVersionDomainTag | build_ruler_overlay_picture_stage_.last_version(), options);

            const uint64_t compose_version = ComposeStage::data_version_for(
                kLayoutVersionDomainTag | design_version, kLayoutVersionDomainTag,
                kLayoutVersionDomainTag | build_selection_overlay_picture_stage_.last_version(), kLayoutVersionDomainTag | build_ruler_overlay_picture_stage_.last_version(),
                kLayoutVersionDomainTag | build_overlay_picture_stage_.last_version());

            return frame.run_compose(ComposeInput{
                                          .design_frame = design_frame,
                                          .tiny_shapes_frame = tiny_frame,
                                          .selection_frame = selection_frame,
                                          .ruler_frame = ruler_frame,
                                          .overlay_picture = overlay_picture,
                                      },
                                      compose_version, options);
        }

    private:
        struct Epoch
        {
            uint64_t root_mutation_version = 0;
            uint64_t view_layers_generation = 0;
            uint64_t visibility_version = 0;
            double scale = -1.0;
            double min_visible_instance_pixels = -1.0;

            bool operator==(const Epoch &) const = default;
        };

        void ensure_epoch(const Root &root, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            const Epoch current{
                .root_mutation_version = root.mutation_version(),
                .view_layers_generation = view_layers.generation(),
                .visibility_version = scene.visibility_version(),
                .scale = scale,
                .min_visible_instance_pixels = min_visible_instance_pixels_,
            };
            if (current == epoch_)
                return;

            epoch_ = current;
            design_pictures_.clear();
            layout_pictures_.clear();
        }

        PipelineOptions options_for(const Root &root, const ViewLayerSet &view_layers, const Scene &scene) const
        {
            PipelineOptions options;
            options.ctx.root = &root;
            options.ctx.view_layers = &view_layers;
            options.ctx.scene = &scene;
            options.epoch.root_mutation_version = root.mutation_version();
            options.epoch.view_layers_generation = view_layers.generation();
            options.viewport.visibility_version = scene.visibility_version();
            return options;
        }

        sk_sp<SkPicture> build_design_picture(const Root &root, DesignId design_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            const LayoutId layout_id = root.get_design_layout(design_id);
            if (remaining_depth > 0 && layout_id.valid())
                return build_layout_picture(root, layout_id, remaining_depth - 1, view_layers, scene, scale);

            const AbstractId abstract_id = root.get_design_abstract(design_id);
            if (abstract_id.valid())
                return build_abstract_picture(root, abstract_id, view_layers, scene, scale);

            if (unresolved_logged_.insert(design_id).second)
                spdlog::warn("HierarchyResolver: Placement references DesignId {} which resolves to neither an Abstract nor a Layout (or the Layout is unreachable at remaining_depth {}) - skipping every Placement of it", to_string(design_id), remaining_depth);
            return nullptr;
        }

        sk_sp<SkPicture> build_abstract_picture(const Root &root, AbstractId abstract_id, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            recompute_count_++;

            // Fresh, call-local instances every call - see this class's
            // own doc comment for why (a throwaway culling Scene's
            // version counter is a bad content proxy).
            SynchronousStageRunner<ViewportFilterStage, std::vector<RenderedShape>, std::vector<RenderedShape>> viewport_runner{"hierarchy_viewport_filter"};
            SynchronousStageRunner<LayerVisibilityFilterStage, std::vector<RenderedShape>, std::map<ViewLayerId, std::vector<RenderedShape>>> layer_runner{"hierarchy_layer_visibility_filter"};

            const PipelineOptions options = options_for(root, view_layers, scene);
            const uint64_t geometry_data_version = AbstractGeometryStage::data_version_for(abstract_id, options);

            // generate_abstract_stage_ IS a persistent, shared member -
            // copy its result rather than binding a reference, so a
            // recursive call elsewhere in this same frame can't leave
            // `dbu_shapes` aliasing the wrong Design's data by the time
            // record_local_picture below uses it.
            const std::vector<RenderedShape> dbu_shapes = generate_abstract_stage_.run(abstract_id, geometry_data_version, options);
            return record_local_picture(dbu_shapes, geometry_data_version, viewport_runner, layer_runner, view_layers, scene, scale, {}, {}, abstract_declared_bbox(root, abstract_id));
        }

        sk_sp<SkPicture> build_layout_picture_uncached(const Root &root, LayoutId layout_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale, Point local_origin = Point{0, 0})
        {
            recompute_count_++;

            SynchronousStageRunner<ViewportFilterStage, std::vector<RenderedShape>, std::vector<RenderedShape>> viewport_runner{"hierarchy_viewport_filter"};
            SynchronousStageRunner<LayerVisibilityFilterStage, std::vector<RenderedShape>, std::map<ViewLayerId, std::vector<RenderedShape>>> layer_runner{"hierarchy_layer_visibility_filter"};

            const PipelineOptions options = options_for(root, view_layers, scene);
            const uint64_t geometry_data_version = LayoutGeometryStage::data_version_for(layout_id, options);

            // generate_layout_stage_ IS a persistent, shared member - copy
            // its result into a real local rather than binding a
            // reference, BEFORE the recursive placement loop below runs -
            // see this class's own doc comment for why (a nested recursive
            // call for a different LayoutId would otherwise evict this
            // single-slot cache out from under a still-live reference).
            const std::vector<RenderedShape> dbu_shapes = generate_layout_stage_.run(layout_id, geometry_data_version, options);

            std::vector<BuildLayoutPictureStage::ResolvedInstance> instances;
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

            std::vector<PixelRect> tiny_instance_rects;
            const double min_visible_dbu = min_visible_instance_pixels_ / scale;

            for (PlacementId placement_id : root.get_layout_placements(layout_id))
            {
                const PlacementData *placement = root.get_placement(placement_id);
                if (!placement || !placement->location || !placement->reference_design.valid())
                    continue;

                if (!design_is_resolvable(root, placement->reference_design, remaining_depth))
                {
                    resolve_design_picture(root, placement->reference_design, remaining_depth, view_layers, scene, scale);
                    continue;
                }

                const Rect child_local_bbox = resolved_local_bbox(root, placement->reference_design, remaining_depth);
                const Orientation orientation = placement->orientation.value_or(Orientation::N);
                const Geometry::InstanceTransform transform = Geometry::instance_transform(orientation, child_local_bbox, *placement->location);
                const Rect world_bbox = Geometry::transform_bbox(transform, child_local_bbox);

                expand(world_bbox);

                const double width = static_cast<double>(world_bbox.ur.x - world_bbox.ll.x);
                const double height = static_cast<double>(world_bbox.ur.y - world_bbox.ll.y);
                if (width < min_visible_dbu && height < min_visible_dbu)
                {
                    tiny_instance_rects.push_back(PixelRect{
                        .ll = PixelPoint{.x = static_cast<double>(world_bbox.ll.x - local_origin.x) * scale, .y = static_cast<double>(world_bbox.ll.y - local_origin.y) * scale},
                        .ur = PixelPoint{.x = static_cast<double>(world_bbox.ur.x - local_origin.x) * scale, .y = static_cast<double>(world_bbox.ur.y - local_origin.y) * scale},
                    });
                    continue;
                }

                sk_sp<SkPicture> child_picture = resolve_design_picture(root, placement->reference_design, remaining_depth, view_layers, scene, scale);
                if (!child_picture)
                    continue;

                instances.push_back(BuildLayoutPictureStage::ResolvedInstance{
                    .transform = to_instance_matrix(transform, local_origin, scale),
                    .picture = std::move(child_picture),
                });
            }

            return record_local_picture(dbu_shapes, geometry_data_version, viewport_runner, layer_runner, view_layers, scene, scale, instances, tiny_instance_rects, content_bbox, local_origin);
        }

        sk_sp<SkPicture> record_local_picture(const std::vector<RenderedShape> &dbu_shapes, uint64_t geometry_data_version,
                                               SynchronousStageRunner<ViewportFilterStage, std::vector<RenderedShape>, std::vector<RenderedShape>> &viewport_runner,
                                               SynchronousStageRunner<LayerVisibilityFilterStage, std::vector<RenderedShape>, std::map<ViewLayerId, std::vector<RenderedShape>>> &layer_runner,
                                               const ViewLayerSet &view_layers, const Scene &scene, double scale,
                                               const std::vector<BuildLayoutPictureStage::ResolvedInstance> &instances, const std::vector<PixelRect> &tiny_instance_rects, Rect declared_bbox, Point local_origin = Point{0, 0})
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

            // A throwaway Scene whose only job is giving ViewportFilterStage
            // a viewport rect that fully encloses content_bbox at `scale`,
            // so nothing real gets culled - not the real Scene's own
            // current pan/viewport. +2px margin absorbs floating-point
            // rounding at the exact boundary.
            Scene cull_scene;
            cull_scene.set_scale(scale);
            cull_scene.set_pan(content_bbox.ll);
            const double width_dbu = static_cast<double>(content_bbox.ur.x - content_bbox.ll.x);
            const double height_dbu = static_cast<double>(content_bbox.ur.y - content_bbox.ll.y);
            cull_scene.set_viewport_size(static_cast<int>(width_dbu * scale) + 2, static_cast<int>(height_dbu * scale) + 2);

            // Viewport culling uses cull_scene (the throwaway enclosing
            // viewport above) - but layer VISIBILITY must use the real
            // `scene`, not cull_scene: FilterByLayerVisibilityStage checks
            // Scene::is_view_layer_visible against the user's actual
            // layer show/hide toggles, which a fresh cull_scene has no
            // knowledge of (every layer defaults to visible on a
            // just-constructed Scene) - using cull_scene here would
            // silently ignore every real visibility toggle for anything
            // drawn inside a cached instance picture. Matches the
            // original InstanceRenderer::record_local_picture exactly:
            // `viewport_stage.run(..., cull_scene)` then
            // `layer_stage.run(..., scene, view_layers)` - two different
            // Scenes for two different purposes, not a copy-paste slip.
            PipelineOptions viewport_options;
            viewport_options.ctx.scene = &cull_scene;
            viewport_options.viewport.viewport_version = cull_scene.viewport_version();
            viewport_options.viewport.scale = scale;

            PipelineOptions layer_options;
            layer_options.ctx.scene = &scene;
            layer_options.ctx.view_layers = &view_layers;
            layer_options.viewport.visibility_version = scene.visibility_version();

            const std::vector<RenderedShape> &viewport_filtered = viewport_runner.run(dbu_shapes, geometry_data_version, viewport_options);
            const std::map<ViewLayerId, std::vector<RenderedShape>> &layer_filtered = layer_runner.run(viewport_filtered, geometry_data_version, layer_options);
            const auto pixel_shapes = transform_shapes_to_pixel_space(layer_filtered, local_origin, scale);

            const SkRect bounds = SkRect::MakeLTRB(
                static_cast<SkScalar>(static_cast<double>(content_bbox.ll.x - local_origin.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ll.y - local_origin.y) * scale),
                static_cast<SkScalar>(static_cast<double>(content_bbox.ur.x - local_origin.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ur.y - local_origin.y) * scale));

            return BuildLayoutPictureStage::run(bounds, pixel_shapes, instances, tiny_instance_rects, view_layers);
        }

        double min_visible_instance_pixels_ = 100.0;

        Epoch epoch_;
        std::map<std::tuple<DesignId, int>, sk_sp<SkPicture>> design_pictures_;
        std::map<std::tuple<LayoutId, int>, sk_sp<SkPicture>> layout_pictures_;
        std::set<DesignId> unresolved_logged_;
        uint64_t recompute_count_ = 0;

        // Persistent, shared shape-generation stages - see this class's
        // own doc comment for why sharing these is safe (their own result
        // is always copied, not referenced, before any recursive call
        // that could otherwise evict their single-slot cache out from
        // under a still-live reference). Each owns its own tiny private
        // graph/sink (SynchronousStageRunner), independent of the two
        // filter runners above, which are constructed fresh per call.
        SynchronousStageRunner<AbstractGeometryStage, AbstractId, std::vector<RenderedShape>> generate_abstract_stage_{"hierarchy_generate_abstract"};
        SynchronousStageRunner<LayoutGeometryStage, LayoutId, std::vector<RenderedShape>> generate_layout_stage_{"hierarchy_generate_layout"};

        // render_layout_frame's own single-slot cache for the TOP-level
        // picture specifically - see that method's own comment for why it
        // can't share layout_pictures_ (pan-dependent local_origin,
        // unlike every other entry in that map). Stays on core::VersionedStage
        // rather than a SynchronousStageRunner - it's a plain single-slot
        // memo tightly coupled to this class's own recursive
        // build_layout_picture_uncached (its own compute body can't live
        // in a standalone MemoizingStage subclass without exposing this
        // class's private methods), the same "recursion-adjacent
        // internals stay on VersionedStage" reasoning as the fresh-per-call
        // filter runners above.
        VersionedStage<std::tuple<LayoutId, int, uint64_t, uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> top_layout_picture_stage_;

        // render_layout_frame's own build-stage trio - separate, second
        // instances from FrameRenderPipeline's own MouseTargetLayerPipeline/
        // SelectionGhostLayerPipeline (Scene-only content, never view-
        // dependent, so duplicating costs a little memory/CPU, not
        // correctness - see render_layout_frame's own doc comment). The
        // shared RasterizePictureStage/ComposeStage instances that DO get
        // reused live in `frame` (a render_layout_frame parameter), not
        // here.
        SynchronousStageRunner<MouseOverlayStage, int, sk_sp<SkPicture>> build_overlay_picture_stage_{"hierarchy_mouse_overlay"};
        SynchronousStageRunner<RulerOverlayStage, int, sk_sp<SkPicture>> build_ruler_overlay_picture_stage_{"hierarchy_ruler_overlay"};
        SynchronousStageRunner<SelectionOverlayStage, SelectionOverlayRequest, sk_sp<SkPicture>> build_selection_overlay_picture_stage_{"hierarchy_selection_overlay"};
    };
}
