#include <gtest/gtest.h>
#include "../def_reader.hpp"

namespace le
{
    namespace
    {
        std::string complete_fixture_path()
        {
            return std::string(DEF_TEST_DIR) + "/complete.5.8.def";
        }
    }

    class DEFReaderCompleteFixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            result = reader.read_def(complete_fixture_path(), root, "test_lib");
        }

        Root root;
        DEFReader reader;
        int result = -1;
    };

    TEST_F(DEFReaderCompleteFixture, ParsesSuccessfully)
    {
        EXPECT_EQ(result, 0);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesTheDesignAndItsLayout)
    {
        const DesignId design_id = root.get_design_by_name("design");
        ASSERT_TRUE(design_id.valid());
        const LayoutId layout_id = root.get_design_layout(design_id);
        EXPECT_TRUE(layout_id.valid());
    }

    TEST_F(DEFReaderCompleteFixture, DieAreaBecomesABoundaryShapeWithEveryPoint)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id));
        ASSERT_NE(diearea, nullptr);
        EXPECT_EQ(diearea->layer_name, "BOUNDARY");
        ASSERT_EQ(diearea->polygons.size(), 1u);
        // DIEAREA ( -190000 -120000 ) ( -190000 350000 ) ( 190000 350000 )
        //         ( 190000 190000 ) ( 190360 190000 ) ( 190360 -120000 ) ;
        const Polygon &polygon = diearea->polygons.front();
        ASSERT_EQ(polygon.points.size(), 6u);
        EXPECT_EQ(polygon.points[0].x, -190000);
        EXPECT_EQ(polygon.points[0].y, -120000);
        EXPECT_EQ(polygon.points[5].x, 190360);
        EXPECT_EQ(polygon.points[5].y, -120000);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryRowWithCorrectFields)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<RowId> row_ids = root.get_layout_rows(layout_id);
        ASSERT_EQ(row_ids.size(), 24u);

        const RowData *row1 = nullptr;
        const RowData *row2 = nullptr;
        for (const RowId id : row_ids)
        {
            const RowData *row = root.get_row(id);
            ASSERT_NE(row, nullptr);
            if (row->name == "ROW_1")
                row1 = row;
            else if (row->name == "ROW_2")
                row2 = row;
        }

        ASSERT_NE(row1, nullptr);
        EXPECT_EQ(row1->site_name, "CORE");
        ASSERT_TRUE(row1->origin.has_value());
        EXPECT_EQ(row1->origin->x, 1000);
        EXPECT_EQ(row1->origin->y, 1000);
        EXPECT_EQ(row1->orientation, Orientation::N);
        ASSERT_TRUE(row1->num_x.has_value());
        ASSERT_TRUE(row1->num_y.has_value());
        EXPECT_EQ(*row1->num_x, 100);
        EXPECT_EQ(*row1->num_y, 1);
        ASSERT_TRUE(row1->step_x.has_value());
        ASSERT_TRUE(row1->step_y.has_value());
        EXPECT_EQ(*row1->step_x, 700);
        EXPECT_EQ(*row1->step_y, 0);

        ASSERT_NE(row2, nullptr);
        EXPECT_EQ(row2->orientation, Orientation::S);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryTrackWithLayersAndMasks)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<TrackId> track_ids = root.get_layout_tracks(layout_id);
        // TRACKS Y 52 DO 857 STEP 104 MASK 1 ;
        // TRACKS Y 52 DO 857 STEP 104 MASK 1 SAMEMASK LAYER M1 M2 ;
        // TRACKS X 52 DO 1720 STEP 104 MASK 2 LAYER M2 ;
        // TRACKS X 52 DO 1720 STEP 104 ;
        ASSERT_EQ(track_ids.size(), 4u);

        const TrackData *first = root.get_track(track_ids[0]);
        ASSERT_NE(first, nullptr);
        EXPECT_FALSE(first->is_x);
        EXPECT_EQ(first->start, 52);
        EXPECT_EQ(first->count, 857);
        EXPECT_EQ(first->step, 104);
        ASSERT_TRUE(first->mask.has_value());
        EXPECT_EQ(*first->mask, 1);
        EXPECT_FALSE(first->same_mask);

        const TrackData *second = root.get_track(track_ids[1]);
        ASSERT_NE(second, nullptr);
        EXPECT_TRUE(second->same_mask);
        ASSERT_EQ(second->layer_names.size(), 2u);
        EXPECT_EQ(second->layer_names[0], "M1");
        EXPECT_EQ(second->layer_names[1], "M2");

        const TrackData *last = root.get_track(track_ids[3]);
        ASSERT_NE(last, nullptr);
        EXPECT_TRUE(last->is_x);
        EXPECT_FALSE(last->mask.has_value());
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryGcellGrid)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<GCellGridId> grid_ids = root.get_layout_gcell_grids(layout_id);
        // GCELLGRID X 0 DO 100 STEP 600 ;
        // GCELLGRID Y 10 DO 120 STEP 400 ;
        ASSERT_EQ(grid_ids.size(), 2u);

        const GCellGridData *x_grid = root.get_g_cell_grid(grid_ids[0]);
        ASSERT_NE(x_grid, nullptr);
        EXPECT_TRUE(x_grid->is_x);
        EXPECT_EQ(x_grid->start, 0);
        EXPECT_EQ(x_grid->count, 100);
        EXPECT_EQ(x_grid->step, 600);

        const GCellGridData *y_grid = root.get_g_cell_grid(grid_ids[1]);
        ASSERT_NE(y_grid, nullptr);
        EXPECT_FALSE(y_grid->is_x);
        EXPECT_EQ(y_grid->start, 10);
        EXPECT_EQ(y_grid->count, 120);
        EXPECT_EQ(y_grid->step, 400);
    }

    TEST_F(DEFReaderCompleteFixture, CreatesEveryPlacementWithCorrectFields)
    {
        const DesignId design_id = root.get_design_by_name("design");
        const LayoutId layout_id = root.get_design_layout(design_id);
        const std::vector<PlacementId> placement_ids = root.get_layout_placements(layout_id);
        // COMPONENTS declares a stale count of 13, but the fixture actually
        // has 43 real "- ..." entries (mostly bracket/escaping edge cases
        // for name parsing) - the vendored parser doesn't enforce the
        // declared count.
        ASSERT_EQ(placement_ids.size(), 43u);

        auto find_by_name = [&](const std::string &name) -> const PlacementData *
        {
            for (const PlacementId id : placement_ids)
            {
                const PlacementData *placement = root.get_placement(id);
                if (placement && placement->name == name)
                    return placement;
            }
            return nullptr;
        };

        const PlacementData *i1 = find_by_name("I1");
        ASSERT_NE(i1, nullptr);
        EXPECT_EQ(i1->reference_name, "B");
        // No LEF was read in this fixture, so the referenced macro/design
        // is never linked - reference_design stays invalid, same "linked
        // later" convention as Instance.reference_design.
        EXPECT_FALSE(i1->reference_design.valid());
        EXPECT_EQ(i1->placement_status, PlacementStatus::PLACED);
        ASSERT_TRUE(i1->location.has_value());
        EXPECT_EQ(i1->location->x, 100);
        EXPECT_EQ(i1->location->y, 100);
        ASSERT_TRUE(i1->orientation.has_value());
        EXPECT_EQ(*i1->orientation, Orientation::N);
        ASSERT_TRUE(i1->weight.has_value());
        EXPECT_EQ(*i1->weight, 100.0);
        ASSERT_TRUE(i1->source.has_value());
        EXPECT_EQ(*i1->source, "NETLIST");

        const PlacementData *i2 = find_by_name("I2");
        ASSERT_NE(i2, nullptr);
        EXPECT_EQ(i2->reference_name, "A");
        ASSERT_TRUE(i2->orientation.has_value());
        EXPECT_EQ(*i2->orientation, Orientation::S);
        ASSERT_TRUE(i2->source.has_value());
        EXPECT_EQ(*i2->source, "DIST");

        const PlacementData *i9 = find_by_name("I9");
        ASSERT_NE(i9, nullptr);
        EXPECT_EQ(i9->placement_status, PlacementStatus::FIXED);

        const PlacementData *i10 = find_by_name("I10");
        ASSERT_NE(i10, nullptr);
        EXPECT_EQ(i10->placement_status, PlacementStatus::COVER);

        const PlacementData *i11 = find_by_name("I11");
        ASSERT_NE(i11, nullptr);
        EXPECT_EQ(i11->placement_status, PlacementStatus::UNPLACED);
        EXPECT_FALSE(i11->location.has_value());
        EXPECT_FALSE(i11->orientation.has_value());

        // Bracket/escaping edge cases in instance names parse without
        // crashing or truncating - a handful of representative ones.
        EXPECT_NE(find_by_name("I12[0]"), nullptr);
        EXPECT_NE(find_by_name("I13[0][10]"), nullptr);
        EXPECT_NE(find_by_name("I14\\[1\\]"), nullptr);
        EXPECT_NE(find_by_name("vectormodule[1]/scalarname"), nullptr);
    }

    TEST(DEFReaderErrors, FileNotFoundReturnsOne)
    {
        Root root;
        DEFReader reader;
        EXPECT_EQ(reader.read_def("/nonexistent/path.def", root, "test_lib"), 1);
    }
}
