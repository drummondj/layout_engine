#pragma once
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <taskflow/taskflow.hpp>
#include <vector>

namespace le::flow
{
    /// @brief Per-stage-name aggregated timing, attached to a tf::Executor
    /// via `executor.make_observer<StageProfiler>()`. Taskflow's own
    /// tf::ObserverInterface gives per-task-instance (worker, span) events;
    /// this turns that into the same call-count/total/min/max shape this
    /// codebase's own benchmarks already report (see
    /// pipeline_benchmark.cpp), aggregated by tf::Task::name() - every task
    /// built via flow::stage() (flow.hpp) or given an explicit `.name(...)`
    /// is grouped under that name across however many times it actually
    /// ran, whether from one taskflow run or several.
    class StageProfiler : public tf::ObserverInterface
    {
    public:
        struct StageStats
        {
            uint64_t calls = 0;
            std::chrono::nanoseconds total{0};
            std::chrono::nanoseconds min{std::chrono::nanoseconds::max()};
            std::chrono::nanoseconds max{0};
        };

        void set_up(size_t num_workers) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            starts_.assign(num_workers, {});
            stats_.clear();
        }

        void on_entry(tf::WorkerView wv, tf::TaskView) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            starts_[wv.id()] = std::chrono::steady_clock::now();
        }

        void on_exit(tf::WorkerView wv, tf::TaskView tv) override
        {
            const auto end = std::chrono::steady_clock::now();

            std::lock_guard<std::mutex> lock(mutex_);
            const auto duration = end - starts_[wv.id()];

            auto &stats = stats_[std::string(tv.name())];
            ++stats.calls;
            stats.total += duration;
            stats.min = std::min(stats.min, duration);
            stats.max = std::max(stats.max, duration);
        }

        /// Snapshot of every stage name observed so far, keyed by
        /// tf::Task::name(). A task with no name is grouped under "".
        std::map<std::string, StageStats> report() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return stats_;
        }

    private:
        mutable std::mutex mutex_;
        std::vector<std::chrono::steady_clock::time_point> starts_;
        std::map<std::string, StageStats> stats_;
    };
}
