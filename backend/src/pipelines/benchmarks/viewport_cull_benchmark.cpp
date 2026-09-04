#include "../../core/placement_geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../stages/hierarchy_resolver_stage.hpp"
#include "../stages/viewport_cull_stage.hpp"
#include "../tests/synchronous_stage_runner.hpp"
#include "aes_scaling_fixture.hpp"
#include "pipeline_benchmarks.hpp"

#include <benchmark/benchmark.h>

#include <memory>
#include <string>
#include <vector>

using namespace le;
using namespace le::benchmarks;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using ViewportCullRunner = SynchronousStageRunner<ViewportCullStage, HierarchyResolverStage::OutputHandle, HierarchyResolverOutput, ViewRenderOptions>;

    // Warm stage 1's own cost is expected to track HierarchyResolver's own
    // placement count directly - it walks the exact same worklist shape,
    // just with an extra bbox-overlap test per placement instead of a
    // shape/bbox-generation cost. Measures the "zoom always re-computes"
    // case named in PIPELINE_REFACTOR.md (a fresh ViewportCullRunner per
    // iteration, same convention BM_HierarchyResolver uses) - pan-time
    // per-id caching is an explicit, separate follow-up.
    void BM_ViewportCull(benchmark::State &state, TileConfig config)
    {
        const AesScalingFixture &fixture = cached_aes_scaling_fixture(config);
        const std::vector<TechnologyId> technology_ids = fixture.root.get_technology_ids();
        const ViewLayerSetHandle view_layers_handle = std::make_shared<const ViewLayerSet>(
            technology_ids.empty() ? ViewLayerSet{} : ViewLayerSet::build_for_technology(fixture.root, technology_ids.front()));

        const ViewRenderOptions cold_options{
            .root = &fixture.root,
            .root_mutation_version = fixture.root.mutation_version(),
            .top_level = HierarchyId{fixture.layout_id},
            .hierarchy_depth = 1,
        };

        HierarchyResolverRunner hierarchy_resolver_runner{"bm_viewport_cull_hierarchy_resolver"};
        hierarchy_resolver_runner.run(view_layers_handle, 0, cold_options);
        const HierarchyResolverStage::OutputHandle cold_output = hierarchy_resolver_runner.last_handle();

        // Half the design's own declared bbox, by area (a quarter of each
        // axis's own extent kept) - representative of a real "zoomed in
        // partway" viewport rather than either extreme (whole-design keeps
        // everything; empty keeps nothing).
        const Rect design_bbox = layout_declared_bbox(fixture.root, fixture.layout_id);
        const int64_t width = design_bbox.ur.x - design_bbox.ll.x;
        const int64_t height = design_bbox.ur.y - design_bbox.ll.y;
        const ViewRenderOptions warm_options{
            .root = &fixture.root,
            .root_mutation_version = fixture.root.mutation_version(),
            .top_level = HierarchyId{fixture.layout_id},
            .hierarchy_depth = 1,
            .viewport = Rect{
                .ll = Point{design_bbox.ll.x + width / 4, design_bbox.ll.y + height / 4},
                .ur = Point{design_bbox.ur.x - width / 4, design_bbox.ur.y - height / 4},
            },
        };

        for (auto _ : state)
        {
            ViewportCullRunner runner{"bm_viewport_cull"};
            const HierarchyResolverOutput &output = runner.run(cold_output, 0, warm_options);
            benchmark::DoNotOptimize(output.view_data.size());
        }

        state.counters["PeakRSS_MB"] = peak_rss_mb();
    }
}

namespace le::benchmarks
{
    void register_viewport_cull_benchmarks()
    {
        for (const TileConfig &config : kAesScalingTileConfigs)
        {
            benchmark::RegisterBenchmark(("BM_ViewportCull/" + std::string(config.label)).c_str(), BM_ViewportCull, config)
                ->Unit(benchmark::kMillisecond);
        }

        benchmark::RegisterBenchmark(("BM_ViewportCull/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_ViewportCull, kAesScalingLargeConfig)
            ->Unit(benchmark::kMillisecond);
    }
}
