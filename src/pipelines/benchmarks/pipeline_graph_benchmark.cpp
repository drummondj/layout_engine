#include "../../geometry/placement_geometry.hpp"
#include "../pipeline_options.hpp"
#include "../stages/compose_stage.hpp"
#include "../stages/hierarchy_resolver_stage.hpp"
#include "../stages/layer_generation_stage.hpp"
#include "../stages/rasterize_blend2d_stage.hpp"
#include "../stages/viewport_cull_stage.hpp"
#include "../tbb_core.hpp"
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
    // What ViewRenderPipeline's flow::graph costs over calling its five
    // stages' execute() in sequence. Both paths run the same stage types on
    // the same inputs; only the hand-off differs. Each owns its stages so
    // their caches stay independent.
    enum class Driver
    {
        GRAPH,
        DIRECT,
    };

    struct Chain
    {
        explicit Chain(Driver driver)
            : driver(driver),
              layer_generation(graph, "bm_graph.LayerGeneration"),
              hierarchy_resolver(graph, "bm_graph.HierarchyResolver"),
              viewport_cull(graph, "bm_graph.ViewportCull"),
              rasterize(graph, "bm_graph.Rasterize"),
              compose(graph, "bm_graph.Compose"),
              sink(graph, oneapi::tbb::flow::serial,
                   [this](StageData<ComposeStage::OutputHandle, ViewRenderOptions> in) { frame = std::move(in.data); })
        {
            make_edge(layer_generation.node(), hierarchy_resolver.node());
            make_edge(hierarchy_resolver.node(), viewport_cull.node());
            make_edge(viewport_cull.node(), rasterize.node());
            make_edge(rasterize.node(), compose.node());
            make_edge(compose.node(), sink);
        }

        /// @brief Runs every stage, cache hit or not (no would_recompute
        /// short-circuit), so a no-op tick measures the hand-off alone.
        void run(const Root *root, const ViewRenderOptions &options)
        {
            StageData<const Root *, ViewRenderOptions> in{.data = root, .data_version = 0, .options = options};
            if (driver == Driver::GRAPH)
            {
                layer_generation.try_put(std::move(in));
                graph.wait_for_all();
                return;
            }
            frame = compose.execute(rasterize.execute(viewport_cull.execute(
                                        hierarchy_resolver.execute(layer_generation.execute(std::move(in))))))
                        .data;
        }

        Driver driver;
        oneapi::tbb::flow::graph graph;
        LayerGenerationStage layer_generation;
        HierarchyResolverStage hierarchy_resolver;
        ViewportCullStage viewport_cull;
        RasterizeBlend2DStage rasterize;
        ComposeStage compose;
        oneapi::tbb::flow::function_node<StageData<ComposeStage::OutputHandle, ViewRenderOptions>> sink;
        ComposeStage::OutputHandle frame;
    };

    /// @brief BM_WarmTier's pan sequence: 16 windows of 10% x 10% across
    /// the middle of the design, rendered at 1000px wide.
    struct PanSequence
    {
        std::vector<Rect> viewports;
        double scale = 1.0;
    };

    PanSequence make_pan_sequence(const AesScalingFixture &fixture)
    {
        const Rect bbox = layout_declared_bbox(fixture.root, fixture.layout_id);
        const int64_t width = bbox.ur.x - bbox.ll.x;
        const int64_t height = bbox.ur.y - bbox.ll.y;
        const int64_t window_w = width / 10;
        const int64_t window_h = height / 10;
        constexpr int kPanSteps = 16;

        PanSequence pan;
        pan.scale = 1000.0 / static_cast<double>(std::max<int64_t>(window_w, 1));
        for (int i = 0; i < kPanSteps; ++i)
        {
            const double t = static_cast<double>(i) / (kPanSteps - 1);
            const int64_t cx = bbox.ll.x + width / 4 + static_cast<int64_t>(t * static_cast<double>(width) / 2);
            const int64_t cy = bbox.ll.y + height / 4 + static_cast<int64_t>(t * static_cast<double>(height) / 2);
            pan.viewports.push_back(Rect{
                .ll = Point{cx - window_w / 2, cy - window_h / 2},
                .ur = Point{cx + window_w / 2, cy + window_h / 2},
            });
        }
        return pan;
    }

    ViewRenderOptions pan_options(const AesScalingFixture &fixture, const PanSequence &pan, size_t index)
    {
        return ViewRenderOptions{
            .root = &fixture.root, .root_mutation_version = fixture.root.mutation_version(),
            .top_level = HierarchyId{fixture.layout_id}, .hierarchy_depth = 1,
            .viewport = pan.viewports[index], .scale = pan.scale,
        };
    }

    // Every stage cache-hits: the cost of passing five messages through.
    void BM_PipelineNoOpTick(benchmark::State &state, TileConfig config, Driver driver)
    {
        const AesScalingFixture &fixture = cached_aes_scaling_fixture(config);
        const PanSequence pan = make_pan_sequence(fixture);
        const ViewRenderOptions options = pan_options(fixture, pan, 0);

        Chain chain{driver};
        chain.run(&fixture.root, options);
        for (auto _ : state)
        {
            chain.run(&fixture.root, options);
            benchmark::DoNotOptimize(chain.frame.get());
        }
    }

    // Cull, Rasterize and Compose recompute: a real pan frame.
    void BM_PipelinePanTick(benchmark::State &state, TileConfig config, Driver driver)
    {
        const AesScalingFixture &fixture = cached_aes_scaling_fixture(config);
        const PanSequence pan = make_pan_sequence(fixture);

        Chain chain{driver};
        chain.run(&fixture.root, pan_options(fixture, pan, 0));
        size_t pan_index = 0;
        for (auto _ : state)
        {
            pan_index = (pan_index + 1) % pan.viewports.size();
            chain.run(&fixture.root, pan_options(fixture, pan, pan_index));
            benchmark::DoNotOptimize(chain.frame.get());
        }
    }
}

namespace le::benchmarks
{
    void register_pipeline_graph_benchmarks()
    {
        for (const TileConfig &config : {kAesScalingTileConfigs.front(), kAesScalingTileConfigs.back()})
        {
            const std::string label = config.label;
            benchmark::RegisterBenchmark(("BM_PipelineNoOpTick/Graph/" + label).c_str(), BM_PipelineNoOpTick, config, Driver::GRAPH)
                ->Unit(benchmark::kMicrosecond);
            benchmark::RegisterBenchmark(("BM_PipelineNoOpTick/Direct/" + label).c_str(), BM_PipelineNoOpTick, config, Driver::DIRECT)
                ->Unit(benchmark::kMicrosecond);
            benchmark::RegisterBenchmark(("BM_PipelinePanTick/Graph/" + label).c_str(), BM_PipelinePanTick, config, Driver::GRAPH)
                ->Unit(benchmark::kMillisecond);
            benchmark::RegisterBenchmark(("BM_PipelinePanTick/Direct/" + label).c_str(), BM_PipelinePanTick, config, Driver::DIRECT)
                ->Unit(benchmark::kMillisecond);
        }
    }
}
