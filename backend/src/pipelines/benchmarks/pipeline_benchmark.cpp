#include "../../api/api.hpp"
#include "../../database/database.hpp"
#include "../abstract_shape_pipeline.hpp"
#include "../design_render_pipeline.hpp"
#include "../draw_helpers.hpp"
#include "../frame_render_pipeline.hpp"
#include "../hierarchy_resolver.hpp"
#include "../hit_test.hpp"
#include "../synchronous_stage_runner.hpp"
#include "../stages/abstract_geometry_stage.hpp"
#include "../stages/build_design_picture_stage.hpp"
#include "../stages/build_tiny_dots_picture_stage.hpp"
#include "../stages/layer_visibility_filter_stage.hpp"
#include "../stages/pixel_transform_stage.hpp"
#include "../stages/rasterize_picture_stage.hpp"
#include "../stages/selection_overlay_stage.hpp"
#include "../stages/tiled_rasterize_picture_stage.hpp"
#include "../stages/tiny_pixel_transform_stage.hpp"
#include "../stages/tiny_viewport_filter_stage.hpp"
#include "../stages/viewport_filter_stage.hpp"
#include "layout_stress_data.hpp"
#include "real_design_data.hpp"
#include "stress_data.hpp"
#include "include/core/SkBBHFactory.h"
#include "include/core/SkPictureRecorder.h"
#include <benchmark/benchmark.h>
#include <oneapi/tbb.h>

using namespace le;

namespace
{
    // Builds a PipelineOptions snapshot of root/view_layers/scene's current
    // state - mirrors api.cpp's own pipeline_options_for helper (same
    // fields, same source), duplicated here per this file's own "each
    // including .cpp gets its own private copy" convention (see
    // stress_data.hpp's own comment) rather than sharing a header, since
    // this benchmark's own le::Root/Scene never come from an LeHandle.
    PipelineOptions pipeline_options_for(const Root &root, const ViewLayerSet &view_layers, const Scene &scene)
    {
        PipelineOptions options;
        options.ctx.root = &root;
        options.ctx.view_layers = &view_layers;
        options.ctx.scene = &scene;
        options.epoch.root_mutation_version = root.mutation_version();
        options.epoch.view_layers_generation = view_layers.generation();
        options.viewport.viewport_version = scene.viewport_version();
        options.viewport.visibility_version = scene.visibility_version();
        options.viewport.scale = scene.scale();
        options.interaction.mouse_version = scene.mouse_version();
        options.interaction.selection_version = scene.selection_version();
        options.interaction.ruler_version = scene.ruler_version();
        return options;
    }
}

// Every isolated-stage benchmark below drives its stage class directly via a
// fresh SynchronousStageRunner each iteration (backend/ONETBB_INTEGRATION.md
// migration, Phase 5c) - the oneTBB replacement for "a fresh Pipeline/
// Renderer every iteration" (a brand-new MemoizingStage's own
// last_data_version_ starts unset, so its very first call always
// recomputes regardless of what data_version is passed - the same "force a
// real cache miss" property a fresh Pipeline/Renderer had via its own
// fresh CachedStage members).

