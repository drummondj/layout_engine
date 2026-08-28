#include "../pipeline/generate_and_filter_pipeline.hpp"
#include <benchmark/benchmark.h>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace le;

// Real generate_shapes/filter_by_viewport_and_size workload (unlike
// flow_benchmark.cpp's own synthetic stand-in) run through the per-layer
// DAG (TASKFLOW_EXPERIMENT.md's per-layer-granularity redesign) - answers
// goal 1 against the actual algorithm. Deliberately builds its own
// 1,000,000-item Root directly via the database's create_* API (matching
// pipeline_test.cpp's own fixture style) rather than parsing a LEF file
// (pipeline_benchmark.cpp's stress_data.hpp) or including anything from
// src/pipeline - this stays fully self-contained under src/flow (see
// backend/TASKFLOW_EXPERIMENT.md).
//
// Only "isolated stage" benchmark left is the end-to-end
// GenerateAndFilterPipeline itself - the earlier per-stage
// BM_Flow_GenerateAbstractShapes/BM_Flow_FilterByViewportAndSize numbers
// don't have a like-for-like equivalent anymore: "one stage" used to mean
// "the whole Abstract's worth of work", now it means "one physical
// layer's worth", so the only number that's still directly comparable to
// the pre-redesign numbers already recorded in TASKFLOW_EXPERIMENT.md is
// the full pipeline.
namespace
{
    // Mirrors src/pipeline/benchmarks/stress_data.hpp's own 90/10 Terminal/
    // Obstruction split at the same 1,000,000-shape scale, so the numbers
    // here read on comparable (not identical - different construction path)
    // terms to BENCHMARKS.md's existing BM_GenerateShapes/
    // BM_FilterByViewportAndSize entries, and to this same file's own
    // pre-redesign numbers already recorded in TASKFLOW_EXPERIMENT.md.
    constexpr int kTerminalCount = 100'000;
    constexpr int kObstructionCount = 4;
    constexpr int kObstructionShapesTotal = 900'000;
    constexpr int kTotalShapes = kTerminalCount + kObstructionShapesTotal;

    struct StressData
    {
        Root root;
        AbstractId abstract_id;
        ViewLayerSet view_layers;
    };

    // layer_count is a real, deliberate parameter, not just a knob for its
    // own sake: the per-layer DAG's own parallelism ceiling is bounded by
    // (layer_count * 2) independent (generate, filter) chains, unlike the
    // pre-redesign chunked-within-a-stage design, which scaled with
    // hardware_concurrency() regardless of layer count. Comparing a
    // realistic-metal-stack layer count against the original 2-layer
    // (M1/M2) fixture is how that hypothesis actually gets checked,
    // instead of just asserted - see TASKFLOW_EXPERIMENT.md.
    const StressData &stress_data(int layer_count)
    {
        static std::map<int, StressData> cache;
        auto it = cache.find(layer_count);
        if (it != cache.end())
            return it->second;

        StressData d;
        const TechnologyId technology_id = d.root.create_technology(TechnologyData{.database_units_microns = 1000.0});
        std::vector<LayerId> layers;
        for (int i = 0; i < layer_count; ++i)
            layers.push_back(d.root.create_layer(LayerData{.technology = technology_id, .name = "M" + std::to_string(i + 1), .type = "ROUTING"}));
        d.view_layers = ViewLayerSet::build_for_technology(d.root, technology_id);
        d.abstract_id = d.root.create_abstract(AbstractData{});

        // Deterministic per-item spread, same shape as stress_data.hpp's
        // own item_geometry: 1000 columns x N rows, size ramping so
        // roughly half the shapes land under a mid-range sub-pixel
        // threshold, cycling through every layer in `layers`.
        auto place = [](int index)
        {
            struct
            {
                int64_t x, y, size;
            } p;
            p.x = (index % 1000) * 200;
            p.y = (index / 1000) * 200;
            p.size = 1 + (index % 1000);
            return p;
        };

        // Alternates RECT/POLYGON/PATH exactly like stress_data.hpp's own
        // write_geometry_item - matters for a fair comparison against the
        // real Pipeline's own benchmarks: a PATH shape costs a real
        // Geometry::path_to_polygons call (~768ns/call per
        // BM_PathToPolygonsSingleCall, BENCHMARKS.md) that an
        // all-RECT fixture would never exercise, silently making this
        // benchmark cheaper per item than the one it's meant to compare
        // against.
        auto make_geometry = [&](int index, Shape &shape)
        {
            const auto p = place(index);
            switch (index % 3)
            {
            case 0:
                shape.rects.push_back(Rect{.ll = {p.x, p.y}, .ur = {p.x + p.size, p.y + p.size}});
                break;
            case 1:
                shape.polygons.push_back(Polygon{.points = {{p.x, p.y}, {p.x + p.size, p.y}, {p.x + p.size, p.y + p.size}, {p.x, p.y + p.size}}});
                break;
            case 2:
                shape.paths.push_back(Path{.polygon = Polygon{.points = {{p.x, p.y}, {p.x + p.size, p.y}}}, .width = p.size});
                break;
            }
        };

        for (int i = 0; i < kTerminalCount; ++i)
        {
            const LayerId layer = layers[i % layers.size()];
            const TerminalId terminal_id = d.root.create_terminal(TerminalData{.abstract = d.abstract_id, .name = "T" + std::to_string(i)});
            const TerminalPortId port_id = d.root.create_terminal_port(TerminalPortData{.terminal = terminal_id});
            Shape shape{.layer = layer, .terminal_port = port_id};
            make_geometry(i, shape);
            d.root.create_shape(std::move(shape));
        }

        int shape_index = 0;
        const int shapes_per_obstruction = kObstructionShapesTotal / kObstructionCount;
        for (int o = 0; o < kObstructionCount; ++o)
        {
            const ObstructionId obstruction_id = d.root.create_obstruction(ObstructionData{.abstract = d.abstract_id});
            for (int i = 0; i < shapes_per_obstruction; ++i, ++shape_index)
            {
                const LayerId layer = layers[shape_index % layers.size()];
                Shape shape{.layer = layer, .obstruction = obstruction_id};
                make_geometry(shape_index, shape);
                d.root.create_shape(std::move(shape));
            }
        }

        return cache.emplace(layer_count, std::move(d)).first->second;
    }

