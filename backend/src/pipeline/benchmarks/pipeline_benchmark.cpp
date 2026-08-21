#include "../../api/api.hpp"
#include "../../render/render.hpp"
#include "../pipeline.hpp"
#include "stress_data.hpp"
#include <benchmark/benchmark.h>

using namespace le;

// Pipeline now caches internally per-instance (see pipeline.hpp), so
// measuring a single stage's *uncached* per-call cost requires a fresh
// Pipeline every iteration - otherwise iterations after the first would be
// cache hits and the benchmark would measure ~nothing instead of real work.

// Now includes the ViewLayerId resolution that used to be a separate
// resolve_view_layers stage - see pipeline.hpp's class comment for why
// they were merged.
static void BM_GenerateShapes(benchmark::State &state)
{
    const auto &data = stress_data();
    for (auto _ : state)
    {
        Pipeline pipeline;
        const auto &shapes = pipeline.generate_shapes(data.root, data.abstract_id, data.view_layers);
        const auto *shapes_data = shapes.data();
        benchmark::DoNotOptimize(shapes_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_GenerateShapes)->Unit(benchmark::kMillisecond);

// Benchmarked on the full 1M-shape generated (and resolved) set, matching
// how Pipeline::run() actually calls it.
static void BM_FilterByViewportAndSize(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup;
    const auto &generated = setup.generate_shapes(data.root, data.abstract_id, data.view_layers);

    for (auto _ : state)
    {
        Pipeline pipeline;
        const auto &filtered = pipeline.filter_by_viewport_and_size(data.root, generated, scene, data.view_layers);
        const auto *filtered_data = filtered.data();
        benchmark::DoNotOptimize(filtered_data);
    }
    state.SetItemsProcessed(state.iterations() * generated.size());
}
BENCHMARK(BM_FilterByViewportAndSize)->Unit(benchmark::kMillisecond);

static void BM_FilterByLayerVisibility(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup;
    const auto &generated = setup.generate_shapes(data.root, data.abstract_id, data.view_layers);
    const auto &viewport_filtered = setup.filter_by_viewport_and_size(data.root, generated, scene, data.view_layers);

    for (auto _ : state)
    {
        Pipeline pipeline;
        const auto &filtered = pipeline.filter_by_layer_visibility(data.root, viewport_filtered, scene, data.view_layers);
        const auto *filtered_ptr = &filtered;
        benchmark::DoNotOptimize(filtered_ptr);
    }
    state.SetItemsProcessed(state.iterations() * viewport_filtered.size());
}
BENCHMARK(BM_FilterByLayerVisibility)->Unit(benchmark::kMillisecond);

// Zoomed out far enough that virtually every shape in the 1M-shape stress
// design (max size ~100um - see stress_data.hpp's ItemGeometry) is below
// the sub-pixel threshold - the "whole design fits on screen, dots
// everywhere" case UPDATES.md item 6 targets, as opposed to make_scene's
// own scale (chosen so shapes straddle the threshold, roughly half tiny).
namespace
{
    Scene make_zoomed_out_scene(const StressData &data)
    {
        Scene scene;
        scene.set_current_abstract(data.abstract_id);
        scene.set_pan(Point{0, 0});
        scene.set_scale(0.0000001); // 1px == 10,000,000 dbu == 10,000um
        scene.set_viewport_size(2000, 2000);
        return scene;
    }
}

// Isolated cost of tiny_shapes_by_viewport's own "second pass" over
// generate_shapes's output (UPDATES.md item 6, see its own doc comment for
// why this is a second pass rather than a second return value bolted onto
// filter_by_viewport_and_size). One Pipeline reused across iterations so
// generate_shapes itself stays a cache hit (its key, {AbstractId,
// view_layers.generation()}, doesn't include viewport_version - see its
// own code); only pan is varied each iteration, which invalidates
// tiny_shapes_by_viewport's own cache (keyed additionally on
// viewport_version) without touching generate_shapes's. This isolates
// exactly the cost this stage adds on top of generate_shapes, the same
// way BM_FilterByViewportAndSize isolates its own stage above.
static void BM_TinyShapesByViewport(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;
    pipeline.generate_shapes(data.root, data.abstract_id, data.view_layers); // warm the cache

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const auto &result = pipeline.tiny_shapes_by_viewport(data.root, data.abstract_id, scene, data.view_layers);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_TinyShapesByViewport)->Unit(benchmark::kMillisecond);

// A fresh Pipeline every iteration, one run() call each - the "just
// switched to a different Abstract" cold-start case, where every stage is
// a cache miss.
static void BM_Run(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    for (auto _ : state)
    {
        Pipeline pipeline;
        const auto &result = pipeline.run(data.root, scene, data.view_layers);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_Run)->Unit(benchmark::kMillisecond);

// The interactive/reused-instance case: one Pipeline constructed once and
// reused across iterations (as a real caller would keep one alive for a
// Scene's whole interactive lifetime), compared against BM_Run's cold-start
// baseline above to measure the actual caching benefit, per BENCHMARKS.md.

// Nothing changes between calls - the steady-state "no input this frame"
// case. Expect near-zero: every stage hits its cache.
static void BM_RunReused_NoChange(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;

    for (auto _ : state)
    {
        const auto &result = pipeline.run(data.root, scene, data.view_layers);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RunReused_NoChange)->Unit(benchmark::kMillisecond);

// Only pan changes each call, simulating interactive panning - the common
// case generate_shapes's AbstractId-keyed cache is meant for. Expect close
// to the uncached viewport-filter and layer-filter costs, not the full
// BM_Run cost, since generate_shapes is skipped every iteration.
static void BM_RunReused_PanOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const auto &result = pipeline.run(data.root, scene, data.view_layers);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RunReused_PanOnly)->Unit(benchmark::kMillisecond);

// Only a layer's visibility changes each call, simulating toggling a layer
// on/off in the UI. Expect close to just the uncached layer-filter stage
// cost, since generate_shapes and the viewport filter are both still
// cached.
static void BM_RunReused_VisibilityOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;

    bool visible = true;
    for (auto _ : state)
    {
        scene.set_layer_name_visible("M1", visible);
        visible = !visible;
        const auto &result = pipeline.run(data.root, scene, data.view_layers);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RunReused_VisibilityOnly)->Unit(benchmark::kMillisecond);

// UPDATES.md 7.1's mouse-hover feature calls Pipeline::hit_test_point on
// every pointer-move event (see le_set_mouse_position in api.cpp) - unlike
// the stages above, it's not CachedStage-backed (the query point changes
// every call), so its real per-call cost matters directly, not just its
// cold-vs-warm delta. Measures a worst-case miss (scans every visible
// shape without ever finding a hit, since a real hit could short-circuit
// early) at the center of make_scene's own visible viewport - the
// candidate set is already viewport-culled/visibility-filtered by `run`,
// not the full kTotalShapes design, which is the whole point of bounding
// hit-testing to on-screen shapes rather than a full design scan.
static void BM_HitTestPoint(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup;
    const auto &shapes = setup.run(data.root, scene, data.view_layers);

    const Point query{50'000'000, 50'000'000}; // center of make_scene's visible [0, 100,000,000) range

    for (auto _ : state)
    {
        const auto hit = Pipeline::hit_test_point(shapes, data.view_layers, scene, query);
        const auto *hit_ptr = &hit;
        benchmark::DoNotOptimize(hit_ptr);
    }
}
BENCHMARK(BM_HitTestPoint)->Unit(benchmark::kMicrosecond);

// render module benchmarks - Renderer also caches internally per-instance
// (see render.hpp), so isolated-stage benchmarks need a fresh Renderer per
// iteration too, same reasoning as the Pipeline benchmarks above.

static void BM_TransformToPixels(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.root, scene, data.view_layers);

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &pixel_shapes = renderer.transform_to_pixels(data.root, generated, scene);
        const auto *pixel_shapes_ptr = &pixel_shapes;
        benchmark::DoNotOptimize(pixel_shapes_ptr);
    }
    state.SetItemsProcessed(state.iterations() * generated.size());
}
BENCHMARK(BM_TransformToPixels)->Unit(benchmark::kMillisecond);

// Isolated cost of transform_tiny_shapes_to_pixels (UPDATES.md item 6), at
// make_zoomed_out_scene's scale where virtually every one of the 1M
// stress shapes is tiny - the realistic worst case for this per-point
// transform.
static void BM_TransformTinyShapesToPixels(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_zoomed_out_scene(data);

    Pipeline setup_pipeline;
    const auto &tiny_shapes = setup_pipeline.run_tiny_shapes(data.root, scene, data.view_layers);

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &tiny_pixel_shapes = renderer.transform_tiny_shapes_to_pixels(data.root, tiny_shapes, scene);
        const auto *tiny_pixel_shapes_ptr = &tiny_pixel_shapes;
        benchmark::DoNotOptimize(tiny_pixel_shapes_ptr);
    }
    size_t total_dots = 0;
    for (const auto &[view_layer, group] : tiny_shapes)
        total_dots += group.size();
    state.SetItemsProcessed(state.iterations() * total_dots);
}
BENCHMARK(BM_TransformTinyShapesToPixels)->Unit(benchmark::kMillisecond);

