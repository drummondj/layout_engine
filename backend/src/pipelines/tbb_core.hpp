#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <oneapi/tbb.h>
#include <tracy/Tracy.hpp>

namespace le
{
    /// @brief This process's own current VmRSS/VmSwap, in KB, read fresh
    /// from /proc/self/status - Linux-only (backend/CLAUDE.md's own
    /// "Target: Linux servers" scope), same technique
    /// aes_scaling_fixture.hpp's own peak_rss_mb() uses via getrusage,
    /// but instantaneous-current rather than peak-so-far, and also
    /// reporting swap - MemoizingStage::execute() (below) uses this to
    /// bracket each real compute() call for the pipeline_stage_benchmark
    /// tool (src/pipelines/benchmarks/), so a per-stage memory
    /// delta/swap-use reading is possible without a heavier per-
    /// allocation profiler. Returns {0, 0} if the file can't be read
    /// (e.g. non-Linux) - callers treat that the same as "no swap, no
    /// RSS change", not an error.
    struct ProcMemSample
    {
        long rss_kb = 0;
        long swap_kb = 0;
    };

    inline ProcMemSample read_proc_mem_sample()
    {
        ProcMemSample sample;
        if (FILE *f = std::fopen("/proc/self/status", "r"))
        {
            char line[256];
            while (std::fgets(line, sizeof(line), f))
            {
                if (std::sscanf(line, "VmRSS: %ld kB", &sample.rss_kb) == 1)
                    continue;
                std::sscanf(line, "VmSwap: %ld kB", &sample.swap_kb);
            }
            std::fclose(f);
        }
        return sample;
    }

    /// @brief This calling thread's own CPU time so far, in nanoseconds
    /// (CLOCK_THREAD_CPUTIME_ID) - meaningful as a per-compute() bracket
    /// only because every MemoizingStage node in this pipeline runs
    /// single-threaded compute() bodies (confirmed directly -
    /// HierarchyResolverStage's own compute() is a plain BFS loop, no
    /// nested parallel_for/graph calls) - a stage that ever became
    /// internally parallel would need this measured differently (e.g.
    /// summing per-task thread times), not a drop-in fix.
    inline std::int64_t thread_cpu_time_ns()
    {
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }

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
    /// The node's own OutputData travels as `OutputHandle`
    /// (`std::shared_ptr<const OutputData>`), not `OutputData` by value - a
    /// cache hit has to hand back the *same* result on every call (that's
    /// the whole point of memoizing), which otherwise means deep-copying
    /// the full OutputData on every single execute() (cache hit or miss:
    /// `return {last_result_, ...}` copy-constructs from the `last_result_`
    /// member either way), not just the recompute case - free for a small
    /// OutputData, but a real, measured multi-second cost for a large one
    /// (confirmed directly: ~1.75s of real compute() work vs. several
    /// seconds more just moving a ~1,000,000-entry HierarchyResolverOutput
    /// through the cache and this node's own TBB message-passing/
    /// buffering down to a successor, PIPELINE_REFACTOR_BENCHMARK_RESULTS.md's
    /// 5x5 entries). A `shared_ptr` copy is one atomic refcount bump
    /// regardless of payload size - `compute()` itself is unaffected
    /// (subclasses still just return a plain `OutputData` by value; this
    /// wraps it exactly once, in execute()). A downstream stage wired via
    /// make_edge receives this same `OutputHandle`, not a copy of the
    /// referenced data, so chaining stages is zero-copy on this side too.
    ///
    /// @note Must be constructed via std::make_unique and never moved or copied.
    template <typename InputData, typename OutputData, typename PipelineOptions>
    class MemoizingStage
    {
    public:
        /// @brief This stage's own OutputData, as it actually travels
        /// through the cache and this node's own output edge - see the
        /// class's own doc comment for why a plain `OutputData` by value
        /// isn't used here.
        using OutputHandle = std::shared_ptr<const OutputData>;

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
            StageData<InputData, PipelineOptions>, StageData<OutputHandle, PipelineOptions>> &
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

        // --- pipeline_stage_benchmark instrumentation (per-stage wall/
        // CPU time, memory, cache size) - all populated only on a real
        // compute() call (see execute()'s own comment for why a cache
        // hit deliberately skips the sampling work); stay at whatever
        // the previous real compute() left them on a cache hit, so a
        // caller distinguishes "fresh this call" from "stale, carried
        // over" via last_call_recomputed() rather than these silently
        // reading as zero. ---

        /// @brief Whether the most recent execute() call actually ran
        /// compute() (true) or was a cache hit (false) - set on every
        /// call, unlike the stats below.
        bool last_call_recomputed() const { return last_call_recomputed_; }

        /// @brief Wall-clock duration of the most recent real compute()
        /// call, in nanoseconds. 0 if compute() has never run.
        std::int64_t last_compute_wall_ns() const { return last_compute_wall_ns_; }

