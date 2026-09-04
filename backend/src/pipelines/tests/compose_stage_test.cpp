#include "../stages/compose_stage.hpp"
#include "synchronous_stage_runner.hpp"
#include <gtest/gtest.h>

#include "include/core/SkColor.h"
#include "include/core/SkPixmap.h"

#include <memory>

using namespace le;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using RasterizeRunner = SynchronousStageRunner<RasterizeStage, HierarchyResolverStage::OutputHandle, RasterizeOutput, ViewRenderOptions>;
    using ComposeRunner = SynchronousStageRunner<ComposeStage, RasterizeStage::OutputHandle, RasterizedFrame, ViewRenderOptions>;

    // TOP (diearea (0,0)-(200,200)) places BLOCK once at (30,30) orientation
    // N ("block0"); BLOCK (diearea (0,0)-(50,50)) places LEAF once at
    // (20,20) under a caller-chosen orientation ("leaf0"); LEAF (boundary
    // (0,0)-(10,10)) has one asymmetric Terminal rect (1,1)-(4,2) on M1 -
    // asymmetric specifically so a 90-degree placement orientation moves
    // it somewhere a symmetric rect couldn't distinguish from an
    // unrotated one, exercising ComposeStage's own matrix math for real.
    struct ComposeStageFixture : public ::testing::Test
    {
        void build_fixture(Orientation leaf_orientation)
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            view_layers_handle = std::make_shared<const ViewLayerSet>(ViewLayerSet::build_for_technology(root, technology_id));

            const LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});

            const DesignId leaf_design = root.create_design(DesignData{.library = library_id, .name = "LEAF"});
            leaf_abstract = root.create_abstract(AbstractData{.design = leaf_design});
            root.create_shape(ShapeData{.abstract = leaf_abstract, .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});
            const TerminalId leaf_terminal = root.create_terminal(TerminalData{.abstract = leaf_abstract, .name = "A", .direction = SignalDirection::INPUT});
            const TerminalPortId leaf_port = root.create_terminal_port(TerminalPortData{.terminal = leaf_terminal});
            root.create_shape(ShapeData{.terminal_port = leaf_port, .layer = m1, .rects = {Rect{.ll = Point{1, 1}, .ur = Point{4, 2}}}});

            const DesignId block_design = root.create_design(DesignData{.library = library_id, .name = "BLOCK"});
            block_layout = root.create_layout(LayoutData{.design = block_design});
            root.create_shape(ShapeData{.layout = block_layout, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{50, 50}}}}});
            root.create_placement(PlacementData{.layout = block_layout, .name = "leaf0", .reference_design = leaf_design, .placement_status = PlacementStatus::PLACED, .location = Point{20, 20}, .orientation = leaf_orientation});

            const DesignId top_design = root.create_design(DesignData{.library = library_id, .name = "TOP"});
            top_layout = root.create_layout(LayoutData{.design = top_design});
            root.create_shape(ShapeData{.layout = top_layout, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{200, 200}}}}});
            root.create_placement(PlacementData{.layout = top_layout, .name = "block0", .reference_design = block_design, .placement_status = PlacementStatus::PLACED, .location = Point{30, 30}, .orientation = Orientation::N});

            const ViewRenderOptions options{
                .root = &root, .root_mutation_version = root.mutation_version(), .top_level = HierarchyId{top_layout},
                .hierarchy_depth = 2, .viewport = Rect{.ll = Point{0, 0}, .ur = Point{200, 200}}, .scale = kScale, .view_layers = view_layers_handle,
            };

            hierarchy_resolver_runner.run(view_layers_handle, 0, options);
            rasterize_runner.run(hierarchy_resolver_runner.last_handle(), 0, options);
            frame = &compose_runner.run(rasterize_runner.last_handle(), 0, options);
        }

        static SkColor sample(const RasterizedFrame &frame, int x, int y)
        {
            if (frame.empty || !frame.surface)
                return 0;
            SkPixmap pixmap;
            if (!frame.surface->peekPixels(&pixmap))
                return 0;
            return pixmap.getColor(x, y);
        }

        static constexpr double kScale = 4.0;

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        ViewLayerSetHandle view_layers_handle;
        AbstractId leaf_abstract;
        LayoutId block_layout;
        LayoutId top_layout;
        HierarchyResolverRunner hierarchy_resolver_runner{"HierarchyResolver"};
        RasterizeRunner rasterize_runner{"Rasterize"};
        ComposeRunner compose_runner{"Compose"};
        const RasterizedFrame *frame = nullptr;
    };
}

