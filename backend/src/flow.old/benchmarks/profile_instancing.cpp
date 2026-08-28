#include "../../instancing/instance_renderer.hpp"
#include "../../io/def_reader.hpp"
#include "../../io/lef_reader.hpp"
#include "../flow.hpp"
#include "../instancing/parallel_layout_picture_builder.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

// Dev-only tool - true A/B (real, unmodified InstanceRenderer vs.
// flow::ParallelLayoutPictureBuilder) against a real design
// (test_data/ispd19_test10) - see TASKFLOW_EXPERIMENT.md. Parses the
// real LEF/DEF via the real io library, cached once per process
// (parse cost reported separately from render cost, same
// separation-of-concerns as every other flow_profile_*/
// flow_*_benchmarks tool).
using namespace le;

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: flow_profile_instancing <lef> <def>\n";
        return 1;
    }

    Root root;

    LEFReader lef_reader;
    const auto lef_start = std::chrono::steady_clock::now();
    const int lef_result = lef_reader.read_lef(argv[1], root, "ispd19");
    const auto lef_elapsed = std::chrono::steady_clock::now() - lef_start;
    std::cout << "LEF read (" << argv[1] << "): result=" << lef_result << ", "
               << std::chrono::duration<double>(lef_elapsed).count() << "s\n";
    if (lef_result != 0)
        return 1;

    DEFReader def_reader;
    const auto def_start = std::chrono::steady_clock::now();
    const int def_result = def_reader.read_def(argv[2], root, "ispd19");
    const auto def_elapsed = std::chrono::steady_clock::now() - def_start;
    std::cout << "DEF read (" << argv[2] << "): result=" << def_result << ", "
               << std::chrono::duration<double>(def_elapsed).count() << "s\n";
    if (def_result != 0)
        return 1;

    const DesignId design_id = root.get_design_by_name("ispd19_test10");
    if (!design_id.valid())
    {
        std::cerr << "top design 'ispd19_test10' not found after DEF read\n";
        return 1;
    }
    const LayoutId layout_id = root.get_design_layout(design_id);
    if (!layout_id.valid())
    {
        std::cerr << "top design has no Layout\n";
        return 1;
    }

    std::cout << "placements: " << root.get_layout_placements(layout_id).size() << "\n";
    const TechnologyId technology_id = root.get_technology_ids().front();
    std::cout << "technology layers: " << root.get_technology_layers(technology_id).size() << "\n";

    // ParallelGenerateLayoutShapesStage deliberately never generates
    // TRACKS (TASKFLOW_EXPERIMENT.md) - removing every real Track from
    // the Root here, before either renderer runs, makes the
    // InstanceRenderer baseline below directly comparable: both are then
    // measured against the exact same, track-free data, isolating the
    // real placement-resolve/draw speedup from the earlier (now-answered)
    // "is generate_own_shapes actually the bottleneck" question.
    const size_t track_count = root.get_track_ids().size();
    root.clear_track();
    std::cout << "tracks removed from Root before either renderer runs: " << track_count << "\n\n";

    const ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_id);
    Scene scene; // default - every layer visible; no pan/viewport needed, "local pixel space" convention (see ParallelLayoutPictureBuilder's own comment)
    constexpr double kScale = 0.001; // arbitrary but fixed - both builders must use the same scale for a valid A/B
    constexpr int kHierarchyDepth = 1; // ispd19_test10 is flat (leaf standard cells only) - one level is enough to exercise the real placement-drawing cost

    // min_visible_instance_pixels lowered to 1.0 (both builders, matching
    // knob) for this run - the default (100.0) means most of
    // ispd19_test10's own real standard-cell instances collapse to an
    // unfilled outline dot at kScale=0.001 rather than resolving/drawing
    // their own real Abstract content at all, which understates the real
    // per-placement resolve/draw cost this pipeline is meant to measure.

    // Baseline: the real, unmodified InstanceRenderer.
    InstanceRenderer real_renderer;
    real_renderer.set_min_visible_instance_pixels(1.0);
    const auto real_start = std::chrono::steady_clock::now();
    const sk_sp<SkPicture> real_picture = real_renderer.build_layout_picture(root, layout_id, kHierarchyDepth, view_layers, scene, kScale);
    const auto real_elapsed = std::chrono::steady_clock::now() - real_start;
    std::cout << "InstanceRenderer::build_layout_picture (real, serial): "
               << std::fixed << std::setprecision(2) << std::chrono::duration<double, std::milli>(real_elapsed).count() << " ms\n";

    // New: flow::ParallelLayoutPictureBuilder, swept across worker counts.
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned workers : {1u, 2u, 4u, 8u, hw})
    {
        if (workers != hw && workers >= hw)
            continue; // avoid a duplicate row when hw is small (e.g. 1, 2, 4, or 8)

        tf::Executor executor(workers);
        auto profiler = executor.make_observer<flow::StageProfiler>();
        flow::ParallelLayoutPictureBuilder parallel_builder;
        parallel_builder.set_min_visible_instance_pixels(1.0);
        const auto start = std::chrono::steady_clock::now();
        const sk_sp<SkPicture> parallel_picture = parallel_builder.run(root, layout_id, kHierarchyDepth, view_layers, scene, kScale, executor);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        std::cout << "ParallelLayoutPictureBuilder::run (workers=" << workers << "): "
                   << std::fixed << std::setprecision(2) << std::chrono::duration<double, std::milli>(elapsed).count() << " ms"
                   << (parallel_picture ? "" : "  [WARNING: null picture]") << "\n";
        for (const auto &[name, stats] : profiler->report())
            std::cout << "    " << std::left << std::setw(24) << name << std::right
                       << " calls=" << std::setw(6) << stats.calls
                       << "  total=" << std::setw(9) << std::fixed << std::setprecision(2) << std::chrono::duration<double, std::milli>(stats.total).count() << " ms\n";
    }

    return 0;
}
