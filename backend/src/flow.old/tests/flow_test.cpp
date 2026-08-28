#include "../flow.hpp"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace le::flow;

// A linear 3-stage chain (flow::stage + .precede()) runs each stage exactly
// once, in dependency order - the "consistent way to build pipelines out of
// small reusable functions" goal (TASKFLOW_EXPERIMENT.md goal 2).
TEST(FlowTest, LinearPipelineRunsStagesInOrderAndOnce)
{
    std::vector<std::string> order;
    std::mutex order_mutex;
    auto record = [&](const char *name)
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(name);
    };

    tf::Taskflow taskflow;
    tf::Task a = stage(taskflow, "A", [&]
                        { record("A"); });
    tf::Task b = stage(taskflow, "B", [&]
                        { record("B"); });
    tf::Task c = stage(taskflow, "C", [&]
                        { record("C"); });
    a.precede(b);
    b.precede(c);

    tf::Executor executor;
    executor.run(taskflow).wait();

    EXPECT_EQ(order, (std::vector<std::string>{"A", "B", "C"}));
}

// parallel_by_key fans out one task per map entry and joins the results
// back into a Key-ordered map matching a serial reference computation -
// correctness of the "split processing steps per layer" building block
// (goal 4), independent of how many workers actually ran it.
TEST(FlowTest, ParallelByKeyProducesCorrectResults)
{
    std::map<int, int> items;
    for (int i = 0; i < 100; ++i)
        items[i] = i * 2;

    std::map<int, int> results;
    tf::Taskflow taskflow;
    taskflow.emplace([&](tf::Subflow &sf)
                      { results = parallel_by_key(sf, items, [](const int &, const int &value)
                                                   { return value + 1; }); });

    tf::Executor executor;
    executor.run(taskflow).wait();

    ASSERT_EQ(results.size(), items.size());
    for (const auto &[key, value] : items)
        EXPECT_EQ(results.at(key), value + 1);
}

// parallel_by_key actually exercises more than one worker - a coarse
// wall-clock sanity check (generous threshold, not a strict timing
// assertion, to avoid flaking under CI scheduling noise), skipped on
// single-core hardware where there's nothing to parallelize onto.
TEST(FlowTest, ParallelByKeyActuallyUsesMultipleWorkers)
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw < 2)
        GTEST_SKIP() << "needs at least 2 hardware threads to observe parallel speedup";

    const unsigned item_count = hw;
    constexpr auto kPerItemDelay = std::chrono::milliseconds(15);

    std::map<unsigned, unsigned> items;
    for (unsigned i = 0; i < item_count; ++i)
        items[i] = i;

    tf::Executor executor(hw);
    tf::Taskflow taskflow;

    const auto start = std::chrono::steady_clock::now();
    taskflow.emplace([&](tf::Subflow &sf)
                      { parallel_by_key(sf, items, [kPerItemDelay](const unsigned &, const unsigned &value)
                                         {
                std::this_thread::sleep_for(kPerItemDelay);
                return value; }); });
    executor.run(taskflow).wait();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    const auto serial_equivalent = kPerItemDelay * item_count;
    EXPECT_LT(elapsed, serial_equivalent * 6 / 10)
        << "parallel_by_key over " << item_count << " items on " << hw
        << " hardware threads took as long as a serial run would - not actually running in parallel?";
}

// StageProfiler aggregates call count/total duration per task name across
// however many times a same-named task actually ran - the per-stage
// profiling building block (goal 3).
TEST(FlowTest, StageProfilerAggregatesPerStageStats)
{
    constexpr int kRuns = 20;

    tf::Taskflow taskflow;
    for (int i = 0; i < kRuns; ++i)
        stage(taskflow, "compute", []
              { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });

    tf::Executor executor;
    auto profiler = executor.make_observer<StageProfiler>();
    executor.run(taskflow).wait();

    const auto report = profiler->report();
    ASSERT_TRUE(report.count("compute"));
    const auto &stats = report.at("compute");
    EXPECT_EQ(stats.calls, static_cast<uint64_t>(kRuns));
    EXPECT_GT(stats.total.count(), 0);
    EXPECT_LE(stats.min, stats.max);
}

// A dynamic subflow whose task count is only known at run time (goal 4's
// literal "dynamic DAG" case, e.g. a per-layer split where the layer count
// isn't known until the design is loaded) - every runtime item is
// processed exactly once.
TEST(FlowTest, DynamicSubflowSpawnsTaskPerRuntimeItem)
{
    const std::vector<int> runtime_items(37, 0); // size deliberately not a compile-time constant elsewhere
    std::atomic<int> processed{0};

    tf::Taskflow taskflow;
    taskflow.emplace([&](tf::Subflow &sf)
                      {
        for (size_t i = 0; i < runtime_items.size(); ++i)
            sf.emplace([&processed] { ++processed; }).name("per_item");
        sf.join(); });

    tf::Executor executor;
    executor.run(taskflow).wait();

    EXPECT_EQ(processed.load(), static_cast<int>(runtime_items.size()));
}
