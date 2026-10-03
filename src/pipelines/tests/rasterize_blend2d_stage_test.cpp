#include "../stages/rasterize_blend2d_stage.hpp"
#include "synchronous_stage_runner.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>

using namespace le;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using RasterizeBlend2DRunner = SynchronousStageRunner<RasterizeBlend2DStage, HierarchyResolverStage::OutputHandle, RasterizeOutput, ViewRenderOptions>;

    struct SampledColor
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 0;
    };

    // A BLImage's own BL_FORMAT_PRGB32 pixel data is premultiplied BGRA
    // in memory on this little-endian target - unpremultiply
    // (comparing against a raw, straight ViewLayerStyle color) and
    // reorder to RGBA here.
    SampledColor sample(const BLImage &image, int x, int y)
    {
        BLImageData data;
        if (image.is_empty() || image.get_data(&data) != BL_SUCCESS)
            return SampledColor{};
        const auto *row = static_cast<const uint8_t *>(data.pixel_data) + static_cast<std::ptrdiff_t>(y) * data.stride;
        const uint8_t *px = row + static_cast<std::ptrdiff_t>(x) * 4;
        const uint8_t b = px[0];
        const uint8_t g = px[1];
        const uint8_t r = px[2];
        const uint8_t a = px[3];
        if (a == 0)
            return SampledColor{};
        const auto unpremul = [a](uint8_t c)
        { return static_cast<uint8_t>(std::min(255, (static_cast<int>(c) * 255 + a / 2) / a)); };
        return SampledColor{unpremul(r), unpremul(g), unpremul(b), a};
    }

    // Boundary (0,0)-(10,10), one Terminal rect
    // (1,1)-(2,2) and one Obstruction rect (3,3)-(4,4), both on the M1
    // routing layer.
    struct RasterizeBlend2DStageFixture : public ::testing::Test
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
                .hierarchy_depth = hierarchy_depth, .viewport = viewport, .scale = scale,
            };
        }

        // A real per-object-type FillPattern means a single hardcoded
        // sample point can legitimately land on a pattern "gap", so tests
        // scan a small block instead. Tolerance is 30 because Blend2D
        // always antialiases: the brick tile's 1px lines are offset by
        // +0.5 to land fully within one pixel row/column (solid 255/255,
        // see pattern_blend2d), but the diagonal-stripe tile's 45-degree
        // lines always split AA coverage along their length (~233/255 max
        // alpha).
        static bool region_contains_color_near(const BLImage &image, int x0, int y0, int x1, int y1, Color expected, int tolerance)
        {
            for (int y = y0; y < y1; ++y)
            {
                for (int x = x0; x < x1; ++x)
                {
                    const SampledColor c = sample(image, x, y);
                    if (std::abs(static_cast<int>(c.r) - static_cast<int>(expected.r)) <= tolerance &&
                        std::abs(static_cast<int>(c.g) - static_cast<int>(expected.g)) <= tolerance &&
                        std::abs(static_cast<int>(c.b) - static_cast<int>(expected.b)) <= tolerance &&
                        std::abs(static_cast<int>(c.a) - static_cast<int>(expected.a)) <= tolerance)
                        return true;
                }
            }
            return false;
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        ViewLayerSet view_layers;
        ViewLayerSetHandle view_layers_handle;
        AbstractId leaf_abstract;
        HierarchyResolverRunner hierarchy_resolver_runner{"HierarchyResolver"};
        HierarchyResolverStage::OutputHandle hierarchy_output;
        RasterizeBlend2DRunner rasterize_runner{"RasterizeBlend2D"};
    };
}

TEST_F(RasterizeBlend2DStageFixture, FillsTerminalRectWithItsOwnLayerFillColor)
{
    const ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    const RasterizeOutput &output = rasterize_runner.run(hierarchy_output, 0, options);

    ASSERT_TRUE(output.images.contains(HierarchyId{leaf_abstract}));
    const BLImage &image = output.images.at(HierarchyId{leaf_abstract}).image;
    ASSERT_FALSE(image.is_empty());
    EXPECT_EQ(image.width(), 100);
    EXPECT_EQ(image.height(), 100);

    const ViewLayerId terminal_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const ViewLayerData *terminal_style = view_layers.get(terminal_layer);
    ASSERT_NE(terminal_style, nullptr);

    // M1 is a ROUTING-type Layer, so its own TERMINAL row draws with a
    // real diagonal-stripe FillPattern (pattern_blend2d's own ink uses the
    // layer's own outline color) - scan the whole rect rather than one
    // exact pixel.
    EXPECT_TRUE(region_contains_color_near(image, 10, 80, 20, 90, terminal_style->style.outline_color, 30));
}

