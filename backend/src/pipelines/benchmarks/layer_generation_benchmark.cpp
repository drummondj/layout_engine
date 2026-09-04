#include "../pipeline_options.hpp"
#include "../stages/layer_generation_stage.hpp"
#include "../tests/synchronous_stage_runner.hpp"
#include "aes_scaling_fixture.hpp"
#include "pipeline_benchmarks.hpp"

#include <benchmark/benchmark.h>

#include <string>

using namespace le;
using namespace le::benchmarks;

namespace
{
    using LayerGenerationRunner = SynchronousStageRunner<LayerGenerationStage, const Root *, ViewLayerSet, ColdPipelineOptions>;

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
        const AesScalingFixture &fixture = cached_aes_scaling_fixture(config);
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
}

namespace le::benchmarks
{
    void register_layer_generation_benchmarks()
    {
        for (const TileConfig &config : kAesScalingTileConfigs)
        {
            benchmark::RegisterBenchmark(("BM_LayerGeneration/" + std::string(config.label)).c_str(), BM_LayerGeneration, config)
                ->Unit(benchmark::kMicrosecond);
        }
    }
}
