// backend/ONETBB_INTEGRATION.md migration plan, Phase 3 (render half):
// DesignRenderPipeline/MouseTargetLayerPipeline/SelectionGhostLayerPipeline/
// FrameRenderPipeline - split into its own file from pipelines_test.cpp
// once that file grew large, mirroring the old code's own render_test.cpp
// vs. pipeline_test.cpp split (one file per sub-area, not one giant file
// per module). A representative subset of render_test.cpp's own cases,
// for the same reason every other section in this migration is
// representative rather than exhaustive - each stage's compute() body is
// an unchanged copy of the original stage's own already-covered logic.
#include "../abstract_shape_pipeline.hpp"
#include "../frame_render_pipeline.hpp"
#include "../mouse_target_layer_pipeline.hpp"
#include "../selection_ghost_layer_pipeline.hpp"
#include "../stages/pixel_transform_stage.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include <gtest/gtest.h>
#include <memory>

namespace
{
    // Common scaffolding shared by every fixture below - a Root with one
    // Technology/M1 layer/matching ViewLayerSet/one Abstract, plus a Scene
    // sized to enclose every test shape, and a PipelineOptions builder
    // mirroring the shape-pipeline fixtures' own options() helper.
    struct RenderPipelineFixtureBase : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(le::TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(le::LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            view_layers = le::ViewLayerSet::build_for_technology(root, technology_id);
            abstract_id = root.create_abstract(le::AbstractData{});

            scene.set_current_abstract(abstract_id);
            scene.set_pan(le::Point{0, 0});
            scene.set_scale(1.0);
            scene.set_viewport_size(100, 100);
        }

        le::ShapeId add_port_shape(le::TerminalId terminal_id, const le::Shape &shape)
        {
            le::TerminalPortId port_id = root.create_terminal_port(le::TerminalPortData{.terminal = terminal_id});
            le::Shape owned_shape = shape;
            owned_shape.terminal_port = port_id;
            return root.create_shape(std::move(owned_shape));
        }

        le::ShapeId add_terminal_shape(const le::Shape &shape)
        {
            le::TerminalId terminal_id = root.create_terminal(le::TerminalData{.abstract = abstract_id});
            return add_port_shape(terminal_id, shape);
        }

        le::PipelineOptions options() const
        {
            le::PipelineOptions o;
            o.ctx.root = &root;
            o.ctx.view_layers = &view_layers;
            o.ctx.scene = &scene;
            o.epoch.root_mutation_version = root.mutation_version();
            o.epoch.view_layers_generation = view_layers.generation();
            o.viewport.viewport_version = scene.viewport_version();
            o.viewport.visibility_version = scene.visibility_version();
            o.viewport.scale = scene.scale();
            o.interaction.mouse_version = scene.mouse_version();
            o.interaction.selection_version = scene.selection_version();
            o.interaction.ruler_version = scene.ruler_version();
            return o;
        }

        // Same helper shape as render_test.cpp's own rasterize()/sample_pixel -
        // rasterizes a picture into a fresh, explicitly-cleared-to-transparent
        // surface and reads back its pixels for a color check.
        SkBitmap rasterize_picture(const sk_sp<SkPicture> &picture, int width, int height)
        {
            sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
            surface->getCanvas()->clear(SK_ColorTRANSPARENT);
            surface->getCanvas()->drawPicture(picture);

            SkBitmap bitmap;
            bitmap.allocPixels(SkImageInfo::MakeN32Premul(width, height));
            surface->readPixels(bitmap, 0, 0);
            return bitmap;
        }

        bool region_shows_color(const SkBitmap &bitmap, int x0, int y0, int x1, int y1, SkColor rgb)
        {
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    SkColor c = bitmap.getColor(x, y);
                    if (SkColorGetA(c) > 0 && SkColorGetR(c) == SkColorGetR(rgb) && SkColorGetG(c) == SkColorGetG(rgb) && SkColorGetB(c) == SkColorGetB(rgb))
                        return true;
                }
            return false;
        }

