// selection_profile - dev-only tool: how long a Select-mode click takes on
// one real design, through the real C API (le_mouse_down/le_mouse_up, the
// same path the GUI uses). One design per process:
//
//   selection_profile <aes_scaling label, e.g. 4x4> [repeats=5]
//
// Loads NangateOpenCellLibrary.lef + test_data/aes_scaling_<label>.def,
// opens its Layout (hierarchy depth 1) in a 1280x800 viewport, renders it
// once (as the GUI does before anyone can click - clicks read the render
// tree), and reports the first click's time (ms) and the median of the
// clicks after it, at the view center, zoomed to fit and zoomed in 20x,
// plus the render time. Not run by ctest.

#include "../../api/api.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    double peak_rss_mb()
    {
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        return static_cast<double>(usage.ru_maxrss) / 1024.0; // KB on Linux
    }

    double median(std::vector<double> values)
    {
        std::ranges::sort(values);
        return values[values.size() / 2];
    }

    double time_clicks(LeHandle *handle, int x, int y, int repeats, int &selected)
    {
        std::vector<double> times;
        for (int r = 0; r < repeats; ++r)
        {
            const auto start = std::chrono::steady_clock::now();
            le_mouse_down(handle, x, y);
            le_mouse_up(handle, x, y);
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        selected = le_selection_count(handle);
        return median(times);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <aes_scaling label, e.g. 4x4> [repeats=5]\n", argv[0]);
        return 2;
    }
    const std::string label = argv[1];
    const int repeats = argc > 2 ? std::max(1, std::atoi(argv[2])) : 5;
    const std::string data = REAL_DESIGN_TEST_DATA_DIR;

    LeHandle *handle = le_create();
    if (le_read_lef(handle, (data + "/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef").c_str(), "tech") != 0 ||
        le_read_def(handle, (data + "/aes_scaling_" + label + ".def").c_str(), "aes_scaling") != 0)
    {
        std::fprintf(stderr, "failed to read design\n");
        return 1;
    }
    // The DEF's design is the last one read.
    le_set_current_design_layout(handle, le_design_count(handle) - 1);
    le_set_hierarchy_depth(handle, 1);
    le_set_viewport_size(handle, 1280, 800);
    le_fit_scene(handle, 0);

    std::printf("design aes_scaling_%s\n", label.c_str());
    const auto render_start = std::chrono::steady_clock::now();
    le_render_pixel_buffer(handle);
    std::printf("first_render_ms %.3f\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - render_start).count());
    int selected = 0;
    std::printf("rss_before_first_click_mb %.1f\n", peak_rss_mb());
    std::printf("first_click_ms %.3f\n", time_clicks(handle, 640, 400, 1, selected));
    std::printf("rss_after_first_click_mb %.1f\n", peak_rss_mb());
    std::printf("fit.click_ms %.3f\n", time_clicks(handle, 640, 400, repeats, selected));
    std::printf("fit.selected %d\n", selected);
    le_zoom(handle, 20.0, 640, 400);
    le_render_pixel_buffer(handle);
    std::printf("zoom.click_ms %.3f\n", time_clicks(handle, 640, 400, repeats, selected));
    std::printf("zoom.selected %d\n", selected);
    le_destroy(handle);
    return 0;
}