TEST_F(RasterizeBlend2DStageFixture, HidingAPurposeSkipsItsWholeLayerGroupButNotOthers)
{
    ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    options.purpose_visible[ViewLayerPurpose::TERMINAL] = false;
    const RasterizeOutput &output = rasterize_runner.run(hierarchy_output, 0, options);

    const BLImage &image = output.images.at(HierarchyId{leaf_abstract}).image;
    // Terminal rect (1,1)-(2,2) -> pixel (15, 85) - now hidden.
    EXPECT_EQ(sample(image, 15, 85).a, 0u);

    // Obstruction rect (3,3)-(4,4) -> pixel x:[30,40], y:[60,70] - untouched.
    const ViewLayerId obstruction_layer = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    const Color obstruction_outline = view_layers.get(obstruction_layer)->style.outline_color;
    EXPECT_TRUE(region_contains_color_near(image, 30, 60, 40, 70, obstruction_outline, 30));
}

TEST_F(RasterizeBlend2DStageFixture, ShapeFarOutsideTheRenderViewportIsCulledButTheOneInsideStillDraws)
{
    // Confirms draw_view_shapes_blend2d's shapes_index-or-fallback dispatch is
    // wired correctly for this backend.
    const TerminalId far_terminal = root.create_terminal(TerminalData{.abstract = leaf_abstract, .name = "FAR", .direction = SignalDirection::INPUT});
    const TerminalPortId far_port = root.create_terminal_port(TerminalPortData{.terminal = far_terminal});
    root.create_shape(ShapeData{.terminal_port = far_port, .layer = m1, .rects = {Rect{.ll = Point{1000, 1000}, .ur = Point{1001, 1001}}}});

    HierarchyResolverRunner fresh_hierarchy_runner{"HierarchyResolverCullingTest"};
    const ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    fresh_hierarchy_runner.run(view_layers_handle, 0, options);

    RasterizeBlend2DRunner fresh_rasterize_runner{"RasterizeBlend2DCullingTest"};
    const RasterizeOutput &output = fresh_rasterize_runner.run(fresh_hierarchy_runner.last_handle(), 0, options);

    ASSERT_TRUE(output.images.contains(HierarchyId{leaf_abstract}));
    const BLImage &image = output.images.at(HierarchyId{leaf_abstract}).image;
    ASSERT_FALSE(image.is_empty());
    EXPECT_EQ(image.width(), 100);
    EXPECT_EQ(image.height(), 100);

    const ViewLayerId terminal_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const ViewLayerData *terminal_style = view_layers.get(terminal_layer);
    ASSERT_NE(terminal_style, nullptr);
    EXPECT_TRUE(region_contains_color_near(image, 10, 80, 20, 90, terminal_style->style.outline_color, 30));
}

