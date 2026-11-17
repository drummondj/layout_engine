// GuiProvider (src/gui/gui_provider.cpp) is compiled straight into
// backend_tests: it only talks to the C++ views, so no ImGui/GLFW is needed.

#include "gui/gui_provider.hpp"
#include "api/le_handle.hpp"
#include "generated/pipelines/renderable_classes.hpp"

#include <gtest/gtest.h>
#include "../../test_support/scratch_dir.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <shared_mutex>
#include <string>
#include <variant>
#include <vector>

namespace
{
    std::string fixture_path(const std::string &name)
    {
        return std::string(API_TEST_FIXTURES_DIR) + "/" + name;
    }

    struct GuiProviderFixture : ::testing::Test
    {
        le::Session session;
        LeHandle *handle = session.handle();

        void read_lef(const std::string &fixture, const std::string &library)
        {
            ASSERT_TRUE(session.write().read_lef(fixture_path(fixture), library).has_value());
        }

        void create_library(const std::string &name)
        {
            ASSERT_TRUE(session.write().create_library(le::LibraryData{.name = name}).has_value());
        }

        std::string queued()
        {
            return session.requests().take_next_tcl_command().value_or("");
        }

        // `name`'s Abstract (the testcell fixture's TESTCELL).
        le::AbstractId abstract_named(const std::string &name)
        {
            const le::ReadView view = session.read();
            for (const le::DesignId design : view.root().get_design_ids())
                if (view.root().get_design(design)->name == name)
                    return view.root().get_design_abstract(design);
            return {};
        }
    };
}

// A frame keeps its layer rows while the console thread rebuilds the
// handle's (a colour reset, another read_lef).
TEST_F(GuiProviderFixture, LayerRowsKeepTheirNamesAcrossALayerRowRebuild)
{
    read_lef("testcell.lef", "testcell");
    le::gui::GuiProvider provider(handle);
    provider.refresh();
    const auto &layers = provider.state().layer_manager.layers;
    ASSERT_EQ(layers.size(), 1u);

    {
        le::WriteView view = session.write();
        view.set_layer_color("M1", le::Color{.r = 1, .g = 2, .b = 3});
        view.reset_layer_color("M1");
    }
    read_lef("many_routing_layers.lef", "many");
    ASSERT_FALSE(session.read().layer_rows().empty());

    EXPECT_EQ(layers[0].name, "M1");
}

// A user-picked color shows in the Layers panel's swatch.
TEST_F(GuiProviderFixture, LayerRowsShowAPickedColor)
{
    read_lef("testcell.lef", "testcell");
    session.write().set_layer_color("M1", le::Color{.r = 1, .g = 2, .b = 3});
    le::gui::GuiProvider provider(handle);
    provider.refresh();
    ASSERT_EQ(provider.state().layer_manager.layers.size(), 1u);
    EXPECT_EQ(provider.state().layer_manager.layers[0].color, (le::Color{.r = 1, .g = 2, .b = 3}));
}

// The Layers panel lists technology layers only. Pseudo-rows with
// no physical Layer (ROW, BOUNDARY, PLACEMENT,
// GCELLGRID, PLACEMENT_BLOCKAGE, REGION, DEBUG, FLIGHTLINE, ...) have only a
// purpose, and are listed once, under purposes.
TEST_F(GuiProviderFixture, LayersListsOnlyTechnologyLayersAndPseudoRowsOnlyAsPurposes)
{
    read_lef("testcell.lef", "testcell");
    ASSERT_GT(session.read().layer_rows().size(), 1u); // M1 plus the pseudo-rows

    le::gui::GuiProvider provider(handle);
    provider.refresh();
    const auto &layers = provider.state().layer_manager.layers;
    ASSERT_EQ(layers.size(), 1u); // testcell.lef declares one Technology layer
    EXPECT_EQ(layers[0].name, "M1");

    // The pseudo-rows' own purposes are still listed, by their labels.
    const auto &purposes = provider.state().layer_manager.purposes;
    const auto has_purpose = [&](le::ViewLayerPurpose purpose, std::string_view label)
    { return std::ranges::any_of(purposes, [&](const auto &p)
                                 { return p.purpose == purpose && p.name == label; }); };
    EXPECT_TRUE(has_purpose(le::ViewLayerPurpose::BOUNDARY, "boundary"));
    EXPECT_TRUE(has_purpose(le::ViewLayerPurpose::ROW, "row"));
    EXPECT_TRUE(has_purpose(le::ViewLayerPurpose::PLACEMENT, "placement"));
    EXPECT_TRUE(has_purpose(le::ViewLayerPurpose::DEBUG, "debug"));
    EXPECT_TRUE(has_purpose(le::ViewLayerPurpose::FLIGHTLINE, "flightline"));

    provider.refresh(); // rebuilt, not appended to
    EXPECT_EQ(provider.state().layer_manager.layers.size(), 1u);
}

