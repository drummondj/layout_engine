#include "../../core/placement_geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../stages/compose_stage.hpp"
#include "../stages/hierarchy_resolver_stage.hpp"
#include "../stages/rasterize_blend2d_stage.hpp"
#include "../stages/viewport_cull_stage.hpp"
#include "../tests/synchronous_stage_runner.hpp"
#include "aes_scaling_fixture.hpp"
#include "pipeline_benchmarks.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace le;
using namespace le::benchmarks;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using ViewportCullRunner = SynchronousStageRunner<ViewportCullStage, HierarchyResolverStage::OutputHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using RasterizeRunner = SynchronousStageRunner<RasterizeBlend2DStage, HierarchyResolverStage::OutputHandle, RasterizeOutput, ViewRenderOptions>;
    using ComposeRunner = SynchronousStageRunner<ComposeStage, RasterizeOutputHandle, RasterizedFrame, ViewRenderOptions>;

    // Isolates ComposeStage's own cost from BM_WarmTier's combined number
    // the same way BM_RasterizeBlend2D isolates RasterizeBlend2DStage's - every
    // (viewport, RasterizeOutput) pair is precomputed OUTSIDE the timed
    // loop (ViewportCull + Rasterize both run once per pan position ahead
    // of time; only their OUTPUT matters here, not their own cost), so
    // the loop below times ComposeStage's own compute() alone.
    void BM_Compose(benchmark::State &state, TileConfig config)
    {
        const AesScalingFixture &fixture = cached_aes_scaling_fixture(config);
        const std::vector<TechnologyId> technology_ids = fixture.root.get_technology_ids();
        const ViewLayerSetHandle view_layers_handle = std::make_shared<const ViewLayerSet>(
            technology_ids.empty() ? ViewLayerSet{} : ViewLayerSet::build_for_technology(fixture.root, technology_ids.front()));

        const ViewRenderOptions cold_options{
            .root = &fixture.root, .root_mutation_version = fixture.root.mutation_version(),
            .top_level = HierarchyId{fixture.layout_id}, .hierarchy_depth = 1,
        };
        HierarchyResolverRunner hierarchy_resolver_runner{"bm_compose_hierarchy_resolver"};
        hierarchy_resolver_runner.run(view_layers_handle, 0, cold_options);
        const HierarchyResolverStage::OutputHandle cold_output = hierarchy_resolver_runner.last_handle();

        const Rect design_bbox = layout_declared_bbox(fixture.root, fixture.layout_id);
        const int64_t width = design_bbox.ur.x - design_bbox.ll.x;
        const int64_t height = design_bbox.ur.y - design_bbox.ll.y;
        const int64_t window_w = width / 10;
        const int64_t window_h = height / 10;
        constexpr int kPanSteps = 16;
        const double scale = 1000.0 / static_cast<double>(std::max<int64_t>(window_w, 1));

        std::vector<ViewRenderOptions> warm_options_by_pan;
        std::vector<RasterizeOutputHandle> rasterized_by_pan;
        warm_options_by_pan.reserve(kPanSteps);
        rasterized_by_pan.reserve(kPanSteps);

        ViewportCullRunner viewport_cull_runner{"bm_compose_viewport_cull"};
        RasterizeRunner rasterize_runner{"bm_compose_rasterize"};
        for (int i = 0; i < kPanSteps; ++i)
        {
            const double t = static_cast<double>(i) / (kPanSteps - 1);
            const int64_t cx = design_bbox.ll.x + width / 4 + static_cast<int64_t>(t * static_cast<double>(width) / 2);
            const int64_t cy = design_bbox.ll.y + height / 4 + static_cast<int64_t>(t * static_cast<double>(height) / 2);
            const Rect viewport{.ll = Point{cx - window_w / 2, cy - window_h / 2}, .ur = Point{cx + window_w / 2, cy + window_h / 2}};

            ViewRenderOptions warm_options = cold_options;
            warm_options.viewport = viewport;
            warm_options.scale = scale;

            viewport_cull_runner.run(cold_output, static_cast<std::uint64_t>(i + 1), warm_options);
            rasterize_runner.run(viewport_cull_runner.last_handle(), static_cast<std::uint64_t>(i + 1), warm_options);
            warm_options_by_pan.push_back(warm_options);
            rasterized_by_pan.push_back(rasterize_runner.last_handle());
        }

        ComposeRunner compose_runner{"bm_compose"};
        compose_runner.run(rasterized_by_pan.front(), 0, warm_options_by_pan.front()); // warm-up, not timed

        int pan_index = 0;
        for (auto _ : state)
        {
            pan_index = (pan_index + 1) % kPanSteps;
            const RasterizedFrame &frame = compose_runner.run(
                rasterized_by_pan[pan_index], static_cast<std::uint64_t>(pan_index + 1), warm_options_by_pan[pan_index]);
            int frame_width = frame.buffer.width;
            benchmark::DoNotOptimize(frame_width);
        }

        state.counters["PeakRSS_MB"] = peak_rss_mb();
    }
}

namespace le::benchmarks
{
    void register_compose_benchmarks()
    {
        for (const TileConfig &config : kAesScalingTileConfigs)
        {
            benchmark::RegisterBenchmark(("BM_Compose/" + std::string(config.label)).c_str(), BM_Compose, config)
                ->Unit(benchmark::kMillisecond);
        }

        benchmark::RegisterBenchmark(("BM_Compose/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_Compose, kAesScalingLargeConfig)
            ->Unit(benchmark::kMillisecond);
    }
}
