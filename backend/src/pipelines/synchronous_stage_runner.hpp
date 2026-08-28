#pragma once
#include "pipeline_options.hpp"
#include "tbb_core.hpp"
#include <string>
#include <utility>

namespace le
{
    /// @brief Synchronous single-call wrapper around a persistent
    /// MemoizingStage-derived stage - constructs its own tiny private
    /// graph + sink once, then every call is just try_put + wait_for_all +
    /// read the sink's last result. Lets a caller reach a stage's own
    /// cached result directly, bypassing the protected-visibility
    /// compute()/try_put()-without-a-graph-to-wait-on problem a bare
    /// stage reference would otherwise have (see HierarchyResolver's own
    /// doc comment for the full oneTBB-nested-graph reasoning this relies
    /// on - a stage's own compute() safely constructing and tearing down
    /// an independent nested tbb::flow::graph is a proven-safe pattern,
    /// confirmed by the sibling oneTBB_test project's own experiment).
    ///
    /// Originally factored out of HierarchyResolver (its own
    /// generate_abstract_stage_/generate_layout_stage_/build_overlay_picture_stage_/
    /// etc. members all use this) once api.cpp's own LeHandle needed the
    /// exact same shape for a standalone stage call outside any pipeline's
    /// own wired graph (backend/ONETBB_INTEGRATION.md migration plan,
    /// Phase 5).
    template <typename Stage, typename InputData, typename OutputData>
    class SynchronousStageRunner
    {
    public:
        explicit SynchronousStageRunner(std::string label)
            : stage_(graph_, std::move(label)),
              sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<OutputData, PipelineOptions> in)
                    { result_ = std::move(in); })
        {
            make_edge(stage_.node(), sink_);
        }

        SynchronousStageRunner(const SynchronousStageRunner &) = delete;
        SynchronousStageRunner &operator=(const SynchronousStageRunner &) = delete;

        const OutputData &run(InputData data, uint64_t data_version, const PipelineOptions &options)
        {
            stage_.try_put({.data = std::move(data), .data_version = data_version, .options = options});
            graph_.wait_for_all();
            return result_.data;
        }

        // The last-emitted output's own data_version (bumped only on a
        // real recompute, see MemoizingStage::execute) - the "reach into
        // an upstream stage's own version()" pattern needed to build
        // domain-tagged keys for a shared downstream stage.
        uint64_t last_version() const { return result_.data_version; }

    private:
        oneapi::tbb::flow::graph graph_;
        Stage stage_;
        oneapi::tbb::flow::function_node<StageData<OutputData, PipelineOptions>> sink_;
        StageData<OutputData, PipelineOptions> result_{};
    };
}
