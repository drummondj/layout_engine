#include "session_test.hpp"
#include "../le_handle.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>

// Routed wiring held as a Wire selects, moves and deletes like a route
// Shape: its pieces are those of the Shape it converts to (paths, then
// vias), and every edit is stored back as a Wire and undoes as one.

namespace
{
    using le::test::fixture_path;

    struct WireEditFixture : le::test::SessionTest
    {
        void SetUp() override
        {
            ASSERT_TRUE(read_lef(fixture_path("testcell.lef").c_str(), "testcell")); // M1, 1 um wide
            le::Root &root = handle->root;
            const le::LayerId m1 = root.get_layer_by_name("M1");
            const le::ViaId via = root.create_via(le::ViaData{.technology = root.get_technology_ids().front(), .name = "VIA1"});
            root.create_via_layer(le::ViaLayerData{.via = via, .layer_name = "M1", .rects = {le::Rect{.ll = {-500, -500}, .ur = {500, 500}}}});
            const le::LibraryId library = root.create_library(le::LibraryData{.name = "TOPLIB"});
            const le::DesignId design = root.create_design(le::DesignData{.library = library, .name = "TOP"});
            const le::LayoutId layout = root.create_layout(le::LayoutData{.design = design});
            route = root.create_route(le::RouteData{.layout = layout, .name = "NET1"});

            // A path (1,5)-(11,5) um and a via at its end.
            le::WireBuilder builder(root, m1);
            ASSERT_TRUE(builder.add_path(le::Path{.width = 1000, .polygon = {.points = {le::Point{1000, 5000}, le::Point{11000, 5000}}}}));
            ASSERT_TRUE(builder.add_via(le::WireViaTarget{.via = via}, le::Point{11000, 5000}, le::Orientation::N, 0, 1000));
            wire = root.create_wire(std::move(builder).build(route));
            root.bump_mutation_version();

            ASSERT_TRUE(open_layout(le::DesignId{.index = design.index, .generation = design.generation}));
            write().set_hierarchy_depth(1);
            write().set_viewport_size(100, 100);
            write().zoom(0.005 - 1.0, 0, 100); // 0.2 um per pixel, (0,0) um at pixel (0,100)
        }

        int32_t click(int x, int y)
        {
            write().clear_selection();
            write().mouse_down(x, y);
            write().mouse_up(x, y);
            return selection_count();
        }

        const LeHandle::ShapePiece *selected_piece(size_t i = 0) const
        {
            return std::get_if<LeHandle::ShapePiece>(&handle->selection().at(i));
        }

        const le::WireData *data() const { return handle->root.get_wire(wire); }

        bool is_selected(const LeHandle::ShapePiece &piece) const
        {
            return std::ranges::find(handle->selection(), LeHandle::SelectedObject{piece}) != handle->selection().end();
        }
        le::RouteId route;
        le::WireId wire;
    };
}

TEST_F(WireEditFixture, AClickOnAWireSegmentSelectsItsPathWithOrWithoutARender)
{
    for (const bool rendered : {false, true})
    {
        if (rendered)
            render();
        ASSERT_EQ(click(20, 75), 1) << rendered; // (4,5) um
        const LeHandle::ShapePiece *piece = selected_piece();
        ASSERT_NE(piece, nullptr);
        EXPECT_EQ(piece->wire_id, wire);
        EXPECT_FALSE(piece->shape_id.valid());
        EXPECT_EQ(piece->piece_kind, le::PieceKind::PATH);
        EXPECT_EQ(piece->piece_index, 0u);
        EXPECT_EQ(selected(0), le::ObjectId{wire});
    }
}

TEST_F(WireEditFixture, AClickOnAWireViaSelectsTheVia)
{
    render();
    ASSERT_GE(click(55, 75), 1); // (11,5) um
    const LeHandle::ShapePiece *piece = selected_piece();
    ASSERT_NE(piece, nullptr);
    EXPECT_EQ(piece->wire_id, wire);
    EXPECT_EQ(piece->piece_kind, le::PieceKind::VIA);
    EXPECT_EQ(piece->piece_index, 0u);
}

TEST_F(WireEditFixture, ARubberBandSelectsTheWiresPathAndVia)
{
    render();
    write().mouse_down(0, 90);
    write().set_mouse_position(70, 60);
    write().mouse_up(70, 60);
    ASSERT_EQ(selection_count(), 2);
    for (size_t i = 0; i < 2; ++i)
        EXPECT_EQ(selected_piece(i)->wire_id, wire);
}

TEST_F(WireEditFixture, SelectingTheRouteSelectsEveryPieceOfItsWire)
{
    ASSERT_TRUE(select(route));
    EXPECT_EQ(selection_count(), 2);
    EXPECT_TRUE(is_selected(LeHandle::ShapePiece{.wire_id = wire, .piece_kind = le::PieceKind::PATH, .piece_index = 0}));
    EXPECT_TRUE(is_selected(LeHandle::ShapePiece{.wire_id = wire, .piece_kind = le::PieceKind::VIA, .piece_index = 0}));
}

