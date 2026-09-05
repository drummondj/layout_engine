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

        // TERMINAL/OBSTRUCTION on a real ROUTING-type Layer (this
        // fixture's own M1/M2) now draw with a real FillPattern
        // (view_style.hpp's own terminal_fill_pattern/BRICK - the fill-
        // pattern-by-object-type feature this module ported from
        // pipelines.old/draw_helpers.hpp) rather than a flat fill, so a
        // single hardcoded sample point can legitimately land on a
        // pattern "gap" (fully or partially transparent) even where the
        // shape's own fill genuinely covers that pixel - a real, correct
        // rendering difference, not a regression. Tests that need to
        // confirm "this fill color appears somewhere in this shape's own
        // region" (rather than "at this exact pixel") scan a small block
        // instead of sampling one point.
        static bool region_contains_color_near(const sk_sp<SkImage> &image, int x0, int y0, int x1, int y1, SkColor expected, int tolerance)
        {
            SkPixmap pixmap;
            if (!image->peekPixels(&pixmap))
                return false;
            for (int y = y0; y < y1; ++y)
            {
                for (int x = x0; x < x1; ++x)
                {
                    const SkColor c = pixmap.getColor(x, y);
                    if (std::abs(static_cast<int>(SkColorGetR(c)) - static_cast<int>(SkColorGetR(expected))) <= tolerance &&
                        std::abs(static_cast<int>(SkColorGetG(c)) - static_cast<int>(SkColorGetG(expected))) <= tolerance &&
                        std::abs(static_cast<int>(SkColorGetB(c)) - static_cast<int>(SkColorGetB(expected))) <= tolerance &&
                        std::abs(static_cast<int>(SkColorGetA(c)) - static_cast<int>(SkColorGetA(expected))) <= tolerance)
                        return true;
                }
            }
            return false;
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
    // (dbu y up, pixel y down - canvas's own y-flip). M1 is a ROUTING-type
    // Layer, so its own TERMINAL row draws with a real diagonal-stripe
    // FillPattern (view_style.hpp's own terminal_fill_pattern) instead of
    // a flat fill_color - pattern_shader's own tile paints its "ink" in
    // the layer's own OUTLINE color at full opacity (draw_view_shapes'
    // own comment: a paint's alpha still modulates a shader's own output,
    // so fill_color's own translucent alpha is deliberately overridden to
    // 1.0 rather than washing out the pattern), so that - not fill_color -
    // is the color that actually appears on screen wherever this pattern
    // has ink. Scan the whole rect rather than one exact pixel (see
    // region_contains_color_near's own comment) so this test doesn't
    // depend on the pattern's own exact phase at one hardcoded point.
    const SkColor expected = to_sk_color(terminal_style->style.outline_color);
    // +/-5 tolerance, not exact equality - a few ULPs of 8-bit storage
    // round-trip rounding is expected, not a sign of a real mismatch.
    EXPECT_TRUE(region_contains_color_near(image, 10, 80, 20, 90, expected, 5));
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

    // Obstruction rect (3,3)-(4,4) -> pixel x:[30,40], y:[60,70] -
    // untouched, since only TERMINAL was hidden, not OBSTRUCTION. M1's
    // own OBSTRUCTION row draws with a real BRICK FillPattern, whose ink
    // is the layer's own OUTLINE color, not fill_color (see
    // FillsTerminalRectWithItsOwnLayerFillColor's own comment) - scan the
    // rect rather than one exact pixel.
    const ViewLayerId obstruction_layer = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    const Color obstruction_outline = view_layers.get(obstruction_layer)->style.outline_color;
    EXPECT_TRUE(region_contains_color_near(image, 30, 60, 40, 70, to_sk_color(obstruction_outline), 5));
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
    // (HierarchyResolverStage's own ViewLayerShapes/draw_view_shapes' own
    // doc comments): two fully-overlapping shapes on two DIFFERENT layers
    // - M2 (created AFTER M1, so its own ViewLayerId.index - and
    // therefore z-order - is higher than every one of M1's own purposes)
    // should draw on TOP of M1's, not the other way around, regardless of
    // which one this stage happens to iterate first internally.
    //
    // Both layers use a non-ROUTING/non-CUT `type` so their own TERMINAL
    // row resolves to FillPattern::DOTS (view_style.hpp's own
    // terminal_fill_pattern - real per-object-type fill patterns, this
    // module's own ported feature), not a flat fill_color - a single
    // hardcoded sample point can no longer be trusted to land on either
    // shape's own pattern "ink" (see FillsTerminalRectWithItsOwnLayerFillColor's
    // own comment). DOTS is used deliberately, not BRICK/stripes: its
    // tile geometry is identical regardless of which layer/color uses it
    // (pattern_shader's own dot is always centered in its 12px tile), and
    // both shapes share the exact same canvas/local-matrix phase (same
    // node, same scale) - so M1's own dot grid and M2's own dot grid are
    // pixel-for-pixel IDENTICAL, guaranteeing a real ink/ink intersection
    // to test order at, unlike two out-of-phase pattern types where
    // finding one by construction would need hand-deriving exact tile
    // geometry. Found empirically (render M1 alone, locate one of its own
    // ink pixels) rather than analytically, so this doesn't depend on
    // pattern_shader's own exact tile pixel layout staying unchanged.
    // Both fresh Layers, NOT the fixture's own `m1` (type "ROUTING", from
    // SetUp - its own TERMINAL would get DIAGONAL_STRIPES instead, a
    // different pattern with different tile geometry from M2's DOTS) -
    // "OVERLAP" is an arbitrary non-ROUTING/non-CUT type so both resolve
    // to the same DOTS pattern.
    const LayerId dots1 = root.create_layer(LayerData{.technology = technology_id, .name = "DOTS1", .type = "OVERLAP"});
    const LayerId m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "OVERLAP"});

    const TerminalId m1_terminal = root.create_terminal(TerminalData{.abstract = leaf_abstract, .name = "B1", .direction = SignalDirection::INPUT});
    const TerminalPortId m1_port = root.create_terminal_port(TerminalPortData{.terminal = m1_terminal});
    root.create_shape(ShapeData{.terminal_port = m1_port, .layer = dots1, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});

    const ViewLayerSet one_layer_view_layers = ViewLayerSet::build_for_technology(root, technology_id);
    const ViewLayerSetHandle one_layer_view_layers_handle = std::make_shared<const ViewLayerSet>(one_layer_view_layers);
    const ViewLayerId m1_terminal_layer = one_layer_view_layers.find(dots1, ViewLayerPurpose::TERMINAL);
    ASSERT_TRUE(one_layer_view_layers.get(m1_terminal_layer)->style.fill_pattern == FillPattern::DOTS);
    const Color m1_outline = one_layer_view_layers.get(m1_terminal_layer)->style.outline_color;

    HierarchyResolverRunner m1_only_hierarchy_runner{"HierarchyResolverM1Only"};
    ViewRenderOptions m1_only_options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    m1_only_options.view_layers = one_layer_view_layers_handle;
    m1_only_hierarchy_runner.run(one_layer_view_layers_handle, 0, m1_only_options);
    RasterizeRunner m1_only_rasterize_runner{"RasterizeM1Only"};
    const RasterizeOutput &m1_only_output = m1_only_rasterize_runner.run(m1_only_hierarchy_runner.last_handle(), 0, m1_only_options);
    const sk_sp<SkImage> &m1_only_image = m1_only_output.images.at(HierarchyId{leaf_abstract}).image;

    int ink_x = -1, ink_y = -1;
    {
        SkPixmap pixmap;
        ASSERT_TRUE(m1_only_image->peekPixels(&pixmap));
        const SkColor target = to_sk_color(m1_outline);
        for (int y = 0; y < 100 && ink_x < 0; ++y)
            for (int x = 0; x < 100; ++x)
            {
                const SkColor c = pixmap.getColor(x, y);
                if (std::abs(static_cast<int>(SkColorGetR(c)) - static_cast<int>(SkColorGetR(target))) <= 5 &&
                    std::abs(static_cast<int>(SkColorGetG(c)) - static_cast<int>(SkColorGetG(target))) <= 5 &&
                    std::abs(static_cast<int>(SkColorGetB(c)) - static_cast<int>(SkColorGetB(target))) <= 5 &&
                    std::abs(static_cast<int>(SkColorGetA(c)) - static_cast<int>(SkColorGetA(target))) <= 5)
                {
                    ink_x = x;
                    ink_y = y;
                    break;
                }
            }
    }
    ASSERT_GE(ink_x, 0) << "M1's own DOTS pattern never showed its own outline color anywhere - can't locate an ink pixel to test order at";

    // Now add M2's own fully-overlapping TERMINAL shape (higher
    // ViewLayerId index than M1's, since created after it) and re-render
    // with both present - M2's own DOTS tile shares the exact same phase
    // as M1's (see this test's own top comment), so `(ink_x, ink_y)` is
    // guaranteed to be one of M2's own ink pixels too.
    const TerminalId m2_terminal = root.create_terminal(TerminalData{.abstract = leaf_abstract, .name = "B2", .direction = SignalDirection::INPUT});
    const TerminalPortId m2_port = root.create_terminal_port(TerminalPortData{.terminal = m2_terminal});
    root.create_shape(ShapeData{.terminal_port = m2_port, .layer = m2, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});

    const ViewLayerSet two_layer_view_layers = ViewLayerSet::build_for_technology(root, technology_id);
    const ViewLayerSetHandle two_layer_view_layers_handle = std::make_shared<const ViewLayerSet>(two_layer_view_layers);
    const ViewLayerId m2_terminal_layer = two_layer_view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    ASSERT_LT(m1_terminal_layer.index, m2_terminal_layer.index); // the property this test actually exercises
    const Color m2_outline = two_layer_view_layers.get(m2_terminal_layer)->style.outline_color;
    ASSERT_NE(m1_outline.r, m2_outline.r); // the palette must actually distinguish them, or this test can't tell who won

    HierarchyResolverRunner fresh_hierarchy_runner{"HierarchyResolverTwoLayer"};
    ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    options.view_layers = two_layer_view_layers_handle;
    fresh_hierarchy_runner.run(two_layer_view_layers_handle, 0, options);
    RasterizeRunner fresh_rasterize_runner{"RasterizeTwoLayer"};
    const RasterizeOutput &output = fresh_rasterize_runner.run(fresh_hierarchy_runner.last_handle(), 0, options);
    const sk_sp<SkImage> &image = output.images.at(HierarchyId{leaf_abstract}).image;

    // At the exact pixel where M1's own pattern alone showed ink, the
    // combined render should now show M2's own ink color instead - proof
    // M2 (the later ViewLayer) drew on top of M1 (the earlier one) at a
    // point where both genuinely overlap, not just that both drew
    // *something*, somewhere.
    EXPECT_TRUE(region_contains_color_near(image, ink_x, ink_y, ink_x + 1, ink_y + 1, to_sk_color(m2_outline), 5));
}

TEST_F(RasterizeStageFixture, NullInputProducesEmptyOutput)
{
    const ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    const RasterizeOutput &output = rasterize_runner.run(nullptr, 0, options);
    EXPECT_TRUE(output.images.empty());
    EXPECT_EQ(output.culled, nullptr);
}
