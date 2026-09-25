// GuiProvider (src/gui/gui_provider.cpp) is compiled straight into
// backend_tests - it only talks to the C API, so no ImGui/GLFW is needed.

#include "gui/gui_provider.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace
{
    std::string fixture_path(const std::string &name)
    {
        return std::string(API_TEST_FIXTURES_DIR) + "/" + name;
    }

    struct GuiProviderFixture : ::testing::Test
    {
        LeHandle *handle = le_create();
        ~GuiProviderFixture() override { le_destroy(handle); }
    };
}

// Regression test (BUGS_AND_ENHANCEMENTS.md E12, NEW_FEATURES_SEPT_2026.md
// item 7): the Layers panel lists technology layers only. Pseudo-rows with
// no physical Layer (ROW, BOUNDARY, PLACEMENT,
// GCELLGRID, PLACEMENT_BLOCKAGE, REGION, DEBUG, FLIGHTLINE, ...) have only a
// purpose, and are listed once, under purposes. The filter lived in the old
// Flutter frontend and was lost when GuiProvider::refresh took over the loop.
TEST_F(GuiProviderFixture, LayersListsOnlyTechnologyLayersAndPseudoRowsOnlyAsPurposes)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str(), "testcell"), 0);
    ASSERT_GT(le_layer_count(handle), 1); // M1 plus the pseudo-rows

    le::gui::GuiProvider provider(handle);
    provider.refresh();
    const auto &layers = provider.state().layer_manager.layers;
    ASSERT_EQ(layers.size(), 1u); // testcell.lef declares one Technology layer
    EXPECT_STREQ(layers[0].row.name, "M1");

    // The pseudo-rows' own purposes are still listed.
    const auto &purposes = provider.state().layer_manager.purposes;
    const auto has_purpose = [&](int32_t ordinal)
    { return std::ranges::any_of(purposes, [&](const auto &p)
                                 { return p.ordinal == ordinal; }); };
    EXPECT_TRUE(has_purpose(2));  // BOUNDARY
    EXPECT_TRUE(has_purpose(6));  // ROW
    EXPECT_TRUE(has_purpose(11)); // PLACEMENT
    EXPECT_TRUE(has_purpose(13)); // DEBUG
    EXPECT_TRUE(has_purpose(14)); // FLIGHTLINE

    provider.refresh(); // rebuilt, not appended to
    EXPECT_EQ(provider.state().layer_manager.layers.size(), 1u);
}

TEST_F(GuiProviderFixture, EveryTechnologyLayerIsListedInDeclarationOrder)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("many_routing_layers.lef").c_str(), "many"), 0);

    le::gui::GuiProvider provider(handle);
    provider.refresh();
    const auto &layers = provider.state().layer_manager.layers;
    ASSERT_FALSE(layers.empty());
    int32_t physical_rows = 0;
    for (int32_t i = 0; i < le_layer_count(handle); ++i)
    {
        const LeLayerRow row = le_layer_at(handle, i);
        if (row.name && row.has_physical_layer)
        {
            ASSERT_LT(static_cast<size_t>(physical_rows), layers.size());
            EXPECT_STREQ(layers[physical_rows].row.name, row.name);
            ++physical_rows;
        }
    }
    EXPECT_EQ(layers.size(), static_cast<size_t>(physical_rows));
}
