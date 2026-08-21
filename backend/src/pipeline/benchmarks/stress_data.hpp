#pragma once
#include "../../io/lef_reader.hpp"
#include "../pipeline.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// Generates a deliberately unrealistic single-macro LEF file with
// kTotalShapes individual Shape entries (a fresh "LAYER ;" before every
// geometry item forces shapes_from_parser to finalize a separate Shape each
// time - see pipeline.hpp's comment on Shape granularity), split across
// PINs (10%) and one OBS block (90%), alternating M1/M2 and RECT/POLYGON/
// PATH, with position and size spread across a wide range so the pipeline's
// viewport, sub-pixel-size, and layer filters all have real work to do -
// not a realistic design, a stress test.
//
// Shared between pipeline_benchmark.cpp and render_preview.cpp (a
// visualization dev tool, not a benchmark itself) so both work against the
// exact same data/Scene setup - the point of render_preview is to let you
// see what's actually being benchmarked, not a separately-tuned scene.
// Anonymous namespace, matching how this lived directly in
// pipeline_benchmark.cpp before being factored out: each including .cpp
// gets its own private copy (each is a separate executable, never linked
// together, so that's harmless) rather than needing `inline` annotations
// throughout.
namespace
{
    constexpr int kTotalShapes = 1'000'000;

    // Deterministic per-item spread: 1000 columns x 1000 rows, size ramping
    // 0..~100um so roughly half the shapes land under any mid-range
    // sub-pixel threshold, alternating M1/M2 and RECT/POLYGON/PATH.
    struct ItemGeometry
    {
        double x_um;
        double y_um;
        double size_um;
        const char *layer;
    };

    ItemGeometry item_geometry(int i)
    {
        return ItemGeometry{
            .x_um = (i % 1000) * 200.0,
            .y_um = (i / 1000) * 200.0,
            .size_um = 0.001 + (i % 1000) * 0.1,
            .layer = (i % 2 == 0) ? "M1" : "M2",
        };
    }

    void write_geometry_item(std::ofstream &out, int i)
    {
        const ItemGeometry g = item_geometry(i);
        out << "      LAYER " << g.layer << " ;\n";
        switch (i % 3)
        {
        case 0: // RECT
            out << "      RECT " << g.x_um << " " << g.y_um << " "
                << (g.x_um + g.size_um) << " " << (g.y_um + g.size_um) << " ;\n";
            break;
        case 1: // POLYGON (axis-aligned quadrilateral - just exercises the POLYGON path distinctly from RECT)
            out << "      POLYGON "
                << g.x_um << " " << g.y_um << " "
                << (g.x_um + g.size_um) << " " << g.y_um << " "
                << (g.x_um + g.size_um) << " " << (g.y_um + g.size_um) << " "
                << g.x_um << " " << (g.y_um + g.size_um) << " ;\n";
            break;
        case 2: // PATH
            out << "      WIDTH " << g.size_um << " ;\n";
            out << "      PATH " << g.x_um << " " << g.y_um << " "
                << (g.x_um + g.size_um) << " " << g.y_um << " ;\n";
            break;
        }
    }

    std::string generate_stress_lef(int total_shapes)
    {
        std::filesystem::create_directories(BENCHMARK_DATA_DIR);
        std::string path = std::string(BENCHMARK_DATA_DIR) + "/stress.lef";

        std::ofstream out(path, std::ios::trunc);
        out << "VERSION 5.8 ;\n";
        out << "BUSBITCHARS \"<>\" ;\n";
        out << "DIVIDERCHAR \"/\" ;\n\n";
        out << "UNITS\n   DATABASE MICRONS 1000 ;\nEND UNITS\n\n";
        out << "LAYER M1\n   TYPE ROUTING ;\n   WIDTH 1 ;\n   PITCH 2 ;\n   DIRECTION HORIZONTAL ;\nEND M1\n\n";
        out << "LAYER M2\n   TYPE ROUTING ;\n   WIDTH 1 ;\n   PITCH 2 ;\n   DIRECTION VERTICAL ;\nEND M2\n\n";

        out << "MACRO STRESSTEST\n   CLASS CORE ;\n   SIZE 250000 BY 250000 ;\n\n";

        const int pin_shapes = total_shapes / 10;
        const int obs_shapes = total_shapes - pin_shapes;

        for (int i = 0; i < pin_shapes; ++i)
        {
            out << "   PIN P" << i << "\n      DIRECTION INPUT ;\n      PORT\n";
            write_geometry_item(out, i);
            out << "      END\n   END P" << i << "\n";
        }

        out << "   OBS\n";
        for (int i = 0; i < obs_shapes; ++i)
            write_geometry_item(out, pin_shapes + i);
        out << "   END\n";

        out << "END STRESSTEST\n";
        return path;
    }

    struct StressData
    {
        le::Root root;
        le::AbstractId abstract_id;
        le::ViewLayerSet view_layers;
    };

    const StressData &stress_data()
    {
        static const StressData data = [] {
            StressData d;

            auto t0 = std::chrono::steady_clock::now();
            std::string path = generate_stress_lef(kTotalShapes);
            auto t1 = std::chrono::steady_clock::now();

            le::LEFReader reader;
            int result = reader.read_lef(path, d.root, "stress_lib");
            auto t2 = std::chrono::steady_clock::now();

            if (result != 0)
            {
                std::cerr << "Failed to parse generated stress LEF (result=" << result << ")\n";
                std::exit(1);
            }

            le::DesignId design_id = d.root.get_design_by_name("STRESSTEST");
            d.abstract_id = d.root.get_design_abstract(design_id);
            d.view_layers = le::ViewLayerSet::build_for_technology(d.root, d.root.get_technology_ids().front());

            std::cerr << "[setup] generated " << kTotalShapes << "-shape LEF in "
                      << std::chrono::duration<double>(t1 - t0).count() << "s, parsed in "
                      << std::chrono::duration<double>(t2 - t1).count() << "s\n";

            return d;
        }();
        return data;
    }

    // scale chosen so the ~0.001..100um size spread straddles the 1px
    // threshold; viewport chosen to cover a quarter of the position grid -
    // both filters end up doing real, non-trivial culling rather than an
    // all-or-nothing pass.
    le::Scene make_scene(const StressData &data)
    {
        le::Scene scene;
        scene.set_current_abstract(data.abstract_id);
        scene.set_pan(le::Point{0, 0});
        scene.set_scale(0.00002); // 1px == 50,000 dbu == 50um
        scene.set_viewport_size(2000, 2000); // visible: [0, 100,000,000) dbu on each axis

        // M2 shapes appear as both PINs and OBS items in the generated LEF
        // (item_geometry alternates M1/M2 regardless of which), so hide the
        // whole M2 layer name - covers both purposes in one call.
        scene.set_layer_name_visible("M2", false);

        return scene;
    }
}
