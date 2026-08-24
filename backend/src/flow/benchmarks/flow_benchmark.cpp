#include "../flow.hpp"
#include <benchmark/benchmark.h>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <thread>
#include <vector>

using namespace le;

// Synthetic stand-in for real per-shape stage cost (a handful of transcendental
// ops, roughly the shape of geometry transform/overlap work in pipeline's own
// stages) - NOT a benchmark of the real Pipeline/Renderer classes. Answers
// TASKFLOW_EXPERIMENT.md goal 1 ("max achievable performance on a multi-cpu
// architecture") for the workload *shape* this codebase actually has, ahead
// of any real pipeline/render/instancing integration.
namespace
{
    // Matches the ~1M-shape scale pipeline_benchmark.cpp's own stress_data.hpp
    // fixture uses, so the two are read on comparable terms.
    constexpr uint64_t kTotalItems = 1'000'000;

    // A design's real routing/via layer count is typically in the low tens -
    // this stands in for GenerateAbstractShapesStage/FilterByLayerVisibilityStage's
    // own per-ViewLayerId grouping (see backend/CLAUDE.md's src/pipeline/ bullet),
    // the shape flow::parallel_by_key is designed to fan out over.
    constexpr int kLayerCount = 64;

    double process_item(uint64_t item)
    {
        double x = static_cast<double>(item % 997) * 0.0001;
        double acc = 0.0;
        for (int i = 0; i < 8; ++i)
        {
            x = std::sin(x) * std::cos(x) + std::sqrt(std::abs(x) + 1.0);
            acc += x;
        }
        return acc;
    }

    std::map<int, std::vector<uint64_t>> make_items_by_layer(uint64_t count, int layer_count)
    {
        std::map<int, std::vector<uint64_t>> layers;
        for (uint64_t item = 0; item < count; ++item)
            layers[static_cast<int>(item % static_cast<uint64_t>(layer_count))].push_back(item);
        return layers;
    }
}

static void BM_Serial_SyntheticPipeline(benchmark::State &state)
{
    const auto layers = make_items_by_layer(kTotalItems, kLayerCount);

    for (auto _ : state)
    {
        double total = 0.0;
        for (const auto &[layer, items] : layers)
            for (uint64_t item : items)
                total += process_item(item);
        benchmark::DoNotOptimize(total);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kTotalItems));
}
BENCHMARK(BM_Serial_SyntheticPipeline)->Unit(benchmark::kMillisecond);

// tf::Executor is constructed once per worker-count (outside the timed loop),
// reused across iterations - matches how a real integration would keep one
// persistent executor for a Scene's whole lifetime (Pipeline's own
// convention, see pipeline.hpp), rather than paying thread-pool spin-up cost
// every iteration.
static void BM_Taskflow_SyntheticPipeline(benchmark::State &state)
{
    const auto layers = make_items_by_layer(kTotalItems, kLayerCount);
    const auto num_workers = static_cast<unsigned>(state.range(0));
    tf::Executor executor(num_workers);

    for (auto _ : state)
    {
        tf::Taskflow taskflow;
        double total = 0.0;
        taskflow.emplace([&](tf::Subflow &sf)
                          {
            auto partials = flow::parallel_by_key(sf, layers, [](const int &, const std::vector<uint64_t> &items) {
                double sum = 0.0;
                for (uint64_t item : items)
                    sum += process_item(item);
                return sum;
            });
            for (const auto &[layer, partial] : partials)
                total += partial; });
        executor.run(taskflow).wait();
        benchmark::DoNotOptimize(total);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kTotalItems));
}

namespace
{
    // Registers BM_Taskflow_SyntheticPipeline once per worker count in
    // {1, 2, 4, 8, hardware_concurrency()}, deduplicated so a machine where
    // hardware_concurrency() is e.g. 8 doesn't run that count twice.
    struct RegisterTaskflowBenchmark
    {
        RegisterTaskflowBenchmark()
        {
            std::set<unsigned> worker_counts{1, 2, 4, 8, std::thread::hardware_concurrency()};
            for (unsigned workers : worker_counts)
            {
                if (workers == 0)
                    continue;
                benchmark::RegisterBenchmark("BM_Taskflow_SyntheticPipeline", BM_Taskflow_SyntheticPipeline)
                    ->Arg(static_cast<int64_t>(workers))
                    ->ArgName("workers")
                    ->Unit(benchmark::kMillisecond)
                    ->UseRealTime(); // default CPU-time only tracks the calling thread - misleading once work spreads across workers
            }
        }
    } register_taskflow_benchmark;
}

BENCHMARK_MAIN();