static void BM_BuildPicture(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.root, scene, data.view_layers);
    Renderer setup_renderer;
    const auto &pixel_shapes = setup_renderer.transform_to_pixels(data.root, generated, scene);

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        benchmark::DoNotOptimize(picture.get());
    }
    state.SetItemsProcessed(state.iterations() * pixel_shapes.size());
}
BENCHMARK(BM_BuildPicture)->Unit(benchmark::kMillisecond);

// Regression guard for a real, measured bug (see BENCHMARKS.md):
// build_picture used to have a whole-object selection-outline pass that
// called Scene::is_selected_as_whole_object once per *visible* PixelShape,
// itself a linear scan of the entire current selection - O(visible
// shapes * selection size), re-paid on every select() call (build_picture
// recomputed whenever selection_version() changed). That pass was also
// provably unreachable through the public API (api.cpp's le_mouse_up
// always calls Scene::select() with a piece - see Pipeline::hit_test_rect)
// so it was pure wasted cost. Removed entirely, along with
// Scene::is_selected_as_whole_object and build_picture's own dependency
// on selection_version. This benchmark now confirms build_picture stays
// flat-cost regardless of selection size, not just that it happens to be
// fast today.
static void BM_BuildPicture_WithLargeSelection(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.root, scene, data.view_layers);
    Renderer setup_renderer;
    const auto &pixel_shapes = setup_renderer.transform_to_pixels(data.root, generated, scene);

    const int n = static_cast<int>(state.range(0));
    int selected = 0;
    for (const auto &[view_layer, group] : generated)
    {
        for (const auto &rs : group)
        {
            if (!rs.shape_id || selected >= n)
                continue;
            scene.select(*rs.shape_id); // matching le_mouse_up's shape-level selection
            ++selected;
        }
    }

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        benchmark::DoNotOptimize(picture.get());
    }
    state.SetItemsProcessed(state.iterations() * pixel_shapes.size());
}
BENCHMARK(BM_BuildPicture_WithLargeSelection)->Arg(0)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// Isolated cost of build_tiny_shapes_picture (UPDATES.md item 6), at
// make_zoomed_out_scene's scale where virtually every one of the 1M
// stress shapes is tiny - the realistic worst case for this stage's own
// batched-drawPoints-per-ViewLayer-group approach (see its own doc
// comment for why it's one drawPoints call per group, not one per dot).
static void BM_BuildTinyShapesPicture(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_zoomed_out_scene(data);

    Pipeline setup_pipeline;
    const auto &tiny_shapes = setup_pipeline.run_tiny_shapes(data.root, scene, data.view_layers);
    Renderer setup_renderer;
    const auto &tiny_pixel_shapes = setup_renderer.transform_tiny_shapes_to_pixels(data.root, tiny_shapes, scene);

    size_t total_dots = 0;
    for (const auto &[view_layer, group] : tiny_pixel_shapes)
        total_dots += group.size();

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &picture = renderer.build_tiny_shapes_picture(data.root, tiny_pixel_shapes, scene, data.view_layers);
        benchmark::DoNotOptimize(picture.get());
    }
    state.SetItemsProcessed(state.iterations() * total_dots);
}
BENCHMARK(BM_BuildTinyShapesPicture)->Unit(benchmark::kMillisecond);

