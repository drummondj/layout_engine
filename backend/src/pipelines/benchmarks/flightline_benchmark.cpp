// Benchmarks for flightlines (NEW_FEATURES_SEPT_2026.md item 5,
// core/flightlines.hpp), on a synthetic linked netlist: P placements of one
// 4-pin cell, every pin on a net of 2-10 pins (so roughly P*4/6 nets) - the
// shape of a real gate-level design's signal nets.
//
//  - BM_Flightline_BuildIndex: NetEndpointIndex over the whole Layout -
//    what api.cpp's flightline cache pays once per Root mutation.
//  - BM_Flightline_Lines/S: placement_flightlines for S selected
//    placements against a prebuilt index - what a selection change pays.
//
// Justifies the cache's two levels: if the index were rebuilt per
// selection change, every click would pay BuildIndex too.

#include "../../core/flightlines.hpp"
#include "pipeline_benchmarks.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace le;

namespace
{
    struct Netlist
    {
        Root root;
        LayoutId layout;
        std::vector<PlacementId> placements;
    };

    std::unique_ptr<Netlist> make_netlist(int placement_count)
    {
        auto n = std::make_unique<Netlist>();
        Root &root = n->root;
        root.create_technology(TechnologyData{.database_units_microns = 1000.0});
        const LibraryId library = root.create_library(LibraryData{.name = "LIB"});

        const DesignId cell = root.create_design(DesignData{.library = library, .name = "CELL"});
        AbstractData abstract_data{};
        abstract_data.design = cell;
        abstract_data.size = Point{1000, 2000};
        const AbstractId abstract = root.create_abstract(abstract_data);
        const char *pin_names[] = {"A", "B", "C", "Y"};
        for (int i = 0; i < 4; ++i)
        {
            TerminalData terminal{};
            terminal.abstract = abstract;
            terminal.name = pin_names[i];
            ShapeData shape{};
            shape.terminal_port = root.create_terminal_port(TerminalPortData{.terminal = root.create_terminal(terminal)});
            shape.rects = {Rect{.ll = {100 + 200 * i, 100}, .ur = {200 + 200 * i, 400}}};
            root.create_shape(shape);
        }

        const DesignId top = root.create_design(DesignData{.library = library, .name = "TOP"});
        const SchematicId schematic = root.create_schematic(SchematicData{.design = top});
        n->layout = root.create_layout(LayoutData{.design = top});

        // Pins dealt round-robin onto nets of 2-10 pins, in shuffled order
        // so a net's pins are spread across the design.
        std::mt19937 rng(42);
        std::vector<std::pair<InstanceId, int>> pins;
        const int side = static_cast<int>(std::sqrt(placement_count)) + 1;
        for (int p = 0; p < placement_count; ++p)
        {
            InstanceData inst{};
            inst.schematic = schematic;
            inst.name = "U" + std::to_string(p);
            inst.reference_design = cell;
            const InstanceId instance = root.create_instance(inst);
            for (int i = 0; i < 4; ++i)
                pins.emplace_back(instance, i);
            n->placements.push_back(root.create_placement(PlacementData{
                .layout = n->layout, .name = inst.name, .reference_design = cell, .instance = instance,
                .placement_status = PlacementStatus::PLACED, .location = Point{(p % side) * 1200, (p / side) * 2000}, .orientation = Orientation::N}));
        }
        std::shuffle(pins.begin(), pins.end(), rng);
        std::uniform_int_distribution<int> net_size(2, 10);
        size_t next = 0;
        for (int net_index = 0; next < pins.size(); ++net_index)
        {
            NetData net_data{};
            net_data.schematic = schematic;
            net_data.name = "n" + std::to_string(net_index);
            const NetId net = root.create_net(net_data);
            for (int k = net_size(rng); k > 0 && next < pins.size(); --k, ++next)
            {
                PinData pin{};
                pin.instance = pins[next].first;
                pin.name = pin_names[pins[next].second];
                pin.net = net;
                root.create_pin(pin);
            }
        }
        return n;
    }

    void BM_Flightline_BuildIndex(benchmark::State &state)
    {
        const auto netlist = make_netlist(static_cast<int>(state.range(0)));
        for (auto _ : state)
        {
            NetEndpointIndex index(netlist->root, netlist->layout);
            benchmark::DoNotOptimize(index);
        }
    }

    void BM_Flightline_Lines(benchmark::State &state)
    {
        const auto netlist = make_netlist(static_cast<int>(state.range(0)));
        const NetEndpointIndex index(netlist->root, netlist->layout);
        const std::vector<PlacementId> selected(netlist->placements.begin(), netlist->placements.begin() + state.range(1));
        size_t lines = 0;
        for (auto _ : state)
        {
            const std::vector<Flightline> result = placement_flightlines(netlist->root, index, selected);
            lines = result.size();
            benchmark::DoNotOptimize(result);
        }
        state.counters["lines"] = static_cast<double>(lines);
    }
}

namespace le::benchmarks
{
    void register_flightline_benchmarks()
    {
        benchmark::RegisterBenchmark("BM_Flightline_BuildIndex", BM_Flightline_BuildIndex)->Arg(10'000)->Arg(100'000)->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark("BM_Flightline_Lines", BM_Flightline_Lines)->Args({100'000, 1})->Args({100'000, 100})->Args({100'000, 10'000})->Unit(benchmark::kMillisecond);
    }
}
