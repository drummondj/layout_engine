#include "../view_style.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    struct ViewStyleFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        LayerId m2;
        ViewLayerSet view_layers;
    };
}

TEST_F(ViewStyleFixture, CreatesSixPurposesPerLayerPlusSixPseudoLayers)
{
    // 2 layers x 6 purposes (TERMINAL/OBSTRUCTION/TRACK_PREFERRED/
    // TRACK_NON_PREFERRED/ROUTING_BLOCKAGE/ROUTE) + 6 pseudo-ViewLayers
    // with no physical Layer (BOUNDARY/ROW/PLACEMENT_NAME/GCELLGRID/
    // PLACEMENT_BLOCKAGE/REGION) = 18.
    EXPECT_EQ(view_layers.all().size(), 18u);
}

TEST_F(ViewStyleFixture, FindResolvesDistinctViewLayersPerLayerAndPurpose)
{
    ViewLayerId m1_terminal = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    ViewLayerId m1_obstruction = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    ViewLayerId m2_terminal = view_layers.find(m2, ViewLayerPurpose::TERMINAL);

    ASSERT_TRUE(m1_terminal.valid());
    ASSERT_TRUE(m1_obstruction.valid());
    ASSERT_TRUE(m2_terminal.valid());

    EXPECT_NE(m1_terminal, m1_obstruction);
    EXPECT_NE(m1_terminal, m2_terminal);
}

TEST_F(ViewStyleFixture, FindReturnsInvalidForUnknownLayer)
{
    LayerId unknown{999, 0};
    EXPECT_FALSE(view_layers.find(unknown, ViewLayerPurpose::TERMINAL).valid());
}

TEST_F(ViewStyleFixture, BoundaryViewLayerHasNoAssociatedLayer)
{
    ViewLayerId boundary_id = view_layers.boundary_view_layer();
    ASSERT_TRUE(boundary_id.valid());

    const ViewLayerData *boundary = view_layers.get(boundary_id);
    ASSERT_NE(boundary, nullptr);
    EXPECT_EQ(boundary->purpose, ViewLayerPurpose::BOUNDARY);
    EXPECT_FALSE(boundary->layer.valid());
}

TEST_F(ViewStyleFixture, GetReturnsCorrectDataForEachViewLayer)
{
    ViewLayerId m1_obstruction_id = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    const ViewLayerData *data = view_layers.get(m1_obstruction_id);

    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->name, "M1/OBSTRUCTION");
    EXPECT_EQ(data->purpose, ViewLayerPurpose::OBSTRUCTION);
    EXPECT_EQ(data->layer, m1);
}

TEST_F(ViewStyleFixture, DifferentLayersGetDifferentColors)
{
    const ViewLayerData *m1_data = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    const ViewLayerData *m2_data = view_layers.get(view_layers.find(m2, ViewLayerPurpose::TERMINAL));

    ASSERT_NE(m1_data, nullptr);
    ASSERT_NE(m2_data, nullptr);

    const Color &m1_color = m1_data->style.outline_color;
    const Color &m2_color = m2_data->style.outline_color;
    EXPECT_FALSE(m1_color.r == m2_color.r && m1_color.g == m2_color.g && m1_color.b == m2_color.b);
}

TEST_F(ViewStyleFixture, TerminalAndObstructionOfSameLayerShareTheSameColor)
{
    // No fill-pattern differentiation yet (a future update) - for now
    // TERMINAL and OBSTRUCTION of the same physical Layer are visually
    // identical.
    const ViewLayerData *terminal = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    const ViewLayerData *obstruction = view_layers.get(view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION));

    ASSERT_NE(terminal, nullptr);
    ASSERT_NE(obstruction, nullptr);

    const Color &t = terminal->style.outline_color;
    const Color &o = obstruction->style.outline_color;
    EXPECT_EQ(t.r, o.r);
    EXPECT_EQ(t.g, o.g);
    EXPECT_EQ(t.b, o.b);
    EXPECT_EQ(t.a, o.a);
    EXPECT_EQ(terminal->style.fill_color.a, obstruction->style.fill_color.a);
}