TEST_F(ComposeStageFixture, ComposesUnrotatedChildAtItsOwnGlobalOffset)
{
    build_fixture(Orientation::N);
    ASSERT_NE(frame, nullptr);
    ASSERT_FALSE(frame->empty);
    EXPECT_EQ(frame->buffer.width, 800);  // 200 dbu * scale 4
    EXPECT_EQ(frame->buffer.height, 800);

    const ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_id);
    const ViewLayerId terminal_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const Color expected_color = view_layers.get(terminal_layer)->style.fill_color;
    ASSERT_GT(expected_color.a, 0);

    // Global (unrotated) terminal bbox: block0 at (30,30) + leaf0 at
    // BLOCK-local (20,20) + terminal at LEAF-local (1,1)-(4,2) =
    // (51,51)-(54,52) in TOP's own dbu space -> pixel x:[204,216],
    // y: 800-52*4=592 to 800-51*4=596.
    // +/-5 tolerance (not exact equality) - same premultiplied-alpha
    // round-trip rounding RasterizeStageFixture's own color-sampling
    // tests already tolerate, compounded slightly further here by a
    // second composite (LEAF's own image drawn onto BLOCK's, then
    // BLOCK's onto TOP's).
    // x=206 (2px inset from the rect's own left edge at 204), not the
    // rect's own horizontal center (210) - the Terminal's own name ("A",
    // this fixture's own TerminalData) draws a label on top of its own
    // fill (HierarchyResolverStage's own by_layer/get_label_location
    // machinery), roughly centered in the combined shape's own bbox -
    // confirmed via direct pixel inspection that x=210 lands on that
    // label's own antialiased glyph edge, x=206 doesn't.
    const SkColor sampled = sample(*frame, 206, 594);
    EXPECT_NEAR(SkColorGetR(sampled), expected_color.r, 5);
    EXPECT_NEAR(SkColorGetG(sampled), expected_color.g, 5);
    EXPECT_NEAR(SkColorGetB(sampled), expected_color.b, 5);
    EXPECT_NEAR(SkColorGetA(sampled), expected_color.a, 5);
}

TEST_F(ComposeStageFixture, ComposesA90DegreeRotatedChildAtTheCorrectlyTransformedOffset)
{
    // leaf0 placed under orientation E (not N) - Geometry::orientation_linear(E)
    // rotates+repositions the terminal's own asymmetric (1,1)-(4,2) local
    // rect to BLOCK-local (21,26)-(22,29) (hand-derived the same way
    // Geometry::instance_transform/transform_bbox already computes it -
    // both independently covered by geometry_test.cpp, so this test is
    // really only exercising ComposeStage's own new
    // child_image_to_parent_dbu_matrix, not re-deriving that math from
    // scratch): + block0's own (30,30) offset (orientation N, so no
    // further rotation) = TOP-local (51,56)-(52,59).
    build_fixture(Orientation::E);
    ASSERT_NE(frame, nullptr);
    ASSERT_FALSE(frame->empty);

    const ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_id);
    const ViewLayerId terminal_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const Color expected_color = view_layers.get(terminal_layer)->style.fill_color;

    // TOP-local (51,56)-(52,59) @ scale 4 -> pixel x:[204,208],
    // y: 800-59*4=564 to 800-56*4=576.
    const SkColor rotated_position = sample(*frame, 206, 570);
    EXPECT_NEAR(SkColorGetR(rotated_position), expected_color.r, 5);
    EXPECT_NEAR(SkColorGetA(rotated_position), expected_color.a, 5);

    // The UNROTATED (Orientation::N) test's own sample point should now
    // be empty - if this test only "passed" because the whole image is
    // one big blob of fill color regardless of orientation, this catches
    // it.
    const SkColor unrotated_position = sample(*frame, 210, 594);
    EXPECT_EQ(SkColorGetA(unrotated_position), 0u);
}

TEST_F(ComposeStageFixture, NullInputProducesEmptyFrame)
{
    build_fixture(Orientation::N); // populates root/view_layers_handle/top_layout, result discarded below
    const ViewRenderOptions options{
        .root = &root, .root_mutation_version = root.mutation_version(), .top_level = HierarchyId{top_layout},
        .hierarchy_depth = 2, .viewport = Rect{.ll = Point{0, 0}, .ur = Point{200, 200}}, .scale = kScale, .view_layers = view_layers_handle,
    };
    // A fresh runner, not the fixture's own compose_runner (already
    // primed with a real result from build_fixture() above at the same
    // data_version 0 - MemoizingStage's own cache would just hand that
    // stale result back unchanged rather than re-running compute() with
    // this null input, since nothing it looks at - data_version or
    // options_did_change() - actually differs).
    ComposeRunner fresh_runner{"ComposeFresh"};
    const RasterizedFrame &empty_frame = fresh_runner.run(nullptr, 0, options);
    EXPECT_TRUE(empty_frame.empty);
    EXPECT_EQ(empty_frame.buffer.data, nullptr);
}
