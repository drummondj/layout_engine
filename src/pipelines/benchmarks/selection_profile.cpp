// selection_profile - dev-only tool: how long a Select-mode click takes on
// one real design, through the C++ API's mouse_down/mouse_up (the same
// path the GUI uses). One design per process:
//
//   selection_profile <aes_scaling label, e.g. 4x4> [repeats=5]
//
// Loads NangateOpenCellLibrary.lef + test_data/aes_scaling_<label>.def,
// opens its Layout (hierarchy depth 1) in a 1280x800 viewport, renders it
// once (as the GUI does before anyone can click - clicks read the render
// tree), and reports the first click's time (ms) and the median of the
// clicks after it, at the view center, zoomed to fit and zoomed in 20x,
// plus the render time. Not run by ctest.

#include "../../api/views.hpp"

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

    // Down and up are separate events, each its own write, as in the GUI.
    double time_clicks(le::Session &session, int x, int y, int repeats, size_t &selected)
    {
        std::vector<double> times;
        for (int r = 0; r < repeats; ++r)
        {
            const auto start = std::chrono::steady_clock::now();
            session.write().mouse_down(x, y);
            session.write().mouse_up(x, y);
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        selected = session.read().selection().size();
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

    le::Session session;
    {
        le::WriteView view = session.write();
        if (!view.read_lef(data + "/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef", "tech") ||
            !view.read_def(data + "/aes_scaling_" + label + ".def", "aes_scaling"))
        {
            std::fprintf(stderr, "failed to read design\n");
            return 1;
        }
        // The DEF's design is the last one read.
        if (!view.open_layout(view.root().get_design_ids().back()))
        {
            std::fprintf(stderr, "the design has no Layout\n");
            return 1;
        }
        view.set_hierarchy_depth(1);
        view.set_viewport_size(1280, 800);
        view.fit_view(0);
    }
    const le::Renderer renderer = session.renderer();

    std::printf("design aes_scaling_%s\n", label.c_str());
    const auto render_start = std::chrono::steady_clock::now();
    renderer.render();
    std::printf("first_render_ms %.3f\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - render_start).count());
    size_t selected = 0;
    std::printf("rss_before_first_click_mb %.1f\n", peak_rss_mb());
    std::printf("first_click_ms %.3f\n", time_clicks(session, 640, 400, 1, selected));
    std::printf("rss_after_first_click_mb %.1f\n", peak_rss_mb());
    std::printf("fit.click_ms %.3f\n", time_clicks(session, 640, 400, repeats, selected));
    std::printf("fit.selected %zu\n", selected);
    session.write().zoom(20.0, 640, 400);
    renderer.render();
    std::printf("zoom.click_ms %.3f\n", time_clicks(session, 640, 400, repeats, selected));
    std::printf("zoom.selected %zu\n", selected);
    return 0;
}