TEST_F(ViewStyleFixture, BoundaryColorIsUnaffectedByThePerLayerPaletteAndLighterThanRow)
{
    // BUGS_AND_ENHANCEMENTS.md E8 - row_style()'s own former color
    // ({160, 160, 160}), lighter than row_style()'s own new, darker
    // ({100, 100, 100}) - still a plain outline, no fill, same as
    // row_style().
    const ViewLayerData *boundary = view_layers.get(view_layers.boundary_view_layer());
    ASSERT_NE(boundary, nullptr);

    EXPECT_EQ(boundary->style.outline_color.r, 160);
    EXPECT_EQ(boundary->style.outline_color.g, 160);
    EXPECT_EQ(boundary->style.outline_color.b, 160);
    EXPECT_GT(boundary->style.outline_color.r, 100) << "should be lighter than row_style()'s own 100";
    EXPECT_EQ(boundary->style.fill_color.a, 0);
}

TEST_F(ViewStyleFixture, RowsHasRowThenBoundaryThenPlacementNameThenOneRowPerPhysicalLayerThenThreePseudoRows)
{
    // ROW + BOUNDARY + PLACEMENT_NAME + 2 physical Layers (M1, M2) +
    // GCELLGRID + PLACEMENT_BLOCKAGE + REGION = 8 rows - ROW then BOUNDARY
    // then PLACEMENT_NAME first (BUGS_AND_ENHANCEMENTS.md E8/E13 - this
    // declaration order is also the real draw z-order, see rows()'s own
    // doc comment), everything else unchanged.
    const auto &rows = view_layers.rows();
    ASSERT_EQ(rows.size(), 8u);
    EXPECT_EQ(rows[0].name, "ROW");
    EXPECT_EQ(rows[1].name, "BOUNDARY");
    EXPECT_EQ(rows[2].name, "PLACEMENT_NAME");
    EXPECT_EQ(rows[3].name, "M1");
    EXPECT_EQ(rows[4].name, "M2");
    EXPECT_EQ(rows[5].name, "GCELLGRID");
    EXPECT_EQ(rows[6].name, "PLACEMENT_BLOCKAGE");
    EXPECT_EQ(rows[7].name, "REGION");
}

TEST_F(ViewStyleFixture, PhysicalLayerRowHasTerminalObstructionTrackRoutingBlockageAndRouteColumns)
{
    const auto &row = view_layers.rows().at(3);
    ASSERT_EQ(row.columns.size(), 6u);
    EXPECT_EQ(row.columns[0].purpose, ViewLayerPurpose::TERMINAL);
    EXPECT_EQ(row.columns[0].id, view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    EXPECT_EQ(row.columns[1].purpose, ViewLayerPurpose::OBSTRUCTION);
    EXPECT_EQ(row.columns[1].id, view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION));
    EXPECT_EQ(row.columns[2].purpose, ViewLayerPurpose::TRACK_PREFERRED);
    EXPECT_EQ(row.columns[2].id, view_layers.find(m1, ViewLayerPurpose::TRACK_PREFERRED));
    EXPECT_EQ(row.columns[3].purpose, ViewLayerPurpose::TRACK_NON_PREFERRED);
    EXPECT_EQ(row.columns[3].id, view_layers.find(m1, ViewLayerPurpose::TRACK_NON_PREFERRED));
    EXPECT_EQ(row.columns[4].purpose, ViewLayerPurpose::ROUTING_BLOCKAGE);
    EXPECT_EQ(row.columns[4].id, view_layers.find(m1, ViewLayerPurpose::ROUTING_BLOCKAGE));
    EXPECT_EQ(row.columns[5].purpose, ViewLayerPurpose::ROUTE);
    EXPECT_EQ(row.columns[5].id, view_layers.find(m1, ViewLayerPurpose::ROUTE));
}