// Now includes the ViewLayerId resolution that used to be a separate
// resolve_view_layers stage - see AbstractGeometryStage's own class comment
// for why they were merged (carried over from the original Pipeline).
static void BM_GenerateShapes(benchmark::State &state)
{
    const auto &data = stress_data();
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, Scene{});

    for (auto _ : state)
    {
        SynchronousStageRunner<AbstractGeometryStage, AbstractId, std::vector<RenderedShape>> runner{"bm_generate_shapes"};
        const auto &shapes = runner.run(data.abstract_id, AbstractGeometryStage::data_version_for(data.abstract_id, options), options);
        const auto *shapes_data = shapes.data();
        benchmark::DoNotOptimize(shapes_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_GenerateShapes)->Unit(benchmark::kMillisecond);

// Benchmarked on the full 1M-shape generated (and resolved) set, matching
// how AbstractShapePipeline::run() actually calls it.
static void BM_FilterByViewportAndSize(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    SynchronousStageRunner<AbstractGeometryStage, AbstractId, std::vector<RenderedShape>> setup{"bm_generate_setup"};
    const auto &generated = setup.run(data.abstract_id, AbstractGeometryStage::data_version_for(data.abstract_id, options), options);

    for (auto _ : state)
    {
        SynchronousStageRunner<ViewportFilterStage, std::vector<RenderedShape>, std::vector<RenderedShape>> runner{"bm_viewport_filter"};
        const auto &filtered = runner.run(generated, /*data_version=*/0, options);
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
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    SynchronousStageRunner<AbstractGeometryStage, AbstractId, std::vector<RenderedShape>> generate_setup{"bm_generate_setup"};
    const auto &generated = generate_setup.run(data.abstract_id, AbstractGeometryStage::data_version_for(data.abstract_id, options), options);
    SynchronousStageRunner<ViewportFilterStage, std::vector<RenderedShape>, std::vector<RenderedShape>> viewport_setup{"bm_viewport_setup"};
    const auto &viewport_filtered = viewport_setup.run(generated, 0, options);

    for (auto _ : state)
    {
        SynchronousStageRunner<LayerVisibilityFilterStage, std::vector<RenderedShape>, std::map<ViewLayerId, std::vector<RenderedShape>>> runner{"bm_layer_filter"};
        const auto &filtered = runner.run(viewport_filtered, /*data_version=*/0, options);
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

// Isolated cost of TinyViewportFilterStage's own "second pass" over
// AbstractGeometryStage's output (UPDATES.md item 6, see its own doc
// comment for why this is a second pass rather than a second return value
// bolted onto ViewportFilterStage). Both stages reused across iterations so
// AbstractGeometryStage itself stays a cache hit (its own data_version
// doesn't depend on viewport_version - see AbstractGeometryStage::
// data_version_for); only pan is varied each iteration, which bumps
// scene.viewport_version() and therefore forces TinyViewportFilterStage's
// own options_did_change to recompute without touching the geometry
// stage's cache. Isolates exactly the cost this stage adds on top of
// AbstractGeometryStage, the same way BM_FilterByViewportAndSize isolates
// its own stage above.
static void BM_TinyShapesByViewport(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    SynchronousStageRunner<AbstractGeometryStage, AbstractId, std::vector<RenderedShape>> generate_runner{"bm_generate"};
    const uint64_t geometry_data_version = AbstractGeometryStage::data_version_for(data.abstract_id, options);
    const auto &generated = generate_runner.run(data.abstract_id, geometry_data_version, options); // warm the cache

    SynchronousStageRunner<TinyViewportFilterStage, std::vector<RenderedShape>, std::vector<TinyShapeDot>> tiny_viewport_runner{"bm_tiny_viewport_filter"};

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        options = pipeline_options_for(data.root, data.view_layers, scene);
        const auto &result = tiny_viewport_runner.run(generated, geometry_data_version, options);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_TinyShapesByViewport)->Unit(benchmark::kMillisecond);

// A fresh AbstractShapePipeline every iteration, one run() call each - the
// "just switched to a different Abstract" cold-start case, where every
// stage is a cache miss.
static void BM_Run(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    for (auto _ : state)
    {
        AbstractShapePipeline pipeline;
        const auto &result = pipeline.run(data.abstract_id, options);
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_Run)->Unit(benchmark::kMillisecond);

// The interactive/reused-instance case: one AbstractShapePipeline
// constructed once and reused across iterations (as a real caller - LeHandle -
// keeps one alive for a Scene's whole interactive lifetime), compared
// against BM_Run's cold-start baseline above to measure the actual caching
// benefit, per BENCHMARKS.md.

// Nothing changes between calls - the steady-state "no input this frame"
// case. Expect near-zero: every stage hits its cache.
static void BM_RunReused_NoChange(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    AbstractShapePipeline pipeline;

    for (auto _ : state)
    {
        const auto &result = pipeline.run(data.abstract_id, pipeline_options_for(data.root, data.view_layers, scene));
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RunReused_NoChange)->Unit(benchmark::kMillisecond);

// Only pan changes each call, simulating interactive panning - the common
// case AbstractGeometryStage's AbstractId-keyed cache is meant for. Expect
// close to the uncached viewport-filter and layer-filter costs, not the
// full BM_Run cost, since AbstractGeometryStage is skipped every iteration.
static void BM_RunReused_PanOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    AbstractShapePipeline pipeline;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const auto &result = pipeline.run(data.abstract_id, pipeline_options_for(data.root, data.view_layers, scene));
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RunReused_PanOnly)->Unit(benchmark::kMillisecond);

// Only a layer's visibility changes each call, simulating toggling a layer
// on/off in the UI. Expect close to just the uncached layer-filter stage
// cost, since AbstractGeometryStage and the viewport filter are both still
// cached.
static void BM_RunReused_VisibilityOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    AbstractShapePipeline pipeline;

    bool visible = true;
    for (auto _ : state)
    {
        scene.set_layer_name_visible("M1", visible);
        visible = !visible;
        const auto &result = pipeline.run(data.abstract_id, pipeline_options_for(data.root, data.view_layers, scene));
        const auto *result_ptr = &result;
        benchmark::DoNotOptimize(result_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RunReused_VisibilityOnly)->Unit(benchmark::kMillisecond);

// UPDATES.md 7.1's mouse-hover feature calls le::hit_test_point on every
// pointer-move event (see le_set_mouse_position in api.cpp) - unlike the
// stages above, it's not MemoizingStage-backed (the query point changes
// every call), so its real per-call cost matters directly, not just its
// cold-vs-warm delta. Measures a worst-case miss (scans every visible
// shape without ever finding a hit, since a real hit could short-circuit
// early) at the center of make_scene's own visible viewport - the
// candidate set is already viewport-culled/visibility-filtered by
// AbstractShapePipeline::run, not the full kTotalShapes design, which is
// the whole point of bounding hit-testing to on-screen shapes rather than
// a full design scan.
static void BM_HitTestPoint(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    AbstractShapePipeline setup;
    const auto &shapes = setup.run(data.abstract_id, pipeline_options_for(data.root, data.view_layers, scene));

    const Point query{50'000'000, 50'000'000}; // center of make_scene's visible [0, 100,000,000) range

    for (auto _ : state)
    {
        const auto hit = hit_test_point(shapes, data.view_layers, scene, query);
        const auto *hit_ptr = &hit;
        benchmark::DoNotOptimize(hit_ptr);
    }
}
BENCHMARK(BM_HitTestPoint)->Unit(benchmark::kMicrosecond);

// pipelines-module render-stage benchmarks - PixelTransformStage/
// BuildDesignPictureStage/RasterizePictureStage also cache internally per-
// instance, so isolated-stage benchmarks need a fresh SynchronousStageRunner
// per iteration too, same reasoning as the shape-pipeline benchmarks above.

static void BM_TransformToPixels(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    AbstractShapePipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.abstract_id, options);

    for (auto _ : state)
    {
        SynchronousStageRunner<PixelTransformStage, std::map<ViewLayerId, std::vector<RenderedShape>>, std::map<ViewLayerId, std::vector<PixelShape>>> runner{"bm_pixel_transform"};
        const auto &pixel_shapes = runner.run(generated, PixelTransformStage::data_version_for(data.abstract_id, options), options);
        const auto *pixel_shapes_ptr = &pixel_shapes;
        benchmark::DoNotOptimize(pixel_shapes_ptr);
    }
    state.SetItemsProcessed(state.iterations() * generated.size());
}
BENCHMARK(BM_TransformToPixels)->Unit(benchmark::kMillisecond);

// Isolated cost of TinyPixelTransformStage (UPDATES.md item 6), at
// make_zoomed_out_scene's scale where virtually every one of the 1M
// stress shapes is tiny - the realistic worst case for this per-point
// transform.
static void BM_TransformTinyShapesToPixels(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_zoomed_out_scene(data);
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    AbstractShapePipeline setup_pipeline;
    const auto &tiny_shapes = setup_pipeline.run_tiny_shapes(data.abstract_id, options);

    for (auto _ : state)
    {
        SynchronousStageRunner<TinyPixelTransformStage, std::map<ViewLayerId, std::vector<Point>>, std::map<ViewLayerId, std::vector<PixelPoint>>> runner{"bm_tiny_pixel_transform"};
        const auto &tiny_pixel_shapes = runner.run(tiny_shapes, TinyPixelTransformStage::data_version_for(data.abstract_id, options), options);
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
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    AbstractShapePipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.abstract_id, options);
    SynchronousStageRunner<PixelTransformStage, std::map<ViewLayerId, std::vector<RenderedShape>>, std::map<ViewLayerId, std::vector<PixelShape>>> setup_transform{"bm_pixel_transform_setup"};
    const auto &pixel_shapes = setup_transform.run(generated, PixelTransformStage::data_version_for(data.abstract_id, options), options);

    for (auto _ : state)
    {
        SynchronousStageRunner<BuildDesignPictureStage, std::map<ViewLayerId, std::vector<PixelShape>>, sk_sp<SkPicture>> runner{"bm_build_picture"};
        const auto &picture = runner.run(pixel_shapes, /*data_version=*/0, options);
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
// always calls Scene::select() with a piece - see le::hit_test_rect) so it
// was pure wasted cost. Removed entirely, along with
// Scene::is_selected_as_whole_object and build_picture's own dependency
// on selection_version. This benchmark now confirms BuildDesignPictureStage
// stays flat-cost regardless of selection size, not just that it happens
// to be fast today.
static void BM_BuildPicture_WithLargeSelection(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    const PipelineOptions setup_options = pipeline_options_for(data.root, data.view_layers, scene);

    AbstractShapePipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.abstract_id, setup_options);
    SynchronousStageRunner<PixelTransformStage, std::map<ViewLayerId, std::vector<RenderedShape>>, std::map<ViewLayerId, std::vector<PixelShape>>> setup_transform{"bm_pixel_transform_setup"};
    const auto &pixel_shapes = setup_transform.run(generated, PixelTransformStage::data_version_for(data.abstract_id, setup_options), setup_options);

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
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    for (auto _ : state)
    {
        SynchronousStageRunner<BuildDesignPictureStage, std::map<ViewLayerId, std::vector<PixelShape>>, sk_sp<SkPicture>> runner{"bm_build_picture"};
        const auto &picture = runner.run(pixel_shapes, /*data_version=*/0, options);
        benchmark::DoNotOptimize(picture.get());
    }
    state.SetItemsProcessed(state.iterations() * pixel_shapes.size());
}
BENCHMARK(BM_BuildPicture_WithLargeSelection)->Arg(0)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// Isolated cost of BuildTinyDotsPictureStage (UPDATES.md item 6), at
// make_zoomed_out_scene's scale where virtually every one of the 1M
// stress shapes is tiny - the realistic worst case for this stage's own
// batched-drawPoints-per-ViewLayer-group approach (see its own doc
// comment for why it's one drawPoints call per group, not one per dot).
static void BM_BuildTinyShapesPicture(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_zoomed_out_scene(data);
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    AbstractShapePipeline setup_pipeline;
    const auto &tiny_shapes = setup_pipeline.run_tiny_shapes(data.abstract_id, options);
    SynchronousStageRunner<TinyPixelTransformStage, std::map<ViewLayerId, std::vector<Point>>, std::map<ViewLayerId, std::vector<PixelPoint>>> setup_transform{"bm_tiny_pixel_transform_setup"};
    const auto &tiny_pixel_shapes = setup_transform.run(tiny_shapes, TinyPixelTransformStage::data_version_for(data.abstract_id, options), options);

    size_t total_dots = 0;
    for (const auto &[view_layer, group] : tiny_pixel_shapes)
        total_dots += group.size();

    for (auto _ : state)
    {
        SynchronousStageRunner<BuildTinyDotsPictureStage, std::map<ViewLayerId, std::vector<PixelPoint>>, sk_sp<SkPicture>> runner{"bm_build_tiny_dots_picture"};
        const auto &picture = runner.run(tiny_pixel_shapes, /*data_version=*/0, options);
        benchmark::DoNotOptimize(picture.get());
    }
    state.SetItemsProcessed(state.iterations() * total_dots);
}
BENCHMARK(BM_BuildTinyShapesPicture)->Unit(benchmark::kMillisecond);

static void BM_Rasterize(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    AbstractShapePipeline setup_pipeline;
    const auto &generated = setup_pipeline.run(data.abstract_id, options);
    SynchronousStageRunner<PixelTransformStage, std::map<ViewLayerId, std::vector<RenderedShape>>, std::map<ViewLayerId, std::vector<PixelShape>>> setup_transform{"bm_pixel_transform_setup"};
    const auto &pixel_shapes = setup_transform.run(generated, PixelTransformStage::data_version_for(data.abstract_id, options), options);
    SynchronousStageRunner<BuildDesignPictureStage, std::map<ViewLayerId, std::vector<PixelShape>>, sk_sp<SkPicture>> setup_build{"bm_build_picture_setup"};
    const auto &picture = setup_build.run(pixel_shapes, 0, options);

    for (auto _ : state)
    {
        SynchronousStageRunner<RasterizePictureStage, sk_sp<SkPicture>, RasterizedFrame> runner{"bm_rasterize"};
        const auto &frame = runner.run(picture, /*data_version=*/0, options);
        const uint8_t *buffer_data = frame.buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * pixel_shapes.size());
}
BENCHMARK(BM_Rasterize)->Unit(benchmark::kMillisecond);

// BUGS_AND_ENHANCEMENTS.md E25 - does recording an SkPicture with an
// SkRTreeFactory-backed bounding box hierarchy pay for itself? A BBH lets
// SkPicture::playback skip subtrees of recorded ops that don't intersect
// the current canvas clip - real per-clipped-draw benefit specifically
// requires a canvas with a NON-trivial clip smaller than the picture's
// own bounds, which is exactly TiledRasterizePictureStage's own access
// pattern (RasterizeComposePipeline's content-heavy slot): each tile is a
// small SkSurface covering one row-band, and drawPicture is called
// against it with the WHOLE 1,000,000-shape picture every time. Without a
// BBH, playback still has to walk (and clip-test) every recorded op for
// every tile; with one, most ops outside a tile's own row band should
// never be visited at all. Records the same real stress-fixture content
// (draw_grid + draw_shape_groups, matching BuildDesignPictureStage::compute
// exactly) into two pictures - one plain, one RTree-backed - then times
// TiledRasterizePictureStage's own real tile-splitting/drawPicture loop
// (copied verbatim, not reimplemented, so this can't drift from what
// production code actually does) against each. See BENCHMARKS.md for the
// resulting numbers and the adopt/defer decision.
namespace
{
    sk_sp<SkPicture> record_stress_picture_for_rtree_benchmark(const StressData &data, const Scene &scene, const PipelineOptions &options, SkBBHFactory *bbh_factory)
    {
        AbstractShapePipeline setup_pipeline;
        const auto &generated = setup_pipeline.run(data.abstract_id, options);
        SynchronousStageRunner<PixelTransformStage, std::map<ViewLayerId, std::vector<RenderedShape>>, std::map<ViewLayerId, std::vector<PixelShape>>> setup_transform{"bm_pixel_transform_setup_rtree"};
        const auto &pixel_shapes = setup_transform.run(generated, PixelTransformStage::data_version_for(data.abstract_id, options), options);

        SkPictureRecorder recorder;
        SkCanvas *canvas = recorder.beginRecording(
            SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())), bbh_factory);
        draw_grid(*canvas, scene);
        draw_shape_groups(*canvas, pixel_shapes, data.view_layers, scene.antialiasing_enabled());
        return recorder.finishRecordingAsPicture();
    }

}

static void BM_TiledRasterizePlayback_NoBBH(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);
    const sk_sp<SkPicture> picture = record_stress_picture_for_rtree_benchmark(data, scene, options, nullptr);

    for (auto _ : state)
    {
        // A fresh SynchronousStageRunner every iteration - matching
        // BM_BuildPicture's own idiom above - since MemoizingStage caches
        // on data_version, and a reused runner would only pay the real
        // rasterize cost on the first iteration.
        SynchronousStageRunner<TiledRasterizePictureStage, sk_sp<SkPicture>, RasterizedFrame> runner{"bm_tiled_rasterize_no_bbh"};
        const auto &frame = runner.run(picture, /*data_version=*/0, options);
        const uint8_t *buffer_data = frame.buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
}
BENCHMARK(BM_TiledRasterizePlayback_NoBBH)->Unit(benchmark::kMillisecond);

static void BM_TiledRasterizePlayback_WithRTree(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);
    SkRTreeFactory rtree_factory;
    const sk_sp<SkPicture> picture = record_stress_picture_for_rtree_benchmark(data, scene, options, &rtree_factory);

    for (auto _ : state)
    {
        SynchronousStageRunner<TiledRasterizePictureStage, sk_sp<SkPicture>, RasterizedFrame> runner{"bm_tiled_rasterize_with_rtree"};
        const auto &frame = runner.run(picture, /*data_version=*/0, options);
        const uint8_t *buffer_data = frame.buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
}
BENCHMARK(BM_TiledRasterizePlayback_WithRTree)->Unit(benchmark::kMillisecond);

// Recording cost itself - a BBH isn't free to build at record time either,
// paid once per BuildDesignPictureStage recompute (not once per rasterize/
// pan tick the way the playback benchmarks above are), so a real adopt
// decision needs both sides of this tradeoff, not just the playback win.
static void BM_RecordPicture_NoBBH(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);

    for (auto _ : state)
    {
        const sk_sp<SkPicture> picture = record_stress_picture_for_rtree_benchmark(data, scene, options, nullptr);
        benchmark::DoNotOptimize(picture.get());
    }
}
BENCHMARK(BM_RecordPicture_NoBBH)->Unit(benchmark::kMillisecond);

static void BM_RecordPicture_WithRTree(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);
    SkRTreeFactory rtree_factory;

    for (auto _ : state)
    {
        const sk_sp<SkPicture> picture = record_stress_picture_for_rtree_benchmark(data, scene, options, &rtree_factory);
        benchmark::DoNotOptimize(picture.get());
    }
}
BENCHMARK(BM_RecordPicture_WithRTree)->Unit(benchmark::kMillisecond);

// A fresh AbstractShapePipeline + FrameRenderPipeline every iteration,
// running the full generate -> filter -> filter -> transform -> picture ->
// rasterize -> compose chain once each - the "just switched to a different
// Abstract" cold-start case, now including all render stages.
static void BM_Render(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    for (auto _ : state)
    {
        AbstractShapePipeline pipeline;
        FrameRenderPipeline frame;
        const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);
        const auto &shapes = pipeline.run(data.abstract_id, options);
        const auto &tiny_shapes = pipeline.run_tiny_shapes(data.abstract_id, options);
        const auto &buffer = frame.run(data.abstract_id, shapes, tiny_shapes, options);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_Render)->Unit(benchmark::kMillisecond);

// Reused AbstractShapePipeline + FrameRenderPipeline across iterations,
// mirroring the BM_RunReused_* scenarios above but through the full render
// chain - real numbers for the threading question (README's open design
// question): is Skia picture generation/rasterization actually a
// bottleneck on the interactive path, or does it stay cheap because it's
// caching-aware too?

static void BM_RenderReused_NoChange(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);
    AbstractShapePipeline pipeline;
    FrameRenderPipeline frame;

    for (auto _ : state)
    {
        const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);
        const auto &shapes = pipeline.run(data.abstract_id, options);
        const auto &tiny_shapes = pipeline.run_tiny_shapes(data.abstract_id, options);
        const auto &buffer = frame.run(data.abstract_id, shapes, tiny_shapes, options);
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
    AbstractShapePipeline pipeline;
    FrameRenderPipeline frame;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);
        const auto &shapes = pipeline.run(data.abstract_id, options);
        const auto &tiny_shapes = pipeline.run_tiny_shapes(data.abstract_id, options);
        const auto &buffer = frame.run(data.abstract_id, shapes, tiny_shapes, options);
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RenderReused_PanOnly)->Unit(benchmark::kMillisecond);

