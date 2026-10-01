#include "core/placement_move.hpp"

#include <gtest/gtest.h>

#include <ostream>

// The generated Point/Rect have no operator== - test-local ones (found
// by ADL, so std::vector<Point> comparisons work too).
namespace le
{
    bool operator==(const Point &a, const Point &b) { return a.x == b.x && a.y == b.y; }
    bool operator==(const Rect &a, const Rect &b) { return a.ll == b.ll && a.ur == b.ur; }
    void PrintTo(const Point &p, std::ostream *os) { *os << "(" << p.x << "," << p.y << ")"; }
}

using namespace le;

TEST(FinGrid, ParsesLef58FinfetWithOffsetAndDirection)
{
    const auto grid = parse_lef58_finfet(" FINFET PITCH 0.048 OFFSET 0.012 VERTICAL ; ", 1000.0);
    ASSERT_TRUE(grid.has_value());
    EXPECT_EQ(*grid, (FinGrid{.pitch = 48, .offset = 12, .horizontal = false}));
}

TEST(FinGrid, OffsetAndDirectionDefaultToZeroAndHorizontal)
{
    const auto grid = parse_lef58_finfet("\"finfet pitch 0.027 ;\"", 2000.0);
    ASSERT_TRUE(grid.has_value());
    EXPECT_EQ(*grid, (FinGrid{.pitch = 54, .offset = 0, .horizontal = true}));
}

TEST(FinGrid, RejectsMissingKeywordOrPitch)
{
    EXPECT_FALSE(parse_lef58_finfet("PITCH 0.048 ;", 1000.0).has_value());
    EXPECT_FALSE(parse_lef58_finfet("FINFET OFFSET 0.0 ;", 1000.0).has_value());
    EXPECT_FALSE(parse_lef58_finfet("FINFET PITCH 0 ;", 1000.0).has_value());
    EXPECT_FALSE(parse_lef58_finfet("FINFET PITCH 0.048 ;", 0.0).has_value());
}