TEST_F(ViewStyleFixture, TrackAndRoutingBlockageOfSameLayerShareItsColorButNotObstructionsPattern)
{
    const ViewLayerData *obstruction = view_layers.get(view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION));
    const ViewLayerData *track_preferred = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TRACK_PREFERRED));
    const ViewLayerData *track_non_preferred = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TRACK_NON_PREFERRED));
    const ViewLayerData *routing_blockage = view_layers.get(view_layers.find(m1, ViewLayerPurpose::ROUTING_BLOCKAGE));
    ASSERT_NE(obstruction, nullptr);
    ASSERT_NE(track_preferred, nullptr);
    ASSERT_NE(track_non_preferred, nullptr);
    ASSERT_NE(routing_blockage, nullptr);

    // Same per-Layer color as OBSTRUCTION/TERMINAL...
    EXPECT_EQ(track_preferred->style.outline_color.r, obstruction->style.outline_color.r);
    EXPECT_EQ(track_preferred->style.outline_color.g, obstruction->style.outline_color.g);
    EXPECT_EQ(track_preferred->style.outline_color.b, obstruction->style.outline_color.b);
    EXPECT_EQ(track_non_preferred->style.outline_color.r, obstruction->style.outline_color.r);
    EXPECT_EQ(track_non_preferred->style.outline_color.g, obstruction->style.outline_color.g);
    EXPECT_EQ(track_non_preferred->style.outline_color.b, obstruction->style.outline_color.b);
    EXPECT_EQ(routing_blockage->style.outline_color.r, obstruction->style.outline_color.r);
    EXPECT_EQ(routing_blockage->style.outline_color.g, obstruction->style.outline_color.g);
    EXPECT_EQ(routing_blockage->style.outline_color.b, obstruction->style.outline_color.b);

    // ...but distinct FillPatterns from OBSTRUCTION's own BRICK.
    EXPECT_EQ(track_preferred->style.fill_pattern, FillPattern::NONE);
    EXPECT_EQ(track_non_preferred->style.fill_pattern, FillPattern::NONE);
    EXPECT_EQ(routing_blockage->style.fill_pattern, FillPattern::DOTS);
    EXPECT_EQ(obstruction->style.fill_pattern, FillPattern::BRICK);
}

TEST_F(ViewStyleFixture, TrackAndGCellGridStylesAreDashedButRowAndRoutingBlockageAreNot)
{
    // BUGS_AND_ENHANCEMENTS.md E2 - tracks/gcellgrid lines are dashed
    // scaffolding, distinct from solid real geometry.
    const ViewLayerData *track_preferred = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TRACK_PREFERRED));
    const ViewLayerData *track_non_preferred = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TRACK_NON_PREFERRED));
    const ViewLayerData *routing_blockage = view_layers.get(view_layers.find(m1, ViewLayerPurpose::ROUTING_BLOCKAGE));
    ASSERT_NE(track_preferred, nullptr);
    ASSERT_NE(track_non_preferred, nullptr);
    ASSERT_NE(routing_blockage, nullptr);
    EXPECT_TRUE(track_preferred->style.dashed);
    EXPECT_TRUE(track_non_preferred->style.dashed);
    EXPECT_FALSE(routing_blockage->style.dashed);

    const auto &rows = view_layers.rows();
    const ViewLayerData *row_data = view_layers.get(rows.at(0).columns[0].id);
    const ViewLayerData *gcellgrid_data = view_layers.get(rows.at(5).columns[0].id);
    ASSERT_NE(row_data, nullptr);
    ASSERT_NE(gcellgrid_data, nullptr);
    EXPECT_FALSE(row_data->style.dashed);
    EXPECT_TRUE(gcellgrid_data->style.dashed);
}