        le::Root root;
        le::TechnologyId technology_id;
        le::LayerId m1;
        le::ViewLayerSet view_layers;
        le::AbstractId abstract_id;
        le::Scene scene;
    };
}

// --- PixelTransformStage (oneTBB port of TransformToPixelsStage) ---

namespace
{
    struct PixelTransformStageFixture : public RenderPipelineFixtureBase
    {
        oneapi::tbb::flow::graph g{};
        std::unique_ptr<le::PixelTransformStage> stage = std::make_unique<le::PixelTransformStage>(g, "pixel_transform");
        le::StageData<std::map<le::ViewLayerId, std::vector<le::PixelShape>>, le::PipelineOptions> received;
        oneapi::tbb::flow::function_node<le::StageData<std::map<le::ViewLayerId, std::vector<le::PixelShape>>, le::PipelineOptions>> sink{
            g, oneapi::tbb::flow::serial, [this](le::StageData<std::map<le::ViewLayerId, std::vector<le::PixelShape>>, le::PipelineOptions> in)
            { received = std::move(in); }};

        void SetUp() override
        {
            RenderPipelineFixtureBase::SetUp();
            make_edge(stage->node(), sink);
        }
    };
}

TEST_F(PixelTransformStageFixture, AppliesPanAndScale)
{
    add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {10, 20}, .ur = {30, 40}}}});
    scene.set_pan(le::Point{5, 5});
    scene.set_scale(2.0);
    scene.set_viewport_size(200, 200);

    le::AbstractShapePipeline shape_pipeline;
    const auto &shapes = shape_pipeline.run(abstract_id, options());

    const uint64_t data_version = le::PixelTransformStage::data_version_for(abstract_id, options());
    stage->try_put({.data = shapes, .data_version = data_version, .options = options()});
    g.wait_for_all();

    ASSERT_EQ(received.data.size(), 1u);
    const auto &group = received.data.begin()->second;
    ASSERT_EQ(group.size(), 1u);
    const auto &r = group.front().rects.front();
    EXPECT_DOUBLE_EQ(r.ll.x, (10 - 5) * 2.0);
    EXPECT_DOUBLE_EQ(r.ll.y, (20 - 5) * 2.0);
    EXPECT_DOUBLE_EQ(r.ur.x, (30 - 5) * 2.0);
    EXPECT_DOUBLE_EQ(r.ur.y, (40 - 5) * 2.0);
}

// --- DesignRenderPipeline (oneTBB wiring of PixelTransformStage ->
// BuildDesignPictureStage -> RasterizePictureStage, design + tiny chains) ---

namespace
{
    struct DesignRenderPipelineFixture : public RenderPipelineFixtureBase
    {
        le::AbstractShapePipeline shape_pipeline;
        le::DesignRenderPipeline pipeline;
    };
}

TEST_F(DesignRenderPipelineFixture, FillsInteriorPixelWithLayerStyleColor)
{
    add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {10, 10}, .ur = {30, 30}}}});

    const auto &shapes = shape_pipeline.run(abstract_id, options());
    const uint64_t data_version = le::PixelTransformStage::data_version_for(abstract_id, options());
    const auto &picture = pipeline.run(shapes, data_version, options());

    ASSERT_NE(picture, nullptr);
    const le::ViewLayerData *view_layer = view_layers.get(shapes.begin()->first);
    ASSERT_NE(view_layer, nullptr);
    // DesignRenderPipeline no longer rasterizes (RasterizeComposePipeline
    // owns that, applying the Y-flip) - sample the raw picture directly,
    // pre-flip, at (10,10)-(30,30)'s own interior strip.
    SkBitmap bitmap = rasterize_picture(picture, 100, 100);
    EXPECT_TRUE(region_shows_color(bitmap, 11, 11, 15, 29, to_sk_color(view_layer->style.outline_color)));
}

