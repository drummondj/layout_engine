#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <oneapi/tbb.h>
#include <tracy/Tracy.hpp>

namespace le
{
    /// @brief Uniform message passed between pipeline stages.
    /// @tparam T Payload type.
    /// @tparam PipelineOptions Pipeline-wide options type threaded alongside the payload.
    ///
    /// @note Callers must bump `data_version` themselves whenever `data` actually changes -
    ///       stages compare versions instead of deep-comparing `data`.
    template <typename T, typename PipelineOptions>
    struct StageData
    {
        T data;
        std::uint64_t data_version;
        PipelineOptions options;
    };

    /// @brief Base class for a memoizing pipeline stage (Template Method pattern).
    ///
    /// Calls the subclass's compute() only when the input's data_version or options (per
    /// options_did_change()) changed since the last invocation; otherwise returns the
    /// previous result.
    ///
    /// @tparam InputData Input payload type.
    /// @tparam OutputData Output payload type produced by compute().
    /// @tparam PipelineOptions Options type threaded through this pipeline; every stage
    ///         connected via make_edge must use the same one.
    ///
    /// @note Must be constructed via std::make_unique and never moved or copied.
    template <typename InputData, typename OutputData, typename PipelineOptions>
    class MemoizingStage
    {
    public:
        /// @brief Constructs the stage and its underlying node.
        /// @param g Flow graph this stage's node belongs to.
        /// @param label Optional label identifying this instance in Tracy traces.
        explicit MemoizingStage(oneapi::tbb::flow::graph &g, std::string label = {})
            : node_(
                  g, oneapi::tbb::flow::serial,
                  [this](StageData<InputData, PipelineOptions> in)
                  { return execute(std::move(in)); }),
              label_(std::move(label)) {}

        MemoizingStage(const MemoizingStage &) = delete;
        MemoizingStage &operator=(const MemoizingStage &) = delete;

        virtual ~MemoizingStage() = default;

        /// @brief The underlying function_node, for wiring with make_edge.
        oneapi::tbb::flow::function_node<
            StageData<InputData, PipelineOptions>, StageData<OutputData, PipelineOptions>> &
        node() { return node_; }

        /// @brief Submits input to this stage's node.
        /// @param v Input message.
        /// @return Whether the node accepted the input.
        bool try_put(StageData<InputData, PipelineOptions> v)
        {
            return node_.try_put(std::move(v));
        }

        /// @brief This stage's own current output version - bumped only
        /// on a real compute() call (see execute()), unchanged on a
        /// cache hit. A downstream stage's own data_version input, so a
        /// chain of stages only ever recomputes as far as the first one
        /// whose own inputs actually changed.
        std::uint64_t version() const { return version_; }

        /// @brief Whether calling execute() with this exact
        /// (data_version, options) pair right now would trigger a real
        /// compute() call, without running it or mutating any state.
        /// BUGS_AND_ENHANCEMENTS.md E31's own SynchronousStageChain
        /// follow-up - lets a caller decide whether even TRIGGERING the
        /// underlying flow::graph node is worth its own real per-call
        /// TBB scheduling overhead, which execute()'s own early-return
        /// (on should_recompute == false) does NOT avoid by itself: the
        /// message still has to be try_put and the graph still has to be
        /// waited on to get the (unchanged) result back out - measured,
        /// not assumed, at 300-600ms on a real ~478,000-shape Layout
        /// even on a guaranteed cache hit, before this method existed.
        bool would_recompute(std::uint64_t data_version, const PipelineOptions &options) const
        {
            return last_data_version_ != data_version || options_did_change(last_options_, options);
        }

    protected:
        /// @brief Computes this stage's output. Called only when recomputation is needed.
        /// @param data Input payload.
        /// @param options Current pipeline options.
        /// @return The computed output payload.
        virtual OutputData compute(const InputData &data, const PipelineOptions &options) = 0;

        /// @brief Whether a change in options alone should force recomputation.
        /// @param last Options from the previous invocation.
        /// @param current Options for this invocation.
        /// @return True if compute() should run even though data didn't change. Default:
        ///         false (ignore options).
        virtual bool
        options_did_change(const PipelineOptions &last, const PipelineOptions &current) const
        {
            return false;
        }

    private:
        /// @brief Recomputes via compute() if needed, else returns the cached result.
        StageData<OutputData, PipelineOptions> execute(StageData<InputData, PipelineOptions> in)
        {
            ZoneScoped;
            if (!label_.empty())
            {
                ZoneName(label_.data(), label_.size());
            }

            bool should_recompute =
                last_data_version_ != in.data_version || options_did_change(last_options_, in.options);

            if (should_recompute)
            {
                last_result_ = compute(in.data, in.options);
                ++version_;
            }
            else
            {
                ZoneText("cache hit", 9);
            }

            last_data_version_ = in.data_version;
            last_options_ = in.options;
            return {last_result_, version_, in.options};
        }

