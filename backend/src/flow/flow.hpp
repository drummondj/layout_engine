#pragma once
#include "parallel_by_key.hpp"
#include "parallel_chunks.hpp"
#include "task_profiler.hpp"
#include <taskflow/taskflow.hpp>
#include <utility>

namespace le::flow
{
    /// @brief `taskflow.emplace(fn).name(name)` in one call - its only job
    /// is making "every stage gets a name" a convention rather than
    /// something each call site has to remember to do itself; StageProfiler
    /// (task_profiler.hpp) aggregates by that name, so an unnamed stage
    /// just shows up ungrouped ("").
    template <typename Fn>
    tf::Task stage(tf::Taskflow &taskflow, const char *name, Fn &&fn)
    {
        return taskflow.emplace(std::forward<Fn>(fn)).name(name);
    }

    /// @brief The one place a top-level (non-worker-thread) caller starts
    /// a Subflow-based flow computation - builds a single one-task
    /// `tf::Taskflow` that hands `fn` a `tf::Subflow&`, runs it on
    /// `executor`, waits, and returns whatever `fn` returned.
    ///
    /// Every Subflow-taking piece in this module (parallel_chunks,
    /// GenerateAbstractShapesStage::run, FilterByViewportAndSizeStage::run,
    /// ...) is designed to be nested arbitrarily deep inside a bigger
    /// graph - see parallel_chunks.hpp's own comment for why none of them
    /// own an `executor.run(...).wait()` of their own. Something still has
    /// to make that one real top-level call exactly once, from a thread
    /// that actually isn't already one of `executor`'s own workers (a
    /// test, a benchmark, `main`, ...) - this is that call. Composing two
    /// or more Subflow-based stages together into one real graph (as
    /// opposed to just invoking one in isolation) is `.precede()` on their
    /// own `tf::Task`s inside a single `flow::run_subflow` call instead -
    /// see `GenerateAndFilterPipeline` (src/flow/pipeline/) for that case.
    ///
    /// `fn`'s return type must be default-constructible (a plain value is
    /// assigned into it inside the task) - for a stage whose `run()`
    /// returns a reference into its own long-lived cache storage (every
    /// stage in this module does), have `fn` return a pointer to that
    /// reference's target and dereference the result here.
    template <typename Fn>
    auto run_subflow(tf::Executor &executor, Fn &&fn) -> std::invoke_result_t<Fn, tf::Subflow &>
    {
        using Result = std::invoke_result_t<Fn, tf::Subflow &>;

        Result result{};
        tf::Taskflow taskflow;
        taskflow.emplace([&](tf::Subflow &sf)
                          { result = fn(sf); })
            .name("flow::run_subflow");
        executor.run(taskflow).wait();
        return result;
    }
}