TEST_F(DesignRenderPipelineFixture, ReusesCacheUntilMutationEvenForTheSameAbstractId)
{
    const auto &shapes = shape_pipeline.run(abstract_id, options());
    const uint64_t data_version = le::PixelTransformStage::data_version_for(abstract_id, options());

    pipeline.run(shapes, data_version, options());
    const uint64_t first_version = pipeline.design_version();
    pipeline.run(shapes, data_version, options());
    EXPECT_EQ(pipeline.design_version(), first_version);

    add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    root.bump_mutation_version();

    const auto &new_options = options(); // reflects the bumped mutation_version
    const auto &new_shapes = shape_pipeline.run(abstract_id, new_options);
    const uint64_t new_data_version = le::PixelTransformStage::data_version_for(abstract_id, new_options);
    pipeline.run(new_shapes, new_data_version, new_options);
    EXPECT_NE(pipeline.design_version(), first_version);
}

// --- MouseTargetLayerPipeline (oneTBB wiring of MouseOverlayStage) ---

namespace
{
    struct MouseTargetLayerPipelineFixture : public RenderPipelineFixtureBase
    {
        le::MouseTargetLayerPipeline pipeline;
    };
}

TEST_F(MouseTargetLayerPipelineFixture, DrawsTheLiveDragRectangleWhileDragging)
{
    // Screen/image pixel space (top-left origin, y down - see
    // set_mouse_position's own comment); with pan {0,0}/scale 1.0/viewport
    // 100x100, pixel_to_dbu(x_px, y_px) = {x_px, 100 - y_px}. begin_drag at
    // screen (10, 90) -> dbu (10, 10); set_mouse_position at screen
    // (40, 60) -> dbu (40, 40), giving a drag rect of dbu (10,10)-(40,40).
    scene.begin_drag(10, 90, le::Scene::DragKind::SELECT);
    scene.set_mouse_position(40, 60);

    const auto &picture = pipeline.run(options());
    ASSERT_NE(picture, nullptr);

    SkBitmap bitmap = rasterize_picture(picture, 100, 100);
    // Stroke color at the drag rect's own left edge (dbu x=10 == pixel
    // x=10 at pan {0,0}/scale 1.0), scanning a small strip since the
    // exact hairline pixel can shift by AA - mirrors
    // BuildOverlayPictureDrawsTheLiveDragRectangleWhileDragging's own
    // region-scan style.
    EXPECT_TRUE(region_shows_color(bitmap, 9, 15, 11, 35, to_sk_color(le::kDragRectStrokeColor)));
}

TEST_F(MouseTargetLayerPipelineFixture, IsEmptyWhenNoMousePositionSet)
{
    const auto &picture = pipeline.run(options());
    ASSERT_NE(picture, nullptr);
    SkBitmap bitmap = rasterize_picture(picture, 100, 100);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x)
            ASSERT_EQ(SkColorGetA(bitmap.getColor(x, y)), 0) << x << "," << y;
}

// --- SelectionGhostLayerPipeline (oneTBB wiring of SelectionOverlayStage/
// RulerOverlayStage -> RasterizePictureStage) ---

namespace
{
    struct SelectionGhostLayerPipelineFixture : public RenderPipelineFixtureBase
    {
        le::SelectionGhostLayerPipeline pipeline;
    };
}

TEST_F(SelectionGhostLayerPipelineFixture, OutlinesOnlyTheSelectedPieceNotTheWholeObject)
{
    const le::TerminalId terminal_id = root.create_terminal(le::TerminalData{.abstract = abstract_id});
    const le::ShapeId first_shape_id = add_port_shape(terminal_id, le::Shape{.layer = m1, .rects = {le::Rect{.ll = {10, 10}, .ur = {30, 30}}}});
    add_port_shape(terminal_id, le::Shape{.layer = m1, .rects = {le::Rect{.ll = {60, 60}, .ur = {80, 80}}}});
    scene.select(first_shape_id);

    const auto &picture = pipeline.run(abstract_id, options());
    ASSERT_NE(picture, nullptr);
    // SelectionGhostLayerPipeline no longer rasterizes
    // (RasterizeComposePipeline owns that, applying the Y-flip) - sample
    // the raw picture directly, pre-flip: the selected piece's own left
    // edge is at x=9,y=20; the second, unselected piece's own left edge
    // (x=59,y=70) must show nothing.
    SkBitmap bitmap = rasterize_picture(picture, 100, 100);

    auto is_white = [&](int x, int y)
    {
        SkColor c = bitmap.getColor(x, y);
        return SkColorGetR(c) == 255 && SkColorGetG(c) == 255 && SkColorGetB(c) == 255 && SkColorGetA(c) > 200;
    };
    EXPECT_TRUE(is_white(9, 20));
    EXPECT_FALSE(is_white(59, 70));
}

