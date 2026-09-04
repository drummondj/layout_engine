#include "../stages/rasterize_stage.hpp"
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

TEST_F(RasterizeStageFixture, NullInputProducesEmptyOutput)
{
    const ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    const RasterizeOutput &output = rasterize_runner.run(nullptr, 0, options);
    EXPECT_TRUE(output.images.empty());
    EXPECT_EQ(output.culled, nullptr);
}