        oneapi::tbb::flow::function_node<
            StageData<InputData, PipelineOptions>, StageData<OutputData, PipelineOptions>>
            node_;
        std::optional<std::uint64_t> last_data_version_;
        PipelineOptions last_options_{};
        OutputData last_result_{};
        std::uint64_t version_{0};
        std::string label_;
    };

    /// @brief Generic parallel many-in/one-out fan-in accumulator for a flow graph.
    ///
    /// Gathers exactly `fan_in_count` inputs per round into one `OutputData`, forwarding the
    /// merged result only if at least one input's `data_version` actually differed from what
    /// that same input slot reported last round - a round simply completing isn't sufficient on
    /// its own. Runs at `unlimited` concurrency: unlike a `serial` node, multiple inputs can be
    /// dispatched to it at once instead of queueing behind a single in-flight slot, so
    /// accumulator state is guarded by a `spin_mutex` instead of being safely lock-free.
    ///
    /// Which input slot a given message belongs to (e.g. a fan-out index like layer_id) can't be
    /// recovered from which edge it arrived on - the fan-in loses that - so `slot_fn` must
    /// recover it directly from the payload, and `merge_fn` folds that payload into the round's
    /// accumulator.
    ///
    /// @tparam InputData Input payload type (arrives once per fan-in edge per round).
    /// @tparam OutputData Output payload type, built up by repeated merge_fn calls.
    /// @tparam PipelineOptions Options type threaded through this pipeline (unused by the
    ///         accumulation logic itself, but required to match the upstream producer's
    ///         StageData shape for make_edge).
    ///
    /// @note Must be constructed via std::make_unique and never moved or copied.
    template <typename InputData, typename OutputData, typename PipelineOptions>
    class FanInCollectStage
    {
    public:
        /// @brief Recovers the stable fan-in slot index (e.g. layer_id) for an input, in
        ///        [0, fan_in_count). Used to track each slot's last-seen data_version.
        using SlotFn = std::function<std::size_t(const InputData &)>;
        /// @brief Merges one input's data into the round's accumulator.
        using MergeFn = std::function<void(OutputData &, const InputData &)>;

        /// @brief Constructs the stage and its underlying node.
        /// @param g Flow graph this stage's node belongs to.
        /// @param fan_in_count Number of inputs gathered per round before emitting.
        /// @param slot_fn Recovers an input's fan-in slot index.
        /// @param merge_fn Merges one input's data into the round's accumulator.
        /// @param label Optional label identifying this instance in Tracy traces.
        FanInCollectStage(
            oneapi::tbb::flow::graph &g, std::size_t fan_in_count, SlotFn slot_fn, MergeFn merge_fn,
            std::string label = {})
            : node_(
                  g, oneapi::tbb::flow::unlimited,
                  [this](StageData<InputData, PipelineOptions> in)
                  { return execute(std::move(in)); }),
              fan_in_count_(fan_in_count), slot_fn_(std::move(slot_fn)), merge_fn_(std::move(merge_fn)),
              last_versions_(fan_in_count), label_(std::move(label)) {}

        FanInCollectStage(const FanInCollectStage &) = delete;
        FanInCollectStage &operator=(const FanInCollectStage &) = delete;

        /// @brief The underlying function_node, for wiring with make_edge.
        oneapi::tbb::flow::function_node<StageData<InputData, PipelineOptions>, OutputData> &node() { return node_; }

        /// @brief Submits one round's input to this stage's node.
        /// @param v Input message.
        /// @return Whether the node accepted the input.
        bool try_put(StageData<InputData, PipelineOptions> v)
        {
            return node_.try_put(std::move(v));
        }

    private:
        /// @brief Merges `in` into the current round; once `fan_in_count_` inputs have arrived,
        ///        resets for the next round and returns the merged result - but only if some
        ///        input's data_version actually changed this round, otherwise an empty OutputData.
        OutputData execute(StageData<InputData, PipelineOptions> in)
        {
            ZoneScoped;
            if (!label_.empty())
            {
                ZoneName(label_.data(), label_.size());
            }

            oneapi::tbb::spin_mutex::scoped_lock lock(mutex_);

            std::size_t slot = slot_fn_(in.data);
            if (last_versions_[slot] != in.data_version)
            {
                any_changed_ = true;
                last_versions_[slot] = in.data_version;
            }
            merge_fn_(accumulated_, in.data);

            if (++received_ < fan_in_count_)
            {
                ZoneText("accumulating", 12);
                return {};
            }

            received_ = 0;
            OutputData result = std::exchange(accumulated_, OutputData{});
            if (!std::exchange(any_changed_, false))
            {
                ZoneText("no change", 9);
                return {};
            }

            return result;
        }

        oneapi::tbb::flow::function_node<StageData<InputData, PipelineOptions>, OutputData> node_;
        std::size_t fan_in_count_;
        SlotFn slot_fn_;
        MergeFn merge_fn_;

        oneapi::tbb::spin_mutex mutex_;
        std::size_t received_{0};
        OutputData accumulated_{};
        bool any_changed_{false};
        std::vector<std::optional<std::uint64_t>> last_versions_;

        std::string label_;
    };
}