TEST_F(ViewStyleFixture, RouteSharesTerminalsOwnFillPatternNotObstructionsOrRoutingBlockages)
{
    // ROUTE (a routed net's real conductor geometry) reads visually as
    // the same kind of thing as TERMINAL (a Terminal's own real pin
    // geometry) - both get terminal_fill_pattern()'s own per-layer-type
    // pattern, not OBSTRUCTION's BRICK or ROUTING_BLOCKAGE's DOTS
    // (keep-out region patterns, a different visual category).
    const ViewLayerData *terminal = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    const ViewLayerData *route = view_layers.get(view_layers.find(m1, ViewLayerPurpose::ROUTE));
    const ViewLayerData *obstruction = view_layers.get(view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION));
    const ViewLayerData *routing_blockage = view_layers.get(view_layers.find(m1, ViewLayerPurpose::ROUTING_BLOCKAGE));
    ASSERT_NE(terminal, nullptr);
    ASSERT_NE(route, nullptr);
    ASSERT_NE(obstruction, nullptr);
    ASSERT_NE(routing_blockage, nullptr);

    EXPECT_EQ(route->style.fill_pattern, terminal->style.fill_pattern);
    EXPECT_NE(route->style.fill_pattern, obstruction->style.fill_pattern);
    EXPECT_NE(route->style.fill_pattern, routing_blockage->style.fill_pattern);

    // Same per-Layer color as every other column of this row too.
    EXPECT_EQ(route->style.outline_color.r, terminal->style.outline_color.r);
    EXPECT_EQ(route->style.outline_color.g, terminal->style.outline_color.g);
    EXPECT_EQ(route->style.outline_color.b, terminal->style.outline_color.b);
}

TEST_F(ViewStyleFixture, BoundaryRowHasASingleBoundaryColumn)
{
    const auto &row = view_layers.rows().at(1);
    ASSERT_EQ(row.columns.size(), 1u);
    EXPECT_EQ(row.columns[0].purpose, ViewLayerPurpose::BOUNDARY);
    EXPECT_EQ(row.columns[0].id, view_layers.boundary_view_layer());
}

TEST_F(ViewStyleFixture, PlacementNameRowHasASingleColumnNoLayerAndIsOneShadeLighterThanBoundary)
{
    // BUGS_AND_ENHANCEMENTS.md E13 - split out from BOUNDARY into its own
    // purpose/row so a placement's own name label can be colored/toggled
    // independently, one shade lighter than boundary_style()'s own color
    // (same "derives from the row above it, one shade lighter" relation
    // boundary_style() itself has to row_style()).
    const auto &placement_name_row = view_layers.rows().at(2);
    ASSERT_EQ(placement_name_row.columns.size(), 1u);
    EXPECT_EQ(placement_name_row.columns[0].purpose, ViewLayerPurpose::PLACEMENT_NAME);
    EXPECT_EQ(placement_name_row.columns[0].id, view_layers.placement_name_view_layer());

    const ViewLayerData *placement_name = view_layers.get(view_layers.placement_name_view_layer());
    ASSERT_NE(placement_name, nullptr);
    EXPECT_FALSE(placement_name->layer.valid());

    const ViewLayerData *boundary = view_layers.get(view_layers.boundary_view_layer());
    ASSERT_NE(boundary, nullptr);
    EXPECT_EQ(placement_name->style.outline_color.r, 220);
    EXPECT_EQ(placement_name->style.outline_color.g, 220);
    EXPECT_EQ(placement_name->style.outline_color.b, 220);
    EXPECT_GT(placement_name->style.outline_color.r, boundary->style.outline_color.r) << "should be lighter than boundary_style()'s own 160";
    EXPECT_EQ(placement_name->style.fill_color.a, 0);
}

TEST_F(ViewStyleFixture, RowGCellGridAndPlacementBlockagePseudoRowsEachHaveTheirOwnSingleColumnAndNoLayer)
{
    const auto &rows = view_layers.rows();

    const auto &row_row = rows.at(0);
    ASSERT_EQ(row_row.columns.size(), 1u);
    EXPECT_EQ(row_row.columns[0].purpose, ViewLayerPurpose::ROW);
    const ViewLayerData *row_data = view_layers.get(row_row.columns[0].id);
    ASSERT_NE(row_data, nullptr);
    EXPECT_FALSE(row_data->layer.valid());

    const auto &gcellgrid_row = rows.at(5);
    ASSERT_EQ(gcellgrid_row.columns.size(), 1u);
    EXPECT_EQ(gcellgrid_row.columns[0].purpose, ViewLayerPurpose::GCELLGRID);
    const ViewLayerData *gcellgrid_data = view_layers.get(gcellgrid_row.columns[0].id);
    ASSERT_NE(gcellgrid_data, nullptr);
    EXPECT_FALSE(gcellgrid_data->layer.valid());

    const auto &placement_blockage_row = rows.at(6);
    ASSERT_EQ(placement_blockage_row.columns.size(), 1u);
    EXPECT_EQ(placement_blockage_row.columns[0].purpose, ViewLayerPurpose::PLACEMENT_BLOCKAGE);
    const ViewLayerData *placement_blockage_data = view_layers.get(placement_blockage_row.columns[0].id);
    ASSERT_NE(placement_blockage_data, nullptr);
    EXPECT_FALSE(placement_blockage_data->layer.valid());

    const auto &region_row = rows.at(7);
    ASSERT_EQ(region_row.columns.size(), 1u);
    EXPECT_EQ(region_row.columns[0].purpose, ViewLayerPurpose::REGION);
    const ViewLayerData *region_data = view_layers.get(region_row.columns[0].id);
    ASSERT_NE(region_data, nullptr);
    EXPECT_FALSE(region_data->layer.valid());
}

