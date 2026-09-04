#pragma once
#include "../../core/placement_geometry.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../database/database.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../synchronous_stage_runner.hpp"
#include "../tbb_core.hpp"
#include "abstract_geometry_stage.hpp"
#include "hierarchy_node_base.hpp"
#include "hierarchy_stage_support.hpp"
#include "layer_visibility_filter_stage.hpp"
#include "viewport_filter_stage.hpp"
#include <atomic>
#include <string>
#include <utility>

namespace le
{
    /// @brief One HierarchyResolver graph node per distinct AbstractId
    /// (Kind::Abstract's own NodeKey has no remaining_depth component - an
    /// Abstract's own content never depends on it, see NodeKey's own doc
    /// comment) - resolves and caches that Abstract's own local-pixel-space
    /// SkPicture. compute()'s own body is the old
    /// HierarchyResolver::build_abstract_picture's body, moved verbatim,
    /// except generate_abstract_stage_/viewport_then_layer_chain_ are now
    /// permanent, per-node members instead of a HierarchyResolver-wide
    /// shared member (generate_abstract_stage_) plus fresh-per-call locals.
    ///
    /// Per-node-permanent runners are safe to reuse across many calls to
    /// this same node's own compute() (this node is permanently and
    /// exclusively bound to one AbstractId for its entire lifetime, so
    /// there's no risk of aliasing a DIFFERENT id's own stale output the
    /// way a single shared-across-many-ids runner once could) - but reuse
    /// across calls at *different scales* for the SAME id needed its own
    /// real fix: see ViewportThenLayerVisibilityChain's own doc comment
    /// (synchronous_stage_runner.hpp) for the sub-pixel-cull staleness
    /// bug a hand-threaded data_version between two separate
    /// SynchronousStageRunners used to allow, and BUGS_AND_ENHANCEMENTS.md
    /// B3's own postmortem for how it actually manifested. This node's
    /// own MemoizingStage runs at tbb::flow::serial concurrency, so its
    /// own compute() never runs concurrently with itself; its private
    /// nested-graph runners are therefore only ever exercised one at a
    /// time. Concurrent *sibling* nodes each touch only their own private
    /// nested graphs - the same pattern already proven safe and in
    /// production use for generate_abstract_stage_/generate_layout_stage_
    /// before this rewrite.
    class HierarchyAbstractLeafStage : public MemoizingStage<int, sk_sp<SkPicture>, PipelineOptions>, public HierarchyNodeBase
    {
    public:
        HierarchyAbstractLeafStage(oneapi::tbb::flow::graph &g, AbstractId abstract_id, std::atomic<uint64_t> &recompute_count, double scale, std::string label = {})
            : MemoizingStage(g, std::move(label)), abstract_id_(abstract_id), recompute_count_(recompute_count), scale_(scale)
        {
        }

        sk_sp<SkPicture> last_picture() const override { return last_picture_; }
        oneapi::tbb::flow::sender<StageData<sk_sp<SkPicture>, PipelineOptions>> &picture_sender() override { return node(); }

        // A leaf is always a pure source - no upstream producer ever
        // try_puts into it, so HierarchyResolver::run_pending calls this
        // directly once discovery for the current call finishes. A fixed
        // data_version of 0 is enough: MemoizingStage::execute compares
        // against last_data_version_, which starts as std::nullopt, and
        // this node's own underlying function_node is only ever fed
        // exactly once in its whole lifetime (a later reference to the
        // same AbstractId within this epoch is a cache hit at the
        // HierarchyResolver level - a map lookup returning last_picture()
        // directly, never a second trigger).
        void trigger(const PipelineOptions &options) override { try_put({.data = 0, .data_version = 0, .options = options}); }

    protected:
        sk_sp<SkPicture> compute(const int &, const PipelineOptions &options) override
        {
            recompute_count_.fetch_add(1, std::memory_order_relaxed);
            const Root &root = *options.ctx.root;
            const ViewLayerSet &view_layers = *options.ctx.view_layers;

            const uint64_t geometry_data_version = AbstractGeometryStage::data_version_for(abstract_id_, options);

            // generate_abstract_stage_ IS a persistent, per-node member -
            // copy its result rather than binding a reference, matching
            // the old class's own "never alias a single-slot cache across
            // a recursive call" convention, even though nothing recurses
            // through this specific node's own compute() body itself
            // (kept for consistency/defense-in-depth, cheap either way).
            const std::vector<RenderedShape> dbu_shapes = generate_abstract_stage_.run(abstract_id_, geometry_data_version, options);
            last_picture_ = record_local_picture(dbu_shapes, geometry_data_version, viewport_then_layer_chain_, view_layers, *options.ctx.scene, scale_, {}, {}, abstract_declared_bbox(root, abstract_id_));
            computed = true;
            return last_picture_;
        }

    private:
        AbstractId abstract_id_;
        std::atomic<uint64_t> &recompute_count_;
        double scale_;
        sk_sp<SkPicture> last_picture_;

        ViewportThenLayerVisibilityChain viewport_then_layer_chain_{"hierarchy_viewport_filter", "hierarchy_layer_visibility_filter"};
        SynchronousStageRunner<AbstractGeometryStage, AbstractId, std::vector<RenderedShape>> generate_abstract_stage_{"hierarchy_generate_abstract"};
    };
}
