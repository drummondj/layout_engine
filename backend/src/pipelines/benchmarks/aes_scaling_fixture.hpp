#pragma once

#include "../../database/database.hpp"
#include "../../io/def_reader.hpp"
#include "../../io/lef_reader.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

// Shared benchmark fixture (PIPELINE_REFACTOR.md's own "Benchmarking"
// section): every pipelines-module stage/pipeline benchmark measures the
// same 5 points - 1x1, 2x1, 2x2, 3x2, 3x3 tiles of the AES_1 ISPD22 design
// on Nangate45 - so scaling behavior is comparable stage to stage. Each
// including .cpp gets this loaded fresh (a plain header, not a shared
// static) since a benchmark file's own Root is never shared with another
// TU's.

namespace le::benchmarks
{
    struct TileConfig
    {
        int tile_x;
        int tile_y;
        const char *label;
    };

    /// @brief The 5 tiling points PIPELINE_REFACTOR.md asks every benchmark
    /// to report - test_data/aes_scaling_<label>.def, generated from
    /// ISPD22__final_benchmarks/AES_1/design_original.def via
    /// generate_tiled_design (see that tool's own comment).
    inline constexpr std::array<TileConfig, 5> kAesScalingTileConfigs = {{
        {1, 1, "1x1"},
        {2, 1, "2x1"},
        {2, 2, "2x2"},
        {3, 2, "3x2"},
        {3, 3, "3x3"},
    }};

    struct AesScalingFixture
    {
        Root root;
        LayoutId layout_id;
    };

    /// @brief Reads the shared Nangate45 tech+macro LEF plus
    /// test_data/aes_scaling_<config.label>.def into a fresh Root.
    /// REAL_DESIGN_TEST_DATA_DIR is injected by CMake (pipeline_benchmarks'
    /// own target_compile_definitions) - the repo's top-level test_data/
    /// directory, sibling to backend/. Aborts the process on any read
    /// failure (a benchmark can't produce meaningful numbers over partial
    /// data, and every config here is a committed, known-good fixture -
    /// a failure here means the environment itself is broken, not a
    /// recoverable per-iteration condition).
    inline AesScalingFixture load_aes_scaling_fixture(const TileConfig &config)
    {
        AesScalingFixture fixture;

        const std::string tech_lef_path =
            std::string(REAL_DESIGN_TEST_DATA_DIR) + "/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef";
        const std::string def_path =
            std::string(REAL_DESIGN_TEST_DATA_DIR) + "/aes_scaling_" + config.label + ".def";

        LEFReader lef_reader;
        if (lef_reader.read_lef(tech_lef_path, fixture.root, "tech") != 0)
        {
            fprintf(stderr, "aes_scaling_fixture: failed to read tech LEF '%s'\n", tech_lef_path.c_str());
            std::abort();
        }

        DEFReader def_reader;
        if (def_reader.read_def(def_path, fixture.root, "aes_scaling") != 0)
        {
            fprintf(stderr, "aes_scaling_fixture: failed to read DEF '%s'\n", def_path.c_str());
            for (const std::string &message : def_reader.messages())
                fprintf(stderr, "  %s\n", message.c_str());
            std::abort();
        }

        for (const DesignId design_id : fixture.root.get_design_ids())
        {
            const LayoutId layout_id = fixture.root.get_design_layout(design_id);
            if (layout_id.valid())
            {
                fixture.layout_id = layout_id;
                break;
            }
        }

        return fixture;
    }
}