TEST_F(ViewStyleFixture, RoutingAndPlacementBlockagesAreDistinctPurposesNotOneSharedBlockagePurpose)
{
    // Routing and placement blockages must be independently toggleable
    // (different purposes for a user - routing keep-out vs. placement
    // keep-out) - not merged under one shared purpose the way TERMINAL/
    // OBSTRUCTION of the same layer never merge either.
    EXPECT_NE(ViewLayerPurpose::ROUTING_BLOCKAGE, ViewLayerPurpose::PLACEMENT_BLOCKAGE);

    const auto &placement_blockage_row = view_layers.rows().at(6);
    EXPECT_NE(placement_blockage_row.columns[0].id, view_layers.find(m1, ViewLayerPurpose::ROUTING_BLOCKAGE));
}

TEST_F(ViewStyleFixture, PurposesListsEachDistinctPurposeOnceInFirstEncounteredOrder)
{
    // ROW then BOUNDARY then PLACEMENT_NAME each contribute their own
    // purpose first (BUGS_AND_ENHANCEMENTS.md E8/E13); M1's row then
    // contributes TERMINAL/OBSTRUCTION/TRACK_PREFERRED/TRACK_NON_PREFERRED/
    // ROUTING_BLOCKAGE/ROUTE; M2's row repeats all six (deduplicated, not
    // appended again); GCELLGRID/PLACEMENT_BLOCKAGE/REGION each contribute
    // their own single new purpose last.
    const auto purposes = view_layers.purposes();
    ASSERT_EQ(purposes.size(), 12u);
    EXPECT_EQ(purposes[0], ViewLayerPurpose::ROW);
    EXPECT_EQ(purposes[1], ViewLayerPurpose::BOUNDARY);
    EXPECT_EQ(purposes[2], ViewLayerPurpose::PLACEMENT_NAME);
    EXPECT_EQ(purposes[3], ViewLayerPurpose::TERMINAL);
    EXPECT_EQ(purposes[4], ViewLayerPurpose::OBSTRUCTION);
    EXPECT_EQ(purposes[5], ViewLayerPurpose::TRACK_PREFERRED);
    EXPECT_EQ(purposes[6], ViewLayerPurpose::TRACK_NON_PREFERRED);
    EXPECT_EQ(purposes[7], ViewLayerPurpose::ROUTING_BLOCKAGE);
    EXPECT_EQ(purposes[8], ViewLayerPurpose::ROUTE);
    EXPECT_EQ(purposes[9], ViewLayerPurpose::GCELLGRID);
    EXPECT_EQ(purposes[10], ViewLayerPurpose::PLACEMENT_BLOCKAGE);
    EXPECT_EQ(purposes[11], ViewLayerPurpose::REGION);
}