        /// @brief This stage's own thread's CPU time consumed by the
        /// most recent real compute() call, in nanoseconds - see
        /// thread_cpu_time_ns()'s own doc comment for the single-
        /// threaded-compute() assumption this relies on. 0 if compute()
        /// has never run.
        std::int64_t last_compute_cpu_ns() const { return last_compute_cpu_ns_; }

        /// @brief Process VmRSS (KB) immediately before/after the most
        /// recent real compute() call - a delta, not an attribution
        /// (other threads/allocations could interleave), but meaningful
        /// for the substantial per-compute allocations this pipeline's
        /// own Cold/Warm stages make. 0/0 if compute() has never run.
        long last_compute_rss_before_kb() const { return last_rss_before_kb_; }
        long last_compute_rss_after_kb() const { return last_rss_after_kb_; }

        /// @brief Process VmSwap (KB) immediately before/after the most
        /// recent real compute() call - nonzero here means this process
        /// is (at least partly) swapped out, the "if swap was used"
        /// signal pipeline_stage_benchmark reports per stage.
        long last_compute_swap_before_kb() const { return last_swap_before_kb_; }
        long last_compute_swap_after_kb() const { return last_swap_after_kb_; }

        /// @brief How many objects (typically Shapes) this stage's own
        /// currently-cached OutputData holds, per estimate_output_object_count()
        /// below - 0 if this stage doesn't override that hook (nothing
        /// meaningful to count) or nothing has been computed yet.
        std::size_t cache_object_count() const
        {
            return last_result_ ? estimate_output_object_count(*last_result_) : 0;
        }

        /// @brief Approximate memory (bytes) held by this stage's own
        /// currently-cached OutputData, per estimate_output_bytes()
        /// below - defaults to sizeof(OutputData) (a fixed-size struct's
        /// own true size, but a serious undercount for one holding
        /// vectors/maps of real content) when a subclass doesn't
        /// override it; 0 if nothing has been computed yet.
        std::size_t cache_bytes() const { return last_result_ ? estimate_output_bytes(*last_result_) : 0; }

    protected:
        /// @brief How many objects (typically Shapes) `output` holds -
        /// override in a subclass whose OutputData has a meaningful
        /// count to report (see e.g. HierarchyResolverStage); default 0
        /// ("not applicable" - the ask's own "if applicable" case).
        virtual std::size_t estimate_output_object_count(const OutputData &output) const { return 0; }

        /// @brief Approximate memory (bytes) `output` occupies - override
        /// in a subclass that can do better than the default
        /// sizeof(OutputData) (which is exact only for a fixed-size
        /// struct with no owned heap content of its own).
        virtual std::size_t estimate_output_bytes(const OutputData &output) const { return sizeof(OutputData); }


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
        StageData<OutputHandle, PipelineOptions> execute(StageData<InputData, PipelineOptions> in)
        {
            ZoneScoped;
            if (!label_.empty())
            {
                ZoneName(label_.data(), label_.size());
            }

            bool should_recompute =
                last_data_version_ != in.data_version || options_did_change(last_options_, in.options);

            last_call_recomputed_ = should_recompute;
            if (should_recompute)
            {
                // Sampling (clock_gettime x2, /proc/self/status read x2)
                // only happens on a real recompute - the hot cache-hit
                // path (the common case in real interactive use) pays
                // none of this, matching would_recompute()'s own "avoid
                // overhead on the hot path" philosophy above.
                const ProcMemSample mem_before = read_proc_mem_sample();
                const std::int64_t cpu_before = thread_cpu_time_ns();
                const auto wall_before = std::chrono::steady_clock::now();

                last_result_ = std::make_shared<const OutputData>(compute(in.data, in.options));

                last_compute_wall_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - wall_before)
                                             .count();
                last_compute_cpu_ns_ = thread_cpu_time_ns() - cpu_before;
                const ProcMemSample mem_after = read_proc_mem_sample();
                last_rss_before_kb_ = mem_before.rss_kb;
                last_rss_after_kb_ = mem_after.rss_kb;
                last_swap_before_kb_ = mem_before.swap_kb;
                last_swap_after_kb_ = mem_after.swap_kb;
                ++version_;
            }
            else
            {
                ZoneText("cache hit", 9);
            }

            last_data_version_ = in.data_version;
            last_options_ = in.options;
            return {last_result_, version_, in.options}; // shared_ptr copy - cheap regardless of OutputData's own size
        }

        oneapi::tbb::flow::function_node<
            StageData<InputData, PipelineOptions>, StageData<OutputHandle, PipelineOptions>>
            node_;
        std::optional<std::uint64_t> last_data_version_;
        PipelineOptions last_options_{};
        OutputHandle last_result_;
        std::uint64_t version_{0};
        std::string label_;

        // pipeline_stage_benchmark instrumentation - see the public
        // accessors above for what each of these means.
        bool last_call_recomputed_{false};
        std::int64_t last_compute_wall_ns_{0};
        std::int64_t last_compute_cpu_ns_{0};
        long last_rss_before_kb_{0};
        long last_rss_after_kb_{0};
        long last_swap_before_kb_{0};
        long last_swap_after_kb_{0};
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
