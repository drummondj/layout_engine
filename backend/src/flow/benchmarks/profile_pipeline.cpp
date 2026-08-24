#include "../flow.hpp"
#include "../pipeline/generate_and_filter_pipeline.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// Dev-only tool (mirrors src/pipeline/benchmarks/render_preview.cpp's own
// "not a benchmark, a way to actually look at the thing" role) - wires
// flow::StageProfiler (task_profiler.hpp, TASKFLOW_EXPERIMENT.md goal 3)
// onto GenerateAndFilterPipeline's own executor and prints a real,
// per-stage-name report. No pipeline-class changes needed at all: `run()`
// already takes the tf::Executor by reference, and attaching an observer
// is purely an Executor-level operation - the profiler needed nothing
// wired into the pipeline itself to work against it.
using namespace le;

namespace
{
    struct StressData
    {
        Root root;
        AbstractId abstract_id;
        ViewLayerSet view_layers;
    };

    // Same shape as pipeline_stages_benchmark.cpp's own stress_data() -
    // RECT/POLYGON/PATH alternation included (see TASKFLOW_EXPERIMENT.md's
    // "Correction" entry for why that alternation matters for realism) -
    // but a moderate, single-shot scale (not 1,000,000 items): this is a
    // report to read, not a throughput benchmark, and several real layers
    // matter more here than raw item count, so the profiler has more than
    // one interesting row to show.
    StressData build_stress_data(int layer_count, int terminal_count, int obstruction_count, int obstruction_shapes_total)
    {
        StressData d;
        const TechnologyId technology_id = d.root.create_technology(TechnologyData{.database_units_microns = 1000.0});
        std::vector<LayerId> layers;
        for (int i = 0; i < layer_count; ++i)
            layers.push_back(d.root.create_layer(LayerData{.technology = technology_id, .name = "M" + std::to_string(i + 1), .type = "ROUTING"}));
        d.view_layers = ViewLayerSet::build_for_technology(d.root, technology_id);
        d.abstract_id = d.root.create_abstract(AbstractData{});

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

        for (int i = 0; i < terminal_count; ++i)
        {
            const LayerId layer = layers[i % layers.size()];
            const TerminalId terminal_id = d.root.create_terminal(TerminalData{.abstract = d.abstract_id, .name = "T" + std::to_string(i)});
            const TerminalPortId port_id = d.root.create_terminal_port(TerminalPortData{.terminal = terminal_id});
            Shape shape{.layer = layer, .terminal_port = port_id};
            make_geometry(i, shape);
            d.root.create_shape(std::move(shape));
        }

        int shape_index = 0;
        const int shapes_per_obstruction = obstruction_shapes_total / obstruction_count;
        for (int o = 0; o < obstruction_count; ++o)
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

        return d;
    }

    Scene make_scene(const StressData &data)
    {
        Scene scene;
        scene.set_current_abstract(data.abstract_id);
        scene.set_pan(Point{0, 0});
        scene.set_scale(0.005);
        scene.set_viewport_size(2000, 2000);
        return scene;
    }

    void print_report(const std::map<std::string, flow::StageProfiler::StageStats> &report)
    {
        std::cout << std::left << std::setw(28) << "stage"
                   << std::right << std::setw(8) << "calls"
                   << std::setw(12) << "total(ms)"
                   << std::setw(12) << "avg(us)"
                   << std::setw(12) << "min(us)"
                   << std::setw(12) << "max(us)" << "\n";
        std::cout << std::string(84, '-') << "\n";

        double grand_total_ms = 0.0;
        for (const auto &[name, stats] : report)
        {
            const double total_ms = std::chrono::duration<double, std::milli>(stats.total).count();
            const double avg_us = std::chrono::duration<double, std::micro>(stats.total).count() / static_cast<double>(stats.calls);
            const double min_us = std::chrono::duration<double, std::micro>(stats.min).count();
            const double max_us = std::chrono::duration<double, std::micro>(stats.max).count();
            grand_total_ms += total_ms;

            std::cout << std::left << std::setw(28) << (name.empty() ? "(unnamed)" : name)
                       << std::right << std::setw(8) << stats.calls
                       << std::fixed << std::setprecision(2)
                       << std::setw(12) << total_ms
                       << std::setw(12) << avg_us
                       << std::setw(12) << min_us
                       << std::setw(12) << max_us << "\n";
        }
        std::cout << std::string(84, '-') << "\n";
        std::cout << "sum of per-stage total time: " << std::fixed << std::setprecision(2) << grand_total_ms << " ms"
                   << " (wall time is lower - stages ran concurrently)\n";
    }
}

int main()
{
    // Same scale as pipeline_stages_benchmark.cpp's own stress_data() -
    // 1,000,000 shapes, 100,000 terminals / 900,000 obstruction shapes
    // across 4 Obstructions - so this profiler run is directly comparable
    // to that benchmark's own numbers, not a different (smaller) workload
    // that happens to also print a wall time.
    constexpr int kTerminalCount = 100'000;
    constexpr int kObstructionCount = 4;
    constexpr int kObstructionShapesTotal = 900'000;

    for (int layer_count : {2, 8})
    {
        StressData data = build_stress_data(layer_count, kTerminalCount, kObstructionCount, kObstructionShapesTotal);
        Scene scene = make_scene(data);

        tf::Executor executor;
        auto profiler = executor.make_observer<flow::StageProfiler>();

        flow::GenerateAndFilterPipeline pipeline;
        const auto start = std::chrono::steady_clock::now();
        const auto &result = pipeline.run(data.root, data.abstract_id, data.view_layers, scene, executor);
        const auto elapsed = std::chrono::steady_clock::now() - start;

        std::cout << "=== " << layer_count << " layers, " << (kTerminalCount + kObstructionShapesTotal) << " shapes in, "
                   << result.size() << " shapes out, " << executor.num_workers() << " workers ===\n";
        std::cout << "wall time: " << std::fixed << std::setprecision(2)
                   << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n\n";

        print_report(profiler->report());
        std::cout << "\n";
    }
}