TEST(ViewStylePalette, CutLayerAboveARoutingLayerSharesItsColor)
{
    // LEF LAYER declaration order is bottom-up physical stacking order -
    // M1 (ROUTING) then V1 (CUT) means V1 sits directly above M1.
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    LayerId m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
    LayerId v1 = root.create_layer(LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});
    LayerId m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});

    ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_id);

    const Color &m1_color = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TERMINAL))->style.outline_color;
    const Color &v1_color = view_layers.get(view_layers.find(v1, ViewLayerPurpose::TERMINAL))->style.outline_color;
    const Color &m2_color = view_layers.get(view_layers.find(m2, ViewLayerPurpose::TERMINAL))->style.outline_color;

    EXPECT_EQ(v1_color.r, m1_color.r);
    EXPECT_EQ(v1_color.g, m1_color.g);
    EXPECT_EQ(v1_color.b, m1_color.b);

    // M2 still gets its own distinct color, not V1's/M1's.
    EXPECT_FALSE(m2_color.r == m1_color.r && m2_color.g == m1_color.g && m2_color.b == m1_color.b);
}

TEST(ViewStylePalette, CutLayerWithNoRoutingLayerBelowFallsBackToItsOwnBrightColor)
{
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    // A CUT layer declared before any ROUTING layer - no "layer below" to
    // inherit from.
    LayerId v0 = root.create_layer(LayerData{.technology = technology_id, .name = "V0", .type = "CUT"});

    ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_id);

    const Color &v0_color = view_layers.get(view_layers.find(v0, ViewLayerPurpose::TERMINAL))->style.outline_color;

    // First slot of the bright ROUTING/CUT palette is red (255, 0, 0).
    EXPECT_EQ(v0_color.r, 255);
    EXPECT_EQ(v0_color.g, 0);
    EXPECT_EQ(v0_color.b, 0);
}

TEST(ViewStylePalette, NonRoutingNonCutLayersUseTheMutedPaletteNotTheBrightOne)
{
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    LayerId m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
    LayerId slice = root.create_layer(LayerData{.technology = technology_id, .name = "SLICE", .type = "MASTERSLICE"});

    ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_id);

    const Color &m1_color = view_layers.get(view_layers.find(m1, ViewLayerPurpose::TERMINAL))->style.outline_color;
    const Color &slice_color = view_layers.get(view_layers.find(slice, ViewLayerPurpose::TERMINAL))->style.outline_color;

    // Both are the first entry of their own palette - if they were the
    // same list, these would be identical (both red). They must differ.
    EXPECT_FALSE(slice_color.r == m1_color.r && slice_color.g == m1_color.g && slice_color.b == m1_color.b);
}

TEST(ViewStylePalette, DifferentOtherTypeLayersGetDifferentMutedColors)
{
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    LayerId a = root.create_layer(LayerData{.technology = technology_id, .name = "A", .type = "IMPLANT"});
    LayerId b = root.create_layer(LayerData{.technology = technology_id, .name = "B", .type = "IMPLANT"});

    ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_id);

    const Color &a_color = view_layers.get(view_layers.find(a, ViewLayerPurpose::TERMINAL))->style.outline_color;
    const Color &b_color = view_layers.get(view_layers.find(b, ViewLayerPurpose::TERMINAL))->style.outline_color;

    EXPECT_FALSE(a_color.r == b_color.r && a_color.g == b_color.g && a_color.b == b_color.b);
}

TEST(ViewStylePalette, ColorCyclesWithMoreLayersThanPaletteEntries)
{
    // 31 layers - one more than the 30-color palette - should wrap back to
    // the first color rather than reading out of bounds (the ported-from
    // sibling code this replaces has that exact off-by-one bug - see
    // layer_color's comment).
    Root root;
    TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    std::vector<LayerId> layers;
    for (int i = 0; i < 31; ++i)
        layers.push_back(root.create_layer(LayerData{.technology = technology_id, .name = "L" + std::to_string(i), .type = "ROUTING"}));

    ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_id);

    const ViewLayerData *first = view_layers.get(view_layers.find(layers.front(), ViewLayerPurpose::TERMINAL));
    const ViewLayerData *wrapped = view_layers.get(view_layers.find(layers.back(), ViewLayerPurpose::TERMINAL));

    ASSERT_NE(first, nullptr);
    ASSERT_NE(wrapped, nullptr);
    EXPECT_EQ(first->style.outline_color.r, wrapped->style.outline_color.r);
    EXPECT_EQ(first->style.outline_color.g, wrapped->style.outline_color.g);
    EXPECT_EQ(first->style.outline_color.b, wrapped->style.outline_color.b);
}
