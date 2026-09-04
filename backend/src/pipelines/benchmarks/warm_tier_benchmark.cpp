#include "../../core/placement_geometry.hpp"
#include "../pipeline_options.hpp"
#include "../view_render_pipeline.hpp"
#include "aes_scaling_fixture.hpp"
#include "pipeline_benchmarks.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace le;
using namespace le::benchmarks;

namespace
{
    // Measures the WHOLE Warm tier (ViewportCull + Rasterize + Compose,
    // via ViewRenderPipeline::run_warm()) against its own shared 500ms
    // budget - PIPELINE_REFACTOR.md names 500ms for the tier as a whole,
    // not per stage (a real, previously-wrong assumption corrected mid-
    // development - see PIPELINE_REFACTOR_BENCHMARK_RESULTS.md), so this
    // is the number that actually matters, not any one stage's own
    // isolated cost. One persistent ViewRenderPipeline reused across a
    // 16-position pan sequence (ViewportCullStage's own benchmark
    // comment explains why a fresh instance per iteration would be
    // measuring a cold start, not the real "zoom/pan tick against an
    // already-warm pipeline" case this tier actually runs under).
    void BM_WarmTier(benchmark::State &state, TileConfig config)
    {
        const AesScalingFixture &fixture = cached_aes_scaling_fixture(config);

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

        // A fixed 1000x1000px output regardless of the window's own dbu
        // size - scale is derived per point so every config renders the
        // same real screen size, matching how a real viewer would behave
        // (window size is a property of the display, not the design).
        const double scale = 1000.0 / static_cast<double>(std::max<int64_t>(window_w, 1));

        ViewRenderPipeline pipeline{"bm_warm_tier"};
        const ViewRenderOptions warm_up_options{
            .root = &fixture.root, .root_mutation_version = fixture.root.mutation_version(),
            .top_level = HierarchyId{fixture.layout_id}, .hierarchy_depth = 1,
            .viewport = pan_viewports.front(), .scale = scale,
        };
        pipeline.run_warm(&fixture.root, warm_up_options); // not timed - pays every one-time cost (index build, distinct-Abstract rasterization) up front

        // Starts at index 1, not 0 - index 0 is exactly what the untimed
        // warm-up call above already used, and every stage's own
        // MemoizingStage cache compares against the *last* (data_version,
        // options) pair regardless of which call set it - replaying index
        // 0 as the very first timed iteration would be a guaranteed,
        // free cache hit doing zero real work (confirmed directly: this
        // was silently inflating BM_WarmTier's own apparent speed by
        // ~1/state.iterations() - about 10% at the 5x5 point's own 10
        // iterations - until fixed here; BM_Rasterize/BM_Compose already
        // avoid this by bumping their own data_version on every timed
        // call regardless of options, see their own pan_index handling).
        int pan_index = 0;
        for (auto _ : state)
        {
            pan_index = (pan_index + 1) % pan_viewports.size();
            const ViewRenderOptions options{
                .root = &fixture.root, .root_mutation_version = fixture.root.mutation_version(),
                .top_level = HierarchyId{fixture.layout_id}, .hierarchy_depth = 1,
                .viewport = pan_viewports[pan_index], .scale = scale,
            };
            const ViewRenderPipeline::WarmOutput output = pipeline.run_warm(&fixture.root, options);
            int frame_width = output.frame->buffer.width;
            benchmark::DoNotOptimize(frame_width);
        }

        state.counters["PeakRSS_MB"] = peak_rss_mb();
    }

    // The other number BM_WarmTier can't give you: the true cost of the
    // very first render - Cold running for real (LayerGeneration +
    // HierarchyResolver, nothing cached yet) immediately followed by one
    // Warm tick, with no untimed warm-up call priming any stage's own
    // MemoizingStage cache first. A fresh ViewRenderPipeline is
    // constructed INSIDE the timed region for the same reason - reusing
    // one across iterations (BM_WarmTier's own convention) would make
    // every iteration after the first a Cold cache hit, defeating the
    // entire point of this benchmark. ->Iterations(1) on registration
    // (below) is load-bearing, not cosmetic: without it, Google
    // Benchmark's own calibration phase invokes this function's entire
    // body - construction included - more than once to find a stable
    // per-iteration cost, and every invocation after the first would
    // again just be timing a warm pipeline (a fresh Root isn't
    // reloaded per call either, only the ViewRenderPipeline is, so nothing
    // else resets between those extra calibration calls).
    void BM_WarmTierColdStart(benchmark::State &state, TileConfig config)
    {
        const AesScalingFixture &fixture = cached_aes_scaling_fixture(config);

        // Same "10% x 10% window centered a quarter in from the origin"
        // point BM_WarmTier's own pan sequence starts at (its own t=0
        // case) - not that the exact window matters much here, only that
        // it's a realistic partial-design view, not "everything" or
        // "nothing".
        const Rect design_bbox = layout_declared_bbox(fixture.root, fixture.layout_id);
        const int64_t width = design_bbox.ur.x - design_bbox.ll.x;
        const int64_t height = design_bbox.ur.y - design_bbox.ll.y;
        const int64_t window_w = width / 10;
        const int64_t window_h = height / 10;
        const int64_t cx = design_bbox.ll.x + width / 4;
        const int64_t cy = design_bbox.ll.y + height / 4;
        const double scale = 1000.0 / static_cast<double>(std::max<int64_t>(window_w, 1));

        const ViewRenderOptions options{
            .root = &fixture.root, .root_mutation_version = fixture.root.mutation_version(),
            .top_level = HierarchyId{fixture.layout_id}, .hierarchy_depth = 1,
            .viewport = Rect{
                .ll = Point{cx - window_w / 2, cy - window_h / 2},
                .ur = Point{cx + window_w / 2, cy + window_h / 2},
            },
            .scale = scale,
        };

        for (auto _ : state)
        {
            ViewRenderPipeline pipeline{"bm_warm_tier_cold_start"};
            const ViewRenderPipeline::WarmOutput output = pipeline.run_warm(&fixture.root, options);
            int frame_width = output.frame->buffer.width;
            benchmark::DoNotOptimize(frame_width);
        }

        state.counters["PeakRSS_MB"] = peak_rss_mb();
    }
}

namespace le::benchmarks
{
    void register_warm_tier_benchmarks()
    {
        for (const TileConfig &config : kAesScalingTileConfigs)
        {
            benchmark::RegisterBenchmark(("BM_WarmTier/" + std::string(config.label)).c_str(), BM_WarmTier, config)
                ->Unit(benchmark::kMillisecond);
        }

        benchmark::RegisterBenchmark(("BM_WarmTier/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_WarmTier, kAesScalingLargeConfig)
            ->Unit(benchmark::kMillisecond);

        // Isolated to the 5x5 point alone (--benchmark_filter=BM_WarmTierColdStart)
        // - same "run in isolation for a real memory reading" convention
        // every other 1M-component point in this file follows.
        benchmark::RegisterBenchmark(("BM_WarmTierColdStart/" + std::string(kAesScalingLargeConfig.label)).c_str(), BM_WarmTierColdStart, kAesScalingLargeConfig)
            ->Unit(benchmark::kMillisecond)
            ->Iterations(1);
    }
}
