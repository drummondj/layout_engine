#pragma once
#include "parallel_by_key.hpp"
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
}
