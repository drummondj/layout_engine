#include "../stages/rasterize_stage.hpp"
#include "synchronous_stage_runner.hpp"
#include <gtest/gtest.h>

#include "include/core/SkColor.h"
#include "include/core/SkPixmap.h"

#include <algorithm>
#include <cstdint>
#include <memory>

using namespace le;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using RasterizeRunner = SynchronousStageRunner<RasterizeStage, HierarchyResolverStage::OutputHandle, RasterizeOutput, ViewRenderOptions>;

    // Single Abstract (LEAF): boundary (0,0)-(10,10), one Terminal rect
    // (1,1)-(2,2) and one Obstruction rect (3,3)-(4,4), both on the M1
    // routing layer - same fixture shape as HierarchyResolverStageFixture,
    // just without BLOCK/TOP (RasterizeStage's own InputData is a plain
    // HierarchyResolverStage::OutputHandle, so no ViewportCullStage
    // needed to exercise it in isolation).
    struct RasterizeStageFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);
            view_layers_handle = std::make_shared<const ViewLayerSet>(view_layers);

            const LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
            const DesignId leaf_design = root.create_design(DesignData{.library = library_id, .name = "LEAF"});
            leaf_abstract = root.create_abstract(AbstractData{.design = leaf_design});
            root.create_shape(ShapeData{.abstract = leaf_abstract, .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});
            const TerminalId leaf_terminal = root.create_terminal(TerminalData{.abstract = leaf_abstract, .name = "A", .direction = SignalDirection::INPUT});
            const TerminalPortId leaf_port = root.create_terminal_port(TerminalPortData{.terminal = leaf_terminal});
            root.create_shape(ShapeData{.terminal_port = leaf_port, .layer = m1, .rects = {Rect{.ll = Point{1, 1}, .ur = Point{2, 2}}}});
            const ObstructionId leaf_obstruction = root.create_obstruction(ObstructionData{.abstract = leaf_abstract});
            root.create_shape(ShapeData{.obstruction = leaf_obstruction, .layer = m1, .rects = {Rect{.ll = Point{3, 3}, .ur = Point{4, 4}}}});

            hierarchy_resolver_runner.run(view_layers_handle, 0, options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0));
            hierarchy_output = hierarchy_resolver_runner.last_handle();
        }

        ViewRenderOptions options_for(HierarchyId top_level, int hierarchy_depth, Rect viewport, double scale) const
        {
            return ViewRenderOptions{
                .root = &root, .root_mutation_version = root.mutation_version(), .top_level = top_level,
                .hierarchy_depth = hierarchy_depth, .viewport = viewport, .scale = scale, .view_layers = view_layers_handle,
            };
        }

        // Unpremultiplied sRGB sample - matches SkPixmap::getColor()'s own
        // contract regardless of the image's own storage format, so a
        // fully-opaque background comparison is exact, not approximate.
        static SkColor sample(const sk_sp<SkImage> &image, int x, int y)
        {
            SkPixmap pixmap;
            if (!image->peekPixels(&pixmap))
                return 0;
            return pixmap.getColor(x, y);
        }

        // Standard unpremultiplied Porter-Duff "src over dst" - what two
        // translucent fills drawn one after another actually produce, so
        // a z-order test can predict the exact resulting color instead of
        // assuming the top layer's own color shows through unblended.
        static SkColor blend_src_over_dst(Color src, Color dst)
        {
            const double sa = src.a / 255.0;
            const double da = dst.a / 255.0;
            const double out_a = sa + da * (1.0 - sa);
            if (out_a <= 0.0)
                return SkColorSetARGB(0, 0, 0, 0);
            const auto blend_channel = [&](uint8_t s, uint8_t d)
            {
                const double result = (s * sa + d * da * (1.0 - sa)) / out_a;
                return static_cast<uint8_t>(std::clamp(result, 0.0, 255.0));
            };
            return SkColorSetARGB(
                static_cast<uint8_t>(std::clamp(out_a * 255.0, 0.0, 255.0)),
                blend_channel(src.r, dst.r), blend_channel(src.g, dst.g), blend_channel(src.b, dst.b));
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        ViewLayerSet view_layers;
        ViewLayerSetHandle view_layers_handle;
        AbstractId leaf_abstract;
        HierarchyResolverRunner hierarchy_resolver_runner{"HierarchyResolver"};
        HierarchyResolverStage::OutputHandle hierarchy_output;
        RasterizeRunner rasterize_runner{"Rasterize"};
    };
}

