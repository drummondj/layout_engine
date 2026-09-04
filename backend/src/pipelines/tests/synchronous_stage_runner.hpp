#pragma once
#include "../tbb_core.hpp"
#include <string>
#include <utility>

namespace le
{
    /// @brief Synchronous single-call wrapper around a persistent
    /// MemoizingStage-derived stage - constructs its own tiny private
    /// graph + sink once, then every call is just try_put + wait_for_all +
    /// read the sink's last result. Lets a caller (a test, a benchmark, or
    /// eventually api.cpp) reach a stage's own cached result directly,
    /// without needing to wire a whole pipeline's worth of stages into one
    /// graph first.
    ///
    /// @tparam Stage A MemoizingStage<InputData, OutputData, PipelineOptions>
    ///         subclass, constructible from (graph&, std::string label).
    template <typename Stage, typename InputData, typename OutputData, typename PipelineOptions>
    class SynchronousStageRunner
    {
    public:
        // Stage's own node() output travels as Stage::OutputHandle
        // (std::shared_ptr<const OutputData>, see MemoizingStage's own
        // doc comment for why) - matched here so the sink's own edge
        // type-checks against it.
        using OutputHandle = typename Stage::OutputHandle;

        explicit SynchronousStageRunner(std::string label)
            : stage_(graph_, std::move(label)),
              sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<OutputHandle, PipelineOptions> in)
                    { result_ = std::move(in); })
        {
            make_edge(stage_.node(), sink_);
        }

        SynchronousStageRunner(const SynchronousStageRunner &) = delete;
        SynchronousStageRunner &operator=(const SynchronousStageRunner &) = delete;

        const OutputData &run(InputData data, std::uint64_t data_version, const PipelineOptions &options)
        {
            stage_.try_put({.data = std::move(data), .data_version = data_version, .options = options});
            graph_.wait_for_all();
            return *result_.data;
        }

        /// @brief Whether calling run() with this exact (data_version,
        /// options) right now would trigger a real compute() - see
        /// MemoizingStage::would_recompute()'s own doc comment for why
        /// this matters (skipping try_put/wait_for_all entirely on a
        /// guaranteed cache hit has a real, measured cost of its own).
        bool would_recompute(std::uint64_t data_version, const PipelineOptions &options) const
        {
            return stage_.would_recompute(data_version, options);
        }

        // The last-emitted output's own data_version (bumped only on a
        // real recompute, see MemoizingStage::execute) - lets a caller
        // chain a further stage's own data_version to this one without
        // hand-threading a separately-tracked counter.
        std::uint64_t last_version() const { return result_.data_version; }

        // The last-emitted output's own handle (shared_ptr<const
        // OutputData>) - lets a caller feed this stage's own output
        // directly into a downstream stage's InputData without
        // re-wrapping the dereferenced run() result in a new shared_ptr
        // (which would break MemoizingStage's own cache-identity
        // assumptions for that downstream stage).
        OutputHandle last_handle() const { return result_.data; }

    private:
        oneapi::tbb::flow::graph graph_;
        Stage stage_;
        oneapi::tbb::flow::function_node<StageData<OutputHandle, PipelineOptions>> sink_;
        StageData<OutputHandle, PipelineOptions> result_{};
    };
}
