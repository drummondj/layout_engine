#include "session_test.hpp"
#include "../le_handle.hpp"
#include <gtest/gtest.h>

#include <string>

// Deleting selected shape pieces (the
// Edit-mode Delete button, the Del key, `delete_selected_pieces`): only the
// selected rects/polygons/paths/vias/via arrays go (and a shape left with
// no geometry) - never owners or other selected objects - as one undoable
// step.

namespace
{
    using le::test::fixture_path;

    struct DeletePiecesFixture : le::test::SessionTest
    {
        void SetUp() override
        {
            ASSERT_TRUE(read_lef(fixture_path("testcell.lef").c_str(), "testcell"));
            le::Root &root = handle->root;
            const le::LibraryId library = root.create_library(le::LibraryData{.name = "TOPLIB"});
            const le::DesignId design = root.create_design(le::DesignData{.library = library, .name = "TOP"});
            const le::LayoutId layout = root.create_layout(le::LayoutData{.design = design});
            const le::RouteId route = root.create_route(le::RouteData{.layout = layout, .name = "NET1"});
            shape = root.create_shape(le::ShapeData{
                .owner = le::ShapeOwner::route(route),
                .layer = root.get_layer_by_name("M1"),
                .paths = {le::Path{.width = 10, .polygon = {.points = {le::Point{0, 0}, le::Point{100, 0}}}}},
                .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}, le::Rect{.ll = {20, 0}, .ur = {30, 10}}, le::Rect{.ll = {40, 0}, .ur = {50, 10}}},
                .vias = {le::ShapeVia{.via_name = "V0", .origin = le::Point{5, 5}}, le::ShapeVia{.via_name = "V1", .origin = le::Point{45, 5}}},
            });
            const le::DesignId cell = root.get_design_ids().front();
            placement = root.create_placement(le::PlacementData{.layout = layout, .name = "U1", .reference_design = cell, .placement_status = le::PlacementStatus::PLACED,
                                                                .location = le::Point{1000, 1000}, .orientation = le::Orientation::N});
            root.bump_mutation_version();
        }

        const le::ShapeData &data() const { return *handle->root.get_shape(shape); }
        le::ShapeId shape;
        le::PlacementId placement;
    };
}

TEST_F(DeletePiecesFixture, DeletesOnlyTheSelectedPiecesAndKeepsTheShapeAndOtherSelections)
{
    handle->select(shape, le::PieceKind::RECT, 0);
    handle->select(shape, le::PieceKind::RECT, 2);
    handle->select(shape, le::PieceKind::PATH, 0);
    handle->select(shape, le::PieceKind::VIA, 1);
    handle->select(placement);

    EXPECT_EQ(selected_shape_piece_count(), 4);
    EXPECT_EQ(static_cast<int>(write().delete_selected_pieces()), 4);

    ASSERT_NE(handle->root.get_shape(shape), nullptr);
    ASSERT_EQ(data().rects.size(), 1u);
    EXPECT_EQ(data().rects[0].ll.x, 20); // the unselected middle rect
    EXPECT_TRUE(data().paths.empty());
    ASSERT_EQ(data().vias.size(), 1u);
    EXPECT_EQ(data().vias[0].via_name, "V0");
    EXPECT_NE(handle->root.get_placement(placement), nullptr);
    EXPECT_EQ(selection_count(), 1); // only the placement is left selected
    EXPECT_EQ(selected_shape_piece_count(), 0);
}

TEST_F(DeletePiecesFixture, IsOneUndoableStep)
{
    const le::ShapeData before = data();
    handle->select(shape, le::PieceKind::RECT, 1);
    handle->select(shape, le::PieceKind::VIA, 0);
    ASSERT_EQ(static_cast<int>(write().delete_selected_pieces()), 2);

    ASSERT_NE(write().undo(), 0); // nonzero: something was undone
    EXPECT_EQ(data().rects.size(), before.rects.size());
    EXPECT_EQ(data().rects[1].ll.x, 20);
    ASSERT_EQ(data().vias.size(), 2u);
    EXPECT_EQ(data().vias[0].via_name, "V0");

    ASSERT_NE(write().redo(), 0);
    EXPECT_EQ(data().rects.size(), 2u);
    EXPECT_EQ(data().vias.size(), 1u);
}