// Full-chain warm/pan-only comparison at make_zoomed_out_scene's scale
// (UPDATES.md item 6), three variants isolating two different things:
//
// - BM_RenderReused_PanOnly_ZoomedOut: the design content pass alone, via
//   DesignRenderPipeline::run() directly (no rasterize/compose step at
//   all - DesignRenderPipeline only builds a picture; RasterizeComposePipeline
//   owns rasterize+compose now) - the same code BM_RenderReused_PanOnly
//   above exercises through FrameRenderPipeline, just isolated from
//   rasterize/compose and re-run at a scale where nearly every shape is
//   tiny so it's dropped from BuildDesignPictureStage's own output almost
//   entirely.
// - BM_ComposeWithOverlays_PanOnly_ZoomedOut_NoTinyShapes: the same design
//   content, but composited via FrameRenderPipeline::run() with an empty
//   tiny_shapes map - isolates ComposeStage's own fixed overhead
//   (rasterizing+blitting the mouse/selection/ruler overlay frames, even
//   empty, plus the extra blit machinery) from anything tiny-shapes-
//   specific, since BM_RenderReused_PanOnly_ZoomedOut never goes through
//   ComposeStage at all.
// - BM_RenderReusedWithTinyShapes_PanOnly_ZoomedOut: adds the real
//   AbstractGeometryStage -> TinyViewportFilterStage ->
//   TinyLayerVisibilityFilterStage -> TinyPixelTransformStage ->
//   BuildTinyDotsPictureStage chain and a real (non-empty) tiny-shapes map
//   into FrameRenderPipeline::run().
//
// The delta between the second and third is this feature's own marginal
// cost on top of ComposeStage's pre-existing overhead - a same-scale,
// single-variable comparison rather than a git-stash before/after of the
// whole feature, since that isolates exactly what the new stages add
// without conflating it with anything else changed since the last commit.
// See BENCHMARKS.md for the actual numbers.
static void BM_RenderReused_PanOnly_ZoomedOut(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_zoomed_out_scene(data);
    AbstractShapePipeline pipeline;
    DesignRenderPipeline design;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);
        const auto &shapes = pipeline.run(data.abstract_id, options);
        const auto &picture = design.run(shapes, PixelTransformStage::data_version_for(data.abstract_id, options), options);
        const SkPicture *picture_ptr = picture.get();
        benchmark::DoNotOptimize(picture_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTotalShapes);
}
BENCHMARK(BM_RenderReused_PanOnly_ZoomedOut)->Unit(benchmark::kMillisecond);

