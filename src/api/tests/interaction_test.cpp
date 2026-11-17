#include "views.hpp"

#include <gtest/gtest.h>

#include <string>
#include <variant>

// The design view's interaction through the C++ views: modes, keys, click
// and drag select, rectangle zoom, rulers, Move, Delete and rotate/flip.

namespace
{
    std::string fixture_path(const std::string &name) { return std::string(API_TEST_FIXTURES_DIR) + "/" + name; }

    struct InteractionFixture : public ::testing::Test
    {
        void SetUp() override
        {
            {
                le::WriteView view = session.write();
                ASSERT_TRUE(view.read_lef(fixture_path("testcell.lef"), "testcell"));
                const le::DesignId cell = view.root().get_design_ids().front();
                const le::LibraryId library = view.create_library({.name = "TOPLIB"}).value();
                top = view.create_design({.library = library, .name = "TOP"}).value();
                const le::LayoutId layout = view.create_layout({.design = top}).value();
                const le::RouteId route = view.create_route({.layout = layout, .name = "NET1"}).value();
                const le::Units units = view.units().value();

                // A 2x2 um route shape at the origin, and TESTCELL placed at (10,10) um.
                shape = view.create_shape({.owner = le::ShapeOwner::route(route),
                                           .layer = view.root().get_layer_by_name("M1"),
                                           .rects = {units.to_dbu(le::RectUm{{0.0, 0.0}, {2.0, 2.0}})}})
                            .value();
                placement = view.create_placement({.layout = layout,
                                                   .name = "U1",
                                                   .reference_design = cell,
                                                   .placement_status = le::PlacementStatus::PLACED,
                                                   .location = units.to_dbu(le::PointUm{10.0, 10.0}),
                                                   .orientation = le::Orientation::N})
                                .value();
                ASSERT_TRUE(view.open_layout(top));
            }
            le::WriteView view = session.write();
            view.set_hierarchy_depth(1);
            view.set_viewport_size(100, 100);
            view.zoom(0.005 - 1.0, 0, 100); // 0.2 um per pixel, (0,0) um at pixel (0,100)
        }

        void click(int x, int y)
        {
            le::WriteView view = session.write();
            view.mouse_down(x, y);
            view.mouse_up(x, y);
        }

        size_t selected() const { return session.read().selection().size(); }

        bool shape_selected() const
        {
            const le::ReadView view = session.read();
            for (const le::SelectedObject &entry : view.selection())
                if (const le::ShapePiece *piece = std::get_if<le::ShapePiece>(&entry); piece && piece->owner() == le::GeometryId(shape))
                    return true;
            return false;
        }

        bool placement_selected() const
        {
            const le::ReadView view = session.read();
            for (const le::SelectedObject &entry : view.selection())
                if (const le::PlacementId *id = std::get_if<le::PlacementId>(&entry); id && *id == placement)
                    return true;
            return false;
        }

        le::Session session;
        le::DesignId top;
        le::ShapeId shape;
        le::PlacementId placement;
    };
}

TEST_F(InteractionFixture, ModesSwitchThroughTheViewAndKeys)
{
    EXPECT_EQ(session.read().mode(), le::InteractionMode::SELECT);
    session.write().set_mode(le::InteractionMode::EDIT);
    EXPECT_EQ(session.read().mode(), le::InteractionMode::EDIT);

    session.write().key_down(le::Key::RULER_MODE);
    EXPECT_EQ(session.read().mode(), le::InteractionMode::RULER);

    // A mode key with an extra modifier held does nothing.
    {
        le::WriteView view = session.write();
        view.key_down(le::Key::SHIFT);
        view.key_down(le::Key::SELECT_MODE);
    }
    EXPECT_EQ(session.read().mode(), le::InteractionMode::RULER);
    session.write().key_up(le::Key::SHIFT);
    session.write().key_down(le::Key::SELECT_MODE);
    EXPECT_EQ(session.read().mode(), le::InteractionMode::SELECT);
}

