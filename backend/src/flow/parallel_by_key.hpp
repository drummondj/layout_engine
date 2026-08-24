#pragma once
#include <map>
#include <mutex>
#include <taskflow/taskflow.hpp>
#include <type_traits>

namespace le::flow
{
    /// @brief Fans out one dynamically-spawned task per entry of `items`,
    /// joins, and collects each task's result back into a `Key`-ordered
    /// map. Shaped directly after the `std::map<ViewLayerId, ...>` grouping
    /// pipeline's own FilterByLayerVisibilityStage/
    /// TinyShapesByLayerVisibilityStage already use (see backend/CLAUDE.md's
    /// `src/pipeline/` bullet) - the "split processing steps per layer"
    /// building block, so a future integration can call this with exactly
    /// that map shape rather than hand-writing per-layer task wiring.
    ///
    /// Must be called from inside a dynamic task (i.e. `sf` is the Subflow
    /// argument of a `taskflow.emplace([](tf::Subflow &sf){ ... })` lambda,
    /// or a nested one) - the task count depends on `items.size()`, only
    /// known at run time, which is exactly what a Subflow is for.
    template <typename Key, typename Value, typename Fn>
    auto parallel_by_key(tf::Subflow &sf, const std::map<Key, Value> &items, Fn &&fn)
        -> std::map<Key, std::invoke_result_t<Fn, const Key &, const Value &>>
    {
        using Result = std::invoke_result_t<Fn, const Key &, const Value &>;

        std::map<Key, Result> results;
        std::mutex results_mutex;

        for (const auto &[key, value] : items)
        {
            sf.emplace([&fn, &results, &results_mutex, &key, &value]()
                       {
                Result result = fn(key, value);
                std::lock_guard<std::mutex> lock(results_mutex);
                results.emplace(key, std::move(result)); })
                .name("parallel_by_key");
        }
        sf.join();

        return results;
    }
}
