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

TEST_F(ViewStyleFixture, CreatesTerminalAndObstructionPerLayerPlusOneBoundary)
{
    // 2 layers x 2 purposes + 1 special BOUNDARY = 5.
    EXPECT_EQ(view_layers.all().size(), 5u);
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

TEST_F(ViewStyleFixture, BoundaryColorIsUnaffectedByThePerLayerPalette)
{
    const ViewLayerData *boundary = view_layers.get(view_layers.boundary_view_layer());
    ASSERT_NE(boundary, nullptr);

    EXPECT_EQ(boundary->style.outline_color.r, 255);
    EXPECT_EQ(boundary->style.outline_color.g, 255);
    EXPECT_EQ(boundary->style.outline_color.b, 255);
    EXPECT_EQ(boundary->style.fill_color.a, 0);
}

TEST_F(ViewStyleFixture, RowsHasOneRowPerPhysicalLayerPlusBoundaryLast)
{
    // 2 physical Layers (M1, M2) + BOUNDARY = 3 rows, BOUNDARY last -
    // matches ViewLayerRow's contract: a caller never needs to special-case
    // it, but declaration order still puts it after every physical Layer.
    const auto &rows = view_layers.rows();
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].name, "M1");
    EXPECT_EQ(rows[1].name, "M2");
    EXPECT_EQ(rows[2].name, "BOUNDARY");
}

TEST_F(ViewStyleFixture, PhysicalLayerRowHasTerminalAndObstructionColumns)
{
    const auto &row = view_layers.rows().at(0);
    ASSERT_EQ(row.columns.size(), 2u);
    EXPECT_EQ(row.columns[0].purpose, ViewLayerPurpose::TERMINAL);
    EXPECT_EQ(row.columns[0].id, view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    EXPECT_EQ(row.columns[1].purpose, ViewLayerPurpose::OBSTRUCTION);
    EXPECT_EQ(row.columns[1].id, view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION));
}

TEST_F(ViewStyleFixture, BoundaryRowHasASingleBoundaryColumn)
{
    const auto &row = view_layers.rows().back();
    ASSERT_EQ(row.columns.size(), 1u);
    EXPECT_EQ(row.columns[0].purpose, ViewLayerPurpose::BOUNDARY);
    EXPECT_EQ(row.columns[0].id, view_layers.boundary_view_layer());
}

TEST_F(ViewStyleFixture, PurposesListsEachDistinctPurposeOnceInFirstEncounteredOrder)
{
    // M1's row contributes TERMINAL then OBSTRUCTION; M2's row repeats
    // both (deduplicated, not appended again); BOUNDARY's row contributes
    // BOUNDARY last.
    const auto purposes = view_layers.purposes();
    ASSERT_EQ(purposes.size(), 3u);
    EXPECT_EQ(purposes[0], ViewLayerPurpose::TERMINAL);
    EXPECT_EQ(purposes[1], ViewLayerPurpose::OBSTRUCTION);
    EXPECT_EQ(purposes[2], ViewLayerPurpose::BOUNDARY);
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
