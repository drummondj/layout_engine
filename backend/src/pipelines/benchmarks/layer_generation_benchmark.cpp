#include "../pipeline_options.hpp"
#include "../stages/layer_generation_stage.hpp"
#include "../synchronous_stage_runner.hpp"
#include "aes_scaling_fixture.hpp"

#include <benchmark/benchmark.h>

#include <map>
#include <string>

using namespace le;
using namespace le::benchmarks;

namespace
{
    using LayerGenerationRunner = SynchronousStageRunner<LayerGenerationStage, const Root *, ViewLayerSet, ColdPipelineOptions>;

    // Google Benchmark re-invokes a registered benchmark function's entire
    // body several times per reported result (calibration passes, plus
    // once per --benchmark_repetitions) - only the for(auto _ : state)
    // loop's own contents are timed, but everything outside it still runs
    // every time. A fresh AesScalingFixture per invocation would reparse
    // the DEF file (up to ~270,000 shapes at 3x2/3x3) that many times
    // over, dwarfing the thing actually being measured - confirmed
    // directly, an earlier version without this cache took several
    // minutes per config in a Debug build. Loaded once per process per
    // config instead, mirroring the old pipeline_benchmark.cpp's own
    // stress_data()-style memoized fixture.
    const AesScalingFixture &cached_fixture(const TileConfig &config)
    {
        static std::map<std::string, AesScalingFixture> cache;
        auto it = cache.find(config.label);
        if (it == cache.end())
            it = cache.emplace(config.label, load_aes_scaling_fixture(config)).first;
        return it->second;
    }

    // LayerGenerationStage's own cost is a function of the Technology's
    // layer count alone (ViewLayerSet::build_for_technology), never of
    // design/Layout size - so this benchmark's expected finding, across
    // all 5 tile configs below, is flat/O(1) scaling despite the DEF
    // itself growing 9x (1x1 -> 3x3). That's the point of running it over
    // the same fixture matrix every other stage benchmark uses: showing
    // this stage does NOT scale with tile count is itself the useful
    // result, not a benchmark being run pointlessly.
    void BM_LayerGeneration(benchmark::State &state, TileConfig config)
    {
        const AesScalingFixture &fixture = cached_fixture(config);
        const ColdPipelineOptions options{.root_mutation_version = fixture.root.mutation_version()};

        for (auto _ : state)
        {
            // A fresh runner each iteration - its own MemoizingStage's
            // last_data_version_ starts unset, so this always forces a
            // real compute(), measuring cold-recompute cost rather than
            // the (much cheaper) cache-hit path a Warm-tier caller
            // actually hits on every pan/zoom tick.
            LayerGenerationRunner runner{"bm_layer_generation"};
            const ViewLayerSet &view_layers = runner.run(&fixture.root, 0, options);
            benchmark::DoNotOptimize(view_layers.all().size());
        }
    }

    void register_benchmarks()
    {
        for (const TileConfig &config : kAesScalingTileConfigs)
        {
            benchmark::RegisterBenchmark(("BM_LayerGeneration/" + std::string(config.label)).c_str(), BM_LayerGeneration, config)
                ->Unit(benchmark::kMicrosecond);
        }
    }
}

int main(int argc, char **argv)
{
    register_benchmarks();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