// Only purposes something can actually
// be selected on get a selectable checkbox in the Layers panel.
TEST_F(GuiProviderFixture, OnlyPurposesWithSelectableObjectsOfferASelectableToggle)
{
    read_lef("testcell.lef", "testcell");
    le::gui::GuiProvider provider(handle);
    provider.refresh();

    std::vector<le::ViewLayerPurpose> with_toggle;
    for (const auto &purpose : provider.state().layer_manager.purposes)
        if (purpose.has_selectable_objects)
            with_toggle.push_back(purpose.purpose);
    std::ranges::sort(with_toggle);
    std::vector<le::ViewLayerPurpose> expected{le::ViewLayerPurpose::TERMINAL, le::ViewLayerPurpose::OBSTRUCTION, le::ViewLayerPurpose::ROW,
                                               le::ViewLayerPurpose::ROUTE, le::ViewLayerPurpose::PLACEMENT};
    le::renderable::for_each([&]<class R>(R) { expected.push_back(R::purpose); }); // extensions' rows are selectable
    std::ranges::sort(expected);
    EXPECT_EQ(with_toggle, expected);
}

// The Layers panel's indented Placement.type/Route.use rows, with each
// value's current visibility and selectability.
TEST_F(GuiProviderFixture, LayerManagerListsPlacementTypesAndRouteUsesWithTheirState)
{
    read_lef("testcell.lef", "testcell"); // TESTCELL is CLASS CORE
    {
        le::WriteView view = session.write();
        view.set_object_filter_visible(le::ObjectFilter::PLACEMENT_TYPE, "CORE", false);
        view.set_object_filter_selectable(le::ObjectFilter::ROUTE_USE, "POWER", false);
    }

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

// The placement-status, routing-status and route-shape rows, and the Tcl
// command a toggle queues.
TEST_F(GuiProviderFixture, LayerManagerListsStatusesAndShapesAndQueuesTheirCommands)
{
    session.write().set_object_filter_visible(le::ObjectFilter::ROUTE_SHAPE, "FOLLOWPIN", false);
    le::gui::GuiProvider provider(handle);
    provider.refresh();
    const auto &state = provider.state().layer_manager;
    EXPECT_EQ(state.placement_statuses.size(), 5u);
    EXPECT_EQ(state.routing_statuses.front().value, "ROUTED");
    const auto followpin = std::ranges::find_if(state.route_shapes, [](const auto &row)
                                                { return row.value == "FOLLOWPIN"; });
    ASSERT_NE(followpin, state.route_shapes.end());
    EXPECT_FALSE(followpin->visible);
    EXPECT_EQ(state.route_shapes.back().value, "UNSET");

    provider.set_object_filter_visible(le::ObjectFilter::PLACEMENT_STATUS, "FIXED", false);
    EXPECT_EQ(queued(), "set_placement_status_visible \"FIXED\" false");
    provider.set_object_filter_selectable(le::ObjectFilter::ROUTING_STATUS, "COVER", true);
    EXPECT_EQ(queued(), "set_routing_status_selectable \"COVER\" true");
    provider.set_object_filter_visible(le::ObjectFilter::ROUTE_SHAPE, "RING", false);
    EXPECT_EQ(queued(), "set_route_shape_visible \"RING\" false");
}

// The Lock/Unlock button queues set_placement_status, so it shows in the
// console history.
TEST_F(GuiProviderFixture, LockingPlacementsIsQueuedAsATclCommand)
{
    le::gui::GuiProvider provider(handle);
    provider.refresh();
    EXPECT_FALSE(provider.state().placement_move.all_fixed);
    provider.set_selected_placements_fixed(true);
    EXPECT_EQ(queued(), "set_placement_status FIXED");
    provider.set_selected_placements_fixed(false);
    EXPECT_EQ(queued(), "set_placement_status PLACED");
}

TEST_F(GuiProviderFixture, EveryTechnologyLayerIsListedInDeclarationOrder)
{
    read_lef("many_routing_layers.lef", "many");

    le::gui::GuiProvider provider(handle);
    provider.refresh();
    std::vector<std::string> expected;
    for (const le::LayerRow &row : session.read().layer_rows())
        if (row.has_physical_layer)
            expected.push_back(row.name);
    ASSERT_FALSE(expected.empty());
    std::vector<std::string> listed;
    for (const auto &layer : provider.state().layer_manager.layers)
        listed.push_back(layer.name);
    EXPECT_EQ(listed, expected);
}

// Regression test: the GUI thread's per-frame refresh must never wait on the
// handle's write lock while a render holds its shared lock - it froze the
// window for the whole render, so the progress spinner never showed. A
// database change first (here, creating a library) makes the layer list
// stale, the case that could need the write lock.
TEST_F(GuiProviderFixture, RefreshDoesNotBlockWhileARenderHoldsTheHandle)
{
    read_lef("testcell.lef", "testcell");
    create_library("EDITED");

    std::shared_lock<std::shared_mutex> render(handle->mutex_); // what Renderer::render holds
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

    session.write().begin_command("running");
    provider.refresh();
    EXPECT_TRUE(provider.state().is_command_running);

    session.write().end_command(true);
    provider.refresh();
    EXPECT_FALSE(provider.state().is_command_running);
}

TEST_F(GuiProviderFixture, DbFileQueriesAndWriteDbNow)
{
    le::gui::GuiProvider provider(handle);
    EXPECT_TRUE(provider.database_is_empty());
    EXPECT_EQ(provider.db_path(), "");
    EXPECT_FALSE(provider.write_db_now("")) << "nowhere to save";

    read_lef("testcell.lef", "testcell");
    EXPECT_FALSE(provider.database_is_empty());
    const std::string path = le::test::scratch_path("gui_provider_db.led");
    ASSERT_TRUE(session.write().write_db(path, true).has_value());
    EXPECT_EQ(provider.db_path(), path);

    create_library("edited");
    EXPECT_TRUE(provider.has_unsaved_design());
    EXPECT_TRUE(provider.write_db_now(provider.db_path()));
    EXPECT_FALSE(provider.has_unsaved_design());
    std::filesystem::remove(path);
}

// The Settings panel's view flip: refresh() reads it, and setting it queues
// set_view_flip so it shows in the console history.
TEST_F(GuiProviderFixture, ViewFlipIsReadAndQueuedAsATclCommand)
{
    le::gui::GuiProvider provider(handle);
    provider.refresh();
    EXPECT_EQ(provider.state().database_settings.view_flip, le::ViewFlip::NONE);
    session.write().set_view_flip(le::ViewFlip::VERTICAL);
    provider.refresh();
    EXPECT_EQ(provider.state().database_settings.view_flip, le::ViewFlip::VERTICAL);

    provider.set_view_flip(le::ViewFlip::HORIZONTAL);
    EXPECT_EQ(queued(), "set_view_flip horizontal");
}

// File > Exit exits straight away unless the design or the settings are unsaved.
TEST_F(GuiProviderFixture, UnsavedChangesCoverTheDesignAndTheSettings)
{
    le::gui::GuiProvider provider(handle);
    EXPECT_FALSE(provider.has_unsaved_changes());
    session.write().set_label_max_size(30.0);
    EXPECT_TRUE(provider.has_unsaved_changes()) << "a setting";

    le::Session edited;
    le::gui::GuiProvider edited_provider(edited.handle());
    ASSERT_TRUE(edited.write().create_library(le::LibraryData{.name = "edited"}).has_value());
    EXPECT_TRUE(edited_provider.has_unsaved_changes()) << "a design edit";
}

TEST_F(GuiProviderFixture, FileMenuQueuesReadAndWriteDbAsTclCommands)
{
    le::gui::GuiProvider provider(handle);
    provider.write_db("/tmp/a \"b\".led");
    const std::string write = queued();
    EXPECT_EQ(write.rfind("write_db ", 0), 0u) << write;
    EXPECT_NE(write.find("\\\"b\\\""), std::string::npos) << write;
    provider.read_db("/tmp/x.led");
    EXPECT_EQ(queued(), "read_db \"/tmp/x.led\"");
}

// The status bar shows the snapped mouse position in microns, and the
// selection's size; both come from one refresh().
TEST_F(GuiProviderFixture, StatusBarShowsTheSnappedMousePositionInMicrons)
{
    le::gui::GuiProvider provider(handle);
    provider.refresh();
    EXPECT_FALSE(provider.state().status_bar.snapped_mouse_position) << "no Technology, no mouse";

    read_lef("testcell.lef", "testcell");
    {
        le::WriteView view = session.write();
        ASSERT_TRUE(view.open_abstract(view.root().get_design_ids().front()).has_value());
        view.set_viewport_size(200, 200);
        view.fit_view();
        view.set_mouse_position(100, 100);
    }
    provider.refresh();
    const auto &position = provider.state().status_bar.snapped_mouse_position;
    ASSERT_TRUE(position);
    const le::ReadView view = session.read();
    const le::Point snapped = *view.snapped_mouse_position();
    const auto [x, y] = view.display_point(snapped);
    EXPECT_DOUBLE_EQ(position->x, x / view.units()->dbu_per_um());
    EXPECT_DOUBLE_EQ(position->y, y / view.units()->dbu_per_um());
}

// Opening a design from the Browser opens its view and fits it.
TEST_F(GuiProviderFixture, TheBrowserListsDesignsAndOpensTheirViews)
{
    le::gui::GuiProvider provider(handle);
    EXPECT_TRUE(provider.libraries().empty());

    read_lef("testcell.lef", "testcell");
    const auto &libraries = provider.libraries();
    ASSERT_EQ(libraries.size(), 1u);
    EXPECT_EQ(libraries[0].name, "testcell");
    const auto design = std::ranges::find_if(libraries[0].designs, [](const auto &d)
                                             { return d.name == "TESTCELL"; });
    ASSERT_NE(design, libraries[0].designs.end());
    EXPECT_TRUE(design->has_abstract);
    EXPECT_FALSE(design->has_layout);
    const le::DesignId design_id = design->id; // libraries() rebuilds the tree below

    create_library("later");
    EXPECT_EQ(provider.libraries().size(), 2u) << "re-read after the database changed";

    session.write().set_viewport_size(200, 200);
    provider.open_design_abstract(design_id);
    const le::ViewState view = session.read().view();
    EXPECT_EQ(view.abstract, abstract_named("TESTCELL"));
    EXPECT_NE(view.scale, 1.0) << "fitted";
}

// The Property Viewer's reads: a selected piece is its Shape, which it
// walks up from to the Library, and an Abstract links to its children.
TEST_F(GuiProviderFixture, PropertyViewerReadsAncestryChildrenAndTokens)
{
    read_lef("testcell.lef", "testcell");
    const le::AbstractId abstract = abstract_named("TESTCELL");
    ASSERT_TRUE(abstract.valid());

    le::gui::GuiProvider provider(handle);
    const std::vector<le::ObjectId> children = provider.children(abstract);
    const auto terminal = std::ranges::find_if(children, [](const le::ObjectId &id)
                                               { return std::holds_alternative<le::TerminalId>(id); });
    ASSERT_NE(terminal, children.end());

    const std::vector<le::ObjectId> chain = provider.ancestry(*terminal);
    ASSERT_GE(chain.size(), 4u); // Library, Design, Abstract, Terminal
    EXPECT_TRUE(std::holds_alternative<le::LibraryId>(chain.front()));
    EXPECT_EQ(chain.back(), *terminal);
    EXPECT_EQ(chain[chain.size() - 2], le::ObjectId{abstract});

    const std::vector<std::string> tokens = provider.tokens({chain.front(), *terminal});
    EXPECT_EQ(tokens[0], "library:testcell");
    EXPECT_EQ(tokens[1].rfind("terminal:", 0), 0u) << tokens[1];
    EXPECT_GT(tokens[1].size(), std::string("terminal:").size());

    // A Shape has no name: its token is its packed id.
    const std::vector<le::ObjectId> shapes = provider.children(provider.children(*terminal).front());
    ASSERT_FALSE(shapes.empty());
    const le::ShapeId shape = std::get<le::ShapeId>(shapes.front());
    EXPECT_EQ(provider.tokens({shape})[0], "shape:" + std::to_string(uint64_t{shape.generation} << 32 | shape.index));

    EXPECT_FALSE(provider.properties(*terminal).empty());
    EXPECT_FALSE(provider.selected_object(0)) << "nothing selected";
    ASSERT_TRUE(session.write().select(shape).has_value());
    const std::optional<le::ObjectId> selected = provider.selected_object(0);
    ASSERT_TRUE(selected);
    EXPECT_EQ(*selected, le::ObjectId{shape});
}
