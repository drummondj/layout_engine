#include "../wire_helpers.hpp"
#include <gtest/gtest.h>

#include <limits>

using namespace le;

namespace
{
    // M1 (default width 100) and a route in a layout, with a VIA1.
    struct WireHelpers : public ::testing::Test
    {
        void SetUp() override
        {
            const TechnologyId technology = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology, .name = "M1", .type = "ROUTING", .width = 100});
            via = root.create_via(ViaData{.technology = technology, .name = "VIA1"});
            const DesignId design = root.create_design(DesignData{.library = root.create_library(LibraryData{.name = "L"}), .name = "TOP"});
            layout = root.create_layout(LayoutData{.design = design});
            route = root.create_route(RouteData{.layout = layout, .name = "N"});
        }

        static Path path(int64_t width, std::initializer_list<Point> points) { return Path{.width = width, .polygon = {.points = points}}; }

        Root root;
        LayerId m1;
        ViaId via;
        LayoutId layout;
        RouteId route;
    };
}

TEST_F(WireHelpers, ARefusedPathAddsNothingNotEvenItsWidth)
{
    WireBuilder builder(root, m1);
    EXPECT_FALSE(builder.add_path(path(300, {Point{0, 0}, Point{10, 10}})));                                         // diagonal
    EXPECT_FALSE(builder.add_path(path(300, {Point{0, 0}, Point{int64_t{std::numeric_limits<int32_t>::max()} + 1, 0}}))); // out of range
    EXPECT_FALSE(builder.add_path(path(300, {Point{0, 0}})));                                                        // one point
    EXPECT_TRUE(builder.empty());
    const WireData wire = std::move(builder).build(route);
    EXPECT_TRUE(wire.widths.empty());
}

TEST_F(WireHelpers, AWireConvertsToItsPathsAndViasAndBack)
{
    WireBuilder builder(root, m1);
    ASSERT_TRUE(builder.add_path(path(100, {Point{0, 0}, Point{50, 0}, Point{50, -30}})));
    ASSERT_TRUE(builder.add_path(path(250, {Point{100, 0}, Point{100, 40}}), 2));
    ASSERT_TRUE(builder.add_via(WireViaTarget{.via = via}, Point{50, -30}, Orientation::FS, 120, 250));
    const WireData wire = std::move(builder).build(route);
    ASSERT_EQ(wire.segments.size(), 3u);
    EXPECT_EQ(wire.segments[1].length, -30);
    EXPECT_TRUE(wire.segments[1].vertical);
    EXPECT_EQ(wire.widths.size(), 1u); // 250 once, for the path and the via

    EXPECT_EQ(wire_path_range(wire, 0), (std::pair<std::size_t, std::size_t>{0, 2}));
    EXPECT_EQ(wire_path_range(wire, 1), (std::pair<std::size_t, std::size_t>{2, 3}));
    EXPECT_FALSE(wire_path_range(wire, 2).has_value());

    const ShapeData shape = wire_to_shape(root, wire);
    ASSERT_EQ(shape.paths.size(), 2u);
    EXPECT_EQ(shape.paths[0].polygon.points.size(), 3u);
    EXPECT_EQ(shape.paths[1].width, 250);
    EXPECT_EQ(shape.path_masks, (CompactVector<int>{0, 2}));
    ASSERT_EQ(shape.vias.size(), 1u);
    EXPECT_EQ(shape.vias[0].via_name, "VIA1");
    EXPECT_EQ(shape.vias[0].orientation, Orientation::FS);
    EXPECT_EQ(shape.vias[0].mask, 120);
    EXPECT_EQ(shape.vias[0].width, 250);

    const std::optional<WireData> back = shape_to_wire(root, shape, route);
    ASSERT_TRUE(back.has_value());
    const ShapeData again = wire_to_shape(root, *back);
    EXPECT_EQ(to_string(again), to_string(shape));
    EXPECT_EQ(again.paths[0].polygon.points[2].y, -30);
    EXPECT_EQ(again.vias[0].via_name, "VIA1");
}

TEST_F(WireHelpers, AShapeAWireCantHoldIsRefused)
{
    ShapeData with_rect{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}};
    EXPECT_FALSE(shape_to_wire(root, with_rect, route).has_value());
    ShapeData unknown_via{.layer = m1, .vias = {ShapeVia{.via_name = "NOPE", .origin = {0, 0}}}};
    EXPECT_FALSE(shape_to_wire(root, unknown_via, route).has_value());
}

TEST_F(WireHelpers, ADesignsViaIsPreferredOverTheTechnologysOfTheSameName)
{
    const LayoutViaId own = root.create_layout_via(LayoutViaData{.layout = layout, .name = "VIA1"});
    const WireViaTarget target = resolve_wire_via(root, layout, "VIA1");
    EXPECT_EQ(target.layout_via, own);
    EXPECT_FALSE(target.via.valid());
    EXPECT_EQ(resolve_wire_via(root, LayoutId{}, "VIA1").via, via);
    EXPECT_FALSE(resolve_wire_via(root, layout, "NOPE").valid());
}