TEST_F(InteractionFixture, TheTooltipFollowsTheMode)
{
    const std::string select(session.read().tooltip());
    session.write().set_mode(le::InteractionMode::EDIT);
    const std::string edit(session.read().tooltip());
    session.write().set_mode(le::InteractionMode::RULER);
    const std::string ruler(session.read().tooltip());
    EXPECT_FALSE(select.empty());
    EXPECT_NE(select, edit);
    EXPECT_NE(edit, ruler);
    EXPECT_NE(edit.find("Ctrl-M"), std::string::npos);
}

TEST_F(InteractionFixture, ModifiersAreHeldUntilReleased)
{
    session.write().key_down(le::Key::CTRL);
    session.write().key_down(le::Key::SHIFT);
    EXPECT_TRUE(session.read().key_held(le::Key::CTRL));
    EXPECT_TRUE(session.read().key_held(le::Key::SHIFT));
    session.write().key_up(le::Key::CTRL);
    EXPECT_FALSE(session.read().key_held(le::Key::CTRL));
    session.write().clear_keys();
    EXPECT_FALSE(session.read().key_held(le::Key::SHIFT));
}

TEST_F(InteractionFixture, AClickSelectsWhatIsUnderItAndShiftAdds)
{
    click(5, 95); // (1,1) um - the route shape
    EXPECT_EQ(selected(), 1u);
    EXPECT_TRUE(shape_selected());

    click(60, 40); // (12,12) um - inside the placement, replacing the selection
    EXPECT_EQ(selected(), 1u);
    EXPECT_TRUE(placement_selected());

    session.write().key_down(le::Key::SHIFT);
    click(5, 95);
    EXPECT_EQ(selected(), 2u);
    EXPECT_TRUE(shape_selected());
    EXPECT_TRUE(placement_selected());
    session.write().key_up(le::Key::SHIFT);

    click(95, 95); // (19,1) um - nothing
    EXPECT_EQ(selected(), 0u);
}

TEST_F(InteractionFixture, ADragSelectsWhatItEncloses)
{
    {
        le::WriteView view = session.write();
        view.mouse_down(-2, 102);
        view.mouse_up(15, 85); // (-0.4,-0.4)..(3,3) um
    }
    EXPECT_EQ(selected(), 1u);
    EXPECT_TRUE(shape_selected());
}

TEST_F(InteractionFixture, AMouseUpWithoutAMouseDownDoesNothing)
{
    click(5, 95);
    session.write().mouse_up(95, 95);
    EXPECT_TRUE(shape_selected());

    // A cancelled drag ends nothing either.
    {
        le::WriteView view = session.write();
        view.mouse_down(95, 95);
        view.cancel_drag();
        view.mouse_up(95, 95);
    }
    EXPECT_TRUE(shape_selected());
}

TEST_F(InteractionFixture, ARectangleZoomZoomsWithoutChangingTheSelection)
{
    click(5, 95);
    const double before = session.read().view().scale;
    {
        le::WriteView view = session.write();
        view.zoom_drag_down(0, 100);
        view.mouse_up(50, 50);
    }
    EXPECT_GT(session.read().view().scale, before);
    EXPECT_TRUE(shape_selected());

    // A click-sized rectangle doesn't zoom.
    const double zoomed = session.read().view().scale;
    {
        le::WriteView view = session.write();
        view.zoom_drag_down(10, 10);
        view.mouse_up(11, 11);
    }
    EXPECT_EQ(session.read().view().scale, zoomed);
}

TEST_F(InteractionFixture, RulerClicksAddPointsAndEscapeFinishes)
{
    session.write().set_mode(le::InteractionMode::RULER);
    for (const auto [x, y] : {std::pair{10, 90}, std::pair{50, 90}})
    {
        session.write().set_mouse_position(x, y);
        click(x, y);
    }
    {
        const le::ReadView view = session.read();
        ASSERT_EQ(view.rulers().size(), 1u);
        EXPECT_EQ(view.rulers()[0].points.size(), 2u);
    }
    session.write().key_down(le::Key::FINISH_RULER);
    session.write().set_mouse_position(70, 70);
    click(70, 70);
    EXPECT_EQ(session.read().rulers().size(), 2u); // a new ruler
}

