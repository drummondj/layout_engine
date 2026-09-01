#pragma once
#include "../core/placement_geometry.hpp"
#include "../core/versioned_stage.hpp"
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "draw_helpers.hpp"
#include "stages/build_layout_picture_stage.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include "pipeline_options.hpp"
#include "rasterize_compose_pipeline.hpp"
#include "stages/abstract_geometry_stage.hpp"
#include "stages/hierarchy_abstract_leaf_stage.hpp"
#include "stages/hierarchy_layout_node_stage.hpp"
#include "stages/hierarchy_node_base.hpp"
#include "stages/hierarchy_stage_support.hpp"
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
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <spdlog/spdlog.h>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief oneTBB-based Placement -> Design -> Layout hierarchy
    /// resolver, built on MemoizingStage + a dynamically-built
    /// tbb::flow::graph (backend/ONETBB_INTEGRATION.md migration,
    /// follow-up rewrite of the original plain-recursion +
    /// core::VersionedStage design - see git history for that version and
    /// its own rationale). Resolves cached, per-instance-transformable
    /// SkPictures the same way the original design did (same "local pixel
    /// space" convention, same whole-epoch invalidation on a database
    /// mutation/view-layer/visibility/scale change) - see NodeKey's own
    /// doc comment (stages/hierarchy_node_base.hpp) for the caching key
    /// scheme this now runs on instead of the old {id, remaining_depth}-
    /// keyed std::map pair.
    ///
    /// The original design deliberately avoided MemoizingStage/flow::graph
    /// for the recursive walk itself, reasoning that a Placement -> Layout
    /// recursion is a data-dependent tree shape decided fresh per call
    /// (not a fixed graph topology), and that a flow::graph node's own
    /// task body calling try_put/blocking wait_for_all back into its
    /// *own* enclosing graph is a real TBB reentrancy hazard. This
    /// rewrite still avoids that exact hazard - no node's own compute()
    /// ever calls back into the graph it's a node of - but resolves the
    /// "data-dependent topology" problem differently: a single-threaded
    /// discovery pass (discover_layout_children/ensure_node_built) walks
    /// the database first, deciding the graph's own shape (which nodes
    /// exist, which edges connect them) *before* any node ever executes,
    /// incrementally extending one persistent, epoch-scoped flow::graph
    /// (graph_) as new {AbstractId}/{LayoutId, remaining_depth} keys are
    /// discovered rather than rebuilding a fixed topology from scratch
    /// per call. A Layout node's own placement count - a runtime,
    /// data-dependent value from 0 to 1,000,000 in the stress fixture -
    /// is handled by FanInCollectStage (a variable-arity many-in/one-out
    /// accumulator, tbb_core.hpp), not join_node/indexer_node (which need
    /// compile-time-fixed arity).
    ///
    /// One graph node per distinct NodeKey (not one per Placement) is
    /// what preserves the original design's own caching benefit: a design
    /// placed 100 times in one Layout is walked/computed exactly once,
    /// its single computed picture broadcast to all 100 placements' own
    /// per-edge adapters via TBB's own multi-successor fan-out. Node
    /// lifetime is bounded within one epoch by generation-stamped,
    /// reachability-based pruning (HierarchyNodeBase::last_touched_generation,
    /// touch_children, sweep_stale_nodes below) - needed because each
    /// node now carries three permanent private nested-graph runners (not
    /// just an sk_sp<SkPicture>), and a scene.hierarchy_depth() change
    /// alone doesn't bump Epoch (only root_mutation_version/
    /// view_layers_generation/visibility_version/scale/
    /// min_visible_instance_pixels do), so without pruning nodes made
    /// stale by a depth change alone would accumulate for the epoch's
    /// whole lifetime.
    ///
    /// Preserves both of the original design's own load-bearing
    /// correctness fixes verbatim, now at per-node granularity instead of
    /// per-call:
    /// - Each node's own ViewportFilterStage/LayerVisibilityFilterStage
    ///   runners are permanent, per-node members, not shared or
    ///   fresh-per-call - see HierarchyAbstractLeafStage/
    ///   HierarchyLayoutNodeStage's own doc comments for why this is
    ///   *strictly safer* than the original fresh-per-call rule, not
    ///   merely still-safe (each node is permanently bound to one id for
    ///   its whole lifetime, and MemoizingStage's own tbb::flow::serial
    ///   concurrency means a node's compute() never runs concurrently
    ///   with itself).
    /// - Each node's own AbstractGeometryStage/LayoutGeometryStage runner
    ///   copies its result into a real local before use, so a nested
    ///   discovery/compute elsewhere in the same call can't evict a
    ///   single-slot cache out from under a still-live reference.
    ///
    /// This iteration covers the same scope as the design it replaces:
    /// resolve_design_picture/build_layout_picture (the recursive-resolve
    /// surface) plus render_layout_frame (the full displayable-frame
    /// surface, sharing RasterizePictureStage/ComposeStage instances with
    /// the Abstract path via FrameRenderPipeline).
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
            ZoneScopedN("HierarchyResolver: resolve_design_picture");
            ensure_epoch(root, view_layers, scene, scale);
            graph_->current_generation++;

            const DesignTarget target = resolve_design_target(root, design_id, remaining_depth);
            sk_sp<SkPicture> result;
            if (target.kind == DesignTarget::Kind::None)
            {
                if (unresolved_logged_.insert(design_id).second)
                    spdlog::warn("HierarchyResolver: Placement references DesignId {} which resolves to neither an Abstract nor a Layout (or the Layout is unreachable at remaining_depth {}) - skipping every Placement of it", to_string(design_id), remaining_depth);
            }
            else
            {
                const NodeKey key = node_key_for_target(target, remaining_depth);
                HierarchyNodeBase *node = ensure_node_built(key, root, view_layers, scene, scale);
                run_pending(root, view_layers, scene);
                result = node->last_picture();
            }

            sweep_stale_nodes();
            return result;
        }

        /// @brief Builds/returns layout_id's own fully-composited
        /// local-pixel-space picture: its own direct content
        /// (LayoutGeometryStage) plus, per Placement, its resolved
        /// reference_design's own picture (resolve_design_picture above)
        /// drawn through its own SkMatrix.
        sk_sp<SkPicture> build_layout_picture(const Root &root, LayoutId layout_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            ZoneScopedN("HierarchyResolver: build_layout_picture");
            ensure_epoch(root, view_layers, scene, scale);
            graph_->current_generation++;

            const NodeKey key{.kind = NodeKey::Kind::Layout, .abstract_id = {}, .layout_id = layout_id, .remaining_depth = remaining_depth};
            HierarchyNodeBase *node = ensure_node_built(key, root, view_layers, scene, scale);
            run_pending(root, view_layers, scene);
            sk_sp<SkPicture> result = node->last_picture();

            sweep_stale_nodes();
            return result;
        }

        uint64_t design_picture_recompute_count() const { return recompute_count_.load(std::memory_order_relaxed); }

        double min_visible_instance_pixels() const { return min_visible_instance_pixels_; }
        void set_min_visible_instance_pixels(double pixels) { min_visible_instance_pixels_ = pixels; }

        // Test-only accessor (verification's pruning-correctness fixture)
        // - the number of graph nodes currently live this epoch.
        std::size_t node_count_for_test() const { return graph_ ? graph_->nodes.size() : 0; }
        bool has_node_for_test(LayoutId layout_id, int remaining_depth) const
        {
            if (!graph_)
                return false;
            const NodeKey key{.kind = NodeKey::Kind::Layout, .abstract_id = {}, .layout_id = layout_id, .remaining_depth = remaining_depth};
            return graph_->nodes.find(key) != graph_->nodes.end();
        }

        /// @brief Renders the full displayable frame for a Layout view -
        /// `local_origin = scene.pan()` for this one top-level picture
        /// trades away its own pan-independence for correctness at high
        /// zoom on real absolute DBU coordinates; every nested Placement's
        /// own resolved picture keeps its usual local_origin={0,0}
        /// convention untouched.
        ///
        /// This class owns its own private MouseOverlayStage/
        /// RulerOverlayStage/SelectionOverlayStage trio AND its own
        /// private RasterizeComposePipeline, entirely separate from
        /// FrameRenderPipeline's own equivalents (used for the
        /// Abstract-path instead) - a pipeline is a self-contained graph
        /// of stages, so this class reaches into another pipeline's
        /// output, never into its internal nodes (see
        /// RasterizeComposePipeline's own doc comment; this replaced an
        /// earlier design that shared FrameRenderPipeline's rasterize/
        /// compose instances directly via a run_design_rasterize-style
        /// bypass, tagging its own version numbers with a
        /// kLayoutVersionDomainTag high bit to keep them disjoint from
        /// FrameRenderPipeline's - fragile, and the reason a later
        /// per-ViewLayerId rasterize optimization landed in
        /// DesignRenderPipeline without ever reaching this path, since
        /// the bypass fed it a single already-composited picture instead
        /// of the shape map that optimization required). Two independent
        /// MemoizingStage-based pipelines means no shared cache slot, so
        /// there's nothing left to keep disjoint.
        ///
        /// top_layout_picture_stage_ stays a plain core::VersionedStage,
        /// now a thin cache *in front of* the epoch's graph rather than a
        /// node inside it - turning it into a graph node would force a
        /// node rebuild on every pan tick, defeating "rebuild only on
        /// epoch change." On a cache MISS, its compute lambda calls
        /// discover_layout_children for the top {layout_id,
        /// remaining_depth} to get its own edges/tiny-rects (not building
        /// a node for the top level itself - only its *children* need to
        /// be real graph nodes, since the top picture's own pan-baked
        /// content is never reused by anything else), run_pending to
        /// settle any newly discovered children, generates this Layout's
        /// own direct-content geometry via dedicated top_geometry_runner_/
        /// top_viewport_runner_/top_layer_runner_ members (persistent,
        /// single-call-path-safe - this method is never invoked
        /// concurrently with itself), then calls the shared
        /// record_local_picture helper with local_origin=scene.pan(). On
        /// a cache HIT (the ~0ms warm path), none of that runs at all -
        /// the underlying node graph for this frame's own subtree may
        /// have since been pruned away by an intervening call elsewhere;
        /// that's harmless, since the cached SkPicture already holds its
        /// own ref-counted references to every child picture it drew,
        /// independent of the node objects' own lifetime.
        const PixelBuffer &render_layout_frame(const Root &root, LayoutId layout_id, int hierarchy_depth, const ViewLayerSet &view_layers, const Scene &scene)
        {
            ZoneScopedN("HierarchyResolver: render_layout_frame");

            const int remaining_depth = std::max(0, hierarchy_depth - 1);
            ensure_epoch(root, view_layers, scene, scene.scale());
            graph_->current_generation++;

            const auto top_picture_key = std::tuple{layout_id, remaining_depth, root.mutation_version(), view_layers.generation(), scene.visibility_version(), scene.viewport_version()};
            const sk_sp<SkPicture> &local_picture = top_layout_picture_stage_.get(top_picture_key, [&]
                                                                                  { return build_top_layout_picture(root, layout_id, remaining_depth, view_layers, scene); });
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

            static const sk_sp<SkPicture> kEmptyPicture; // no tiny-shapes content for a Layout view yet

            const PixelBuffer &composed = rasterize_compose_.run(RasterizeComposePipeline::Input{
                                                                      .design_picture = design_picture,
                                                                      .design_version = design_version,
                                                                      .tiny_shapes_picture = kEmptyPicture,
                                                                      .tiny_shapes_version = 0,
                                                                      .selection_picture = selection_overlay_picture,
                                                                      .selection_version = build_selection_overlay_picture_stage_.last_version(),
                                                                      .ruler_picture = ruler_overlay_picture,
                                                                      .ruler_version = build_ruler_overlay_picture_stage_.last_version(),
                                                                      .overlay_picture = overlay_picture,
                                                                      .overlay_version = build_overlay_picture_stage_.last_version(),
                                                                  },
                                                                  options);
            sweep_stale_nodes();
            return composed;
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

        /// @brief The persistent, epoch-scoped flow::graph plus every
        /// live NodeKey's own graph node - see HierarchyResolver's own
        /// class doc comment. No graph-global plumbing list: each Layout
        /// node owns its own incoming wiring (HierarchyLayoutNodeStage::
        /// incoming_edges_/fan_in_/wrap_), so there's nothing else for
        /// this struct itself to track. current_generation increments by
        /// one at the start of every top-level public call - see
        /// touch_children/sweep_stale_nodes below.
        struct HierarchyGraph
        {
            oneapi::tbb::flow::graph flow_graph;
            std::map<NodeKey, std::unique_ptr<HierarchyNodeBase>> nodes;
            uint64_t current_generation = 0;
        };

        static NodeKey node_key_for_target(const DesignTarget &target, int remaining_depth)
        {
            if (target.kind == DesignTarget::Kind::Layout)
                return NodeKey{.kind = NodeKey::Kind::Layout, .abstract_id = {}, .layout_id = target.layout_id, .remaining_depth = remaining_depth - 1};
            return NodeKey{.kind = NodeKey::Kind::Abstract, .abstract_id = target.abstract_id, .layout_id = {}, .remaining_depth = 0};
        }

        void ensure_epoch(const Root &root, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            const Epoch current{
                .root_mutation_version = root.mutation_version(),
                .view_layers_generation = view_layers.generation(),
                .visibility_version = scene.visibility_version(),
                .scale = scale,
                .min_visible_instance_pixels = min_visible_instance_pixels_,
            };
            if (graph_ && current == epoch_)
                return;

            epoch_ = current;
            graph_ = std::make_unique<HierarchyGraph>();
            pending_leaf_triggers_.clear();
            pending_layout_triggers_.clear();
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

        /// @brief Result of walking layout_id's own direct Placements
        /// (the discovery pass, §2) - which children it references
        /// (deduped edges/children), which placements collapse to a
        /// tiny-instance outline instead (sub-pixel culling), and the
        /// declared+placement-expanded content bbox. Recursion here is
        /// over *discovery* (via ensure_node_built), not computation, and
        /// terminates on the same remaining_depth-strictly-decreases-on-
        /// every-Layout->Layout-hop guarantee the old recursive code
        /// already relied on.
        struct DiscoverResult
        {
            Rect content_bbox;
            std::vector<DiscoveredEdge> edges;
            std::vector<PixelRect> tiny_instance_rects;
            std::vector<PlacementLabel> placement_labels;
            std::vector<NodeKey> children; // deduped, for touch_children
        };

        DiscoverResult discover_layout_children(LayoutId layout_id, int remaining_depth, const Root &root, const ViewLayerSet &view_layers, const Scene &scene, double scale, Point local_origin = Point{0, 0})
        {
            DiscoverResult result;
            result.content_bbox = layout_declared_bbox(root, layout_id);
            bool have_content_bbox = result.content_bbox.ll.x != result.content_bbox.ur.x || result.content_bbox.ll.y != result.content_bbox.ur.y;

            auto expand = [&](Rect r)
            {
                if (!have_content_bbox)
                {
                    result.content_bbox = r;
                    have_content_bbox = true;
                    return;
                }
                result.content_bbox.ll.x = std::min(result.content_bbox.ll.x, r.ll.x);
                result.content_bbox.ll.y = std::min(result.content_bbox.ll.y, r.ll.y);
                result.content_bbox.ur.x = std::max(result.content_bbox.ur.x, r.ur.x);
                result.content_bbox.ur.y = std::max(result.content_bbox.ur.y, r.ur.y);
            };

            const double min_visible_dbu = min_visible_instance_pixels_ / scale;
            // Same "genuinely invisible" threshold ViewportFilterStage/
            // TinyViewportFilterStage already use for real shapes (1
            // device pixel in both dimensions) - a placement under
            // min_visible_dbu still gets a real outline rect drawn
            // (below) down to any nonzero size, which at high placement
            // density (thousands of adjacent standard cells, zoomed
            // out) rasterizes as a dense grid of hairline-adjacent
            // rectangles that reads as a solid block, not useful
            // information - once a placement is ALSO sub-pixel in both
            // dimensions, its own outline conveys nothing a real user
            // could see, so skip it entirely rather than drawing it.
            const double sub_pixel_dbu = 1.0 / scale;
            std::set<NodeKey> seen_children;

            for (PlacementId placement_id : root.get_layout_placements(layout_id))
            {
                const PlacementData *placement = root.get_placement(placement_id);
                if (!placement || !placement->location || !placement->reference_design.valid())
                    continue;

                const DesignTarget target = resolve_design_target(root, placement->reference_design, remaining_depth);
                if (target.kind == DesignTarget::Kind::None)
                {
                    if (unresolved_logged_.insert(placement->reference_design).second)
                        spdlog::warn("HierarchyResolver: Placement references DesignId {} which resolves to neither an Abstract nor a Layout (or the Layout is unreachable at remaining_depth {}) - skipping every Placement of it", to_string(placement->reference_design), remaining_depth);
                    continue;
                }

                const Rect child_local_bbox = target.kind == DesignTarget::Kind::Layout ? layout_declared_bbox(root, target.layout_id) : abstract_declared_bbox(root, target.abstract_id);
                const Orientation orientation = placement->orientation.value_or(Orientation::N);
                const Geometry::InstanceTransform transform = Geometry::instance_transform(orientation, child_local_bbox, *placement->location);
                const Rect world_bbox = Geometry::transform_bbox(transform, child_local_bbox);

                expand(world_bbox);

                const double width = static_cast<double>(world_bbox.ur.x - world_bbox.ll.x);
                const double height = static_cast<double>(world_bbox.ur.y - world_bbox.ll.y);
                if (width < min_visible_dbu && height < min_visible_dbu)
                {
                    if (width < sub_pixel_dbu && height < sub_pixel_dbu)
                        continue; // sub-pixel outline would be visual noise, not information

                    result.tiny_instance_rects.push_back(PixelRect{
                        .ll = PixelPoint{.x = static_cast<double>(world_bbox.ll.x - local_origin.x) * scale, .y = static_cast<double>(world_bbox.ll.y - local_origin.y) * scale},
                        .ur = PixelPoint{.x = static_cast<double>(world_bbox.ur.x - local_origin.x) * scale, .y = static_cast<double>(world_bbox.ur.y - local_origin.y) * scale},
                    });
                    continue;
                }

                // BUGS_AND_ENHANCEMENTS.md E13 - only real (non-tiny)
                // placements get a label, computed in the same
                // parent-local pixel space tiny_instance_rects already
                // uses above, so BuildLayoutPictureStage::run needs no
                // further transform to draw either one.
                result.placement_labels.push_back(PlacementLabel{
                    .name = placement->name,
                    .rect = PixelRect{
                        .ll = PixelPoint{.x = static_cast<double>(world_bbox.ll.x - local_origin.x) * scale, .y = static_cast<double>(world_bbox.ll.y - local_origin.y) * scale},
                        .ur = PixelPoint{.x = static_cast<double>(world_bbox.ur.x - local_origin.x) * scale, .y = static_cast<double>(world_bbox.ur.y - local_origin.y) * scale},
                    },
                });

                const NodeKey child_key = node_key_for_target(target, remaining_depth);
                HierarchyNodeBase *child_node = ensure_node_built(child_key, root, view_layers, scene, scale);
                if (seen_children.insert(child_key).second)
                    result.children.push_back(child_key);

                result.edges.push_back(DiscoveredEdge{
                    .child_key = child_key,
                    .child_node = child_node,
                    .slot = result.edges.size(),
                    .transform = to_instance_matrix(transform, local_origin, scale),
                });
            }

            return result;
        }

        /// @brief Returns key's own graph node, constructing it (and,
        /// recursively, every not-yet-discovered descendant it needs) on
        /// a first sighting this epoch. graph_->nodes.find(key) is
        /// checked first - a key already discovered this epoch is a pure
        /// map lookup, no new graph construction, which is what preserves
        /// the near-0ms warm path. On EITHER path, the returned node is
        /// stamped with the current generation and its own already-known
        /// children are touch-propagated (touch_children) - this is what
        /// makes "reachable from any request made so far this generation"
        /// fall out of the existing cache-hit path for free, and is the
        /// load-bearing invariant sweep_stale_nodes's own pruning depends
        /// on: a node touched this generation only ever has children also
        /// touched this generation, so a still-live node can never
        /// reference a node about to be pruned.
        HierarchyNodeBase *ensure_node_built(const NodeKey &key, const Root &root, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            if (auto it = graph_->nodes.find(key); it != graph_->nodes.end())
            {
                it->second->last_touched_generation = graph_->current_generation;
                touch_children(*it->second);
                return it->second.get();
            }

            if (key.kind == NodeKey::Kind::Abstract)
            {
                auto owned = std::make_unique<HierarchyAbstractLeafStage>(graph_->flow_graph, key.abstract_id, recompute_count_, scale, "hierarchy_abstract_leaf");
                owned->last_touched_generation = graph_->current_generation;
                HierarchyNodeBase *raw = owned.get();
                graph_->nodes.emplace(key, std::move(owned));
                pending_leaf_triggers_.push_back(raw);
                return raw;
            }

            // Kind::Layout - discover its own children FIRST (recursing
            // into ensure_node_built for each), so by the time this
            // node's own wire_fan_in runs, every child it references
            // already has a real graph node to wire from.
            DiscoverResult disc = discover_layout_children(key.layout_id, key.remaining_depth, root, view_layers, scene, scale);

            // disc.placement_labels is deliberately dropped here, not
            // forwarded - see HierarchyLayoutNodeStage's own constructor
            // comment for why (BUGS_AND_ENHANCEMENTS.md E13 - only
            // build_top_layout_picture's own one-off, never-cached
            // picture draws placement labels).
            auto owned = std::make_unique<HierarchyLayoutNodeStage>(graph_->flow_graph, key.layout_id, recompute_count_, scale, disc.content_bbox, std::move(disc.tiny_instance_rects), "hierarchy_layout_node");
            owned->children = std::move(disc.children);
            const bool needs_trigger = disc.edges.empty();
            if (!needs_trigger)
                owned->wire_fan_in(graph_->flow_graph, disc.edges, options_for(root, view_layers, scene));
            owned->last_touched_generation = graph_->current_generation;

            HierarchyNodeBase *raw = owned.get();
            graph_->nodes.emplace(key, std::move(owned));
            if (needs_trigger)
                pending_layout_triggers_.push_back(raw);
            return raw;
        }

        // Cheap recursive touch - map lookups only, no DB access
        // (discovery/geometry never re-runs here) - linear in
        // reachable-node count per call, not per placement, thanks to
        // skipping any child already stamped this generation. Naturally
        // cycle-safe on the same remaining_depth-strictly-decreases
        // grounds as discovery itself.
        void touch_children(HierarchyNodeBase &node)
        {
            for (const NodeKey &child_key : node.children)
            {
                auto it = graph_->nodes.find(child_key);
                if (it == graph_->nodes.end())
                    continue; // shouldn't happen given the touch invariant - defensive only
                if (it->second->last_touched_generation == graph_->current_generation)
                    continue;
                it->second->last_touched_generation = graph_->current_generation;
                touch_children(*it->second);
            }
        }

        // Explicitly try_puts every node discovered-but-not-yet-triggered
        // this call (a leaf, always a pure source, or a zero-edge Layout
        // node) then settles the whole newly-extended subtree with a
        // single wait_for_all() - a node with edges needs no explicit
        // trigger here, since its own fan-in fires automatically once its
        // own children's outputs propagate up to it.
        //
        // wait_for_all() runs UNCONDITIONALLY, even when both pending-
        // trigger lists are empty: HierarchyLayoutNodeStage::wire_fan_in's
        // own "already computed" shortcut (a child node reused from an
        // earlier, already-settled top-level call this epoch) seeds work
        // via a manual fan_in_->try_put(...) call that is never recorded
        // in either pending list - a top-level request resolved entirely
        // through already-cached children (e.g. two roots sharing a
        // child, see IncrementalMultiRootDiscoverySharesAChildAcrossTwoRootsWithinOneEpoch)
        // can therefore have real in-flight async work with nothing
        // pending. Skipping wait_for_all() in that case used to race
        // node->last_picture() and sweep_stale_nodes()'s own node
        // destruction against that still-running task - a genuine
        // use-after-free/data-race bug (found via ThreadSanitizer-style
        // debugging: a "Pure virtual function called" abort at teardown,
        // plus flaky recompute-count/null-picture failures), not merely
        // theoretical. wait_for_all() on an already-quiescent graph
        // returns immediately, so this costs nothing extra on the warm
        // (nothing-changed) path.
        void run_pending(const Root &root, const ViewLayerSet &view_layers, const Scene &scene)
        {
            const PipelineOptions options = options_for(root, view_layers, scene);
            for (HierarchyNodeBase *n : pending_leaf_triggers_)
                n->trigger(options);
            for (HierarchyNodeBase *n : pending_layout_triggers_)
                n->trigger(options);

            pending_leaf_triggers_.clear();
            pending_layout_triggers_.clear();
            graph_->flow_graph.wait_for_all();
        }

        // "Not touched by this call or the previous one" - a node
        // survives being touched in either of the last kStaleGenerationWindow+1
        // generations (including the current one), so two callers
        // alternating between two roots every other call don't thrash
        // each other's nodes out; it's pruned once that many full calls
        // have passed with no touch at all.
        static constexpr uint64_t kStaleGenerationWindow = 1;

        // Reachability-based pruning, run after every top-level public
        // call's own run_pending()/wait_for_all() settles. Two passes,
        // never interleaved: (1) unwire every stale node's own incoming
        // wiring (a no-op for a leaf), for the ENTIRE stale set, before
        // (2) destroying any of them - TBB's flow::graph nodes don't
        // self-deregister from a predecessor's/successor's edge list on
        // destruction, so remove_edge must run before either endpoint of
        // an edge is destroyed. By the touch-propagation invariant in
        // ensure_node_built/touch_children above, every edge touching a
        // stale node has *both* endpoints in the stale set (a live node
        // can't have a stale child) - so this sweep never touches,
        // unwires, or invalidates anything reachable from a still-live
        // root.
        void sweep_stale_nodes()
        {
            if (!graph_ || graph_->current_generation <= kStaleGenerationWindow)
                return;

            const uint64_t threshold = graph_->current_generation - kStaleGenerationWindow;
            std::vector<NodeKey> stale;
            for (const auto &[key, node] : graph_->nodes)
                if (node->last_touched_generation < threshold)
                    stale.push_back(key);

            if (stale.empty())
                return;

            for (const NodeKey &key : stale)
                graph_->nodes.at(key)->unwire_incoming();
            for (const NodeKey &key : stale)
                graph_->nodes.erase(key);
        }

        // render_layout_frame's own cache-miss path - see that method's
        // own doc comment. Not a graph node itself: only its *children*
        // need to be real graph nodes, since this top-level picture's
        // own pan-baked content is never reused by anything else.
        sk_sp<SkPicture> build_top_layout_picture(const Root &root, LayoutId layout_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene)
        {
            ZoneScopedN("HierarchyResolver: build_top_layout_picture");
            recompute_count_.fetch_add(1, std::memory_order_relaxed);

            DiscoverResult disc = discover_layout_children(layout_id, remaining_depth, root, view_layers, scene, scene.scale(), scene.pan());
            run_pending(root, view_layers, scene);

            std::vector<ResolvedInstanceSlot> resolved;
            resolved.reserve(disc.edges.size());
            for (const DiscoveredEdge &e : disc.edges)
                resolved.push_back(ResolvedInstanceSlot{.slot = e.slot, .transform = e.transform, .picture = e.child_node->last_picture()});
            std::sort(resolved.begin(), resolved.end(), [](const ResolvedInstanceSlot &a, const ResolvedInstanceSlot &b)
                      { return a.slot < b.slot; });

            std::vector<BuildLayoutPictureStage::ResolvedInstance> instances;
            instances.reserve(resolved.size());
            for (const ResolvedInstanceSlot &r : resolved)
                if (r.picture)
                    instances.push_back(BuildLayoutPictureStage::ResolvedInstance{.transform = r.transform, .picture = r.picture});

            const PipelineOptions top_options = options_for(root, view_layers, scene);
            const uint64_t geometry_data_version = LayoutGeometryStage::data_version_for(layout_id, top_options);
            const std::vector<RenderedShape> dbu_shapes = top_geometry_runner_.run(layout_id, geometry_data_version, top_options);

            return record_local_picture(dbu_shapes, geometry_data_version, top_viewport_runner_, top_layer_runner_, view_layers, scene, scene.scale(), instances, disc.tiny_instance_rects, disc.content_bbox, scene.pan(), disc.placement_labels);
        }

        double min_visible_instance_pixels_ = 100.0;

        Epoch epoch_;
        std::unique_ptr<HierarchyGraph> graph_;
        std::vector<HierarchyNodeBase *> pending_leaf_triggers_;
        std::vector<HierarchyNodeBase *> pending_layout_triggers_;
        std::set<DesignId> unresolved_logged_;

        // Bumped from every node's own compute() (via a reference
        // captured at node construction) - std::atomic, not a plain
        // uint64_t: distinct sibling nodes' own compute() calls can, in
        // principle, run concurrently once triggered (each touches only
        // its own private nested-graph runners, per this class's own doc
        // comment), so this shared counter needs to be safe under that,
        // even though today's call patterns are still effectively serial.
        std::atomic<uint64_t> recompute_count_{0};

        // render_layout_frame's own single-slot cache for the TOP-level
        // picture specifically - see that method's own comment for why it
        // can't be a graph node (pan-dependent local_origin, unlike every
        // node in graph_).
        VersionedStage<std::tuple<LayoutId, int, uint64_t, uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> top_layout_picture_stage_;

        // render_layout_frame's own top-level geometry/filter runners -
        // persistent members, safe for the same "never invoked
        // concurrently with itself" reason as the old design's own
        // equivalents.
        SynchronousStageRunner<ViewportFilterStage, std::vector<RenderedShape>, std::vector<RenderedShape>> top_viewport_runner_{"hierarchy_top_viewport_filter"};
        SynchronousStageRunner<LayerVisibilityFilterStage, std::vector<RenderedShape>, std::map<ViewLayerId, std::vector<RenderedShape>>> top_layer_runner_{"hierarchy_top_layer_visibility_filter"};
        SynchronousStageRunner<LayoutGeometryStage, LayoutId, std::vector<RenderedShape>> top_geometry_runner_{"hierarchy_top_generate_layout"};

        // render_layout_frame's own build-stage trio - separate, second
        // instances from FrameRenderPipeline's own MouseTargetLayerPipeline/
        // SelectionGhostLayerPipeline (Scene-only content, never view-
        // dependent, so duplicating costs a little memory/CPU, not
        // correctness - see render_layout_frame's own doc comment).
        SynchronousStageRunner<MouseOverlayStage, int, sk_sp<SkPicture>> build_overlay_picture_stage_{"hierarchy_mouse_overlay"};
        SynchronousStageRunner<RulerOverlayStage, int, sk_sp<SkPicture>> build_ruler_overlay_picture_stage_{"hierarchy_ruler_overlay"};
        SynchronousStageRunner<SelectionOverlayStage, SelectionOverlayRequest, sk_sp<SkPicture>> build_selection_overlay_picture_stage_{"hierarchy_selection_overlay"};

        // render_layout_frame's own private rasterize+compose pipeline -
        // separate from FrameRenderPipeline's own equivalent instance, so
        // this class never reaches into another pipeline's internal
        // stage nodes (see this class's own doc comment and
        // RasterizeComposePipeline's own).
        RasterizeComposePipeline rasterize_compose_;
    };
}
