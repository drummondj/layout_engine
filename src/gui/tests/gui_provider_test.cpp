// GuiProvider (src/gui/gui_provider.cpp) is compiled straight into
// backend_tests - it only talks to the C API, so no ImGui/GLFW is needed.

#include "gui/gui_provider.hpp"
#include "api/le_handle.hpp"
#include "generated/pipelines/renderable_classes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unistd.h>
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

// The Layers panel lists technology layers only. Pseudo-rows with
// no physical Layer (ROW, BOUNDARY, PLACEMENT,
// GCELLGRID, PLACEMENT_BLOCKAGE, REGION, DEBUG, FLIGHTLINE, ...) have only a
// purpose, and are listed once, under purposes.
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

// Only purposes something can actually
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
    std::vector<int32_t> expected{0 /* TERMINAL */, 1 /* OBSTRUCTION */, 6 /* ROW */, 9 /* ROUTE */, 11 /* PLACEMENT */};
    le::renderable::for_each([&]<class R>(R) { expected.push_back(static_cast<int32_t>(R::purpose)); }); // extensions' rows are selectable
    EXPECT_EQ(with_toggle, expected);
}

// The Layers panel's indented Placement.type/Route.use rows, with each
// value's current visibility and selectability.
TEST_F(GuiProviderFixture, LayerManagerListsPlacementTypesAndRouteUsesWithTheirState)
{
    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str(), "testcell"), 0); // TESTCELL is CLASS CORE
    le_set_object_filter_value_visible(handle, LE_OBJECT_FILTER_PLACEMENT_TYPE, "CORE", 0);
    le_set_object_filter_value_selectable(handle, LE_OBJECT_FILTER_ROUTE_USE, "POWER", 0);

    le::gui::GuiProvider provider(handle);
    provider.refresh();
    const auto &types = provider.state().layer_manager.placement_types;
    ASSERT_EQ(types.size(), 1u);
    EXPECT_EQ(types[0].value, "CORE");
    EXPECT_FALSE(types[0].visible);
    EXPECT_TRUE(types[0].selectable);

    const auto &uses = provider.state().layer_manager.route_uses;
    const auto power = std::ranges::find_if(uses, [](const auto &row)
                                            { return row.value == "POWER"; });
    ASSERT_NE(power, uses.end());
    EXPECT_TRUE(power->visible);
    EXPECT_FALSE(power->selectable);
    EXPECT_EQ(uses.front().value, "SIGNAL");
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
// stale, the case that could need the write lock.
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

// The design view shows "running..." and stops forwarding input while a
// Tcl command runs - refresh() is where it learns that.
TEST_F(GuiProviderFixture, RefreshReportsARunningCommand)
{
    le::gui::GuiProvider provider(handle);
    provider.refresh();
    EXPECT_FALSE(provider.state().is_command_running);

    le_begin_command(handle, "running");
    provider.refresh();
    EXPECT_TRUE(provider.state().is_command_running);

    le_end_command(handle, 1);
    provider.refresh();
    EXPECT_FALSE(provider.state().is_command_running);
}

TEST_F(GuiProviderFixture, DbFileQueriesAndWriteDbNow)
{
    le::gui::GuiProvider provider(handle);
    EXPECT_TRUE(provider.database_is_empty());
    EXPECT_EQ(provider.db_path(), "");
    EXPECT_FALSE(provider.write_db_now("")) << "nowhere to save";

    ASSERT_EQ(le_read_lef(handle, fixture_path("testcell.lef").c_str(), "testcell"), 0);
    EXPECT_FALSE(provider.database_is_empty());
    const std::string path = (std::filesystem::temp_directory_path() / ("gui_provider_db_" + std::to_string(::getpid()) + ".led")).string();
    ASSERT_EQ(le_write_db(handle, path.c_str(), 1), 0);
    EXPECT_EQ(provider.db_path(), path);

    le_create_library(handle, "edited");
    EXPECT_TRUE(provider.has_unsaved_design());
    EXPECT_TRUE(provider.write_db_now(provider.db_path()));
    EXPECT_FALSE(provider.has_unsaved_design());
    std::filesystem::remove(path);
}

TEST_F(GuiProviderFixture, FileMenuQueuesReadAndWriteDbAsTclCommands)
{
    le::gui::GuiProvider provider(handle);
    provider.write_db("/tmp/a \"b\".led");
    const char *command = le_take_next_pending_tcl_command(handle);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(std::string(command).rfind("write_db ", 0), 0u) << command;
    EXPECT_NE(std::string(command).find("\\\"b\\\""), std::string::npos) << command;
    provider.read_db("/tmp/x.led");
    command = le_take_next_pending_tcl_command(handle);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(std::string(command), "read_db \"/tmp/x.led\"");
}
