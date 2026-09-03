#include "../../geometry/geometry.hpp"
#include "../../io/def_reader.hpp"
#include "../../io/lef_reader.hpp"
#include "../hierarchy_resolver.hpp"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <tracy/Tracy.hpp>

using namespace le;

namespace
{
    using Clock = std::chrono::steady_clock;
    double elapsed_ms(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
}

// Dev-only profiling driver, not part of the render pipeline itself
// (BUGS_AND_ENHANCEMENTS.md E31): loads a real LEF+DEF Layout (meant for
// generate_tiled_design's own multi-million-placement output, but works
// against any Layout-bearing DEF) at a given pixel resolution, zoom-fits
// it (Layout-view fit-to-content - the DIEAREA bbox, same convention
// api.cpp's own fit_scene_unlocked uses for a Layout view), and runs
// HierarchyResolver::render_layout_frame - the exact production code
// path le_render_pixel_buffer calls for a real Layout-view render -
// several times (a cold-cache pass, then warm-cache repeats) so a Tracy
// capture connected while this runs sees both a real cold render and the
// warm-cache no-op case, matching BENCHMARKS.md's own Cold/WarmCache
// pairing convention. No GUI/window involved at all - this is a
// headless CLI driver specifically so it can run under `tracy-capture`
// (Tracy's own CLI capture tool, matching the vendored TracyClient
// version - v0.13.1 as of this writing, see CMakeLists.txt's own tracy
// FetchContent block) without needing an interactive Tracy Profiler GUI
// session or a real on-screen window at all.
//
//   ./profile_tiled_render <tech.lef> <design.def> <width_px> <height_px> [output.png]
//
// Usage (Release build strongly recommended - Debug timings/Tracy zones
// aren't representative of real performance):
//   cmake --build build_release --target profile_tiled_render
//   tracy-capture -o out.tracy -a 127.0.0.1 -p 8086 -f &
//   ./build_release/profile_tiled_render \
//     test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef \
//     test_data/aes_4M.def 1920 1080 preview/aes_4M.png
// (start tracy-capture a moment before running this, so it's already
// listening when the client connects - it exits and writes out.tracy on
// its own once the client disconnects, i.e. once this program exits)
int main(int argc, char **argv)
{
    if (argc != 5 && argc != 6)
    {
        fprintf(stderr, "Usage: %s <tech.lef> <design.def> <width_px> <height_px> [output.png]\n", argv[0]);
        return 1;
    }

    const std::string lef_path = argv[1];
    const std::string def_path = argv[2];
    const int width_px = std::atoi(argv[3]);
    const int height_px = std::atoi(argv[4]);
    const std::string png_path = argc == 6 ? argv[5] : std::string{};

    Root root;

    printf("Reading LEF '%s'...\n", lef_path.c_str());
    auto t0 = Clock::now();
    LEFReader lef_reader;
    if (lef_reader.read_lef(lef_path, root, "tech") != 0)
    {
        fprintf(stderr, "Failed to read LEF '%s'\n", lef_path.c_str());
        return 1;
    }
    const double lef_read_ms = elapsed_ms(t0);

    printf("Reading DEF '%s'...\n", def_path.c_str());
    t0 = Clock::now();
    DEFReader def_reader;
    if (def_reader.read_def(def_path, root, "design_lib") != 0)
    {
        fprintf(stderr, "Failed to read DEF '%s'\n", def_path.c_str());
        for (const std::string &m : def_reader.messages())
            fprintf(stderr, "  %s\n", m.c_str());
        return 1;
    }
    const double def_read_ms = elapsed_ms(t0);

    LayoutId layout_id;
    for (const DesignId design_id : root.get_design_ids())
    {
        const LayoutId id = root.get_design_layout(design_id);
        if (id.valid())
        {
            layout_id = id;
            break;
        }
    }
    if (!layout_id.valid())
    {
        fprintf(stderr, "No Design with a Layout found in '%s'\n", def_path.c_str());
        return 1;
    }

    const auto technology_ids = root.get_technology_ids();
    if (technology_ids.empty())
    {
        fprintf(stderr, "No Technology declared\n");
        return 1;
    }
    const ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_ids.front());

    Scene scene;
    scene.set_current_layout(layout_id);
    scene.set_viewport_size(width_px, height_px);
    scene.set_hierarchy_depth(1);

    const ShapeId diearea_id = root.get_layout_diearea(layout_id);
    const ShapeData *diearea = root.get_shape(diearea_id);
    scene.fit_to_content(diearea ? Geometry::bbox(*diearea) : std::nullopt, 20);

    printf("Placements: %zu, Routes: %zu, viewport %dx%d, scale %.9g\n",
           root.get_layout_placements(layout_id).size(), root.get_layout_routes(layout_id).size(),
           width_px, height_px, scene.scale());

    HierarchyResolver resolver;

