// GuiProvider (src/gui/gui_provider.cpp) is compiled straight into
// backend_tests - it only talks to the C API, so no ImGui/GLFW is needed.

#include "gui/gui_provider.hpp"
#include "api/le_handle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <shared_mutex>
#include <string>
#include <vector>

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

// NEW_FEATURES_SEPT_2026.md item 8: only purposes something can actually
// be selected on get a selectable checkbox in the Layers panel.
TEST_F(GuiProviderFixture, OnlyPurposesWithSelectableObjectsOfferASelectableToggle)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str(), "testcell"), 0);
    le::gui::GuiProvider provider(handle);
    provider.refresh();

    std::vector<int32_t> with_toggle;
    for (const auto &purpose : provider.state().layer_manager.purposes)
        if (purpose.has_selectable_objects)
            with_toggle.push_back(purpose.ordinal);
    std::ranges::sort(with_toggle);
    EXPECT_EQ(with_toggle, (std::vector<int32_t>{0 /* TERMINAL */, 1 /* OBSTRUCTION */, 9 /* ROUTE */, 11 /* PLACEMENT */}));
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

// Regression test: the GUI thread's per-frame refresh must never wait on the
// handle's write lock while a render holds its shared lock - it froze the
// window for the whole render, so the progress spinner never showed. A
// database change first (here, creating a library) makes the layer list
// stale, the case that used to need the write lock.
TEST_F(GuiProviderFixture, RefreshDoesNotBlockWhileARenderHoldsTheHandle)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str(), "testcell"), 0);
    le_create_library(handle, "EDITED");

    std::shared_lock<std::shared_mutex> render(handle->mutex_); // what le_render_pixel_buffer holds
    le::gui::GuiProvider provider(handle);
    std::future<void> refreshed = std::async(std::launch::async, [&]
                                             { provider.refresh(); });
    EXPECT_EQ(refreshed.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    render.unlock();
    refreshed.wait();
    EXPECT_EQ(provider.state().layer_manager.layers.size(), 1u);
}