TEST_F(RasterizeBlend2DStageFixture, NullInputProducesEmptyOutput)
{
    const ViewRenderOptions options = options_for(HierarchyId{leaf_abstract}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    const RasterizeOutput &output = rasterize_runner.run(nullptr, 0, options);
    EXPECT_TRUE(output.images.empty());
    EXPECT_EQ(output.culled, nullptr);
}

// A PhysicalPort's direction marker
// draws under everything, so a port label running over its own marker (a
// port on the right edge, label drawn left to right) stays readable.
TEST_F(RasterizeBlend2DStageFixture, PortLabelDrawsOverItsOwnMarker)
{
    const LibraryId library_id = root.create_library(LibraryData{.name = "TOPLIB"});
    const DesignId top_design = root.create_design(DesignData{.library = library_id, .name = "TOP"});
    const LayoutId top_layout = root.create_layout(LayoutData{.design = top_design});
    root.create_shape(ShapeData{.layout = top_layout, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{1000, 0}, Point{1000, 1000}, Point{0, 1000}}}}});
    const PhysicalPortId port = root.create_physical_port(PhysicalPortData{.layout = top_layout, .name = "PORT_WITH_A_LONG_NAME", .direction = SignalDirection::INPUT});
    const PhysicalPortSegmentId segment = root.create_physical_port_segment(PhysicalPortSegmentData{.physical_port = port});
    root.create_shape(ShapeData{.physical_port_segment = segment, .layer = m1, .rects = {Rect{.ll = Point{960, 480}, .ur = Point{1000, 520}}}});

    // 2 px/dbu over (800,400)-(1100,600): 600x400 px. The marker points in
    // to its apex at (1000, 500), capped at 16 px: its base is at
    // (1008, 496..504).
    const Rect viewport{.ll = Point{800, 400}, .ur = Point{1100, 600}};
    HierarchyResolverRunner resolver{"HierarchyResolver"};
    resolver.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 0, viewport, 2.0));
    const RasterizeOutput &output = rasterize_runner.run(resolver.last_handle(), 0, options_for(HierarchyId{top_layout}, 0, viewport, 2.0));
    const BLImage &image = output.images.at(HierarchyId{top_layout}).image;
    ASSERT_EQ(image.width(), 600);

    // Inside the marker, x 1003..1008 / y 498..502 dbu: the label's ink
    // (the M1 outline color) shows through the solid gray.
    const Color label_color = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TERMINAL))->style.outline_color;
    EXPECT_TRUE(region_contains_color_near(image, 406, 196, 416, 204, label_color, 30));
    // And the marker itself is still drawn there.
    EXPECT_TRUE(region_contains_color_near(image, 406, 196, 416, 204, Color{200, 200, 200, 255}, 10));
}

// ChunkVisibility masks (ViewportCullStage's Placement.type/Route.use
// filter): a hidden route shape, and a hidden placement's rect inside the
// batched PLACEMENT shape, aren't drawn; their unhidden siblings are.
TEST_F(RasterizeBlend2DStageFixture, ChunkVisibilityMasksHideRouteShapesAndPlacementsIndividually)
{
    const LibraryId library_id = root.create_library(LibraryData{.name = "TOPLIB"});
    const DesignId small_design = root.create_design(DesignData{.library = library_id, .name = "SMALL"});
    const AbstractId small = root.create_abstract(AbstractData{.design = small_design});
    root.create_shape(ShapeData{.abstract = small, .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{2, 2}}}});
    const LayoutId top = root.create_layout(LayoutData{.design = root.create_design(DesignData{.library = library_id, .name = "TOP"})});
    root.create_shape(ShapeData{.layout = top, .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});
    const PlacementId p0 = root.create_placement(PlacementData{.layout = top, .name = "p0", .reference_design = small_design, .placement_status = PlacementStatus::PLACED, .location = Point{6, 1}, .orientation = Orientation::N});
    root.create_placement(PlacementData{.layout = top, .name = "p1", .reference_design = small_design, .placement_status = PlacementStatus::PLACED, .location = Point{6, 4}, .orientation = Orientation::N});
    const RouteId route = root.create_route(RouteData{.layout = top, .name = "n1"});
    const ShapeId hidden_shape = root.create_shape(ShapeData{.route = route, .layer = m1, .rects = {Rect{.ll = Point{1, 1}, .ur = Point{3, 2}}}});
    root.create_shape(ShapeData{.route = route, .layer = m1, .rects = {Rect{.ll = Point{1, 4}, .ur = Point{3, 5}}}});

    // scale 10 over (0,0)-(10,10): device (x, y) is dbu (x / 10, 10 - y / 10).
    const ViewRenderOptions options = options_for(HierarchyId{top}, 0, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, 10.0);
    HierarchyResolverRunner resolver{"HierarchyResolverMaskTest"};
    resolver.run(view_layers_handle, 0, options);
    HierarchyResolverOutput masked = *resolver.last_handle();
    ViewData &data = masked.view_data.at(HierarchyId{top});

    const ViewLayerId route_layer = view_layers.find(m1, ViewLayerPurpose::ROUTE);
    data.chunk_visibility.assign(data.chunks.size(), nullptr);
    for (std::size_t c = 0; c < data.chunks.size(); ++c)
    {
        const ChunkSourcesHandle &sources = data.chunks[c].sources;
        if (!sources)
            continue;
        ChunkVisibility visibility;
        if (const auto it = sources->shapes.find(route_layer); it != sources->shapes.end())
        {
            std::vector<bool> bits(it->second.size());
            for (std::size_t i = 0; i < bits.size(); ++i)
                bits[i] = it->second[i] == hidden_shape;
            visibility.hidden_shapes.emplace(route_layer, std::move(bits));
        }
        for (const PlacementId id : sources->placements)
            visibility.hidden_placements.push_back(id == p0);
        data.chunk_visibility[c] = std::make_shared<const ChunkVisibility>(std::move(visibility));
    }

    RasterizeBlend2DRunner rasterizer{"RasterizeBlend2DMaskTest"};
    const BLImage &image = rasterizer.run(std::make_shared<const HierarchyResolverOutput>(std::move(masked)), 0, options).images.at(HierarchyId{top}).image;

    const Color route_ink = view_layers.get(route_layer)->style.outline_color;
    const Color placement_ink = view_layers.get(view_layers.placement_view_layer())->style.outline_color;
    EXPECT_FALSE(region_contains_color_near(image, 9, 79, 32, 91, route_ink, 30)); // hidden route shape (1,1)-(3,2)
    EXPECT_TRUE(region_contains_color_near(image, 9, 49, 32, 61, route_ink, 30));  // its sibling (1,4)-(3,5)
    EXPECT_FALSE(region_contains_color_near(image, 59, 69, 82, 91, placement_ink, 30)); // hidden p0 (6,1)-(8,3)
    EXPECT_TRUE(region_contains_color_near(image, 59, 39, 82, 61, placement_ink, 30));  // p1 (6,4)-(8,6)
}