static void BM_ComposeWithOverlays_PanOnly_ZoomedOut_NoTinyShapes(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_zoomed_out_scene(data);
    AbstractShapePipeline pipeline;
    FrameRenderPipeline frame;
    const std::map<ViewLayerId, std::vector<Point>> no_tiny_shapes;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);
        const auto &shapes = pipeline.run(data.abstract_id, options);
        const auto &buffer = frame.run(data.abstract_id, shapes, no_tiny_shapes, options);
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
    AbstractShapePipeline pipeline;
    FrameRenderPipeline frame;

    int64_t pan_x = 0;
    for (auto _ : state)
    {
        scene.set_pan(Point{pan_x++, 0});
        const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);
        const auto &shapes = pipeline.run(data.abstract_id, options);
        const auto &tiny_shapes = pipeline.run_tiny_shapes(data.abstract_id, options);
        const auto &buffer = frame.run(data.abstract_id, shapes, tiny_shapes, options);
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
// stress LEF would produce) from the stress design's own filtered output,
// then measures the steady-state "mouse moves, selection doesn't change"
// cost through the actual pipelines-module entry points a real interactive
// session calls (api.cpp's le_render_pixel_buffer).
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

    AbstractShapePipeline setup_pipeline;
    const auto &shapes = setup_pipeline.run(data.abstract_id, pipeline_options_for(data.root, data.view_layers, scene));
    select_pieces(scene, shapes, static_cast<int>(state.range(0)));

    const SelectionOverlayRequest request{.abstract_id = data.abstract_id, .current_layout = {}, .remaining_depth = 0};

    for (auto _ : state)
    {
        const PipelineOptions options = pipeline_options_for(data.root, data.view_layers, scene);
        SynchronousStageRunner<SelectionOverlayStage, SelectionOverlayRequest, sk_sp<SkPicture>> runner{"bm_selection_overlay"};
        const auto &picture = runner.run(request, SelectionOverlayStage::data_version_for(request, options), options);
        benchmark::DoNotOptimize(picture.get());
    }
}
BENCHMARK(BM_BuildSelectionOverlayPicture_ManySelectedPieces)->Arg(100)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// Steady state: selection is built once (one FrameRenderPipeline reused
// across iterations, matching how api.cpp keeps one FrameRenderPipeline per
// handle for a Scene's whole interactive lifetime, so the design/selection
// pictures' own caches stay warm), then only mouse position changes every
// iteration - the exact scenario reported as slow. Reusing
// FrameRenderPipeline::run() directly (rather than driving MouseOverlayStage/
// SelectionOverlayStage/ComposeStage by hand) both matches the real call
// path and relies on each stage's own MemoizingStage cache to keep the
// unrelated design/selection/ruler work a no-op while only the mouse-driven
// chrome and the final compose actually recompute each iteration.
static void BM_ComposeWithOverlays_ManySelectedPieces_MouseMoveOnly(benchmark::State &state)
{
    const auto &data = stress_data();
    Scene scene = make_scene(data);

    AbstractShapePipeline pipeline;
    const auto &shapes = pipeline.run(data.abstract_id, pipeline_options_for(data.root, data.view_layers, scene));
    select_pieces(scene, shapes, static_cast<int>(state.range(0)));
    const auto &tiny_shapes = pipeline.run_tiny_shapes(data.abstract_id, pipeline_options_for(data.root, data.view_layers, scene));

    FrameRenderPipeline frame;

    int64_t x = 0;
    for (auto _ : state)
    {
        scene.set_mouse_position(static_cast<int>(x++ % 2000), 0);
        const auto &buffer = frame.run(data.abstract_id, shapes, tiny_shapes, pipeline_options_for(data.root, data.view_layers, scene));
        const uint8_t *buffer_data = buffer.data;
        benchmark::DoNotOptimize(buffer_data);
    }
}
BENCHMARK(BM_ComposeWithOverlays_ManySelectedPieces_MouseMoveOnly)->Arg(0)->Arg(100)->Arg(1000)->Arg(5000)->Arg(20000)->Unit(benchmark::kMillisecond);