TEST_F(InteractionFixture, MoveCommitsAsOneUndoStep)
{
    click(5, 95);
    ASSERT_TRUE(shape_selected());
    const le::Rect before = session.read().root().get_shape(shape)->rects[0];
    {
        le::WriteView view = session.write();
        view.set_mode(le::InteractionMode::EDIT);
        view.arm_move();
    }
    EXPECT_TRUE(session.read().move_armed());
    session.write().set_mouse_position(5, 95);
    click(5, 95); // the start point
    EXPECT_TRUE(session.read().move_anchored());
    session.write().set_mouse_position(30, 95); // 5 um right
    click(30, 95);

    const le::Rect after = session.read().root().get_shape(shape)->rects[0];
    EXPECT_GT(after.ll.x, before.ll.x);
    EXPECT_EQ(after.ll.y, before.ll.y);
    EXPECT_TRUE(session.read().move_armed()); // Move stays armed

    EXPECT_EQ(session.write().undo(), true);
    EXPECT_EQ(session.read().root().get_shape(shape)->rects[0].ll.x, before.ll.x);
}

TEST_F(InteractionFixture, DeleteActsInEditModeOnly)
{
    click(5, 95);
    session.write().key_down(le::Key::DELETE);
    EXPECT_NE(session.read().root().get_shape(shape), nullptr);

    session.write().set_mode(le::InteractionMode::EDIT);
    session.write().key_down(le::Key::DELETE);
    EXPECT_EQ(session.read().root().get_shape(shape), nullptr); // its only rect went, so it did too
    EXPECT_EQ(session.read().selection().size(), 0u);

    EXPECT_EQ(session.write().undo(), true);
    EXPECT_NE(session.read().root().get_shape(shape), nullptr);
}

TEST_F(InteractionFixture, DeleteSelectedPiecesCountsWhatItDeleted)
{
    EXPECT_EQ(session.write().delete_selected_pieces(), 0u);
    click(5, 95);
    EXPECT_EQ(session.write().delete_selected_pieces(), 1u);
}

TEST_F(InteractionFixture, OrientationOpErrorsSayWhy)
{
    std::expected<void, le::OrientationOpError> result = session.write().apply_placement_orientation_op(le::OrientationOp::ROTATE_CCW);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), le::OrientationOpError::NOTHING_SELECTED);

    click(60, 40);
    ASSERT_TRUE(placement_selected());
    ASSERT_TRUE(session.write().update_placement(placement, {.placement_status = le::PlacementStatus::FIXED}));
    result = session.write().apply_placement_orientation_op(le::OrientationOp::ROTATE_CCW);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), le::OrientationOpError::LOCKED);
    EXPECT_TRUE(session.read().selected_placements_all_fixed());
    EXPECT_EQ(session.read().selected_locked_placement_count(), 1u);
    EXPECT_EQ(session.read().placement_orientation_ops_enabled(), 0u);
}

TEST_F(InteractionFixture, RotateRotatesTheSelectedPlacement)
{
    click(60, 40);
    ASSERT_NE(session.read().placement_orientation_ops_enabled() & (1u << static_cast<uint32_t>(le::OrientationOp::ROTATE_CCW)), 0u);
    ASSERT_TRUE(session.write().apply_placement_orientation_op(le::OrientationOp::ROTATE_CCW));
    EXPECT_EQ(session.read().root().get_placement(placement)->orientation, le::Orientation::W);
    EXPECT_EQ(session.write().undo(), true);
    EXPECT_EQ(session.read().root().get_placement(placement)->orientation, le::Orientation::N);
}

// Inside an open step (a typed Tcl command or a Transaction), a rotate
// records into it and leaves it open, so edits after it are part of the
// same step.
TEST_F(InteractionFixture, RotateJoinsAnOpenUndoStep)
{
    click(60, 40);
    {
        le::Transaction transaction = session.transaction("rotate and add");
        session.write().create_library({.name = "BEFORE"}).value();
        ASSERT_TRUE(session.write().apply_placement_orientation_op(le::OrientationOp::ROTATE_CCW));
        session.write().create_library({.name = "AFTER"}).value();
    }
    EXPECT_EQ(session.read().root().get_library_ids().size(), 4u); // testcell, TOPLIB and the two new ones
    EXPECT_EQ(session.write().undo(), true);
    EXPECT_EQ(session.read().root().get_library_ids().size(), 2u);
    EXPECT_EQ(session.read().root().get_placement(placement)->orientation, le::Orientation::N);
}