TEST_F(WireEditFixture, MovingAWirePathKeepsItAWireAndUndoes)
{
    handle->select(wire, le::PieceKind::PATH, 0);
    write().set_mode(le::InteractionMode::EDIT);
    write().set_shape_snap_mode(le::PieceKind::PATH, le::ShapeSnapMode::NONE);
    write().arm_move();
    write().set_mouse_position(20, 75);
    write().mouse_down(20, 75);
    write().mouse_up(20, 75);
    write().set_mouse_position(20, 70); // +1 um in y
    write().mouse_down(20, 70);
    write().mouse_up(20, 70);

    ASSERT_NE(data(), nullptr);
    ASSERT_EQ(data()->segments.size(), 1u);
    EXPECT_EQ(data()->segments[0].y, 6000);
    EXPECT_EQ(data()->segments[0].x, 1000);
    EXPECT_EQ(data()->segments[0].length, 10000);
    ASSERT_EQ(data()->vias.size(), 1u);
    EXPECT_EQ(data()->vias[0].y, 5000); // only the selected piece moves
    EXPECT_TRUE(handle->root.get_route_shapes(route).empty());

    ASSERT_EQ(write().undo(), 1);
    EXPECT_EQ(data()->segments[0].y, 5000);
}

TEST_F(WireEditFixture, DeletingSomeOfAWiresPiecesKeepsTheRestAndUndoes)
{
    handle->select(wire, le::PieceKind::VIA, 0);
    ASSERT_EQ(static_cast<int>(write().delete_selected_pieces()), 1);
    ASSERT_NE(data(), nullptr);
    EXPECT_TRUE(data()->vias.empty());
    EXPECT_EQ(data()->segments.size(), 1u);

    ASSERT_EQ(write().undo(), 1);
    EXPECT_EQ(data()->vias.size(), 1u);
}

TEST_F(WireEditFixture, DeletingEveryPieceOfAWireDeletesItAndUndoBringsItBack)
{
    handle->select(wire, le::PieceKind::VIA, 0);
    handle->select(wire, le::PieceKind::PATH, 0);
    ASSERT_EQ(static_cast<int>(write().delete_selected_pieces()), 2);
    EXPECT_EQ(data(), nullptr);
    EXPECT_TRUE(handle->root.get_route_wires(route).empty());
    EXPECT_NE(handle->root.get_route(route), nullptr); // its owner stays

    ASSERT_EQ(write().undo(), 1);
    ASSERT_EQ(handle->root.get_route_wires(route).size(), 1u);
    const le::WireData *restored = handle->root.get_wire(handle->root.get_route_wires(route).front());
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->segments.size(), 1u);
    EXPECT_EQ(restored->vias.size(), 1u);
}

// ResizingAPathSegmentKeepsTheRouteConnected (api_test.cpp) with the runs
// held by a Wire and, for the down run, a route Shape beside it: the
// followers in both move, and the Wire stays a Wire.
TEST_F(WireEditFixture, ResizingAWireSegmentDragsTheRunsMeetingItInWiresAndShapes)
{
    le::Root &root = handle->root;
    const le::LayerId m1 = root.get_layer_by_name("M1");
    le::WireBuilder builder(root, m1);
    ASSERT_TRUE(builder.add_path(le::Path{.width = 200, .polygon = {.points = {le::Point{1000, 1000}, le::Point{1000, 5000}}}})); // up
    ASSERT_TRUE(builder.add_path(le::Path{.width = 200, .polygon = {.points = {le::Point{1000, 5000}, le::Point{8000, 5000}}}})); // across
    const le::RouteId other = root.create_route(le::RouteData{.layout = root.get_route(route)->layout, .name = "NET2"});
    const le::WireId runs = root.create_wire(std::move(builder).build(other));
    const le::ShapeId down = root.create_shape(le::ShapeData{
        .owner = le::ShapeOwner::route(other),
        .layer = m1,
        .paths = {le::Path{.width = 200, .polygon = {.points = {le::Point{8000, 5000}, le::Point{8000, 1000}}}}},
    });
    root.delete_wire(wire); // out of the way of the clicks
    root.bump_mutation_version();

    write().set_viewport_size(200, 200);
    write().zoom(0.02 / 0.005 - 1.0, 0, 200); // 50 dbu/px, pan (0,0): pixel (x,y) = um (x/20, (200-y)/20)
    write().set_minor_grid_spacing(200);
    handle->select(runs, le::PieceKind::PATH, 1);
    write().set_mode(le::InteractionMode::EDIT);
    write().arm_resize();
    ASSERT_NE(read().resize_armed(), 0);

    for (const auto [x, y] : {std::pair{90, 100}, std::pair{90, 60}}) // grab the across run at y=5um, drop it at y=7um
    {
        write().set_mouse_position(x, y);
        write().mouse_down(x, y);
        write().mouse_up(x, y);
    }

    const le::WireData *wire_data = root.get_wire(runs);
    ASSERT_NE(wire_data, nullptr);
    const le::ShapeData moved = le::wire_to_shape(root, *wire_data);
    ASSERT_EQ(moved.paths.size(), 2u);
    EXPECT_EQ(moved.paths[0].polygon.points[1].y, 7000); // the up run's top followed
    EXPECT_EQ(moved.paths[1].polygon.points[0].y, 7000);
    EXPECT_EQ(moved.paths[1].polygon.points[1].y, 7000);
    EXPECT_EQ(root.get_shape(down)->paths[0].polygon.points[0].y, 7000); // the down run's top followed
    EXPECT_EQ(root.get_shape(down)->paths[0].polygon.points[1].y, 1000);

    ASSERT_EQ(write().undo(), 1);
    EXPECT_EQ(le::wire_to_shape(root, *root.get_wire(runs)).paths[1].polygon.points[0].y, 5000);
    EXPECT_EQ(root.get_shape(down)->paths[0].polygon.points[0].y, 5000);
}
