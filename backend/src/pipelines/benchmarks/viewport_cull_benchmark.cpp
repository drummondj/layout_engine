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

    // Warm stage 1's own steady-state cost is the *reuse* case, not a
    // cold start: ViewportCullStage builds each node's own spatial index
    // at most once per distinct Cold input and keeps reusing it across
    // every later viewport-only call (its own doc comment) - real zoom/
    // pan ticks hit one persistent stage instance many times in a row,
    // never a fresh one per tick. A fresh ViewportCullRunner per
    // iteration (BM_HierarchyResolver's own convention, for measuring a
    // real cache *miss*) would defeat the index entirely here, paying a
    // full rebuild every single iteration - confirmed directly, an
    // earlier version doing exactly that made every point in this matrix
    // slower than the pre-index linear scan it was meant to replace, not
    // faster. So: one runner, reused across the whole benchmark; each
    // iteration presents a genuinely different viewport (panning across
    // a small window) so MemoizingStage's own options_did_change() still
    // forces a real compute() call every time, exercising the index
    // *query* path without ever forcing a rebuild.
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

        // A small (10% x 10% of the design's own declared bbox) window,
        // panned across 16 positions spanning the middle half of each
        // axis - representative of a real zoomed-in pan sequence, never
        // repeating the exact same Rect twice in a row (which would let
        // MemoizingStage's own cache skip compute() entirely).
        const Rect design_bbox = layout_declared_bbox(fixture.root, fixture.layout_id);
        const int64_t width = design_bbox.ur.x - design_bbox.ll.x;
        const int64_t height = design_bbox.ur.y - design_bbox.ll.y;
        const int64_t window_w = width / 10;
        const int64_t window_h = height / 10;
        constexpr int kPanSteps = 16;
        std::vector<Rect> pan_viewports;
        pan_viewports.reserve(kPanSteps);
        for (int i = 0; i < kPanSteps; ++i)
        {
            const double t = static_cast<double>(i) / (kPanSteps - 1);
            const int64_t cx = design_bbox.ll.x + width / 4 + static_cast<int64_t>(t * static_cast<double>(width) / 2);
            const int64_t cy = design_bbox.ll.y + height / 4 + static_cast<int64_t>(t * static_cast<double>(height) / 2);
            pan_viewports.push_back(Rect{
                .ll = Point{cx - window_w / 2, cy - window_h / 2},
                .ur = Point{cx + window_w / 2, cy + window_h / 2},
            });
        }

        ViewportCullRunner runner{"bm_viewport_cull"};
        // Warm-up call (not timed) - pays the one-time index-build cost
        // for every node the pan sequence below will touch, same as a
        // real first tick right after a Cold recompute would.
        runner.run(cold_output, 0, ViewRenderOptions{cold_options.root, cold_options.root_mutation_version, cold_options.top_level, cold_options.hierarchy_depth, pan_viewports.front()});

        int pan_index = 0;
        for (auto _ : state)
        {
            const ViewRenderOptions warm_options{cold_options.root, cold_options.root_mutation_version, cold_options.top_level, cold_options.hierarchy_depth, pan_viewports[pan_index]};
            pan_index = (pan_index + 1) % pan_viewports.size();
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