// le_selected_object_ref/le_object_property_* FFI-facing benchmark - added
// because the two pipeline-side fixes above (overlay/selection-picture
// split, then rasterizing the selection overlay instead of replaying it)
// did not resolve a reported mouse-move delay that scaled with selection
// size. The Flutter provider (frontend/lib/providers/le_provider.dart)
// unconditionally calls refreshSelection() on *every* pointer move/hover
// event (handlePointerEvent -> refreshAndNotify -> ...), which rebuilds
// the full selected-object ref list from scratch every time:
// le_selected_object_ref per currently selected object (the Property
// Viewer fetches its properties separately, on demand, once navigated
// to - see le_object_property_count/_at) - none of this is
// pipelines-module content, so it was invisible to every benchmark above.
// This measures the real C API call sequence Dart's refreshSelection()
// makes, through the actual api.cpp (mutex-locked-per-call) surface, not a
// synthetic reconstruction - i.e. the true cost of one refreshSelection()
// call at a given selection size, plus one full property fetch per
// selected object (the worst case if a Property Viewer paged through all
// of them).
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
    le_set_current_design_abstract(handle, 0);
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
// itself (not le::hit_test_rect or any other machinery le_mouse_up
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
// the "polygon needs fracturing" case, since AbstractGeometryStage calls
// this once per (Terminal, distinct layer_name) pair.
static void BM_GetLabelLocationSingleRect(benchmark::State &state)
{
    const Shape shape{.rects = {Rect{.ll = {0, 0}, .ur = {50'000, 20'000}}}};

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

// --- Migration Step 3 Phase D: 1,000,000-instance hierarchical rendering ---
// See layout_stress_data.hpp's own comment for the fixture shape (a
// sub-block with 1,000,000 leaf placements, a top-level with 4 instances of
// that sub-block at different orientations) - the user's own spec for this
// step's real performance verification, not a synthetic shape-count stress
// like stress_data.hpp's own (unrelated) fixture above.

// The picture-resolution step alone (HierarchyResolver::build_layout_picture),
// excluding the final grid/overlay/rasterize compose - isolates whether the
// caching mechanism itself (not the unrelated per-frame compose cost every
// path pays) is what scales acceptably. remaining_depth=1 (not
// hierarchy_depth=2's own raw value) since this calls build_layout_picture
// directly, bypassing render_layout_frame's own "top level consumes one
// depth" bookkeeping - see that method's own comment.
static void BM_HierarchyResolver_BuildLayoutPicture_ColdCache_FullDepth(benchmark::State &state)
{
    const auto &data = layout_stress_data();
    Scene scene = make_layout_scene(data, 2);

    for (auto _ : state)
    {
        HierarchyResolver resolver;
        const auto picture = resolver.build_layout_picture(data.root, data.top_layout_id, /*remaining_depth=*/1, data.view_layers, scene, scene.scale());
        const auto *picture_ptr = picture.get();
        benchmark::DoNotOptimize(picture_ptr);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kSubBlockPlacementCount) * kTopPlacementCount);
}
BENCHMARK(BM_HierarchyResolver_BuildLayoutPicture_ColdCache_FullDepth)->Unit(benchmark::kMillisecond);

