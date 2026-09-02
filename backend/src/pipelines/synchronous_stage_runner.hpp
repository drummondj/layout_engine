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

    /// @brief Two MemoizingStages wired stage1 -> stage2 via a real
    /// make_edge, for a caller that needs to run them synchronously back
    /// to back outside any bigger owning graph (the same use case
    /// SynchronousStageRunner covers for one stage). stage2's own
    /// data_version is always exactly stage1's own bumped version() -
    /// never a value a caller has to separately derive or keep in sync.
    ///
    /// This replaces a real bug: two independent SynchronousStageRunners
    /// called back to back, with the caller hand-threading a data_version
    /// between them, is exactly the "manually keep two things in sync"
    /// failure mode MemoizingStage/flow::graph exists to avoid in the
    /// first place (see this project's own architecture) - and it broke
    /// exactly that way in record_local_picture (hierarchy_stage_support.hpp,
    /// BUGS_AND_ENHANCEMENTS.md B3's own postmortem): the second stage
    /// was fed the SAME data_version as the first on every call, so its
    /// own change-detection could never see the first stage's output
    /// having actually changed. Wiring a real edge between them instead
    /// makes that class of bug structurally impossible - there's no
    /// separate value left to get wrong, TBB's own dataflow carries
    /// stage1's version() to stage2 automatically.
    ///
    /// The two stages often still need genuinely different PipelineOptions
    /// (e.g. record_local_picture's own throwaway cull_scene for viewport
    /// culling vs. the real Scene for layer visibility) - unlike a plain
    /// make_edge, which would forward stage1's own options to stage2
    /// unchanged, run() takes both explicitly and an internal adapter
    /// node swaps in options2 before stage2 ever sees the message -
    /// preserving stage1's own data_version untouched (the whole point),
    /// just remapping which PipelineOptions rides alongside it.
    template <typename Stage1, typename InputData1, typename Mid, typename Stage2, typename OutputData>
    class SynchronousStageChain
    {
    public:
        SynchronousStageChain(std::string label1, std::string label2)
            : stage1_(graph_, std::move(label1)),
              stage2_(graph_, std::move(label2)),
              adapter_(graph_, oneapi::tbb::flow::serial, [this](StageData<Mid, PipelineOptions> in) -> StageData<Mid, PipelineOptions>
                       { return {.data = std::move(in.data), .data_version = in.data_version, .options = options2_}; }),
              sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<OutputData, PipelineOptions> in)
                    { result_ = std::move(in); })
        {
            make_edge(stage1_.node(), adapter_);
            make_edge(adapter_, stage2_.node());
            make_edge(stage2_.node(), sink_);
        }

        SynchronousStageChain(const SynchronousStageChain &) = delete;
        SynchronousStageChain &operator=(const SynchronousStageChain &) = delete;

        const OutputData &run(InputData1 data, uint64_t data_version, const PipelineOptions &options1, const PipelineOptions &options2)
        {
            options2_ = options2;
            stage1_.try_put({.data = std::move(data), .data_version = data_version, .options = options1});
            graph_.wait_for_all();
            return result_.data;
        }

    private:
        oneapi::tbb::flow::graph graph_;
        Stage1 stage1_;
        Stage2 stage2_;
        oneapi::tbb::flow::function_node<StageData<Mid, PipelineOptions>, StageData<Mid, PipelineOptions>> adapter_;
        oneapi::tbb::flow::function_node<StageData<OutputData, PipelineOptions>> sink_;
        PipelineOptions options2_;
        StageData<OutputData, PipelineOptions> result_{};
    };
}
