#include "../../api/api.hpp"
#include "../../api/le_handle.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Dev-only per-stage benchmarking tool (not a GoogleBenchmark target -
// see pipeline_benchmarks/BENCHMARKS.md for that suite): drives the real
// LeHandle/api.hpp surface (le_read_lef/le_read_def/le_fit_scene/le_zoom/
// le_render_pixel_buffer) through 3 phases - Cold Start Zoom-Fit, Zoom-In,
// Final Zoom-Fit - for exactly ONE (design, hierarchy_depth) combination
// per process invocation, printing one CSV row per (phase, stage) to
// stdout. backend/scripts/pipeline_stage_benchmark.py is the outer driver
// that invokes this once per test case and aggregates the results into a
// combined CSV + Markdown report.
//
// Recipe-driven rather than hardcoded to one fixture shape, since the
// flat aes_scaling_<label>.def tiles (DESIGN "tiled", no real hierarchy -
// hierarchy_depth is meaningless for them) and the genuinely
// hierarchical aes_5x5.def (DESIGN "aes_5x5", 25 placements of DESIGN
// "aes" from AES_1/design_original.def - see that file's own header
// comment for the required load order) need different --def sequences
// and a different --top-design to select:
//
//   ./pipeline_stage_benchmark --lef <tech.lef> --def <design.def> [--def <design2.def> ...]
//       --top-design <name> --label <csv-label> --hierarchy-depth <N>
//       [--viewport-px <W> <H>] [--zoom-factor <F>] [--fit-padding-px <P>]
//
// Example (flat scaling tile):
//   ./pipeline_stage_benchmark --lef test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef \
//       --def test_data/aes_scaling_3x2.def --top-design tiled --label 3x2 --hierarchy-depth 0
//
// Example (hierarchical 5x5, depth 1):
//   ./pipeline_stage_benchmark --lef test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef \
//       --def test_data/ISPD22__final_benchmarks/AES_1/design_original.def --def test_data/aes_5x5.def \
//       --top-design aes_5x5 --label "5x5 (d=1)" --hierarchy-depth 1
//
// Release build strongly recommended (see generate_tiled_design.cpp's own
// note) - a Debug build's numbers aren't meaningful.

using namespace le;

namespace
{
    struct Args
    {
        std::string lef_path;
        std::vector<std::string> def_paths;
        std::string top_design;
        std::string label;
        int32_t hierarchy_depth = 0;
        int32_t viewport_width_px = 1000;
        int32_t viewport_height_px = 1000;
        double zoom_factor = 2.0; // new scale = fit scale * zoom_factor - see the Zoom-In phase's own comment for why this isn't le_zoom's own `factor` parameter directly
        int32_t fit_padding_px = 20;
    };

    [[noreturn]] void usage_error(const char *message)
    {
        std::fprintf(stderr, "pipeline_stage_benchmark: %s\n", message);
        std::exit(2);
    }

    Args parse_args(int argc, char **argv)
    {
        Args args;
        bool has_top_design = false;
        bool has_label = false;
        bool has_hierarchy_depth = false;

        for (int i = 1; i < argc; ++i)
        {
            const std::string flag = argv[i];
            auto next = [&](const char *flag_name) -> std::string
            {
                if (i + 1 >= argc)
                {
                    std::fprintf(stderr, "pipeline_stage_benchmark: %s requires a value\n", flag_name);
                    std::exit(2);
                }
                return argv[++i];
            };

            if (flag == "--lef")
                args.lef_path = next("--lef");
            else if (flag == "--def")
                args.def_paths.push_back(next("--def"));
            else if (flag == "--top-design")
            {
                args.top_design = next("--top-design");
                has_top_design = true;
            }
            else if (flag == "--label")
            {
                args.label = next("--label");
                has_label = true;
            }
            else if (flag == "--hierarchy-depth")
            {
                args.hierarchy_depth = std::atoi(next("--hierarchy-depth").c_str());
                has_hierarchy_depth = true;
            }
            else if (flag == "--viewport-px")
            {
                args.viewport_width_px = std::atoi(next("--viewport-px width").c_str());
                args.viewport_height_px = std::atoi(next("--viewport-px height").c_str());
            }
            else if (flag == "--zoom-factor")
                args.zoom_factor = std::atof(next("--zoom-factor").c_str());
            else if (flag == "--fit-padding-px")
                args.fit_padding_px = std::atoi(next("--fit-padding-px").c_str());
            else
                usage_error(("unknown flag " + flag).c_str());
        }

        if (args.lef_path.empty())
            usage_error("--lef is required");
        if (args.def_paths.empty())
            usage_error("at least one --def is required");
        if (!has_top_design)
            usage_error("--top-design is required");
        if (!has_label)
            usage_error("--label is required");
        if (!has_hierarchy_depth)
            usage_error("--hierarchy-depth is required");

        return args;
    }

    DesignId find_design_by_name(const Root &root, const std::string &name)
    {
        for (const DesignId id : root.get_design_ids())
        {
            const DesignData *design = root.get_design(id);
            if (design && design->name == name)
                return id;
        }
        return DesignId{};
    }

    LeDesignId to_c(DesignId id) { return LeDesignId{.index = id.index, .generation = id.generation}; }