// Full end-to-end frame (matches le_render_pixel_buffer's own real call
// path) at hierarchy_depth=2 - deep enough to recurse into every one of the
// sub-block's own 1,000,000 leaf placements, 4 times over (once per
// top-level instance) - with a fresh HierarchyResolver every iteration, the
// "just switched to this Layout view" cold-start case where every stage is
// a cache miss.
static void BM_HierarchyResolver_RenderLayoutFrame_ColdCache_FullDepth(benchmark::State &state)
{
    const auto &data = layout_stress_data();
    Scene scene = make_layout_scene(data, /*hierarchy_depth=*/2);

    for (auto _ : state)
    {
        HierarchyResolver resolver;
        const auto &buffer = resolver.render_layout_frame(data.root, data.top_layout_id, scene.hierarchy_depth(), data.view_layers, scene);
        const auto *buffer_ptr = &buffer;
        benchmark::DoNotOptimize(buffer_ptr);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kSubBlockPlacementCount) * kTopPlacementCount);
}
BENCHMARK(BM_HierarchyResolver_RenderLayoutFrame_ColdCache_FullDepth)->Unit(benchmark::kMillisecond);

// The interactive/reused-instance case: one HierarchyResolver constructed
// once and reused across iterations (as a real caller - LeHandle - keeps
// one alive for a Scene's whole interactive lifetime), compared against
// the cold-start benchmark above to measure the actual caching benefit -
// this is the number that should read as "roughly constant per frame",
// not scaling with the sub-block's own 1,000,000-shape internal content,
// since nothing about the scene changes between iterations (every
// HierarchyResolver/FrameRenderPipeline-chain cache hits).
static void BM_HierarchyResolver_RenderLayoutFrame_WarmCache_FullDepth(benchmark::State &state)
{
    const auto &data = layout_stress_data();
    Scene scene = make_layout_scene(data, 2);
    HierarchyResolver resolver;
    resolver.render_layout_frame(data.root, data.top_layout_id, scene.hierarchy_depth(), data.view_layers, scene); // warm every cache once

    for (auto _ : state)
    {
        const auto &buffer = resolver.render_layout_frame(data.root, data.top_layout_id, scene.hierarchy_depth(), data.view_layers, scene);
        const auto *buffer_ptr = &buffer;
        benchmark::DoNotOptimize(buffer_ptr);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kSubBlockPlacementCount) * kTopPlacementCount);
}
BENCHMARK(BM_HierarchyResolver_RenderLayoutFrame_WarmCache_FullDepth)->Unit(benchmark::kMillisecond);