TEST_F(RasterizeBlend2DStageFixture, PortMarkerIsCappedAtSixteenPixels)
{
    // An INPUT port on TOP's left die edge: its marker points in, apex at
    // (0, 420), base 40 dbu out at x = -40 - 40 px at 1 px/dbu, capped to
    // 16 px (base at x = -16).
    const LibraryId library_id = root.create_library(LibraryData{.name = "TOPLIB"});
    const DesignId top_design = root.create_design(DesignData{.library = library_id, .name = "TOP"});
    const LayoutId top_layout = root.create_layout(LayoutData{.design = top_design});
    root.create_shape(ShapeData{.layout = top_layout, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{1000, 1000}}}}});
    const PhysicalPortId port = root.create_physical_port(PhysicalPortData{.layout = top_layout, .name = "IN", .direction = SignalDirection::INPUT});
    const PhysicalPortSegmentId segment = root.create_physical_port_segment(PhysicalPortSegmentData{.physical_port = port});
    root.create_shape(ShapeData{.physical_port_segment = segment, .layer = m1, .rects = {Rect{.ll = Point{0, 400}, .ur = Point{20, 440}}}});

    // Viewport (-100, 300)-(100, 500): pixel (x, y) is dbu (x - 100, 500 - y).
    ViewRenderOptions options = options_for(HierarchyId{top_layout}, 0, Rect{.ll = Point{-100, 300}, .ur = Point{100, 500}}, 1.0);
    options.minor_grid_spacing_dbu = 0;
    options.major_grid_spacing_dbu = 0;
    hierarchy_resolver_runner.run(view_layers_handle, 0, options);
    const RasterizeOutput &output = rasterize_runner.run(hierarchy_resolver_runner.last_handle(), 0, options);

    const BLImage &image = output.images.at(HierarchyId{top_layout}).image;
    ASSERT_EQ(image.width(), 200);
    EXPECT_GT(sample(image, 90, 80).a, 0u);  // dbu (-10, 420): inside the capped marker
    EXPECT_EQ(sample(image, 80, 80).a, 0u);  // dbu (-20, 420): just past its base
    EXPECT_EQ(sample(image, 70, 80).a, 0u);  // dbu (-30, 420): inside the uncapped one
}
