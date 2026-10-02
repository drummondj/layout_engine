#include "geometry/row_geometry.hpp"

#include <gtest/gtest.h>

using namespace le;

namespace
{
    // Site 100x1000 dbu; ROW0 at (0,0) spans 5 sites, (0,0)-(500,1000);
    // ROW1 at (0,1000) spans 1 site, (0,1000)-(100,2000).
    struct RowGeometryFixture : ::testing::Test
    {
        Root root;
        LayoutId layout;
        RowId row0;
        RowId row1;

        void SetUp() override
        {
            const TechnologyId tech = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            SiteData site{};
            site.technology = tech;
            site.name = "CORE";
            site.size = Point{100, 1000};
            root.create_site(site);

            const LibraryId library = root.create_library(LibraryData{.name = "LIB"});
            layout = root.create_layout(LayoutData{.design = root.create_design(DesignData{.library = library, .name = "TOP"})});
            row0 = root.create_row(RowData{.layout = layout, .name = "ROW0", .site_name = "CORE", .origin = Point{0, 0}, .num_x = 5});
            row1 = root.create_row(RowData{.layout = layout, .name = "ROW1", .site_name = "CORE", .origin = Point{0, 1000}});
        }
    };
}

TEST_F(RowGeometryFixture, PointHitTestFindsEveryRowContainingThePointEdgesInclusive)
{
    EXPECT_EQ(hit_test_rows_point_all(root, layout, Point{250, 500}), std::vector<RowId>{row0});
    EXPECT_EQ(hit_test_rows_point_all(root, layout, Point{50, 1000}), (std::vector<RowId>{row0, row1})); // shared edge
    EXPECT_TRUE(hit_test_rows_point_all(root, layout, Point{300, 1500}).empty());
}

TEST_F(RowGeometryFixture, RectHitTestFindsOnlyFullyEnclosedRows)
{
    EXPECT_EQ(hit_test_rows_rect(root, layout, Rect{.ll = {0, 0}, .ur = {500, 1500}}), std::vector<RowId>{row0});
    EXPECT_EQ(hit_test_rows_rect(root, layout, Rect{.ll = {0, 0}, .ur = {500, 2000}}), (std::vector<RowId>{row0, row1}));
    EXPECT_TRUE(hit_test_rows_rect(root, layout, Rect{.ll = {0, 0}, .ur = {499, 999}}).empty());
}

TEST_F(RowGeometryFixture, RowsWithAnUnresolvedSiteAreNeverHit)
{
    root.create_row(RowData{.layout = layout, .name = "ORPHAN", .site_name = "NO_SUCH_SITE", .origin = Point{0, 0}});
    EXPECT_EQ(hit_test_rows_point_all(root, layout, Point{50, 500}), std::vector<RowId>{row0});
}
