#include "../../core/placement_geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../stages/hierarchy_resolver_stage.hpp"
#include "../stages/rasterize_blend2d_stage.hpp"
#include "../stages/viewport_cull_stage.hpp"
#include "../tests/synchronous_stage_runner.hpp"
#include "aes_scaling_fixture.hpp"
#include "pipeline_benchmarks.hpp"

#include <benchmark/benchmark.h>

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
    using RasterizeBlend2DRunner = SynchronousStageRunner<RasterizeBlend2DStage, HierarchyResolverStage::OutputHandle, RasterizeOutput, ViewRenderOptions>;

    /// @brief Blend2D sibling of rasterize_benchmark.cpp's own BM_Rasterize -
    /// identical fixture/pan-position/warm-up-then-timed-loop structure
    /// (same `aes_scaling` design content, same 16-position pan sequence,
    /// same 1000px-window scale derivation) so the two backends' own
    /// numbers are a fair, apples-to-apples comparison - only the stage
    /// under test differs. `thread_count`/`use_opaque_fast_path` map
    /// directly onto RasterizeBlend2DStage's own set_thread_count()/
    /// set_use_opaque_fast_path() (rasterize_blend2d_stage.hpp) - the
    /// user's own requested 4-step comparison (single-threaded; +multi-
    /// threaded; +opaque fast-path) is just this same function registered
    /// several times with different values, mirroring how BM_Rasterize's
    /// own `apply_default_visibility` bool already works.
    void BM_RasterizeBlend2D(benchmark::State &state, TileConfig config, uint32_t thread_count, bool use_opaque_fast_path)
    {
        const AesScalingFixture &fixture = cached_aes_scaling_fixture(config);
        const std::vector<TechnologyId> technology_ids = fixture.root.get_technology_ids();
        const ViewLayerSetHandle view_layers_handle = std::make_shared<const ViewLayerSet>(
            technology_ids.empty() ? ViewLayerSet{} : ViewLayerSet::build_for_technology(fixture.root, technology_ids.front()));

        ViewRenderOptions cold_options{
            .root = &fixture.root, .root_mutation_version = fixture.root.mutation_version(),
            .top_level = HierarchyId{fixture.layout_id}, .hierarchy_depth = 1, .view_layers = view_layers_handle,
        };

        HierarchyResolverRunner hierarchy_resolver_runner{"bm_rasterize_blend2d_hierarchy_resolver"};
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
        std::vector<HierarchyResolverStage::OutputHandle> culled_by_pan;
        warm_options_by_pan.reserve(kPanSteps);
        culled_by_pan.reserve(kPanSteps);

        ViewportCullRunner viewport_cull_runner{"bm_rasterize_blend2d_viewport_cull"};
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
            warm_options_by_pan.push_back(warm_options);
            culled_by_pan.push_back(viewport_cull_runner.last_handle());
        }

        RasterizeBlend2DRunner rasterize_runner{"bm_rasterize_blend2d"};
        rasterize_runner.stage().set_thread_count(thread_count);
        rasterize_runner.stage().set_use_opaque_fast_path(use_opaque_fast_path);
        rasterize_runner.run(culled_by_pan.front(), 0, warm_options_by_pan.front()); // warm-up, not timed

        int pan_index = 0;
        for (auto _ : state)
        {
            pan_index = (pan_index + 1) % kPanSteps;
            const RasterizeOutput &output = rasterize_runner.run(
                culled_by_pan[pan_index], static_cast<std::uint64_t>(pan_index + 1), warm_options_by_pan[pan_index]);
            benchmark::DoNotOptimize(output.images.size());
        }

        state.counters["PeakRSS_MB"] = peak_rss_mb();
    }
}

namespace le::benchmarks
{
    void register_rasterize_blend2d_benchmarks()
    {
        for (const TileConfig &config : kAesScalingTileConfigs)
        {
            benchmark::RegisterBenchmark(("BM_RasterizeBlend2D/" + std::string(config.label)).c_str(), BM_RasterizeBlend2D, config, 0u, false)
                ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark(("BM_RasterizeBlend2D_MT2/" + std::string(config.label)).c_str(), BM_RasterizeBlend2D, config, 2u, false)
                ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark(("BM_RasterizeBlend2D_MT4/" + std::string(config.label)).c_str(), BM_RasterizeBlend2D, config, 4u, false)
                ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark(("BM_RasterizeBlend2D_MT4Opaque/" + std::string(config.label)).c_str(), BM_RasterizeBlend2D, config, 4u, true)
                ->Unit(benchmark::kMillisecond);
        }

        benchmark::RegisterBenchmark(("BM_RasterizeBlend2D/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_RasterizeBlend2D, kAesScalingLargeConfig, 0u, false)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark(("BM_RasterizeBlend2D_MT2/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_RasterizeBlend2D, kAesScalingLargeConfig, 2u, false)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark(("BM_RasterizeBlend2D_MT4/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_RasterizeBlend2D, kAesScalingLargeConfig, 4u, false)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark(("BM_RasterizeBlend2D_MT4Opaque/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_RasterizeBlend2D, kAesScalingLargeConfig, 4u, true)
            ->Unit(benchmark::kMillisecond);
    }
}
