#include "../../core/placement_geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../stages/hierarchy_resolver_stage.hpp"
#include "../stages/rasterize_stage.hpp"
#include "../stages/viewport_cull_stage.hpp"
#include "../tests/synchronous_stage_runner.hpp"
#include "aes_scaling_fixture.hpp"
#include "pipeline_benchmarks.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace le;
using namespace le::benchmarks;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using ViewportCullRunner = SynchronousStageRunner<ViewportCullStage, HierarchyResolverStage::OutputHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using RasterizeRunner = SynchronousStageRunner<RasterizeStage, HierarchyResolverStage::OutputHandle, RasterizeOutput, ViewRenderOptions>;

    // Mirrors Scene's own pre-seeded default (scene.hpp's own
    // purpose_visible_ member comment, BUGS_AND_ENHANCEMENTS.md E2) -
    // what a real UI session actually renders out of the box, not the
    // "everything visible" case BM_Rasterize/BM_WarmTier otherwise
    // measure. Kept here rather than in scene.hpp itself, since a
    // benchmark shouldn't depend on the scene module just to read one
    // default - this is a plain, independent copy of that same default.
    std::unordered_map<ViewLayerPurpose, bool> default_hidden_purposes()
    {
        return {
            {ViewLayerPurpose::TRACK_PREFERRED, false},
            {ViewLayerPurpose::TRACK_NON_PREFERRED, false},
            {ViewLayerPurpose::ROW, false},
            {ViewLayerPurpose::GCELLGRID, false},
        };
    }

    // Isolates RasterizeStage's own cost from BM_WarmTier's combined
    // number - every (viewport, culled-input) pair is precomputed OUTSIDE
    // the timed loop (a fresh ViewportCullStage per pan position, cost
    // irrelevant here - only correctness of the pairing matters), so the
    // loop below times RasterizeStage's own compute() alone, repeated
    // across the same 16-position pan sequence BM_ViewportCull/BM_WarmTier
    // use, at a fixed 1000x1000px output size. `apply_default_visibility`
    // switches between "everything visible" (BM_Rasterize, comparable
    // with every earlier commit's own numbers) and the real UI-default
    // case (BM_RasterizeDefaultVisibility - default_hidden_purposes()
    // above) - both share every other pan/window/scale setup exactly.
    void BM_Rasterize(benchmark::State &state, TileConfig config, bool apply_default_visibility)
    {
        const AesScalingFixture &fixture = cached_aes_scaling_fixture(config);
        const std::vector<TechnologyId> technology_ids = fixture.root.get_technology_ids();
        const ViewLayerSetHandle view_layers_handle = std::make_shared<const ViewLayerSet>(
            technology_ids.empty() ? ViewLayerSet{} : ViewLayerSet::build_for_technology(fixture.root, technology_ids.front()));

        ViewRenderOptions cold_options{
            .root = &fixture.root, .root_mutation_version = fixture.root.mutation_version(),
            .top_level = HierarchyId{fixture.layout_id}, .hierarchy_depth = 1,
        };
        if (apply_default_visibility)
            cold_options.purpose_visible = default_hidden_purposes();

        HierarchyResolverRunner hierarchy_resolver_runner{"bm_rasterize_hierarchy_resolver"};
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

        ViewportCullRunner viewport_cull_runner{"bm_rasterize_viewport_cull"};
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

        RasterizeRunner rasterize_runner{"bm_rasterize"};
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
    void register_rasterize_benchmarks()
    {
        for (const TileConfig &config : kAesScalingTileConfigs)
        {
            benchmark::RegisterBenchmark(("BM_Rasterize/" + std::string(config.label)).c_str(), BM_Rasterize, config, false)
                ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark(("BM_RasterizeDefaultVisibility/" + std::string(config.label)).c_str(), BM_Rasterize, config, true)
                ->Unit(benchmark::kMillisecond);
        }

        benchmark::RegisterBenchmark(("BM_Rasterize/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_Rasterize, kAesScalingLargeConfig, false)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark(("BM_RasterizeDefaultVisibility/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_Rasterize, kAesScalingLargeConfig, true)
            ->Unit(benchmark::kMillisecond);
    }
}
