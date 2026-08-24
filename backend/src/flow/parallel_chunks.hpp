#pragma once
#include "parallel_by_key.hpp"
#include <algorithm>
#include <cstddef>
#include <map>
#include <taskflow/taskflow.hpp>
#include <type_traits>
#include <utility>

namespace le::flow
{
    /// @brief Partitions `[0, item_count)` into `sf.executor().num_workers()`
    /// contiguous ranges and runs `process_range(begin, end)` for each in
    /// parallel (via parallel_by_key, keyed by chunk index so results
    /// concatenate back in original order) - the building block behind
    /// splitting a flat item list (e.g. a Pipeline stage's per-shape work)
    /// across workers without one task per item, which would spawn far
    /// too many tasks (and too much mutex contention in parallel_by_key's
    /// own result merge) for a workload with hundreds of thousands of
    /// tiny items.
    ///
    /// Takes a `tf::Subflow&`, not a `tf::Executor&` - this only ever adds
    /// tasks to the subflow's own already-running graph, it never starts a
    /// *new* top-level graph of its own. That distinction matters: an
    /// earlier version of this function owned a private `tf::Taskflow` and
    /// called `executor.run(taskflow).wait()` directly, which is safe when
    /// called from ordinary (non-worker) code but becomes Taskflow's
    /// documented deadlock risk once called from *inside* a task already
    /// running on that same executor (see `tf::Executor::corun`'s own doc
    /// comment: "blocking the worker without doing anything will introduce
    /// deadlock"). Being subflow-based instead sidesteps the problem
    /// entirely rather than papering over it with `corun` - there's only
    /// ever one top-level `executor.run(...).wait()` anywhere in a call
    /// chain built from these pieces (see `flow::run_subflow` for where
    /// that one call lives, and `GenerateAndFilterPipeline`
    /// (src/flow/pipeline/) for composing two Subflow-based stages
    /// together into one real graph via `.precede()`).
    ///
    /// Below the threshold `item_count < 2 * num_workers` (not enough work
    /// to give every worker a full chunk), or with a single-worker
    /// executor, skips the fan-out entirely and calls
    /// `process_range(0, item_count)` directly - both the cheap path for
    /// small inputs and, not coincidentally, byte-identical output
    /// ordering to the chunked path, since chunks are contiguous ranges
    /// processed internally in original order and concatenated in
    /// ascending chunk-index order.
    template <typename Fn>
    auto parallel_chunks(tf::Subflow &sf, size_t item_count, Fn &&process_range)
        -> std::invoke_result_t<Fn, size_t, size_t>
    {
        using Result = std::invoke_result_t<Fn, size_t, size_t>;

        const size_t num_workers = sf.executor().num_workers();
        if (item_count == 0 || num_workers <= 1 || item_count < 2 * num_workers)
            return process_range(0, item_count);

        const size_t chunk_size = (item_count + num_workers - 1) / num_workers;
        std::map<int, std::pair<size_t, size_t>> chunks;
        int chunk_index = 0;
        for (size_t begin = 0; begin < item_count; begin += chunk_size, ++chunk_index)
            chunks[chunk_index] = {begin, std::min(begin + chunk_size, item_count)};

        auto per_chunk = parallel_by_key(sf, chunks, [&](const int &, const std::pair<size_t, size_t> &range)
                                          { return process_range(range.first, range.second); });

        Result combined;
        for (auto &[index, chunk_result] : per_chunk)
            combined.insert(combined.end(), std::make_move_iterator(chunk_result.begin()), std::make_move_iterator(chunk_result.end()));
        return combined;
    }
}