TEST_F(SelectionGhostLayerPipelineFixture, RunRulerIsEmptyWhenNoRulersExist)
{
    const auto &picture = pipeline.run_ruler(options());
    ASSERT_NE(picture, nullptr);
    SkBitmap bitmap = rasterize_picture(picture, 100, 100);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x)
            ASSERT_EQ(SkColorGetA(bitmap.getColor(x, y)), 0);
}

// --- FrameRenderPipeline (top-level orchestrator, mirrors Renderer::render()) ---

namespace
{
    struct FrameRenderPipelineFixture : public RenderPipelineFixtureBase
    {
        le::FrameRenderPipeline pipeline;
    };
}

TEST_F(FrameRenderPipelineFixture, ComposesDesignAndSelectionIntoOneFinalBuffer)
{
    const le::ShapeId shape_id = add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {10, 10}, .ur = {30, 30}}}});
    scene.select(shape_id);

    le::AbstractShapePipeline shape_pipeline;
    const auto &shapes = shape_pipeline.run(abstract_id, options());
    const auto &tiny_shapes = shape_pipeline.run_tiny_shapes(abstract_id, options());

    const le::PixelBuffer &buffer = pipeline.run(abstract_id, shapes, tiny_shapes, options());
    ASSERT_NE(buffer.data, nullptr);

    // Selection outline (opaque white) at the selected shape's own left
    // edge, post-flip - same coordinates as
    // SelectionGhostLayerPipelineFixture.OutlinesOnlyTheSelectedPieceNotTheWholeObject.
    auto is_white = [&](int x, int y)
    {
        const uint8_t *p = buffer.data + static_cast<size_t>(y) * buffer.row_bytes + static_cast<size_t>(x) * 4;
        return p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] > 200;
    };
    EXPECT_TRUE(is_white(9, 80));
}

TEST_F(FrameRenderPipelineFixture, RecomputesAfterACrudMutationWithNoOtherSceneChange)
{
    le::AbstractShapePipeline shape_pipeline;
    {
        const auto &shapes = shape_pipeline.run(abstract_id, options());
        const auto &tiny_shapes = shape_pipeline.run_tiny_shapes(abstract_id, options());
        pipeline.run(abstract_id, shapes, tiny_shapes, options());
    }

    add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {10, 10}, .ur = {30, 30}}}});
    root.bump_mutation_version();

    const auto &new_options = options();
    const auto &shapes = shape_pipeline.run(abstract_id, new_options);
    const auto &tiny_shapes = shape_pipeline.run_tiny_shapes(abstract_id, new_options);
    const le::PixelBuffer &buffer = pipeline.run(abstract_id, shapes, tiny_shapes, new_options);

    const le::ViewLayerId view_layer_id = shapes.begin()->first;
    const le::ViewLayerData *view_layer = view_layers.get(view_layer_id);
    ASSERT_NE(view_layer, nullptr);
    const SkColor expected = to_sk_color(view_layer->style.outline_color);

    bool found = false;
    for (int y = 71; y <= 89 && !found; ++y)
        for (int x = 11; x <= 15 && !found; ++x)
        {
            const uint8_t *p = buffer.data + static_cast<size_t>(y) * buffer.row_bytes + static_cast<size_t>(x) * 4;
            if (p[0] == SkColorGetR(expected) && p[1] == SkColorGetG(expected) && p[2] == SkColorGetB(expected) && p[3] > 0)
                found = true;
        }
    EXPECT_TRUE(found);
}
