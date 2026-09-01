#pragma once
#include "../../core/placement_geometry.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../database/database.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../pixel_types.hpp"
#include "../synchronous_stage_runner.hpp"
#include "../tbb_core.hpp"
#include "build_layout_picture_stage.hpp"
#include "hierarchy_node_base.hpp"
#include "hierarchy_stage_support.hpp"
#include "layer_visibility_filter_stage.hpp"
#include "layout_geometry_stage.hpp"
#include "viewport_filter_stage.hpp"
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace le
{
    /// @brief One HierarchyResolver graph node per distinct {LayoutId,
    /// remaining_depth} (mirrors the old layout_pictures_ map's own key
    /// exactly - a Layout's own resolved content DOES depend on
    /// remaining_depth, unlike an Abstract's). Its own InputData is the
    /// fan-in-gathered, discovery-assigned-slot-tagged vector of
    /// already-resolved child pictures (ResolvedInstanceSlot) -
    /// wire_fan_in below wires this node's own incoming edges before it
    /// is ever reachable from HierarchyResolver's own graph_->nodes.
    /// compute() is the old build_layout_picture_uncached's own body
    /// minus its placement loop (moved to
    /// HierarchyResolver::discover_layout_children) plus a sort by
    /// discovery-assigned slot before building `instances` (see the
    /// ordering note on wire_fan_in below - determinism only, not
    /// correctness).
    ///
    /// Owns all of its own incoming wiring (incoming_edges_/fan_in_/wrap_)
    /// - this is what makes unwire_incoming() (HierarchyResolver's own
    /// stale-node sweep) tractable: it only ever needs to unwire edges it
    /// itself registered, never another node's.
    class HierarchyLayoutNodeStage : public MemoizingStage<std::vector<ResolvedInstanceSlot>, sk_sp<SkPicture>, PipelineOptions>, public HierarchyNodeBase
    {
    public:
        // No placement_labels parameter, unlike tiny_instance_rects -
        // BUGS_AND_ENHANCEMENTS.md E13's own name labels are deliberately
        // *not* baked into a recursively-cached Layout node's own picture
        // (this class's whole reason to exist): that picture is reused
        // wholesale, via a plain canvas->drawPicture(), by every parent
        // that places this same Design - baking a Layout's own *internal*
        // placements' labels into it would recurse arbitrarily deep
        // (confirmed empirically: a real 5x5 aes_5x5.def preview at
        // hierarchy_depth 2 rendered as unreadable noise, one label per
        // *every* standard-cell placement inside every instance, not just
        // the 25 top-level ones). Only build_top_layout_picture
        // (hierarchy_resolver.hpp) - the one true top-level, never-cached
        // picture - passes real placement_labels into record_local_picture;
        // every node here always passes its own default (empty, see
        // record_local_picture's own default argument) - the same "only
        // the top level of hierarchy" scoping E1 already applies to
        // selectability.
        HierarchyLayoutNodeStage(oneapi::tbb::flow::graph &g, LayoutId layout_id, std::atomic<uint64_t> &recompute_count, double scale,
                                  Rect content_bbox, std::vector<PixelRect> tiny_instance_rects, std::string label = {})
            : MemoizingStage(g, std::move(label)), layout_id_(layout_id), recompute_count_(recompute_count), scale_(scale),
              content_bbox_(content_bbox), tiny_instance_rects_(std::move(tiny_instance_rects))
        {
        }

        sk_sp<SkPicture> last_picture() const override { return last_picture_; }
        oneapi::tbb::flow::sender<StageData<sk_sp<SkPicture>, PipelineOptions>> &picture_sender() override { return node(); }

        // A zero-edge Layout (no resolvable placements) is, like a leaf,
        // a pure source - HierarchyResolver::run_pending calls this
        // directly. Only ever called when wire_fan_in was never called
        // (edges.empty()) - see the mutual-exclusion note on wire_fan_in.
        void trigger(const PipelineOptions &options) override { try_put({.data = {}, .data_version = 1, .options = options}); }

        /// @brief Wires this node's own fan-in: one adapter per edge
        /// (tagging its child's raw picture with that edge's own fixed
        /// slot + baked instance transform), a FanInCollectStage
        /// gathering exactly edges.size() of them (one per placement -
        /// not per distinct child, so repeated placements of the same
        /// design correctly produce repeated ResolvedInstanceSlot
        /// entries), and a wrap adapter bridging FanInCollectStage's bare
        /// OutputData back into the StageData<...> this node's own
        /// MemoizingStage node expects. join_node/indexer_node need
        /// compile-time-fixed arity via their template parameter pack -
        /// incompatible with a Layout's own placement count, a runtime,
        /// data-dependent value (0 to 1,000,000 in the stress fixture) -
        /// FanInCollectStage is the primitive built for exactly this.
        ///
        /// Mutually exclusive with trigger() above by construction:
        /// called only when edges is non-empty; a Layout node with zero
        /// edges is seeded via trigger() instead
        /// (HierarchyResolver::ensure_node_built's own
        /// pending_layout_triggers_).
        ///
        /// Ordering note (determinism, not correctness): FanInCollectStage
        /// runs at unlimited concurrency and delivers accumulated results
        /// in arrival order, not discovery order. The only real,
        /// documented, tested paint-order invariant in this codebase is
        /// layer z-order for a Layout's own direct content (enforced by
        /// ViewLayerSet::build_for_technology, untouched by this class) -
        /// nothing treats paint order *among sibling placements* as
        /// meaningful. So arrival-order jitter across compute() calls
        /// isn't a correctness bug; compute() still sorts by slot anyway,
        /// purely so identical repeated requests produce byte-identical
        /// SkPictures.
        ///
        /// FanInCollectStage's own "suppress output unless some input's
        /// data_version changed this round" gate is effectively inert
        /// here: this graph runs exactly one round per node in its whole
        /// lifetime (a later reference to an already-built node is a
        /// HierarchyResolver-level cache hit, never a second try_put),
        /// and every input's data_version differs from the runner's
        /// freshly-default-constructed nullopt state on that first round.
        void wire_fan_in(oneapi::tbb::flow::graph &g, const std::vector<DiscoveredEdge> &edges, const PipelineOptions &options)
        {
            if (edges.empty())
                return;

            fan_in_ = std::make_unique<FanInCollectStage<ResolvedInstanceSlot, std::vector<ResolvedInstanceSlot>, PipelineOptions>>(
                g, edges.size(),
                [](const ResolvedInstanceSlot &s) -> std::size_t
                { return s.slot; },
                [](std::vector<ResolvedInstanceSlot> &acc, const ResolvedInstanceSlot &s)
                { acc.push_back(s); },
                "hierarchy_fan_in");

            wrap_ = std::make_unique<WrapNode>(
                g, oneapi::tbb::flow::serial,
                [options](const std::vector<ResolvedInstanceSlot> &v, WrapNode::output_ports_type &out)
                {
                    // FanInCollectStage::execute() returns a default-
                    // constructed (empty) OutputData on every round
                    // except the one that completes accumulation (see
                    // its own doc comment) - as a plain function_node,
                    // the OLD wrap_ forwarded every one of those empty
                    // intermediate results too, each tagged with the
                    // SAME hardcoded data_version=1. This node's own
                    // downstream MemoizingStage (node() below) only
                    // recomputes when data_version changes, so it
                    // locked onto the FIRST (empty, still-accumulating)
                    // message as "the" version-1 result and silently
                    // ignored every later message with that same
                    // version - including the real, fully-accumulated
                    // one - leaving `instances` permanently empty
                    // whenever a Layout node had 2+ real edges. Skipping
                    // the empty ones here (multifunction_node, not
                    // function_node, so "produce no output this round"
                    // is expressible) is what makes exactly one real
                    // message ever reach node(). wire_fan_in's own
                    // edges.empty() guard above means a genuinely
                    // complete accumulation is never legitimately empty
                    // here, so this is a safe "not ready yet" signal,
                    // not a lossy heuristic.
                    if (v.empty())
                        return;
                    std::get<0>(out).try_put({.data = v, .data_version = 1, .options = options});
                });

            make_edge(fan_in_->node(), *wrap_);
            make_edge(std::get<0>(wrap_->output_ports()), node());

            incoming_edges_.reserve(edges.size());
            for (const DiscoveredEdge &e : edges)
            {
                if (e.child_node->computed)
                {
                    // Already fully computed in an earlier, already-settled
                    // top-level call - see HierarchyNodeBase::computed's
                    // own doc comment for why wiring graph propagation for
                    // it here would silently never deliver anything (its
                    // underlying function_node already fired once, before
                    // this edge existed). Feed its already-known picture
                    // directly instead - still goes through fan_in_'s own
                    // real accumulation logic (received_/fan_in_count_),
                    // just via a manual try_put instead of a delivered
                    // message.
                    fan_in_->try_put({.data = ResolvedInstanceSlot{.slot = e.slot, .transform = e.transform, .picture = e.child_node->last_picture()}, .data_version = 0, .options = options});
                    continue;
                }

                auto adapter = std::make_unique<AdapterNode>(
                    g, oneapi::tbb::flow::unlimited,
                    [slot = e.slot, transform = e.transform](StageData<sk_sp<SkPicture>, PipelineOptions> in) -> StageData<ResolvedInstanceSlot, PipelineOptions>
                    { return {.data = ResolvedInstanceSlot{.slot = slot, .transform = transform, .picture = in.data}, .data_version = in.data_version, .options = in.options}; });

                make_edge(e.child_node->picture_sender(), *adapter);
                make_edge(*adapter, fan_in_->node());

                incoming_edges_.push_back(IncomingEdge{.child_sender = &e.child_node->picture_sender(), .adapter = std::move(adapter)});
            }
        }

        // Two-pass unwire-then-destroy sweep (HierarchyResolver::
        // sweep_stale_nodes) calls this on every stale node before any of
        // them are destroyed - TBB's flow::graph nodes don't
        // self-deregister from a predecessor's/successor's edge list on
        // destruction, so remove_edge must be called explicitly before
        // either endpoint of an edge is destroyed, even when both
        // endpoints are being destroyed "together."
        void unwire_incoming() override
        {
            for (IncomingEdge &e : incoming_edges_)
            {
                oneapi::tbb::flow::remove_edge(*e.child_sender, *e.adapter);
                if (fan_in_)
                    oneapi::tbb::flow::remove_edge(*e.adapter, fan_in_->node());
            }
            if (fan_in_ && wrap_)
                oneapi::tbb::flow::remove_edge(fan_in_->node(), *wrap_);
            if (wrap_)
                oneapi::tbb::flow::remove_edge(std::get<0>(wrap_->output_ports()), node());
        }

    protected:
        sk_sp<SkPicture> compute(const std::vector<ResolvedInstanceSlot> &slots, const PipelineOptions &options) override
        {
            recompute_count_.fetch_add(1, std::memory_order_relaxed);
            const ViewLayerSet &view_layers = *options.ctx.view_layers;

            std::vector<ResolvedInstanceSlot> sorted = slots;
            std::sort(sorted.begin(), sorted.end(), [](const ResolvedInstanceSlot &a, const ResolvedInstanceSlot &b)
                      { return a.slot < b.slot; });

            std::vector<BuildLayoutPictureStage::ResolvedInstance> instances;
            instances.reserve(sorted.size());
            for (const ResolvedInstanceSlot &s : sorted)
                if (s.picture)
                    instances.push_back(BuildLayoutPictureStage::ResolvedInstance{.transform = s.transform, .picture = s.picture});

            const uint64_t geometry_data_version = LayoutGeometryStage::data_version_for(layout_id_, options);

            // generate_layout_stage_ IS a persistent, per-node member -
            // copy its result into a real local before it's used, same
            // "never alias a single-slot cache" convention as the old
            // class (see HierarchyAbstractLeafStage's own comment).
            const std::vector<RenderedShape> dbu_shapes = generate_layout_stage_.run(layout_id_, geometry_data_version, options);

            last_picture_ = record_local_picture(dbu_shapes, geometry_data_version, viewport_runner_, layer_runner_, view_layers, *options.ctx.scene, scale_, instances, tiny_instance_rects_, content_bbox_, Point{0, 0});
            computed = true;
            return last_picture_;
        }

    private:
        using AdapterNode = oneapi::tbb::flow::function_node<StageData<sk_sp<SkPicture>, PipelineOptions>, StageData<ResolvedInstanceSlot, PipelineOptions>>;
        // multifunction_node, not function_node - see wire_fan_in's own
        // comment on wrap_'s construction for why "produce no output
        // this round" (for FanInCollectStage's own intermediate
        // still-accumulating results) has to be expressible here.
        using WrapNode = oneapi::tbb::flow::multifunction_node<std::vector<ResolvedInstanceSlot>, std::tuple<StageData<std::vector<ResolvedInstanceSlot>, PipelineOptions>>>;

        struct IncomingEdge
        {
            oneapi::tbb::flow::sender<StageData<sk_sp<SkPicture>, PipelineOptions>> *child_sender = nullptr;
            std::unique_ptr<AdapterNode> adapter;
        };

        LayoutId layout_id_;
        std::atomic<uint64_t> &recompute_count_;
        double scale_;
        Rect content_bbox_;
        std::vector<PixelRect> tiny_instance_rects_;
        std::vector<PlacementLabel> placement_labels_;
        sk_sp<SkPicture> last_picture_;

        std::vector<IncomingEdge> incoming_edges_;
        std::unique_ptr<FanInCollectStage<ResolvedInstanceSlot, std::vector<ResolvedInstanceSlot>, PipelineOptions>> fan_in_;
        std::unique_ptr<WrapNode> wrap_;

        SynchronousStageRunner<ViewportFilterStage, std::vector<RenderedShape>, std::vector<RenderedShape>> viewport_runner_{"hierarchy_viewport_filter"};
        SynchronousStageRunner<LayerVisibilityFilterStage, std::vector<RenderedShape>, std::map<ViewLayerId, std::vector<RenderedShape>>> layer_runner_{"hierarchy_layer_visibility_filter"};
        SynchronousStageRunner<LayoutGeometryStage, LayoutId, std::vector<RenderedShape>> generate_layout_stage_{"hierarchy_generate_layout"};
    };
}