TEST_F(RasterizeStageFixture, FillsTerminalRectWithItsOwnLayerFillColor)
{
    const ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    const RasterizeOutput &output = rasterize_runner.run(hierarchy_output, 0, options);

    ASSERT_TRUE(output.images.contains(HierarchyId{leaf_abstract}));
    const sk_sp<SkImage> &image = output.images.at(HierarchyId{leaf_abstract}).image;
    ASSERT_TRUE(image != nullptr);
    EXPECT_EQ(image->width(), 100);  // 10 dbu * scale 10
    EXPECT_EQ(image->height(), 100);

    const ViewLayerId terminal_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const ViewLayerData *terminal_style = view_layers.get(terminal_layer);
    ASSERT_NE(terminal_style, nullptr);
    ASSERT_GT(terminal_style->style.fill_color.a, 0); // otherwise this test can't sample a fill at all

    // Terminal rect (1,1)-(2,2) in dbu -> pixel x:[10,20], y:[80,90]
    // (dbu y up, pixel y down - canvas's own y-flip) - center is safely
    // away from any antialiased edge.
    const SkColor sampled = sample(image, 15, 85);
    const SkColor expected = to_sk_color(terminal_style->style.fill_color);
    // +/-5 tolerance, not exact equality - fill_color's own alpha (100,
    // not 255) means every channel round-trips through 8-bit
    // premultiplied storage (RGBA8888) and back, which SkPixmap::getColor()
    // un-premultiplies via integer division - a few ULPs of rounding
    // error here is expected, not a sign of a real color mismatch.
    EXPECT_NEAR(SkColorGetR(sampled), SkColorGetR(expected), 5);
    EXPECT_NEAR(SkColorGetG(sampled), SkColorGetG(expected), 5);
    EXPECT_NEAR(SkColorGetB(sampled), SkColorGetB(expected), 5);
    EXPECT_NEAR(SkColorGetA(sampled), SkColorGetA(expected), 5);
}

TEST_F(RasterizeStageFixture, HidingAPurposeSkipsItsWholeLayerGroupButNotOthers)
{
    ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    options.purpose_visible[ViewLayerPurpose::TERMINAL] = false;
    const RasterizeOutput &output = rasterize_runner.run(hierarchy_output, 0, options);

    const sk_sp<SkImage> &image = output.images.at(HierarchyId{leaf_abstract}).image;
    // Terminal rect (1,1)-(2,2) -> pixel (15, 85) - now hidden, so this
    // should read as fully transparent background instead of the
    // terminal's own fill color (matches FillsTerminalRectWithItsOwnLayerFillColor's
    // own sample point exactly, just with the opposite expectation).
    EXPECT_EQ(SkColorGetA(sample(image, 15, 85)), 0u);

    // Obstruction rect (3,3)-(4,4) -> pixel (35, 65) - untouched, since
    // only TERMINAL was hidden, not OBSTRUCTION.
    const ViewLayerId obstruction_layer = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    const Color obstruction_fill = view_layers.get(obstruction_layer)->style.fill_color;
    const SkColor sampled_obstruction = sample(image, 35, 65);
    EXPECT_NEAR(SkColorGetR(sampled_obstruction), obstruction_fill.r, 5);
    EXPECT_NEAR(SkColorGetA(sampled_obstruction), obstruction_fill.a, 5);
}

TEST_F(RasterizeStageFixture, HidingALayerByNameSkipsEveryPurposeOnThatLayer)
{
    ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    options.layer_name_visible["M1"] = false;
    const RasterizeOutput &output = rasterize_runner.run(hierarchy_output, 0, options);

    const sk_sp<SkImage> &image = output.images.at(HierarchyId{leaf_abstract}).image;
    // Both the terminal (1,1)-(2,2) and obstruction (3,3)-(4,4) rects are
    // on M1 - hiding the whole layer by name should hide both purposes at
    // once, unlike the purpose-only test above.
    EXPECT_EQ(SkColorGetA(sample(image, 15, 85)), 0u);
    EXPECT_EQ(SkColorGetA(sample(image, 35, 65)), 0u);
}

TEST_F(RasterizeStageFixture, BackgroundOutsideAnyShapeIsFullyTransparent)
{
    const ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    const RasterizeOutput &output = rasterize_runner.run(hierarchy_output, 0, options);

    const sk_sp<SkImage> &image = output.images.at(HierarchyId{leaf_abstract}).image;
    // (6,6)-(7,7) dbu region has no shape in this fixture (terminal/
    // obstruction are at (1,1)-(2,2)/(3,3)-(4,4)) -> pixel (65, 35).
    const SkColor sampled = sample(image, 65, 35);
    EXPECT_EQ(SkColorGetA(sampled), 0u);
}

TEST_F(RasterizeStageFixture, LocalOriginMatchesTheNodesOwnDeclaredBboxLowerLeft)
{
    const ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    const RasterizeOutput &output = rasterize_runner.run(hierarchy_output, 0, options);

    const Point &local_origin = output.images.at(HierarchyId{leaf_abstract}).local_origin;
    EXPECT_EQ(local_origin.x, 0);
    EXPECT_EQ(local_origin.y, 0);
}