    /// @brief Prints one CSV row for one stage's own current
    /// instrumentation (tbb_core.hpp) - a template so it works uniformly
    /// across the 5 stage classes despite their different InputData/
    /// OutputData types (MemoizingStage's own public accessors are all
    /// non-template, so this only needs to be generic over *which*
    /// concrete stage type is passed in).
    template <typename Stage>
    void print_stage_row(const std::string &label, int32_t hierarchy_depth, const char *phase, const char *stage_name,
                          const Stage &stage)
    {
        const double wall_ms = static_cast<double>(stage.last_compute_wall_ns()) / 1'000'000.0;
        const double cpu_ms = static_cast<double>(stage.last_compute_cpu_ns()) / 1'000'000.0;
        const double rss_after_mb = static_cast<double>(stage.last_compute_rss_after_kb()) / 1024.0;
        const double rss_delta_mb =
            static_cast<double>(stage.last_compute_rss_after_kb() - stage.last_compute_rss_before_kb()) / 1024.0;
        const double swap_after_mb = static_cast<double>(stage.last_compute_swap_after_kb()) / 1024.0;
        const double swap_delta_mb =
            static_cast<double>(stage.last_compute_swap_after_kb() - stage.last_compute_swap_before_kb()) / 1024.0;

        std::printf("%s,%d,%s,%s,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%zu,%zu\n", label.c_str(), hierarchy_depth, phase,
                    stage_name, stage.last_call_recomputed() ? 1 : 0, wall_ms, cpu_ms, rss_after_mb, rss_delta_mb,
                    swap_after_mb, swap_delta_mb, stage.cache_object_count(), stage.cache_bytes());
    }

    void print_phase_rows(const std::string &label, int32_t hierarchy_depth, const char *phase,
                           const ViewRenderPipeline &pipeline)
    {
        print_stage_row(label, hierarchy_depth, phase, "LayerGeneration", pipeline.layer_generation_stage());
        print_stage_row(label, hierarchy_depth, phase, "HierarchyResolver", pipeline.hierarchy_resolver_stage());
        print_stage_row(label, hierarchy_depth, phase, "ViewportCull", pipeline.viewport_cull_stage());
        print_stage_row(label, hierarchy_depth, phase, "Rasterize", pipeline.rasterize_stage());
        print_stage_row(label, hierarchy_depth, phase, "Compose", pipeline.compose_stage());
    }
}

int main(int argc, char **argv)
{
    const Args args = parse_args(argc, argv);

    LeHandle *handle = le_create();

    if (le_read_lef(handle, args.lef_path.c_str(), "test_lib") != 0)
    {
        std::fprintf(stderr, "pipeline_stage_benchmark: failed to read LEF '%s' - see spdlog output above for details\n", args.lef_path.c_str());
        return 1;
    }

    for (const std::string &def_path : args.def_paths)
    {
        if (le_read_def(handle, def_path.c_str(), "test_lib") != 0)
        {
            std::fprintf(stderr, "pipeline_stage_benchmark: failed to read DEF '%s' - see spdlog output above for details\n", def_path.c_str());
            return 1;
        }
    }

    const DesignId top_design = find_design_by_name(handle->root, args.top_design);
    if (!top_design.valid())
    {
        std::fprintf(stderr, "pipeline_stage_benchmark: no Design named '%s' found after reading --def input(s)\n",
                     args.top_design.c_str());
        return 1;
    }

    if (le_set_current_design_layout_by_id(handle, to_c(top_design)) != 0)
    {
        std::fprintf(stderr, "pipeline_stage_benchmark: Design '%s' has no Layout\n", args.top_design.c_str());
        return 1;
    }

    le_set_hierarchy_depth(handle, args.hierarchy_depth);
    le_set_viewport_size(handle, args.viewport_width_px, args.viewport_height_px);

    // Phase 1: Cold Start Zoom-Fit - the true first render, every stage
    // recomputes (nothing cached yet).
    le_fit_scene(handle, args.fit_padding_px);
    le_render_pixel_buffer(handle);
    print_phase_rows(args.label, args.hierarchy_depth, "ColdStartZoomFit", handle->view_render_pipeline);

    // Phase 2: Zoom-In - anchored at the viewport center; root/top_level/
    // hierarchy_depth are unchanged, so LayerGeneration/HierarchyResolver
    // should cache-hit, only the Warm-tier stages (ViewportCull/
    // Rasterize/Compose) recompute (viewport/scale changed).
    //
    // le_zoom's own `factor` isn't a scale multiplier - its doc comment
    // (api.hpp) defines new_scale = scale * (1 + factor), so passing
    // args.zoom_factor directly would make --zoom-factor 2.0 actually
    // zoom to 3x, not 2x. Subtracting 1 here makes --zoom-factor mean
    // what it says: new_scale = scale * (1 + (zoom_factor - 1)) = scale * zoom_factor.
    le_zoom(handle, args.zoom_factor - 1.0, args.viewport_width_px / 2, args.viewport_height_px / 2);
    le_render_pixel_buffer(handle);
    print_phase_rows(args.label, args.hierarchy_depth, "ZoomIn", handle->view_render_pipeline);

    // Phase 3: Final Zoom-Fit - same expected cache-hit/miss split as
    // phase 2, from the opposite direction (scale/pan reset via fit).
    le_fit_scene(handle, args.fit_padding_px);
    le_render_pixel_buffer(handle);
    print_phase_rows(args.label, args.hierarchy_depth, "FinalZoomFit", handle->view_render_pipeline);

    le_destroy(handle);
    return 0;
}