// hierarchy_depth=0 - every top-level placement (of the sub-block) falls
// back straight to the sub-block's own (empty, size-only) Abstract; none of
// the sub-block's own 1,000,000 leaf placements are ever visited. The real
// baseline BM_..._ColdCache_FullDepth's own cost is measured against -
// proves picture-caching (not merely "the shallow case does nothing") is
// what keeps the full-depth case's own warm-cache cost low.
static void BM_HierarchyResolver_RenderLayoutFrame_ColdCache_ShallowDepth(benchmark::State &state)
{
    const auto &data = layout_stress_data();
    Scene scene = make_layout_scene(data, /*hierarchy_depth=*/0);

    for (auto _ : state)
    {
        HierarchyResolver resolver;
        const auto &buffer = resolver.render_layout_frame(data.root, data.top_layout_id, scene.hierarchy_depth(), data.view_layers, scene);
        const auto *buffer_ptr = &buffer;
        benchmark::DoNotOptimize(buffer_ptr);
    }
    state.SetItemsProcessed(state.iterations() * kTopPlacementCount);
}
BENCHMARK(BM_HierarchyResolver_RenderLayoutFrame_ColdCache_ShallowDepth)->Unit(benchmark::kMillisecond);

// The counterpart the three benchmarks above can't stand in for: a real
// design (real_design_data.hpp's aes_5x5.def - 25 real instances of a full
// AES crypto macro, real dense M1/M2/via standard-cell geometry) instead of
// layout_stress_data.hpp's own deliberately-trivial-geometry, huge-
// instance-count fixture. Added while investigating a real tracy capture
// (BUGS_AND_ENHANCEMENTS.md E13) that showed a ~1.4s cold render_layout_frame
// for this exact design/depth - two full orders of magnitude past
// BM_..._ColdCache_FullDepth's own number despite 160,000x fewer
// placements, because that other benchmark's own trivial per-leaf geometry
// never exercises real shape-generation/SkPicture-recording cost at all -
// see that fixture's own file-level comment. hierarchy_depth=2 matches the
// minimum depth at which this design shows any content (see
// make_real_design_scene's own comment).
static void BM_HierarchyResolver_RenderLayoutFrame_ColdCache_RealDesign(benchmark::State &state)
{
    const RealDesignData &data = real_design_data();
    Scene scene = make_real_design_scene(data, /*hierarchy_depth=*/2);

    for (auto _ : state)
    {
        HierarchyResolver resolver;
        const auto &buffer = resolver.render_layout_frame(data.root, data.layout_id, scene.hierarchy_depth(), data.view_layers, scene);
        const auto *buffer_ptr = &buffer;
        benchmark::DoNotOptimize(buffer_ptr);
    }
}
BENCHMARK(BM_HierarchyResolver_RenderLayoutFrame_ColdCache_RealDesign)->Unit(benchmark::kMillisecond);

// BUGS_AND_ENHANCEMENTS.md E23 - the interactive-zoom case: an already-
// warm resolver (same design, same everything) receiving one small scale
// change, the same shape as a single mouse-wheel tick during real
// interactive use. HierarchyResolver::ensure_epoch's own Epoch struct
// includes `scale` under exact operator== comparison, so *any* scale
// change - not just a large one - discards the entire node graph
// (graph_ = std::make_unique<HierarchyGraph>()) and every SkPicture
// cached in it, forcing the next call to rediscover and re-record the
// whole hierarchy from scratch. This measures exactly that: warm the
// resolver once at one scale (untimed), nudge the scale by 1%, then
// time only the following render_layout_frame call - repeated every
// iteration via PauseTiming/ResumeTiming so the re-warm itself is never
// counted.
static void BM_HierarchyResolver_RenderLayoutFrame_WarmCache_RealDesign_OneScaleTick(benchmark::State &state)
{
    const RealDesignData &data = real_design_data();
    HierarchyResolver resolver;

    for (auto _ : state)
    {
        state.PauseTiming();
        Scene scene = make_real_design_scene(data, /*hierarchy_depth=*/2);
        resolver.render_layout_frame(data.root, data.layout_id, scene.hierarchy_depth(), data.view_layers, scene);
        scene.set_scale(scene.scale() * 1.01);
        state.ResumeTiming();

        const auto &buffer = resolver.render_layout_frame(data.root, data.layout_id, scene.hierarchy_depth(), data.view_layers, scene);
        const auto *buffer_ptr = &buffer;
        benchmark::DoNotOptimize(buffer_ptr);
    }
}
BENCHMARK(BM_HierarchyResolver_RenderLayoutFrame_WarmCache_RealDesign_OneScaleTick)->Unit(benchmark::kMillisecond);