TEST_F(RasterizeStageFixture, TopLevelUsesViewportNotItsOwnDeclaredBbox)
{
    // LEAF's own declared bbox is (0,0)-(10,10) (100x100px @ scale 10),
    // but as options.top_level here its own rasterization bbox should be
    // the (smaller) viewport instead - see RasterizeStage's own doc
    // comment on why this asymmetry exists (bounding cost to what's
    // actually visible, only at the top level).
    const ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{5, 5}}, 10.0);
    const RasterizeOutput &output = rasterize_runner.run(hierarchy_output, 0, options);

    const sk_sp<SkImage> &image = output.images.at(HierarchyId{leaf_abstract}).image;
    EXPECT_EQ(image->width(), 50);
    EXPECT_EQ(image->height(), 50);
    EXPECT_EQ(output.images.at(HierarchyId{leaf_abstract}).local_origin.x, 0);
    EXPECT_EQ(output.images.at(HierarchyId{leaf_abstract}).local_origin.y, 0);
}

TEST_F(RasterizeStageFixture, DrawsLaterViewLayerOnTopOfAnEarlierOverlappingOne)
{
    // A real regression test for the ViewLayer draw-order fix
    // (HierarchyResolverStage's own ViewLayerShapes/draw_view_shapes'
    // own doc comments): two fully-overlapping shapes on two DIFFERENT
    // layers - M2 (a second ROUTING layer, created AFTER M1, so its own
    // ViewLayerId.index - and therefore z-order - is higher than every
    // one of M1's own purposes) should draw on TOP of M1's, not the
    // other way around, regardless of which one this stage happens to
    // iterate first internally.
    const LayerId m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
    const ViewLayerSet two_layer_view_layers = ViewLayerSet::build_for_technology(root, technology_id);
    const ViewLayerSetHandle two_layer_view_layers_handle = std::make_shared<const ViewLayerSet>(two_layer_view_layers);

    const ObstructionId obstruction = root.create_obstruction(ObstructionData{.abstract = leaf_abstract});
    root.create_shape(ShapeData{.obstruction = obstruction, .layer = m1, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});
    const TerminalId terminal = root.create_terminal(TerminalData{.abstract = leaf_abstract, .name = "B", .direction = SignalDirection::INPUT});
    const TerminalPortId port = root.create_terminal_port(TerminalPortData{.terminal = terminal});
    root.create_shape(ShapeData{.terminal_port = port, .layer = m2, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});

    const ViewLayerId m1_obstruction_layer = two_layer_view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    const ViewLayerId m2_terminal_layer = two_layer_view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    ASSERT_LT(m1_obstruction_layer.index, m2_terminal_layer.index); // the property this test actually exercises

    const Color m1_color = two_layer_view_layers.get(m1_obstruction_layer)->style.fill_color;
    const Color m2_color = two_layer_view_layers.get(m2_terminal_layer)->style.fill_color;
    ASSERT_NE(m1_color.r, m2_color.r); // the palette must actually distinguish them, or this test can't tell who won

    HierarchyResolverRunner fresh_hierarchy_runner{"HierarchyResolverTwoLayer"};
    ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    options.view_layers = two_layer_view_layers_handle;
    fresh_hierarchy_runner.run(two_layer_view_layers_handle, 0, options);

    RasterizeRunner fresh_rasterize_runner{"RasterizeTwoLayer"};
    const RasterizeOutput &output = fresh_rasterize_runner.run(fresh_hierarchy_runner.last_handle(), 0, options);

    const sk_sp<SkImage> &image = output.images.at(HierarchyId{leaf_abstract}).image;
    const SkColor sampled = sample(image, 50, 50); // dead center of the fully-overlapping 100x100px rects

    // Both fills are translucent (layer_style()'s own alpha=100, not
    // 255), so "drawn on top" means Porter-Duff SrcOver blending, not
    // full replacement - the raw sampled color is neither pure m1_color
    // nor pure m2_color. Compute both possible blends (src-over-dst) by
    // hand and assert the sample matches "m2 over m1" specifically, not
    // "m1 over m2" - the two differ whenever the colors differ, so this
    // still proves order, not just that both layers drew something.
    const SkColor expected_m2_over_m1 = blend_src_over_dst(m2_color, m1_color);
    const SkColor expected_m1_over_m2 = blend_src_over_dst(m1_color, m2_color);
    ASSERT_NE(expected_m2_over_m1, expected_m1_over_m2); // sanity - the two orders must actually be distinguishable

    EXPECT_NEAR(SkColorGetR(sampled), SkColorGetR(expected_m2_over_m1), 8);
    EXPECT_NEAR(SkColorGetG(sampled), SkColorGetG(expected_m2_over_m1), 8);
    EXPECT_NEAR(SkColorGetB(sampled), SkColorGetB(expected_m2_over_m1), 8);
}

TEST_F(RasterizeStageFixture, NullInputProducesEmptyOutput)
{
    const ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    const RasterizeOutput &output = rasterize_runner.run(nullptr, 0, options);
    EXPECT_TRUE(output.images.empty());
    EXPECT_EQ(output.culled, nullptr);
}