    // Scale chosen so the 1..1000 dbu size spread straddles the 1px
    // threshold, matching stress_data.hpp's own make_scene intent.
    Scene make_scene(const StressData &data)
    {
        Scene scene;
        scene.set_current_abstract(data.abstract_id);
        scene.set_pan(Point{0, 0});
        scene.set_scale(0.005);
        scene.set_viewport_size(2000, 2000);
        return scene;
    }
}

// state.range(0) = worker count, state.range(1) = layer count (the per-layer
// DAG's own parallelism ceiling is 2 * layer_count independent chains).
static void BM_Flow_GenerateAndFilterPipeline(benchmark::State &state)
{
    const auto &data = stress_data(static_cast<int>(state.range(1)));
    Scene scene = make_scene(data);
    tf::Executor executor(static_cast<unsigned>(state.range(0)));

    for (auto _ : state)
    {
        flow::GenerateAndFilterPipeline pipeline; // fresh, uncached - measures real per-call cost, not a cache hit
        const auto &filtered = pipeline.run(data.root, data.abstract_id, data.view_layers, scene, executor);
        const auto *filtered_data = filtered.data();
        benchmark::DoNotOptimize(filtered_data);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kTotalShapes));
}

namespace
{
    // Registers once per (worker count, layer count) in
    // {1,2,4,8,hardware_concurrency()} x {2, 8} - 2 layers matches the
    // original M1/M2 stress fixture (comparable to the pre-redesign
    // numbers already in TASKFLOW_EXPERIMENT.md); 8 is a more realistic
    // metal-stack layer count, checking whether the per-layer DAG's own
    // parallelism-ceiling hypothesis actually holds.
    struct RegisterFlowPipelineStageBenchmarks
    {
        RegisterFlowPipelineStageBenchmarks()
        {
            std::set<unsigned> worker_counts{1, 2, 4, 8, std::thread::hardware_concurrency()};
            for (int layer_count : {2, 8})
                for (unsigned workers : worker_counts)
                {
                    if (workers == 0)
                        continue;
                    benchmark::RegisterBenchmark("BM_Flow_GenerateAndFilterPipeline", BM_Flow_GenerateAndFilterPipeline)
                        ->Args({static_cast<int64_t>(workers), layer_count})
                        ->ArgNames({"workers", "layers"})
                        ->Unit(benchmark::kMillisecond)
                        ->UseRealTime();
                }
        }
    } register_flow_pipeline_stage_benchmarks;
}

BENCHMARK_MAIN();