static void BM_Rasterize(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.root, scene, data.view_layers);
    Renderer setup_renderer;
    const auto &pixel_shapes = setup_renderer.transform_to_pixels(data.root, generated, scene);
    const auto &picture = setup_renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &buffer = renderer.rasterize(data.root, picture, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * pixel_shapes.size());
}
BENCHMARK(BM_Rasterize)->Unit(benchmark::kMillisecond);

// A fresh Pipeline + Renderer every iteration, running the full
// generate -> filter -> filter -> transform -> picture -> rasterize chain
// once each - the "just switched to a different Abstract" cold-start case,
// now including all three render stages.
static void BM_Render(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    for (auto _ : state)
    {
        Pipeline pipeline;
        Renderer renderer;
        const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(data.root, shapes, scene);
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        const auto &buffer = renderer.rasterize(data.root, picture, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_Render)->Unit(benchmark::kMillisecond);

// Reused Pipeline + Renderer across iterations, mirroring the
// BM_RunReused_* scenarios above but through the full render chain -
// real numbers for the threading question (README's open design
// question): is Skia picture generation/rasterization actually a
// bottleneck on the interactive path, or does it stay cheap because it's
// caching-aware too?

static void BM_RenderReused_NoChange(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;
    Renderer renderer;

    for (auto _ : state)
    {
        const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(data.root, shapes, scene);
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        const auto &buffer = renderer.rasterize(data.root, picture, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RenderReused_NoChange)->Unit(benchmark::kMillisecond);

static void BM_RenderReused_PanOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    Pipeline pipeline;
    Renderer renderer;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(data.root, shapes, scene);
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        const auto &buffer = renderer.rasterize(data.root, picture, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RenderReused_PanOnly)->Unit(benchmark::kMillisecond);

// Full-chain warm/pan-only comparison at make_zoomed_out_scene's scale
// (UPDATES.md item 6), three variants isolating two different things:
//
// - BM_RenderReused_PanOnly_ZoomedOut: the design content pass alone,
//   via renderer.rasterize(data.root, ) directly (no compositing pass at all) - the
//   same code BM_RenderReused_PanOnly above exercises, just re-run at a
//   scale where nearly every shape is tiny so it's dropped from
//   build_picture's own output almost entirely.
// - BM_ComposeWithOverlays_PanOnly_ZoomedOut_NoTinyShapes: the same
//   design content, but composited via compose_with_overlays with a null
//   tiny-shapes picture - isolates compose_with_overlays's own fixed
//   overhead (rasterizing+blitting the selection-overlay frame, even
//   empty, plus the extra blit machinery) from anything tiny-shapes-
//   specific, since BM_RenderReused_PanOnly_ZoomedOut never goes through
//   compose_with_overlays at all.
// - BM_RenderReusedWithTinyShapes_PanOnly_ZoomedOut: adds the real
//   tiny_shapes_by_viewport -> tiny_shapes_by_layer_visibility ->
//   transform_tiny_shapes_to_pixels -> build_tiny_shapes_picture chain
//   and a real (non-null) tiny-shapes picture into compose_with_overlays.
//
// The delta between the second and third is this feature's own marginal
// cost on top of compose_with_overlays's pre-existing overhead - a
// same-scale, single-variable comparison rather than a git-stash
// before/after of the whole feature, since that isolates exactly what
// the new stages add without conflating it with anything else changed
// since the last commit. See BENCHMARKS.md for the actual numbers.
static void BM_RenderReused_PanOnly_ZoomedOut(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_zoomed_out_scene(data);
    Pipeline pipeline;
    Renderer renderer;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(data.root, shapes, scene);
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        const auto &buffer = renderer.rasterize(data.root, picture, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RenderReused_PanOnly_ZoomedOut)->Unit(benchmark::kMillisecond);

static void BM_ComposeWithOverlays_PanOnly_ZoomedOut_NoTinyShapes(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_zoomed_out_scene(data);
    Pipeline pipeline;
    Renderer renderer;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(data.root, shapes, scene);
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);
        const auto &buffer = renderer.compose_with_overlays(data.root, picture, sk_sp<SkPicture>{}, sk_sp<SkPicture>{}, sk_sp<SkPicture>{}, sk_sp<SkPicture>{}, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_ComposeWithOverlays_PanOnly_ZoomedOut_NoTinyShapes)->Unit(benchmark::kMillisecond);

static void BM_RenderReusedWithTinyShapes_PanOnly_ZoomedOut(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_zoomed_out_scene(data);
    Pipeline pipeline;
    Renderer renderer;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(data.root, shapes, scene);
        const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);

        const auto &tiny_shapes = pipeline.run_tiny_shapes(data.root, scene, data.view_layers);
        const auto &tiny_pixel_shapes = renderer.transform_tiny_shapes_to_pixels(data.root, tiny_shapes, scene);
        const auto &tiny_shapes_picture = renderer.build_tiny_shapes_picture(data.root, tiny_pixel_shapes, scene, data.view_layers);

        const auto &buffer = renderer.compose_with_overlays(data.root, picture, tiny_shapes_picture, sk_sp<SkPicture>{}, sk_sp<SkPicture>{}, sk_sp<SkPicture>{}, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RenderReusedWithTinyShapes_PanOnly_ZoomedOut)->Unit(benchmark::kMillisecond);

// Overlay/composite benchmarks - added to actually locate a reported
// mouse-move/zoom/multi-select slowdown that scaled with selection size,
// rather than continuing to reason about it from code reading alone.
// Selects a batch of real pieces (mixed RECT/POLYGON/PATH, matching the
// stress generator's 1-in-3 PATH ratio, and drawn from the same giant
// single-Obstruction OBS block a real rectangle-drag-select over the
// stress LEF would produce) from the stress design's own filtered
// output, then measures the steady-state "mouse moves, selection doesn't
// change" cost through the actual render.hpp entry points a real
// interactive session calls (api.cpp's le_render_pixel_buffer).
namespace
{
    void select_pieces(le::Scene &scene, const std::map<le::ViewLayerId, std::vector<le::RenderedShape>> &shapes, int count)
    {
        int selected = 0;
        for (const auto &[view_layer, group] : shapes)
        {
            for (const auto &rs : group)
            {
                if (!rs.shape_id)
                    continue;
                scene.select(*rs.shape_id);
                if (++selected >= count)
                    return;
            }
        }
    }
}

static void BM_BuildSelectionOverlayPicture_ManySelectedPieces(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline setup_pipeline;
    const auto &shapes = setup_pipeline.run(data.root, scene, data.view_layers);
    select_pieces(scene, shapes, static_cast<int>(state.range(0)));

    for (auto _ : state)
    {
        Renderer renderer;
        const auto &picture = renderer.build_selection_overlay_picture(scene, data.root, shapes);
        benchmark::DoNotOptimize(picture.get());
    }
}
BENCHMARK(BM_BuildSelectionOverlayPicture_ManySelectedPieces)->Arg(100)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// Steady state: selection is built once (one Renderer reused across
// iterations, matching how api.cpp keeps one Renderer per handle for a
// Scene's whole interactive lifetime, so the selection-overlay picture's
// own cache stays warm), then only mouse position changes every
// iteration - the exact scenario reported as slow.
static void BM_ComposeWithOverlays_ManySelectedPieces_MouseMoveOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline pipeline;
    const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
    select_pieces(scene, shapes, static_cast<int>(state.range(0)));

    Renderer renderer;
    const auto &pixel_shapes = renderer.transform_to_pixels(data.root, shapes, scene);
    const auto &design_picture = renderer.build_picture(pixel_shapes, scene, data.view_layers, data.root);

    int64_t x = 0;
    for (auto _ : state)
    {
        scene.set_mouse_position(x++ % 2000, 0);
        const auto &overlay_picture = renderer.build_overlay_picture(scene, std::nullopt);
        const auto &selection_overlay_picture = renderer.build_selection_overlay_picture(scene, data.root, shapes);
        const auto &buffer = renderer.compose_with_overlays(data.root, design_picture, sk_sp<SkPicture>{}, overlay_picture, selection_overlay_picture, sk_sp<SkPicture>{}, scene);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
}
BENCHMARK(BM_ComposeWithOverlays_ManySelectedPieces_MouseMoveOnly)->Arg(0)->Arg(100)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// le_selected_object_ref/le_object_property_* FFI-facing benchmark - added
// because the two Renderer-side fixes above (overlay/selection-picture
// split, then rasterizing the selection overlay instead of replaying it)
// did not resolve a reported mouse-move delay that scaled with selection
// size. The Flutter provider (frontend/lib/providers/le_provider.dart)
// unconditionally calls refreshSelection() on *every* pointer move/hover
// event (handlePointerEvent -> refreshAndNotify -> ...), which rebuilds
// the full selected-object ref list from scratch every time:
// le_selected_object_ref per currently selected object (the Property
// Viewer fetches its properties separately, on demand, once navigated
// to - see le_object_property_count/_at) - none of this is Renderer/
// Pipeline content, so it was invisible to every benchmark above. This
// measures the real C API call sequence Dart's refreshSelection() makes,
// through the actual api.cpp (mutex-locked-per-call) surface, not a
// synthetic reconstruction - i.e. the true cost of one refreshSelection()
// call at a given selection size, plus one full property fetch per
// selected object (the worst case if a Property Viewer paged through
// all of them).
static void BM_RefreshSelectedObjects_ManySelectedPieces(benchmark::State &state)
{
    // Ensures the shared stress LEF file exists on disk (stress_data()
    // also builds an in-memory le::Root, unused here - le_read_lef needs
    // its own independent Root via the C API, matching how the real app
    // reads a file rather than sharing this file's own le::Root).
    stress_data();
    const std::string path = std::string(BENCHMARK_DATA_DIR) + "/stress.lef";

    LeHandle *handle = le_create();
    le_read_lef(handle, path.c_str());
    le_set_current_design(handle, 0);
    le_set_viewport_size(handle, 2000, 2000);
    le_fit_scene(handle, 10);

    // A full-viewport drag-select from the origin out to (range, range) -
    // varying how much of the fitted design gets enclosed, and therefore
    // how many pieces end up selected, at the scale the bug report
    // described (the more selected, the slower).
    le_mouse_down(handle, 0, 0);
    le_mouse_up(handle, static_cast<int32_t>(state.range(0)), static_cast<int32_t>(state.range(0)));

    const int32_t count = le_selection_count(handle);
    state.counters["selected_pieces"] = static_cast<double>(count);

    for (auto _ : state)
    {
        for (int32_t i = 0; i < count; ++i)
        {
            const LeObjectRef ref = le_selected_object_ref(handle, i);
            benchmark::DoNotOptimize(ref.index);
            const int32_t property_count = le_object_property_count(handle, ref);
            for (int32_t p = 0; p < property_count; ++p)
            {
                const LeProperty property = le_object_property_at(handle, ref, p);
                benchmark::DoNotOptimize(&property);
            }
        }
    }

    le_destroy(handle);
}
BENCHMARK(BM_RefreshSelectedObjects_ManySelectedPieces)->Arg(200)->Arg(800)->Arg(1400)->Arg(2000)->Unit(benchmark::kMillisecond);

// Isolated Scene::select() benchmark - times only the select() loop
// itself (not Pipeline::hit_test_rect or any other machinery le_mouse_up
// also runs), against N distinct synthetic ShapeIds - the exact shape of
// a real drag-select over one Obstruction's whole OBS block (a real
// design puts every OBS item under one shared ObstructionId regardless of
// how many rects/polygons/paths it holds, but selection is now Shape-
// granular, not Obstruction-granular - see scene.hpp's own comment).
// Isolating this from the full le_mouse_up/hit_test_rect/LEF-parsing path
// makes before/after numbers for select()'s own dedup cost directly
// comparable and fast to run, unlike BM_RefreshSelectedObjects_
// ManySelectedPieces above (which depends on this being fast just to
// finish its own setup). Selection dedup is now plain ShapeId identity
// (an unordered_set, see scene.hpp) rather than a geometry-hash bucket
// lookup - this benchmark's own numbers are expected to improve after
// that change, not regress; see BENCHMARKS.md.
static void BM_SceneSelect_ManyPiecesSameOrigin(benchmark::State &state)
{
    const int n = static_cast<int>(state.range(0));

    std::vector<ShapeId> ids;
    ids.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        ids.push_back(ShapeId{static_cast<uint32_t>(i), 0});

    for (auto _ : state)
    {
        state.PauseTiming();
        Scene scene;
        state.ResumeTiming();

        for (const ShapeId &id : ids)
            scene.select(id);
        benchmark::DoNotOptimize(scene.selection().size());
    }
    state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SceneSelect_ManyPiecesSameOrigin)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// Originally investigated a "still slow selecting Obstruction pieces"
// symptom traced to le::to_properties(le::ObstructionData) deep-copying
// ObstructionData's embedded `std::vector<Shape> shapes` on every call
// (see BENCHMARKS.md's 2026-08-07 entry) - fixed at the time by passing
// by const-reference in cmg's generated to_string/to_properties/
// operator<<. Since then, `Shape` was pooled (TCL_EXPLORATION.md Phase
// 3, for stable-id shape CRUD) and `Obstruction.shapes` became an
// is_child relationship, not a struct field at all - ObstructionData no
// longer has a `shapes` field to copy regardless of by-value/by-reference,
// so the original 900K-shapes-in-one-copy scenario this benchmark
// targeted can no longer happen for this specific field. Kept as a
// regression guard that to_properties(ObstructionData) stays cheap (it's
// now trivially so - ObstructionData is just one AbstractId), and
// shapes_count now comes from a fast Root::get_obstruction_shapes(id)
// index lookup (api.cpp's build_obstruction_properties), not a copy.
static void BM_ToPropertiesObstructionCopyCost(benchmark::State &state)
{
    const auto &data = stress_data();

    ObstructionId obstruction_id;
    bool found = false;
    data.root.for_each_obstruction_id([&](ObstructionId id)
                                       {
        if (!found) { obstruction_id = id; found = true; } });

    const ObstructionData *obstruction = data.root.get_obstruction(obstruction_id);
    state.counters["shapes"] = static_cast<double>(data.root.get_obstruction_shapes(obstruction_id).size());

    for (auto _ : state)
    {
        const auto properties = le::to_properties(*obstruction, /*dbu_per_um=*/1000.0);
        benchmark::DoNotOptimize(properties.data());
    }
}
BENCHMARK(BM_ToPropertiesObstructionCopyCost)->Unit(benchmark::kMillisecond);

// Investigates whether Geometry::path_to_polygons (a real Boost.Geometry
// buffer op) is cheap enough to compute once per path at generate_shapes
// time (cached per-AbstractId, not per-frame) for a design with the
// stress data's own path density (~1/3 of 1M shapes, so ~333K paths) -
// informs whether PATH rendering's planned fill/outline redesign should
// reuse this existing, already-correct (miter-joined) buffering, or needs
// a cheaper hand-rolled per-segment-rectangle approximation instead.
static void BM_PathToPolygonsSingleCall(benchmark::State &state)
{
    // A 2-point path matching the stress design's own size formula
    // (roughly - see stress_data.hpp's item_geometry), not a degenerate
    // zero-length one.
    const Path path{.polygon = Polygon{.points = {Point{0, 0}, Point{50'000, 0}}}, .width = 4'000};

    for (auto _ : state)
    {
        const auto polygons = Geometry::path_to_polygons(path);
        benchmark::DoNotOptimize(polygons.data());
    }
}
BENCHMARK(BM_PathToPolygonsSingleCall)->Unit(benchmark::kNanosecond);

// Geometry::get_label_location (UPDATES.md item 8) - isolated cost of a
// single call, for both the trivial "one rect, no fracturing" case and
// the "polygon needs fracturing" case, since Pipeline::generate_shapes
// calls this once per (Terminal, distinct layer_name) pair.
static void BM_GetLabelLocationSingleRect(benchmark::State &state)
{
    const Shape shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {50'000, 20'000}}}};

    for (auto _ : state)
    {
        const auto location = Geometry::get_label_location(shape);
        const auto *location_ptr = &location;
        benchmark::DoNotOptimize(location_ptr);
    }
}
BENCHMARK(BM_GetLabelLocationSingleRect)->Unit(benchmark::kNanosecond);

static void BM_GetLabelLocationLShapedPolygon(benchmark::State &state)
{
    // Same shape (scaled up to dbu) as Geometry.LabelLocationOnAWidePolygonFracturesVerticallyAndPicksTheLargestSlab.
    const Shape shape{
        .layer_name = "M1",
        .polygons = {Polygon{.points = {
                         Point{0, 0}, Point{100'000, 0}, Point{100'000, 60'000},
                         Point{80'000, 60'000}, Point{80'000, 20'000}, Point{0, 20'000}}}},
    };

    for (auto _ : state)
    {
        const auto location = Geometry::get_label_location(shape);
        const auto *location_ptr = &location;
        benchmark::DoNotOptimize(location_ptr);
    }
}
BENCHMARK(BM_GetLabelLocationLShapedPolygon)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