TEST_F(InteractionFixture, SnapModesOnlyTakeModesThatApply)
{
    le::WriteView view = session.write();
    EXPECT_FALSE(view.set_shape_snap_mode(le::PieceKind::PATH, le::ShapeSnapMode::FIN_GRID));
    EXPECT_TRUE(view.set_shape_snap_mode(le::PieceKind::RECT, le::ShapeSnapMode::NONE));
    EXPECT_TRUE(view.set_shape_snap_mode(le::PieceKind::PATH, le::ShapeSnapMode::TRACKS));
    view.set_placement_snap_mode(le::PlacementSnapMode::NONE);
}

TEST_F(InteractionFixture, SnapModesReadBack)
{
    {
        le::WriteView view = session.write();
        view.set_shape_snap_mode(le::PieceKind::RECT, le::ShapeSnapMode::NONE);
        view.set_shape_snap_mode(le::PieceKind::PATH, le::ShapeSnapMode::TRACKS);
        view.set_placement_snap_mode(le::PlacementSnapMode::NONE);
    }
    const le::ReadView view = session.read();
    EXPECT_EQ(view.shape_snap_mode(le::PieceKind::RECT), le::ShapeSnapMode::NONE);
    EXPECT_EQ(view.shape_snap_mode(le::PieceKind::PATH), le::ShapeSnapMode::TRACKS);
    EXPECT_EQ(view.shape_snap_mode(le::PieceKind::VIA), le::ShapeSnapMode::TRACKS); // vias share the path slot
    EXPECT_EQ(view.placement_snap_mode(), le::PlacementSnapMode::NONE);
    EXPECT_TRUE(view.placement_snap_mode_available(le::PlacementSnapMode::NONE));
    EXPECT_TRUE(view.shape_snap_mode_available(le::PieceKind::RECT, le::ShapeSnapMode::USER_GRID));
    EXPECT_FALSE(view.shape_snap_mode_available(le::PieceKind::PATH, le::ShapeSnapMode::FIN_GRID));
}

TEST_F(InteractionFixture, SelectedPieceKindsAreThoseResizeCanChange)
{
    EXPECT_EQ(session.read().selected_piece_kinds(), 0u);
    click(5, 95);
    EXPECT_EQ(session.read().selected_piece_kinds(), 1u << static_cast<uint32_t>(le::PieceKind::RECT));
    EXPECT_EQ(session.read().selected_move_snap_piece_kinds(), 0u); // a rect moves by the shared grid delta
}

TEST_F(InteractionFixture, ResizeArmsInEditModeAndHoversAnEdge)
{
    click(5, 95);
    session.write().arm_resize();
    EXPECT_FALSE(session.read().resize_armed()); // Select mode
    session.write().set_mode(le::InteractionMode::EDIT);
    session.write().key_down(le::Key::CTRL);
    session.write().key_down(le::Key::RULER_MODE); // Ctrl-R
    session.write().key_up(le::Key::CTRL);
    EXPECT_TRUE(session.read().resize_armed());

    session.write().set_mouse_position(10, 95); // the rect's right edge at x = 2 um
    EXPECT_EQ(session.read().resize_hover_axis(), le::ResizeAxis::X);
    session.write().set_mouse_position(60, 40);
    EXPECT_EQ(session.read().resize_hover_axis(), std::nullopt);

    session.write().key_down(le::Key::FINISH_RULER); // Escape disarms
    EXPECT_FALSE(session.read().resize_armed());
}

TEST_F(InteractionFixture, TheSnappedMousePositionFollowsTheMouse)
{
    EXPECT_EQ(session.read().snapped_mouse_position(), std::nullopt);
    session.write().set_mouse_position(50, 50);
    ASSERT_TRUE(session.read().snapped_mouse_position().has_value());
    session.write().clear_mouse_position();
    EXPECT_EQ(session.read().snapped_mouse_position(), std::nullopt);
}