TEST_F(DeletePiecesFixture, AShapeLeftWithNoGeometryIsDeletedAndUndoBringsItBackWhole)
{
    const le::RouteId route = data().route();
    for (size_t i = 0; i < 3; ++i)
        handle->select(shape, le::PieceKind::RECT, i);
    handle->select(shape, le::PieceKind::PATH, 0);
    handle->select(shape, le::PieceKind::VIA, 0);
    handle->select(shape, le::PieceKind::VIA, 1);
    EXPECT_EQ(static_cast<int>(write().delete_selected_pieces()), 6);
    EXPECT_EQ(handle->root.get_shape(shape), nullptr);
    EXPECT_TRUE(handle->root.get_route_shapes(route).empty());
    EXPECT_NE(handle->root.get_route(route), nullptr); // its owner stays
    EXPECT_EQ(selection_count(), 0);

    ASSERT_NE(write().undo(), 0);
    ASSERT_EQ(handle->root.get_route_shapes(route).size(), 1u);
    const le::ShapeData &restored = *handle->root.get_shape(handle->root.get_route_shapes(route).front());
    EXPECT_EQ(restored.rects.size(), 3u);
    EXPECT_EQ(restored.paths.size(), 1u);
    EXPECT_EQ(restored.vias.size(), 2u);

    ASSERT_NE(write().redo(), 0);
    EXPECT_TRUE(handle->root.get_route_shapes(route).empty());
}

TEST_F(DeletePiecesFixture, AShapeWithGeometryLeftIsKept)
{
    for (size_t i = 0; i < 3; ++i)
        handle->select(shape, le::PieceKind::RECT, i);
    EXPECT_EQ(static_cast<int>(write().delete_selected_pieces()), 3);
    ASSERT_NE(handle->root.get_shape(shape), nullptr); // its path and vias remain
    EXPECT_TRUE(data().rects.empty());
    EXPECT_EQ(data().paths.size(), 1u);
}

TEST_F(DeletePiecesFixture, ARectsDefMaskGoesWithIt)
{
    handle->root.get_shape(shape)->rect_masks = {1, 2, 3};
    handle->select(shape, le::PieceKind::RECT, 1);
    ASSERT_EQ(static_cast<int>(write().delete_selected_pieces()), 1);
    EXPECT_EQ(data().rect_masks, (std::vector<int>{1, 3})); // still index-parallel to the rects
    ASSERT_NE(write().undo(), 0);
    EXPECT_EQ(data().rect_masks, (std::vector<int>{1, 2, 3}));
}

TEST_F(DeletePiecesFixture, WithNoPiecesSelectedNothingIsDeletedOrRecorded)
{
    handle->select(placement);
    EXPECT_EQ(static_cast<int>(write().delete_selected_pieces()), 0);
    EXPECT_EQ(read().can_undo(), 0);
    EXPECT_EQ(data().rects.size(), 3u);
}

TEST_F(DeletePiecesFixture, TheDelKeyDeletesInEditModeOnly)
{
    handle->select(shape, le::PieceKind::RECT, 0);

    write().set_mode(le::InteractionMode::SELECT);
    write().key_down(le::Key::DELETE);
    write().key_up(le::Key::DELETE);
    EXPECT_EQ(data().rects.size(), 3u);

    write().set_mode(le::InteractionMode::EDIT);
    write().key_down(le::Key::CTRL);
    write().key_down(le::Key::DELETE); // bare only
    write().key_up(le::Key::DELETE);
    write().key_up(le::Key::CTRL);
    EXPECT_EQ(data().rects.size(), 3u);

    write().key_down(le::Key::DELETE);
    write().key_up(le::Key::DELETE);
    EXPECT_EQ(data().rects.size(), 2u);
}
