#pragma once

#include "pipeline_options.hpp"
#include "stages/hierarchy_resolver_stage.hpp"
#include "stages/layer_generation_stage.hpp"
#include "tbb_core.hpp"

#include <string>
#include <utility>

namespace le
{
    /// @brief PIPELINE_REFACTOR.md's own ViewRenderPipeline - one shared
    /// oneapi::tbb::flow::graph wiring every tier's stages together with
    /// real make_edge connections, not the per-stage-private-graph
    /// SynchronousStageRunner pattern tests/benchmarks use to exercise one
    /// stage in isolation. Currently wires only the Cold tier
    /// (LayerGenerationStage -> HierarchyResolverStage); Warm/Hot stages
    /// get added to this same graph/class later - one pipeline for the
    /// whole thing, not a separate class per tier.
    ///
    /// LayerGenerationStage's own node() has two successors, both wired
    /// via make_edge: layer_generation_sink_ (so a caller can read the
    /// ViewLayerSet even on a call where only options.top_level/
    /// hierarchy_depth changed and HierarchyResolverStage alone needed to
    /// recompute) and hierarchy_resolver_'s own node() - the real
    /// Cold-tier data dependency. Both sides of that second edge are
    /// exactly HierarchyResolverStage's own ViewLayerSetHandle
    /// (== LayerGenerationStage::OutputHandle - both are
    /// std::shared_ptr<const ViewLayerSet>), so no adapter node is needed
    /// to bridge them - this is exactly why ViewLayerSetHandle was
    /// factored out as HierarchyResolverStage's own InputData type rather
    /// than staying folded into a stage-specific input struct (see that
    /// type alias's own doc comment).
    ///
    /// The Root pointer both stages need travels via
    /// ColdPipelineOptions::root, not either stage's own InputData -
    /// run_cold() sets it from its own `root` parameter, so a caller never
    /// has to set it independently.
    class ViewRenderPipeline
    {
    public:
        /// @brief The Cold tier's own combined output (PIPELINE_REFACTOR.md:
        /// "Output: A vector of shapes per Abstract and Layout ... plus a
        /// vector of ViewLayers") - a HierarchyResolverOutput alone isn't
        /// enough for a consumer to actually render anything, since its own
        /// ViewShape.view_layer fields are ids that need resolving against
        /// this same ViewLayerSet for style/color/purpose.
        struct ColdOutput
        {
            LayerGenerationStage::OutputHandle view_layers;
            HierarchyResolverStage::OutputHandle hierarchy;
        };

        explicit ViewRenderPipeline(std::string label = "ViewRenderPipeline")
            : layer_generation_(graph_, label + ".LayerGeneration"),
              hierarchy_resolver_(graph_, label + ".HierarchyResolver"),
              layer_generation_sink_(
                  graph_, oneapi::tbb::flow::serial,
                  [this](StageData<LayerGenerationStage::OutputHandle, ColdPipelineOptions> in)
                  { layer_generation_result_ = std::move(in); }),
              hierarchy_resolver_sink_(
                  graph_, oneapi::tbb::flow::serial,
                  [this](StageData<HierarchyResolverStage::OutputHandle, ColdPipelineOptions> in)
                  { hierarchy_resolver_result_ = std::move(in); })
        {
            make_edge(layer_generation_.node(), layer_generation_sink_);
            make_edge(layer_generation_.node(), hierarchy_resolver_.node());
            make_edge(hierarchy_resolver_.node(), hierarchy_resolver_sink_);
        }

        ViewRenderPipeline(const ViewRenderPipeline &) = delete;
        ViewRenderPipeline &operator=(const ViewRenderPipeline &) = delete;

        /// @brief Runs the Cold tier for `root` under `options` (`options.root`
        /// is overwritten with `root` here - a caller only has to set the
        /// fields that actually vary: root_mutation_version/top_level/
        /// hierarchy_depth). No data_version parameter, unlike
        /// SynchronousStageRunner::run() - neither stage's own recompute
        /// decision ever looks at one (both rely entirely on
        /// options_did_change(), see each stage's own doc comment), so
        /// there is nothing meaningful for a caller to thread through here;
        /// exposing one would only invite a caller to accidentally force
        /// recomputation by bumping it for an unrelated reason.
        ///
        /// Skips try_put/wait_for_all entirely when neither stage would
        /// recompute - see MemoizingStage::would_recompute()'s own doc
        /// comment (tbb_core.hpp) for why that's load-bearing, not just a
        /// nicety, even on a guaranteed cache hit.
        ColdOutput run_cold(const Root *root, ColdPipelineOptions options)
        {
            options.root = root;

            const bool layer_generation_would_recompute = layer_generation_.would_recompute(0, options);
            const bool hierarchy_resolver_would_recompute =
                layer_generation_would_recompute || hierarchy_resolver_.would_recompute(layer_generation_.version(), options);

            if (layer_generation_would_recompute || hierarchy_resolver_would_recompute)
            {
                layer_generation_.try_put({.data = root, .data_version = 0, .options = options});
                graph_.wait_for_all();
            }

            return {layer_generation_result_.data, hierarchy_resolver_result_.data};
        }

    private:
        oneapi::tbb::flow::graph graph_;
        LayerGenerationStage layer_generation_;
        HierarchyResolverStage hierarchy_resolver_;
        oneapi::tbb::flow::function_node<StageData<LayerGenerationStage::OutputHandle, ColdPipelineOptions>> layer_generation_sink_;
        oneapi::tbb::flow::function_node<StageData<HierarchyResolverStage::OutputHandle, ColdPipelineOptions>> hierarchy_resolver_sink_;
        StageData<LayerGenerationStage::OutputHandle, ColdPipelineOptions> layer_generation_result_{};
        StageData<HierarchyResolverStage::OutputHandle, ColdPipelineOptions> hierarchy_resolver_result_{};
    };
}