    printf("Cold-cache render...\n");
    double cold_render_ms = 0.0;
    const PixelBuffer *last_buffer = nullptr;
    {
        ZoneScopedN("profile_tiled_render: cold render");
        t0 = Clock::now();
        last_buffer = &resolver.render_layout_frame(root, layout_id, scene.hierarchy_depth(), view_layers, scene);
        cold_render_ms = elapsed_ms(t0);
        printf("Cold render done (%dx%d buffer) in %.1f ms\n", last_buffer->width, last_buffer->height, cold_render_ms);
    }

    constexpr int kWarmRenders = 3;
    double warm_render_ms_total = 0.0;
    for (int i = 0; i < kWarmRenders; i++)
    {
        ZoneScopedN("profile_tiled_render: warm render");
        t0 = Clock::now();
        last_buffer = &resolver.render_layout_frame(root, layout_id, scene.hierarchy_depth(), view_layers, scene);
        const double ms = elapsed_ms(t0);
        warm_render_ms_total += ms;
        printf("Warm-cache render %d done in %.1f ms\n", i + 1, ms);
    }
    const double warm_render_ms_avg = warm_render_ms_total / kWarmRenders;

    // Zoom-tick timings: the case the warm-cache loop above does NOT
    // cover at all - a real interactive zoom changes scene.scale() and
    // nothing else. Scene::set_scale bumps viewport_version_
    // (scene.hpp), which IS part of render_layout_frame's own top-level
    // picture cache key (hierarchy_resolver.hpp's top_picture_key), so
    // this is never a warm-cache hit - but ensure_epoch's own
    // kScaleDriftTolerance (hierarchy_resolver.hpp, +/-25%) keeps the
    // deeper per-node hierarchy graph alive across a small scale nudge,
    // only discarding/rebuilding it (paying the full cold cost again)
    // once cumulative drift since the last rebuild exceeds that
    // tolerance. kZoomTickFactor mirrors layout_engine_input.dart's own
    // kScrollZoomFactor (10% per real scroll-wheel tick) - five
    // consecutive ticks compounds to 1.1^5 = 1.61x, crossing the 1.25x
    // tolerance boundary partway through, so this sequence is expected
    // to show at least one expensive (rebuild) tick among mostly-cheap
    // ones, not a flat cost.
    constexpr double kZoomTickFactor = 1.10;
    constexpr int kZoomTicks = 5;
    double zoom_tick_ms[kZoomTicks] = {};
    printf("Zoom-tick renders (%.0f%% scale change per tick, nothing else in the scene changed)...\n", (kZoomTickFactor - 1.0) * 100.0);
    for (int i = 0; i < kZoomTicks; i++)
    {
        scene.set_scale(scene.scale() * kZoomTickFactor);
        ZoneScopedN("profile_tiled_render: zoom-tick render");
        t0 = Clock::now();
        last_buffer = &resolver.render_layout_frame(root, layout_id, scene.hierarchy_depth(), view_layers, scene);
        zoom_tick_ms[i] = elapsed_ms(t0);
        printf("Zoom tick %d (scale now %.9g) render done in %.1f ms\n", i + 1, scene.scale(), zoom_tick_ms[i]);
    }

    if (!png_path.empty() && last_buffer)
    {
        const SkImageInfo info = SkImageInfo::Make(last_buffer->width, last_buffer->height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        SkPixmap pixmap(info, last_buffer->data, last_buffer->row_bytes);
        SkFILEWStream stream(png_path.c_str());
        if (stream.isValid() && SkPngEncoder::Encode(&stream, pixmap, SkPngEncoder::Options{}))
            printf("Wrote %dx%d PNG to %s\n", last_buffer->width, last_buffer->height, png_path.c_str());
        else
            fprintf(stderr, "Failed to write PNG to %s\n", png_path.c_str());
    }

    // One CSV-friendly summary line (BUGS_AND_ENHANCEMENTS.md E31's own
    // scaling-study follow-up) - placements/routes/shapes plus every
    // major stage's own wall-clock time, so several runs at different
    // tile-grid sizes can be aggregated into one table without needing
    // to parse stdout's own human-readable lines above or a .tracy
    // trace at all (tracy-csvexport on the paired .tracy capture still
    // gives the finer per-pipeline-stage breakdown - see
    // OVERNIGHT_REVIEW.md's own E31 entry for that half of this).
    printf("CSV,placements,routes,lef_read_ms,def_read_ms,cold_render_ms,warm_render_ms_avg,zoom_tick_1_ms,zoom_tick_2_ms,zoom_tick_3_ms,zoom_tick_4_ms,zoom_tick_5_ms\n");
    printf("CSV,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
           root.get_layout_placements(layout_id).size(), root.get_layout_routes(layout_id).size(),
           lef_read_ms, def_read_ms, cold_render_ms, warm_render_ms_avg,
           zoom_tick_ms[0], zoom_tick_ms[1], zoom_tick_ms[2], zoom_tick_ms[3], zoom_tick_ms[4]);

    return 0;
}