// Direct benchmark of draw_group's own hairline-simplification branch
// (BENCHMARKS.md's dated entry for this) - stress_data.hpp's existing
// 1M-shape fixture deliberately can't exercise it: its RECT/PATH
// geometry uses the same size_um for both length and width (see
// item_geometry's own comment), so a shape there is only ever "both
// dimensions sub-pixel" (culled entirely upstream of draw_group) or
// "both dimensions visible" - never the long-thin-wire case (sub-pixel
// in exactly one dimension) the hairline branch targets. This fixture
// is genuinely long-thin instead: fixed sub-pixel width, length
// spanning many pixels - the real population that survives
// ViewportFilterStage's own cull but used to pay full buffered-outline/
// AA-fill cost in draw_group regardless. Calls draw_group directly
// (not through a whole pipeline) since it's the one function this
// change touches.
namespace
{
    constexpr int kThinWireCount = 200'000;

    std::vector<PixelShape> thin_wire_group(bool as_paths)
    {
        std::vector<PixelShape> group;
        group.reserve(kThinWireCount);
        constexpr int kCols = 500;
        for (int i = 0; i < kThinWireCount; ++i)
        {
            const double x0 = static_cast<double>(i % kCols) * 5.0;
            const double y0 = static_cast<double>(i / kCols) * 5.0;
            const double length = 20.0 + (i % 20);
            const double width = 0.3; // fixed, always sub-pixel at scale 1.0

            PixelShape shape;
            if (as_paths)
            {
                PixelPath path;
                path.width = width;
                path.polygon.points = {{.x = x0, .y = y0}, {.x = x0 + length, .y = y0}};
                path.buffered_outline.push_back(PixelPolygon{.points = {
                                                                   {.x = x0, .y = y0 - width / 2},
                                                                   {.x = x0 + length, .y = y0 - width / 2},
                                                                   {.x = x0 + length, .y = y0 + width / 2},
                                                                   {.x = x0, .y = y0 + width / 2},
                                                               }});
                shape.paths.push_back(std::move(path));
            }
            else
            {
                shape.rects.push_back(PixelRect{.ll = {.x = x0, .y = y0}, .ur = {.x = x0 + length, .y = y0 + width}});
            }
            group.push_back(std::move(shape));
        }
        return group;
    }

    ViewLayerStyle thin_wire_style()
    {
        ViewLayerStyle style;
        style.fill_color = Color{.r = 200, .g = 50, .b = 50, .a = 255};
        style.outline_color = Color{.r = 0, .g = 0, .b = 0, .a = 255};
        return style;
    }
}

static void BM_DrawGroup_ThinRects(benchmark::State &state)
{
    const std::vector<PixelShape> group = thin_wire_group(/*as_paths=*/false);
    const ViewLayerStyle style = thin_wire_style();
    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(2600, 2100));

    for (auto _ : state)
    {
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        draw_group(*surface->getCanvas(), group, style, /*antialiasing_enabled=*/true);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kThinWireCount));
}
BENCHMARK(BM_DrawGroup_ThinRects)->Unit(benchmark::kMillisecond);

static void BM_DrawGroup_ThinPaths(benchmark::State &state)
{
    const std::vector<PixelShape> group = thin_wire_group(/*as_paths=*/true);
    const ViewLayerStyle style = thin_wire_style();
    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(2600, 2100));

    for (auto _ : state)
    {
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        draw_group(*surface->getCanvas(), group, style, /*antialiasing_enabled=*/true);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kThinWireCount));
}
BENCHMARK(BM_DrawGroup_ThinPaths)->Unit(benchmark::kMillisecond);

// BUGS_AND_ENHANCEMENTS.md E19 - "text render is quite expensive, so it
// would be good to benchmark if this helps or not". Real per-shape
// design-content text (terminal/route/placement labels) used to always
// antialias its own glyphs regardless of antialiasing_enabled - two real
// bugs, not one: text_paint's own setAntiAlias was hardcoded true (fixed
// by threading antialiasing_enabled through, matching fill/stroke just
// above in draw_group), and even after that, glyph edges are controlled
// by SkFont::setEdging, not SkPaint::setAntiAlias at all - the paint fix
// alone was a no-op for the actual glyph rasterization (found by writing
// DrawGroupAntiAliasing.DisablingAntiAliasingAlsoProducesHardEdgesForText
// first and watching it fail even with the paint fix already in place).
// This is the "does it actually help" half of that same item: a
// realistic dense-design terminal-label population (20,000 short labels
// - real designs commonly have many thousands of pins), antialiasing on
// vs. off, both through draw_group directly (the one function E19
// touches), a permanent pair rather than a one-off git-stash A/B since
// this is a live, user-toggleable runtime flag, not a one-time change.
namespace
{
    constexpr int kTextLabelCount = 20'000;

    std::vector<PixelShape> text_label_group()
    {
        std::vector<PixelShape> group;
        group.reserve(kTextLabelCount);
        constexpr int kCols = 200;
        for (int i = 0; i < kTextLabelCount; ++i)
        {
            PixelShape shape;
            shape.texts.push_back(PixelText{
                .label = "P" + std::to_string(i % 1000),
                .location = {.x = static_cast<double>(i % kCols) * 12.0, .y = static_cast<double>(i / kCols) * 12.0},
                .size = 10.0,
            });
            group.push_back(std::move(shape));
        }
        return group;
    }
}

static void BM_DrawGroup_TextLabels_AntialiasingOn(benchmark::State &state)
{
    const std::vector<PixelShape> group = text_label_group();
    const ViewLayerStyle style = thin_wire_style();
    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(2400, 1200));

    for (auto _ : state)
    {
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        draw_group(*surface->getCanvas(), group, style, /*antialiasing_enabled=*/true);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kTextLabelCount));
}
BENCHMARK(BM_DrawGroup_TextLabels_AntialiasingOn)->Unit(benchmark::kMillisecond);

static void BM_DrawGroup_TextLabels_AntialiasingOff(benchmark::State &state)
{
    const std::vector<PixelShape> group = text_label_group();
    const ViewLayerStyle style = thin_wire_style();
    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(2400, 1200));

    for (auto _ : state)
    {
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        draw_group(*surface->getCanvas(), group, style, /*antialiasing_enabled=*/false);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kTextLabelCount));
}
BENCHMARK(BM_DrawGroup_TextLabels_AntialiasingOff)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