TEST(FinGrid, TechnologyFieldsOverrideTheLibraryProperty)
{
    Root root;
    const TechnologyId tech = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    EXPECT_FALSE(technology_fin_grid(root).has_value());

    PropertyDefinitionData def{};
    def.technology = tech;
    def.owner_type = "library"; // the vendored parser's own lowercase spelling - matched case-insensitively
    def.name = "LEF58_FINFET";
    def.data_type = "S";
    def.default_string = " FINFET PITCH 0.048 OFFSET 0.000 HORIZONTAL ; ";
    root.create_property_definition(def);
    EXPECT_EQ(technology_fin_grid(root), (FinGrid{.pitch = 48, .offset = 0, .horizontal = true}));

    root.update_technology(tech, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                           std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                           /*fin_pitch=*/std::nullopt, /*fin_offset=*/int64_t{6}, /*fin_direction=*/RoutingDirection::V,
                           std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
    EXPECT_EQ(technology_fin_grid(root), (FinGrid{.pitch = 48, .offset = 6, .horizontal = false}));
}

TEST(FinGrid, TechnologyPitchAloneDefinesAGrid)
{
    Root root;
    TechnologyData tech{.database_units_microns = 1000.0};
    tech.fin_pitch = 30;
    root.create_technology(tech);
    EXPECT_EQ(technology_fin_grid(root), (FinGrid{.pitch = 30, .offset = 0, .horizontal = true}));
}

TEST(OrientationOps, RotationAndFlipsComposeAsTheDihedralGroup)
{
    Orientation o = Orientation::N;
    const Orientation rot = orientation_for_op(OrientationOp::ROTATE_CCW);
    o = compose_orientation(rot, o);
    EXPECT_EQ(o, Orientation::W);
    o = compose_orientation(rot, o);
    EXPECT_EQ(o, Orientation::S);
    o = compose_orientation(rot, o);
    EXPECT_EQ(o, Orientation::E);
    o = compose_orientation(rot, o);
    EXPECT_EQ(o, Orientation::N);

    EXPECT_EQ(compose_orientation(orientation_for_op(OrientationOp::FLIP_HORIZONTAL), Orientation::N), Orientation::FN);
    EXPECT_EQ(compose_orientation(orientation_for_op(OrientationOp::FLIP_VERTICAL), Orientation::N), Orientation::FS);
    EXPECT_EQ(compose_orientation(orientation_for_op(OrientationOp::FLIP_VERTICAL), Orientation::FN), Orientation::S);
    EXPECT_EQ(compose_orientation(Orientation::FN, Orientation::FN), Orientation::N);
}

TEST(OrientationOps, AbstractIsCoreMatchesCoreAndItsSubclassesOnly)
{
    AbstractData a{};
    EXPECT_FALSE(abstract_is_core(a));
    a.type = "CORE";
    EXPECT_TRUE(abstract_is_core(a));
    a.type = "core tiehigh";
    EXPECT_TRUE(abstract_is_core(a));
    a.type = "BLOCK";
    EXPECT_FALSE(abstract_is_core(a));
    a.type = "COREX";
    EXPECT_FALSE(abstract_is_core(a));
}

namespace
{
    // One technology (1000 dbu/um, 0.005um manufacturing grid), a
    // 100x1000 dbu CORE site, a 400x1000 CORE cell (SYMMETRY X Y) and a
    // 3000x2000 BLOCK with no SITE, and a layout with two 50-site rows:
    // ROW0 at y=0 (N) and ROW1 at y=1000 (FS), both starting at x=500.
    struct PlacementMoveFixture : ::testing::Test
    {
        Root root;
        LayoutId layout;
        PlacementId cell;
        PlacementId block;
        TechnologyId tech;

        void SetUp() override
        {
            TechnologyData tech_data{.database_units_microns = 1000.0};
            tech_data.manufacturing_grid = 0.005;
            tech = root.create_technology(tech_data);

            SiteData site{};
            site.technology = tech;
            site.name = "CORE";
            site.size = Point{100, 1000};
            site.symmetry = Symmetry{.r90 = false, .x = false, .y = true};
            root.create_site(site);

            const LibraryId library = root.create_library(LibraryData{.name = "LIB"});
            const DesignId cell_design = root.create_design(DesignData{.library = library, .name = "INV"});
            AbstractData cell_abstract{};
            cell_abstract.design = cell_design;
            cell_abstract.type = "CORE";
            cell_abstract.size = Point{400, 1000};
            cell_abstract.symmetry = Symmetry{.r90 = false, .x = true, .y = true};
            cell_abstract.site = "CORE";
            root.create_abstract(cell_abstract);

            const DesignId block_design = root.create_design(DesignData{.library = library, .name = "RAM"});
            AbstractData block_abstract{};
            block_abstract.design = block_design;
            block_abstract.type = "BLOCK";
            block_abstract.size = Point{3000, 2000};
            root.create_abstract(block_abstract);

            const DesignId top = root.create_design(DesignData{.library = library, .name = "TOP"});
            layout = root.create_layout(LayoutData{.design = top});
            for (int i = 0; i < 2; ++i)
            {
                RowData row{};
                row.layout = layout;
                row.name = "ROW" + std::to_string(i);
                row.site_name = "CORE";
                row.origin = Point{500, 1000 * i};
                row.orientation = i == 0 ? Orientation::N : Orientation::FS;
                row.num_x = 50;
                root.create_row(row);
            }

            cell = root.create_placement(PlacementData{.layout = layout, .name = "U1", .reference_design = cell_design,
                                                       .placement_status = PlacementStatus::PLACED, .location = Point{700, 0}, .orientation = Orientation::N});
            block = root.create_placement(PlacementData{.layout = layout, .name = "M1", .reference_design = block_design,
                                                        .placement_status = PlacementStatus::PLACED, .location = Point{10000, 10000}, .orientation = Orientation::N});
        }

        PlacementMoveTarget plan_one(PlacementId id, Point delta, PlacementSnapMode mode, Orientation pending = Orientation::N)
        {
            const std::vector<PlacementId> ids{id};
            const auto targets = plan_placement_move(root, layout, ids, pending, delta, mode, 0);
            EXPECT_EQ(targets.size(), 1u);
            return targets.empty() ? PlacementMoveTarget{} : targets.front();
        }
    };
}

TEST_F(PlacementMoveFixture, NoSnapTranslatesByTheRawDelta)
{
    const PlacementMoveTarget t = plan_one(cell, Point{123, 457}, PlacementSnapMode::NONE);
    EXPECT_EQ(t.location, (Point{823, 457}));
    EXPECT_EQ(t.orientation, Orientation::N);
}

TEST_F(PlacementMoveFixture, RotationKeepsTheBboxCenterFixed)
{
    // 400x1000 at (700,0): center (900,500). Rotated it's 1000x400, so
    // the lower-left moves to (400,300).
    const PlacementMoveTarget t = plan_one(cell, Point{0, 0}, PlacementSnapMode::NONE, Orientation::W);
    EXPECT_EQ(t.orientation, Orientation::W);
    EXPECT_EQ(t.location, (Point{400, 300}));
}

TEST_F(PlacementMoveFixture, ManufacturingGridSnapsBothAxes)
{
    const PlacementMoveTarget t = plan_one(cell, Point{3, 7}, PlacementSnapMode::MANUFACTURING_GRID);
    EXPECT_EQ(t.location, (Point{705, 5}));
}

TEST_F(PlacementMoveFixture, SiteSnapLandsOnTheNearestRowsSiteGrid)
{
    // (700+140, 0+30) -> nearest site x 800 (500 + 3*100), row y 0.
    const PlacementMoveTarget t = plan_one(cell, Point{140, 30}, PlacementSnapMode::SITE);
    EXPECT_EQ(t.location, (Point{800, 0}));
    EXPECT_EQ(t.orientation, Orientation::N);
}

TEST_F(PlacementMoveFixture, SiteSnapForcesTheRowsOrientationFamily)
{
    // Into ROW1 (FS): an N cell becomes FS (a vertical flip into the row's family).
    PlacementMoveTarget t = plan_one(cell, Point{0, 900}, PlacementSnapMode::SITE);
    EXPECT_EQ(t.location, (Point{700, 1000}));
    EXPECT_EQ(t.orientation, Orientation::FS);

    // A user-chosen FN (horizontal flip) is kept in an FS row as S - the
    // row's X-mirror, allowed since the cell is SYMMETRY Y.
    t = plan_one(cell, Point{0, 900}, PlacementSnapMode::SITE, Orientation::FN);
    EXPECT_EQ(t.orientation, Orientation::S);

    // ... and stays FN in an N row.
    t = plan_one(cell, Point{0, 0}, PlacementSnapMode::SITE, Orientation::FN);
    EXPECT_EQ(t.orientation, Orientation::FN);
}

TEST_F(PlacementMoveFixture, SiteSnapClampsToTheRowsExtent)
{
    const PlacementMoveTarget t = plan_one(cell, Point{-5000, 0}, PlacementSnapMode::SITE);
    EXPECT_EQ(t.location, (Point{500, 0}));
}

TEST_F(PlacementMoveFixture, SiteSnapSkipsNonCoreAbstractsFallingBackToTheManufacturingGrid)
{
    const PlacementMoveTarget t = plan_one(block, Point{-9497, -9998}, PlacementSnapMode::SITE);
    EXPECT_EQ(t.location, (Point{505, 0})); // (503, 2) on the 5-dbu grid - not pulled onto the site grid/row
    EXPECT_EQ(t.orientation, Orientation::N);
}

TEST_F(PlacementMoveFixture, SiteSnapOnlyUsesRowsOfTheCellsOwnSite)
{
    // Rename the cell's SITE - no row is built from it, so it falls back
    // to the manufacturing grid instead of the (wrong-site) rows.
    const DesignId design = root.get_placement(cell)->reference_design;
    AbstractData *abstract = root.get_abstract(root.get_design_abstract(design));
    ASSERT_NE(abstract, nullptr);
    abstract->site = "OTHER";
    const PlacementMoveTarget t = plan_one(cell, Point{142, 31}, PlacementSnapMode::SITE);
    EXPECT_EQ(t.location, (Point{840, 30}));
}

TEST_F(PlacementMoveFixture, FinGridSnapsAcrossTheFinsAndManufacturingGridAlong)
{
    TechnologyData *tech_data = root.get_technology(tech);
    tech_data->fin_pitch = 48;
    tech_data->fin_offset = 10;
    const PlacementMoveTarget t = plan_one(cell, Point{3, 100}, PlacementSnapMode::FIN_GRID);
    EXPECT_EQ(t.location, (Point{705, 106})); // y: 10 + 2*48; x: 703 -> 705

    tech_data->fin_direction = RoutingDirection::V;
    const PlacementMoveTarget v = plan_one(cell, Point{3, 102}, PlacementSnapMode::FIN_GRID);
    EXPECT_EQ(v.location, (Point{682, 100})); // x: 10 + 14*48; y: 102 -> 100
}

TEST_F(PlacementMoveFixture, AvailabilityReflectsWhatEachModeNeeds)
{
    EXPECT_TRUE(PlacementSnapper::available(root, layout, PlacementSnapMode::NONE));
    EXPECT_TRUE(PlacementSnapper::available(root, layout, PlacementSnapMode::SITE));
    EXPECT_TRUE(PlacementSnapper::available(root, layout, PlacementSnapMode::MANUFACTURING_GRID));
    EXPECT_FALSE(PlacementSnapper::available(root, layout, PlacementSnapMode::FIN_GRID));
    EXPECT_FALSE(PlacementSnapper::available(root, LayoutId{}, PlacementSnapMode::SITE));
}

TEST_F(PlacementMoveFixture, GhostIsThePlacedBboxPlusAnOrientationMarker)
{
    PlacementMoveTarget t = plan_one(cell, Point{0, 0}, PlacementSnapMode::NONE);
    Shape ghost = placement_move_ghost(t);
    ASSERT_EQ(ghost.rects.size(), 1u);
    EXPECT_EQ(ghost.rects[0], (Rect{.ll = Point{700, 0}, .ur = Point{1100, 1000}}));
    ASSERT_EQ(ghost.polygons.size(), 1u);
    // m = min(400,1000)/4 = 100: legs 200 along local X, 100 along local Y.
    EXPECT_EQ(ghost.polygons[0].points, (std::vector<Point>{{700, 0}, {900, 0}, {700, 100}}));

    // FN mirrors the marker to the lower-right corner, pointing left.
    t = plan_one(cell, Point{0, 0}, PlacementSnapMode::NONE, Orientation::FN);
    ghost = placement_move_ghost(t);
    EXPECT_EQ(ghost.polygons[0].points, (std::vector<Point>{{1100, 0}, {900, 0}, {1100, 100}}));
}

TEST(OrientationOps, RowAllowedOrientationsAreClosedUnderThePermittedOps)
{
    SiteData site{};
    EXPECT_EQ(row_allowed_orientations(Orientation::N, site), (std::vector<Orientation>{Orientation::N}));

    site.symmetry = Symmetry{.r90 = false, .x = false, .y = true};
    EXPECT_EQ(row_allowed_orientations(Orientation::FS, site), (std::vector<Orientation>{Orientation::FS, Orientation::S}));

    site.symmetry = Symmetry{.r90 = false, .x = true, .y = true};
    auto allowed = row_allowed_orientations(Orientation::N, site);
    std::ranges::sort(allowed);
    std::vector<Orientation> expected{Orientation::N, Orientation::S, Orientation::FN, Orientation::FS};
    std::ranges::sort(expected);
    EXPECT_EQ(allowed, expected);

    site.symmetry = Symmetry{.r90 = true, .x = true, .y = true};
    EXPECT_EQ(row_allowed_orientations(Orientation::N, site).size(), 8u);
}

TEST_F(PlacementMoveFixture, SiteSnappingPermitsOnlyTheOpsTheRowSiteSymmetryAllows)
{
    const PlacementData *placement = root.get_placement(cell);
    const AbstractData *abstract = root.get_abstract(root.get_design_abstract(placement->reference_design));

    // The fixture's CORE site is SYMMETRY Y only.
    const PlacementSnapper site(root, layout, PlacementSnapMode::SITE);
    EXPECT_TRUE(site.permits(OrientationOp::FLIP_HORIZONTAL, *placement->location, abstract));
    EXPECT_FALSE(site.permits(OrientationOp::FLIP_VERTICAL, *placement->location, abstract));
    EXPECT_FALSE(site.permits(OrientationOp::ROTATE_CCW, *placement->location, abstract));

    // Any other snap mode, or a non-CORE abstract, is never restricted.
    const PlacementSnapper none(root, layout, PlacementSnapMode::NONE);
    EXPECT_TRUE(none.permits(OrientationOp::ROTATE_CCW, *placement->location, abstract));
    const PlacementData *block_placement = root.get_placement(block);
    const AbstractData *block_abstract = root.get_abstract(root.get_design_abstract(block_placement->reference_design));
    EXPECT_TRUE(site.permits(OrientationOp::ROTATE_CCW, *block_placement->location, block_abstract));
}
