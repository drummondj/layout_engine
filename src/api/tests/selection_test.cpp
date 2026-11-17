#include "session_test.hpp"
#include "../le_handle.hpp"
#include <gtest/gtest.h>

#include <string>

// Layout-view click and rubber-band selection read their candidates from
// the render tree (the last resolver output's chunk rtrees and sources -
// selection.cpp's layout_candidates), plus anything edited since it, falling
// back to a whole-Layout scan when there's no matching render.

namespace
{
    using le::test::fixture_path;

    struct SelectionFixture : le::test::SessionTest
    {
        void SetUp() override
        {
            ASSERT_TRUE(read_lef(fixture_path("testcell.lef").c_str(), "testcell"));
            const le::test::SessionTest::DesignInfo testcell = library_design_at(0, 0);
            const le::LibraryId library = write().create_library({.name = "TOPLIB"}).value_or(le::LibraryId{});
            top_design = write().create_design({.library = library, .name = "TOP"}).value_or(le::DesignId{});
            const le::LayoutId layout = write().create_layout({.design = top_design}).value_or(le::LayoutId{});

            // A 2x2 um route shape at the origin, and TESTCELL placed at (10,10) um.
            const le::RouteId route = write().create_route({.layout = layout, .name = "NET1", .net = le::NetId{}}).value_or(le::RouteId{});
            m1 = layer_named("M1");
            const double rect_um[4] = {0.0, 0.0, 2.0, 2.0};
            shape = create_rects(le::ShapeOwner::route(route), m1, std::span<const double>(rect_um, 4));
            ASSERT_NE(shape.index, UINT32_MAX);
            placement = create_placement(layout, testcell.id, "U1", le::PointUm{10.0, 10.0}, le::Orientation::N, le::PlacementStatus::PLACED);
            ASSERT_NE(placement.index, UINT32_MAX);

            ASSERT_TRUE(open_layout(top_design));
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
        le::DesignId top_design;
        le::LayerId m1;
        le::ShapeId shape;
        le::PlacementId placement;
    };
}

TEST_F(SelectionFixture, AfterARenderAClickIsAnsweredFromTheRenderTree)
{
    render();
    const std::uint64_t before = handle->render_tree_selections;
    ASSERT_EQ(click(5, 95), 1); // (1,1) um - the route shape
    EXPECT_TRUE(is<le::ShapeId>(selected(0)));
    EXPECT_EQ(selected(0), le::ObjectId{shape});
    ASSERT_EQ(click(60, 40), 1); // (12,12) um - inside the placement
    EXPECT_TRUE(is<le::PlacementId>(selected(0)));
    EXPECT_EQ(click(95, 95), 0); // (19,1) um - nothing
    EXPECT_EQ(handle->render_tree_selections, before + 3);
}

TEST_F(SelectionFixture, AnObjectEditedSinceTheRenderIsFoundAtItsNewPlace)
{
    render();
    const double moved_um[4] = {14.0, 2.0, 16.0, 4.0};
    const le::Rect moved = units().to_dbu(le::RectUm{{moved_um[0], moved_um[1]}, {moved_um[2], moved_um[3]}});
    ASSERT_TRUE(write().update_shape(shape, {.rects = le::CompactVector<le::Rect>{moved}}));
    const std::uint64_t before = handle->render_tree_selections;

    // No render in between: the tree still has the shape at the origin.
    EXPECT_EQ(click(5, 95), 0);
    ASSERT_EQ(click(75, 85), 1); // (15,3) um
    EXPECT_EQ(selected(0), le::ObjectId{shape});
    EXPECT_EQ(handle->render_tree_selections, before + 2);

    render();
    ASSERT_EQ(click(75, 85), 1);
    EXPECT_EQ(selected(0), le::ObjectId{shape});
}

TEST_F(SelectionFixture, ARubberBandIsAnsweredFromTheRenderTree)
{
    render();
    const std::uint64_t before = handle->render_tree_selections;
    write().clear_selection();
    write().mouse_down(-5, 105); // (-1,-1) um
    write().mouse_up(106, -6);   // (21.2,21.2) um - encloses the shape and the 10-20 um placement
    EXPECT_EQ(selection_count(), 2);
    EXPECT_EQ(handle->render_tree_selections, before + 1);
}

TEST_F(SelectionFixture, WithoutAMatchingRenderAClickScansTheLayout)
{
    const std::uint64_t before = handle->render_tree_selections;
    ASSERT_EQ(click(5, 95), 1); // nothing rendered yet
    EXPECT_EQ(selected(0), le::ObjectId{shape});

    render();
    write().set_hierarchy_depth(2); // the render was at depth 1
    ASSERT_EQ(click(5, 95), 1);
    EXPECT_EQ(handle->render_tree_selections, before);
}
