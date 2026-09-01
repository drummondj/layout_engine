#pragma once
#include "../../geometry/geometry.hpp"
#include "../../io/def_reader.hpp"
#include "../../io/lef_reader.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include <chrono>
#include <iostream>
#include <string>

// BUGS_AND_ENHANCEMENTS.md E13's own benchmark comparison (RenderLayoutFrame
// tracy trace against a real design) found layout_stress_data.hpp's own
// fixture (a synthetic 4,000,000-placement Layout, deliberately trivial
// per-leaf geometry) measures a genuinely different axis - hierarchy-node-
// count scaling - than what a real design's cold-render cost is actually
// dominated by: real per-shape geometry generation/SkPicture recording for
// a modest number of *dense* instances. This fixture is that missing
// counterpart: the real ISPD22 AES benchmark's NANGATE tech/cell LEF, the
// real "aes" Layout it references (design_original.def), and a real 5x5
// placement of 25 "aes" instances on top of that (aes_5x5.def) - the exact
// files used, by hand, to investigate that tracy trace.
namespace
{
    struct RealDesignData
    {
        le::Root root;
        le::ViewLayerSet view_layers;
        le::LayoutId layout_id;
    };

    const RealDesignData &real_design_data()
    {
        static const RealDesignData data = [] {
            RealDesignData d;

            const auto t0 = std::chrono::steady_clock::now();

            le::LEFReader lef_reader;
            if (lef_reader.read_lef(std::string(REAL_DESIGN_TEST_DATA_DIR) + "/ISPD22__final_benchmarks/AES_1/NangateOpenCellLibrary.lef", d.root, "NANGATE") != 0)
            {
                std::cerr << "Failed to read NangateOpenCellLibrary.lef\n";
                std::exit(1);
            }

            // aes_5x5.def places 25 instances of the "aes" Design itself (a
            // real Layout, defined by design_original.def - not a LEF
            // macro) - that has to be read first so aes_5x5.def's own
            // COMPONENTs can resolve it.
            le::DEFReader aes_def_reader;
            if (aes_def_reader.read_def(std::string(REAL_DESIGN_TEST_DATA_DIR) + "/ISPD22__final_benchmarks/AES_1/design_original.def", d.root, "AES1") != 0)
            {
                std::cerr << "Failed to read design_original.def\n";
                std::exit(1);
            }

            le::DEFReader def_reader;
            if (def_reader.read_def(std::string(REAL_DESIGN_TEST_DATA_DIR) + "/aes_5x5.def", d.root, "AES5X5") != 0)
            {
                std::cerr << "Failed to read aes_5x5.def\n";
                std::exit(1);
            }

            d.view_layers = le::ViewLayerSet::build_for_technology(d.root, d.root.get_technology_ids().front());

            const le::DesignId design_id = d.root.get_design_by_name("aes_5x5");
            d.layout_id = d.root.get_design_layout(design_id);

            const auto t1 = std::chrono::steady_clock::now();
            std::cerr << "[setup] read real aes_5x5 design (25 real AES macro instances) in "
                      << std::chrono::duration<double>(t1 - t0).count() << "s\n";

            return d;
        }();
        return data;
    }

    // hierarchy_depth=2 is the minimum depth at which aes_5x5.def shows any
    // content at all (depth 0 and 1 both fall back to "aes"'s own empty
    // Abstract for every one of its 25 top-level placements - "aes" is a
    // real Layout with no Abstract fallback) - found while investigating
    // E13's own tracy trace.
    le::Scene make_real_design_scene(const RealDesignData &data, int hierarchy_depth)
    {
        le::Scene scene;
        scene.set_current_layout(data.layout_id);
        scene.set_hierarchy_depth(hierarchy_depth);
        scene.set_viewport_size(1600, 1600);

        const le::Shape *diearea = data.root.get_shape(data.root.get_layout_diearea(data.layout_id));
        const auto bbox = le::Geometry::bbox(*diearea);
        scene.fit_to_content(bbox, 20);

        return scene;
    }
}
