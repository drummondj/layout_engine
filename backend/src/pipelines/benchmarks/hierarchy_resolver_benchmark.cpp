#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../stages/hierarchy_resolver_stage.hpp"
#include "../tests/synchronous_stage_runner.hpp"
#include "aes_scaling_fixture.hpp"
#include "pipeline_benchmarks.hpp"

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

using namespace le;
using namespace le::benchmarks;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, HierarchyResolverInput, HierarchyResolverOutput, ColdPipelineOptions>;

    // Unlike LayerGenerationStage, this stage's own cost DOES scale with
    // design size - it walks every placement in the Layout plus every
    // distinct Abstract/Layout those placements resolve to, collecting
    // each one's own direct shapes. Expected result: roughly linear
    // growth across the 5 tile configs (1x1 -> 3x3 is a real 9x growth in
    // placement/route/row count) - the scaling case LayerGenerationStage's
    // own flat result was making a point of NOT being.
    void BM_HierarchyResolver(benchmark::State &state, TileConfig config)
    {
        const AesScalingFixture &fixture = cached_aes_scaling_fixture(config);
        const std::vector<TechnologyId> technology_ids = fixture.root.get_technology_ids();
        const ViewLayerSet view_layers = technology_ids.empty() ? ViewLayerSet{} : ViewLayerSet::build_for_technology(fixture.root, technology_ids.front());

        // hierarchy_depth 1, not 0 - depth 0 now means "only the top
        // Layout's own direct content, nothing resolved past it at all"
        // (HierarchyResolverStage::compute()'s own doc comment); depth 1
        // is what actually resolves top-level placements to their own
        // content. Every placement in these fixtures is a Nangate
        // standard cell (Abstract only, no Layout of its own), so at
        // depth 1 resolve_design_target already falls back to its
        // Abstract - depth only matters further once a fixture actually
        // nests Layout-in-Layout, which none of the aes_scaling DEFs do.
        const ColdPipelineOptions options{
            .root_mutation_version = fixture.root.mutation_version(),
            .top_level = HierarchyId{fixture.layout_id},
            .hierarchy_depth = 1,
        };
        const HierarchyResolverInput input{.root = &fixture.root, .view_layers = &view_layers};

        for (auto _ : state)
        {
            HierarchyResolverRunner runner{"bm_hierarchy_resolver"};
            const HierarchyResolverOutput &output = runner.run(input, 0, options);
            benchmark::DoNotOptimize(output.view_data.size());
        }

        // Whole-process peak RSS so far - see peak_rss_mb()'s own comment
        // for why this only reads as "this design's own memory" when run
        // in isolation (kAesScalingLargeConfig/"5x5" via
        // --benchmark_filter=5x5), not as part of the full suite.
        state.counters["PeakRSS_MB"] = peak_rss_mb();
    }
}

namespace le::benchmarks
{
    void register_hierarchy_resolver_benchmarks()
    {
        for (const TileConfig &config : kAesScalingTileConfigs)
        {
            benchmark::RegisterBenchmark(("BM_HierarchyResolver/" + std::string(config.label)).c_str(), BM_HierarchyResolver, config)
                ->Unit(benchmark::kMillisecond);
        }

        // kAesScalingLargeConfig (~1,033,600 components) - the Cold tier's
        // own real target scale, not another scaling-matrix point (see
        // that constant's own comment) - run in isolation
        // (--benchmark_filter=5x5) to measure this design's own memory
        // footprint without every other cached fixture also resident.
        benchmark::RegisterBenchmark(("BM_HierarchyResolver/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_HierarchyResolver, kAesScalingLargeConfig)
            ->Unit(benchmark::kMillisecond);
    }
}
