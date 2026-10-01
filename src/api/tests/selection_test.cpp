#include "../le_handle.hpp"
#include <gtest/gtest.h>

#include <string>

// Layout-view click and rubber-band selection read their candidates from
// the render tree (the last resolver output's chunk rtrees and sources -
// api.cpp's layout_candidates), plus anything edited since it, falling
// back to a whole-Layout scan when there's no matching render.

namespace
{
    std::string fixture_path(const std::string &name) { return std::string(API_TEST_FIXTURES_DIR) + "/" + name; }

    struct SelectionFixture : public ::testing::Test
    {
        void SetUp() override
        {
            handle = le_create();
            ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str(), "testcell"), 0);
            const LeDesignInfo testcell = le_library_design_at(handle, 0, 0);
            const LeLibraryId library = le_create_library(handle, "TOPLIB");
            top_design = le_create_design(handle, library, "TOP");
            const LeLayoutId layout = le_create_layout(handle, top_design);

            // A 2x2 um route shape at the origin, and TESTCELL placed at (10,10) um.
            const LeRouteId route = le_create_route(handle, layout, LeNetId{.index = UINT32_MAX, .generation = 0}, "NET1", 0, 0, 0.0, 0, 0.0, nullptr);
            m1 = le_layer_by_name(handle, "M1");
            const double rect_um[4] = {0.0, 0.0, 2.0, 2.0};
            shape = le_create_shape(handle, LeTerminalPortId{.index = UINT32_MAX, .generation = 0}, LeObstructionId{.index = UINT32_MAX, .generation = 0},
                                    LePhysicalPortSegmentId{.index = UINT32_MAX, .generation = 0}, LeBlockageId{.index = UINT32_MAX, .generation = 0}, route,
                                    LeLayoutId{.index = UINT32_MAX, .generation = 0}, LeAbstractId{.index = UINT32_MAX, .generation = 0},
                                    LeAbstractId{.index = UINT32_MAX, .generation = 0}, LeLayoutId{.index = UINT32_MAX, .generation = 0}, m1, nullptr, 0, nullptr, 0, 0,
                                    nullptr, 0, 1, rect_um, 4, 0, 0.0, 0, 0.0, 0);
            ASSERT_NE(shape.index, UINT32_MAX);
            placement = le_create_placement(handle, layout, testcell.id, LeInstanceId{.index = UINT32_MAX, .generation = 0}, "U1", 0, "PLACED", 1, 10.0, 10.0, "N", 0, 0.0, nullptr);
            ASSERT_NE(placement.index, UINT32_MAX);

            ASSERT_EQ(le_set_current_design_layout_by_id(handle, top_design), 0);
            le_set_hierarchy_depth(handle, 1);
            le_set_viewport_size(handle, 100, 100);
            le_zoom(handle, 0.005 - 1.0, 0, 100); // 0.2 um per pixel, (0,0) um at pixel (0,100)
        }
        void TearDown() override { le_destroy(handle); }

        void render() { le_render_pixel_buffer(handle); }

        int32_t click(int x, int y)
        {
            le_deselect_all(handle);
            le_mouse_down(handle, x, y);
            le_mouse_up(handle, x, y);
            return le_selection_count(handle);
        }

        LeHandle *handle = nullptr;
        LeDesignId top_design;
        LeLayerId m1;
        LeShapeId shape;
        LePlacementId placement;
    };
}

TEST_F(SelectionFixture, AfterARenderAClickIsAnsweredFromTheRenderTree)
{
    render();
    const std::uint64_t before = handle->render_tree_selections;
    ASSERT_EQ(click(5, 95), 1); // (1,1) um - the route shape
    EXPECT_EQ(le_selected_object_ref(handle, 0).kind, LE_OBJECT_KIND_SHAPE);
    EXPECT_EQ(le_selected_object_ref(handle, 0).index, shape.index);
    ASSERT_EQ(click(60, 40), 1); // (12,12) um - inside the placement
    EXPECT_EQ(le_selected_object_ref(handle, 0).kind, LE_OBJECT_KIND_PLACEMENT);
    EXPECT_EQ(click(95, 95), 0); // (19,1) um - nothing
    EXPECT_EQ(handle->render_tree_selections, before + 3);
}

TEST_F(SelectionFixture, AnObjectEditedSinceTheRenderIsFoundAtItsNewPlace)
{
    render();
    const double moved_um[4] = {14.0, 2.0, 16.0, 4.0};
    ASSERT_EQ(le_update_shape(handle, shape, 0, m1, nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, moved_um, 4, 0, 0.0, 0, 0.0, 0, 0), 0);
    const std::uint64_t before = handle->render_tree_selections;

    // No render in between: the tree still has the shape at the origin.
    EXPECT_EQ(click(5, 95), 0);
    ASSERT_EQ(click(75, 85), 1); // (15,3) um
    EXPECT_EQ(le_selected_object_ref(handle, 0).index, shape.index);
    EXPECT_EQ(handle->render_tree_selections, before + 2);

    render();
    ASSERT_EQ(click(75, 85), 1);
    EXPECT_EQ(le_selected_object_ref(handle, 0).index, shape.index);
}

TEST_F(SelectionFixture, ARubberBandIsAnsweredFromTheRenderTree)
{
    render();
    const std::uint64_t before = handle->render_tree_selections;
    le_deselect_all(handle);
    le_mouse_down(handle, -5, 105); // (-1,-1) um
    le_mouse_up(handle, 106, -6);   // (21.2,21.2) um - encloses the shape and the 10-20 um placement
    EXPECT_EQ(le_selection_count(handle), 2);
    EXPECT_EQ(handle->render_tree_selections, before + 1);
}

TEST_F(SelectionFixture, WithoutAMatchingRenderAClickScansTheLayout)
{
    const std::uint64_t before = handle->render_tree_selections;
    ASSERT_EQ(click(5, 95), 1); // nothing rendered yet
    EXPECT_EQ(le_selected_object_ref(handle, 0).index, shape.index);

    render();
    le_set_hierarchy_depth(handle, 2); // the render was at depth 1
    ASSERT_EQ(click(5, 95), 1);
    EXPECT_EQ(handle->render_tree_selections, before);
}
