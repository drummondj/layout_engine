#include "hello_ext/hello.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace
{
    struct Session
    {
        LeHandle *handle = le_create();
        ~Session() { le_destroy(handle); }
    };

    // Golden files: .led files written by each schema version of this
    // extension, loaded by every later build.
    const std::filesystem::path kGoldenDir = std::filesystem::path(__FILE__).parent_path() / "golden";

    // schema_ext.py's VERSION: the golden directory a new file goes in.
    constexpr const char *kSchemaVersion = "0.4.0";

    // A HelloMarker owning one Shape (a 2x1 um DEBUG rect), in a new layout.
    LeHelloMarkerId add_marker(LeHandle *handle)
    {
        le::ext::WriteView view = le::ext::ExtensionContext(handle, "hello_ext").write();
        view.create_technology({.database_units_microns = 1000}).value(); // 1000 dbu per micron
        const le::LibraryId library = view.create_library({.name = "markers"}).value();
        const le::DesignId design = view.create_design({.library = library, .name = "top"}).value();
        const le::LayoutId layout = view.create_layout({.design = design}).value();
        const le::HelloMarkerId marker = view.create_hello_marker({.layout = layout, .name = "clock_root"}).value();
        view.build_shape(le::ShapeOwner::hello_marker(marker)).purpose(le::ShapePurpose::DEBUG).rect(0, 0, 2, 1).create().value();
        return le::ext::to_c(marker);
    }

    // How many markers the session has, and how many shapes they own.
    std::pair<size_t, size_t> markers_and_shapes(LeHandle *handle)
    {
        le::ext::ExtensionContext ctx(handle, "hello_ext");
        const le::ext::ReadView view = ctx.read();
        size_t shapes = 0;
        for (const le::HelloMarkerId marker : view.root().get_hello_marker_ids())
            shapes += view.root().get_hello_marker_shapes(marker).size();
        return {view.root().get_hello_marker_ids().size(), shapes};
    }

    LeLibraryId add_two_notes(LeHandle *handle)
    {
        const LeLibraryId library = le_create_library(handle, "lib1");
        le_create_hello_note(handle, library, "route the clock first");
        le_create_hello_note(handle, library, "then the resets");
        return library;
    }
}

TEST(HelloExt, IsRegistered)
{
    le::ext::register_all();
    le::ext::register_all(); // idempotent
    int found = 0;
    for (int32_t i = 0; i < le_extension_count(); ++i)
        if (std::string(le_extension_name(i)) == "hello_ext")
        {
            ++found;
            EXPECT_STREQ(le_extension_version(i), "0.1.0");
        }
    EXPECT_EQ(found, 1);
    EXPECT_EQ(le_extension_name(le_extension_count()), nullptr);
}

TEST(HelloExt, ReadsTheDatabase)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    EXPECT_EQ(hello::library_count(ctx), 0);
    le_create_library(session.handle, "lib1");
    EXPECT_EQ(hello::library_count(ctx), 1);
}

TEST(HelloExt, AnEditIsOneUndoStep)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    ASSERT_TRUE(hello::add_library(ctx, "world"));
    EXPECT_EQ(hello::library_count(ctx), 1);
    ASSERT_NE(le_undo(session.handle), 0);
    EXPECT_EQ(hello::library_count(ctx), 0);
}

TEST(HelloExt, StateIsPerSessionAndPerExtension)
{
    Session a, b;
    le::ext::ExtensionContext in_a(a.handle, "hello_ext");
    le::ext::ExtensionContext in_b(b.handle, "hello_ext");
    le::ext::ExtensionContext other_extension(a.handle, "other_ext");
    ASSERT_TRUE(hello::add_library(in_a, "one"));
    ASSERT_TRUE(hello::add_library(in_a, "two"));
    EXPECT_EQ(in_a.data<hello::State>().libraries_added, 2);
    EXPECT_EQ(in_b.data<hello::State>().libraries_added, 0);
    EXPECT_EQ(other_extension.data<hello::State>().libraries_added, 0);
}

TEST(HelloExt, AnEditIsRecalledByItsLabel)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    ASSERT_TRUE(hello::add_library(ctx, "world"));
    ASSERT_EQ(le_command_history_count(session.handle), 1);
    EXPECT_STREQ(le_command_history_at(session.handle, 0), "hello_add_library world");
}

TEST(HelloExt, AFailedEditLeavesNothingToUndo)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    EXPECT_FALSE(hello::add_library(ctx, ""));
    EXPECT_EQ(ctx.data<hello::State>().libraries_added, 0);
    EXPECT_EQ(le_undo(session.handle), 0);
}

TEST(HelloExt, ItsIdsConvertBetweenTheCApiAndTheDatabase)
{
    Session session;
    const LeHelloMarkerId marker = add_marker(session.handle);
    const le::HelloMarkerId id = le::ext::from_c(marker);
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    EXPECT_EQ(ctx.read().root().get_hello_marker(id)->name, "clock_root");
    EXPECT_EQ(le::ext::to_c(id).index, marker.index);
    EXPECT_EQ(le::ext::to_c(id).generation, marker.generation);
}

TEST(HelloExt, AddsAMarkerToTheCurrentLayoutAsOneUndoStep)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    const LeLayoutId layout = le_create_layout(session.handle, le_create_design(session.handle, le_create_library(session.handle, "lib"), "top"));
    ASSERT_EQ(le_set_current_layout(session.handle, layout), 0);

    ASSERT_TRUE(hello::add_marker(ctx, "clock_root"));
    EXPECT_EQ(ctx.read().root().get_layout_hello_markers(le::ext::from_c(layout)).size(), 1u);
    ASSERT_NE(le_undo(session.handle), 0);
    EXPECT_EQ(ctx.read().root().get_layout_hello_markers(le::ext::from_c(layout)).size(), 0u);
}

TEST(HelloExt, AddingAMarkerWithNoCurrentLayoutFails)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    le_create_layout(session.handle, le_create_design(session.handle, le_create_library(session.handle, "lib"), "top"));
    EXPECT_FALSE(hello::add_marker(ctx, "clock_root"));
    EXPECT_EQ(markers_and_shapes(session.handle).first, 0u);
    EXPECT_EQ(le_undo(session.handle), 0);
}

TEST(HelloExt, AWriteViewEditIsSeenByReaders)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    {
        le::ext::WriteView view = ctx.write();
        view.root().create_library(le::LibraryData{.name = "bulk"});
    }
    EXPECT_EQ(hello::library_count(ctx), 1);
}

TEST(HelloExt, NotesBelongToTheirLibraryAndGoWithIt)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    const LeLibraryId library = add_two_notes(session.handle);
    EXPECT_EQ(hello::notes_on(ctx, "lib1"), (std::vector<std::string>{"route the clock first", "then the resets"}));
    EXPECT_TRUE(hello::notes_on(ctx, "nope").empty());

    le_begin_command(session.handle, "delete_library lib1");
    ASSERT_EQ(le_delete_library(session.handle, library), 0);
    le_end_command(session.handle, 1);
    EXPECT_EQ(ctx.read().root().get_hello_note_ids().size(), 0u);
    ASSERT_NE(le_undo(session.handle), 0);
    EXPECT_EQ(hello::notes_on(ctx, "lib1").size(), 2u);
}

TEST(HelloExt, NotesRoundTripThroughANativeFile)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "hello_ext_notes_test.led";
    {
        Session session;
        add_two_notes(session.handle);
        ASSERT_EQ(le_write_db(session.handle, path.c_str(), 1), 0);
    }
    const std::string info = le_db_info(path.c_str());
    const std::string schema = kSchemaVersion;
    EXPECT_NE(info.find("extension hello_ext 0.1.0 (schema " + schema + "): this build has schema " + schema), std::string::npos) << info;

    Session session;
    ASSERT_EQ(le_read_db(session.handle, path.c_str(), 1), 0);
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    EXPECT_EQ(hello::notes_on(ctx, "lib1"), (std::vector<std::string>{"route the clock first", "then the resets"}));
    std::filesystem::remove(path);
}

TEST(HelloExt, EverySchemaVersionsGoldenFileStillLoads)
{
    if (!std::filesystem::exists(kGoldenDir))
        GTEST_SKIP() << "no golden files yet - write the first with --gtest_also_run_disabled_tests --gtest_filter='*WriteGolden*'";
    int loaded = 0;
    for (const auto &version : std::filesystem::directory_iterator(kGoldenDir))
    {
        Session session;
        const std::filesystem::path file = version.path() / "notes.led";
        ASSERT_EQ(le_read_db(session.handle, file.c_str(), 1), 0) << file;
        le::ext::ExtensionContext ctx(session.handle, "hello_ext");
        EXPECT_EQ(hello::notes_on(ctx, "lib1"), (std::vector<std::string>{"route the clock first", "then the resets"})) << file;
        ++loaded;
    }
    EXPECT_GE(loaded, 1);
}

// db_info lists the extension migrations an older file needs; migrate_db
// runs them and leaves none.
TEST(HelloExt, AnOlderFileListsItsMigrationsAndMigrateDbRunsThem)
{
    const std::filesystem::path oldest = kGoldenDir / "0.1.0" / "notes.led";
    if (!std::filesystem::exists(oldest) || kSchemaVersion == std::string("0.1.0"))
        GTEST_SKIP() << "no older schema version's golden file to migrate yet";
    const std::string info = le_db_info(oldest.c_str());
    EXPECT_NE(info.find("hello_ext 0.2.0: HelloNote.text renamed to body"), std::string::npos) << info;
    EXPECT_NE(info.find("hello_ext 0.4.0: HelloPin added, owning Shapes"), std::string::npos) << info;

    const std::string path = (std::filesystem::temp_directory_path() / "hello_ext_migrated.led").string();
    const std::string summary = le_migrate_db(oldest.c_str(), path.c_str());
    EXPECT_NE(summary.find("applied hello_ext migration to 0.2.0"), std::string::npos) << summary;
    EXPECT_NE(std::string(le_db_info(path.c_str())).find("migrations to run: none"), std::string::npos);
    Session session;
    ASSERT_EQ(le_read_db(session.handle, path.c_str(), 1), 0);
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    EXPECT_EQ(hello::notes_on(ctx, "lib1"), (std::vector<std::string>{"route the clock first", "then the resets"}));
    std::filesystem::remove(path);
}

// Run once per new schema version (--gtest_also_run_disabled_tests
// --gtest_filter='*WriteGolden*'), then commit the file.
TEST(HelloExt, DISABLED_WriteGoldenFileForThisSchemaVersion)
{
    Session session;
    add_two_notes(session.handle);
    add_marker(session.handle);
    // And a HelloPin owning a shape, in a layout of its own.
    const LeLayoutId pins = le_create_layout(session.handle, le_create_design(session.handle, le_create_library(session.handle, "pins"), "top"));
    const double rect[] = {1.0, 1.0, 2.0, 2.0};
    le_create_shape(session.handle, le_shape_owner_hello_pin(le_create_hello_pin(session.handle, pins)), LeLayerId{UINT32_MAX, 0}, nullptr, 0, nullptr, 0, 0, nullptr, 0, 1,
                    rect, 4, 0, 0.0, 0, 0.0, 0);
    std::filesystem::create_directories(kGoldenDir / kSchemaVersion);
    ASSERT_EQ(le_write_db(session.handle, (kGoldenDir / kSchemaVersion / "notes.led").c_str(), 1), 0);
}

// hello_ext's section of settings.json: saved with the core settings, its
// own version beside it; another extension's section survives a build
// without that extension.
TEST(HelloExt, ItsSettingsSectionRoundTripsAndOthersAreKept)
{
    le::ext::register_all();
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "hello_ext_settings_test.json";
    {
        Session session;
        le::ext::ExtensionContext ctx(session.handle, "hello_ext");
        ctx.data<hello::State>().library_name = "mine";
        ASSERT_EQ(le_save_settings(session.handle, path.c_str()), 0);
    }
    std::ifstream in(path);
    nlohmann::json saved = nlohmann::json::parse(in);
    EXPECT_EQ(saved["extensions"]["hello_ext"], (nlohmann::json{{"library_name", "mine"}, {"version", 1}}));

    saved["extensions"]["other_ext"] = {{"version", 3}, {"colour", "red"}};
    std::ofstream(path) << saved.dump();
    Session session;
    ASSERT_EQ(le_load_settings(session.handle, path.c_str()), 0);
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    EXPECT_EQ(ctx.data<hello::State>().library_name, "mine");
    ASSERT_EQ(le_save_settings(session.handle, path.c_str()), 0);
    std::ifstream again(path);
    EXPECT_EQ(nlohmann::json::parse(again)["extensions"]["other_ext"], (nlohmann::json{{"version", 3}, {"colour", "red"}}));
    std::filesystem::remove(path);
}

TEST(HelloExt, RegistersItsOverlay)
{
    le::ext::register_all();
    int overlays = 0;
    for (const auto &[extension, draw] : le::ext::registry().overlays())
        overlays += extension == "hello_ext" && draw != nullptr;
    EXPECT_EQ(overlays, 1);
}

// HelloMarker owns Shapes (an owner option codegen adds to Shape for it):
// created with a hello_marker owner, deleted with the marker, undone
// together and saved.
TEST(HelloExt, MarkersOwnShapes)
{
    Session session;
    le_begin_command(session.handle, "markers");
    const LeHelloMarkerId marker = add_marker(session.handle);
    le_end_command(session.handle, 1);
    EXPECT_EQ(markers_and_shapes(session.handle), (std::pair<size_t, size_t>{1, 1}));

    le_begin_command(session.handle, "delete marker");
    ASSERT_EQ(le_delete_hello_marker(session.handle, marker), 0);
    le_end_command(session.handle, 1);
    EXPECT_EQ(markers_and_shapes(session.handle), (std::pair<size_t, size_t>{0, 0}));
    EXPECT_EQ(le::ext::ExtensionContext(session.handle, "hello_ext").read().root().get_shape_ids().size(), 0u) << "the shape goes with its marker";
    ASSERT_NE(le_undo(session.handle), 0);
    EXPECT_EQ(markers_and_shapes(session.handle), (std::pair<size_t, size_t>{1, 1}));

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "hello_ext_markers_test.led";
    ASSERT_EQ(le_write_db(session.handle, path.c_str(), 1), 0);
    Session loaded;
    ASSERT_EQ(le_read_db(loaded.handle, path.c_str(), 1), 0);
    EXPECT_EQ(markers_and_shapes(loaded.handle), (std::pair<size_t, size_t>{1, 1}));
    std::filesystem::remove(path);
}

// HelloMarker has render=: its shapes draw on the HELLO_MARKER row of the
// Layout view and are selected by a click, and an edit redraws them.
TEST(HelloExt, MarkersDrawAndSelectInTheLayoutView)
{
    le::ext::register_all();
    Session session;
    LeHandle *h = session.handle;
    const LeTechnologyId technology = le_create_technology(h, 1000.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, nullptr, nullptr, 0, 0, 0, nullptr, 0, 0.0, 0, 0.0, 0, 0.0, nullptr, 0, 0,
                         nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0);
    ASSERT_EQ(le_set_current_technology(h, technology), 0); // as reading a LEF does
    const LeDesignId design = le_create_design(h, le_create_library(h, "lib"), "top");
    const LeLayoutId layout = le_create_layout(h, design);
    const LeHelloMarkerId marker = le_create_hello_marker(h, layout, "clock_root");
    const double rect[] = {0.0, 0.0, 2.0, 1.0};
    const LeShapeId shape = le_create_shape(h, le_shape_owner_hello_marker(marker), LeLayerId{UINT32_MAX, 0}, nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, rect, 4, 0, 0.0,
                                            0, 0.0, 0);
    ASSERT_NE(shape.index, UINT32_MAX);
    ASSERT_EQ(le_set_current_design_layout_by_id(h, design), 0);
    le_set_viewport_size(h, 200, 100);
    le_fit_scene(h, 10);

    const auto lit = [&](int x, int y) {
        const LePixelBuffer buffer = le_render_pixel_buffer(h);
        if (!buffer.data || x >= buffer.width || y >= buffer.height)
            return false;
        const uint8_t *px = buffer.data + y * buffer.row_bytes + x * 4;
        return px[0] + px[1] + px[2] > 30;
    };
    EXPECT_TRUE(lit(60, 30)) << "the marker's rect fills the view";

    le_mouse_down(h, 100, 50);
    le_mouse_up(h, 100, 50);
    ASSERT_EQ(le_selection_count(h), 1);
    const LeObjectRef selected = le_selected_object_ref(h, 0);
    EXPECT_EQ(selected.kind, LE_OBJECT_KIND_SHAPE);
    EXPECT_EQ(selected.index, shape.index);

    // Moving the marker's shape out of the way redraws its chunk.
    const double moved[] = {10.0, 10.0, 11.0, 11.0};
    ASSERT_EQ(le_update_shape(h, shape, 0, LeLayerId{UINT32_MAX, 0}, nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, moved, 4, 0, 0.0, 0, 0.0, 0, 0), 0);
    EXPECT_FALSE(lit(60, 30)) << "away from the cursor box the click left at (100, 50)";
}

// HelloPin has render=Render(..., tiled=True): a layout's pins are split into
// spatial tiles. Enough pins for several tiles draw and select, and moving
// one into another tile redraws both tiles.
TEST(HelloExt, TiledPinsDrawSelectAndMoveBetweenTiles)
{
    le::ext::register_all();
    Session session;
    LeHandle *h = session.handle;
    const LeTechnologyId technology = le_create_technology(h, 1000.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, nullptr, nullptr, 0, 0, 0, nullptr, 0, 0.0, 0,
                                                           0.0, 0, 0.0, nullptr, 0, 0, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0);
    ASSERT_EQ(le_set_current_technology(h, technology), 0);
    const LeDesignId design = le_create_design(h, le_create_library(h, "lib"), "top");
    const LeLayoutId layout = le_create_layout(h, design);
    // A 100 x 100 um die area, which the view fits.
    const double die[] = {1, 4, 0, 0, 100, 0, 100, 100, 0, 100};
    ASSERT_NE(le_create_shape(h, le_shape_owner_layout(layout), LeLayerId{UINT32_MAX, 0}, "BOUNDARY", 0, nullptr, 0, 1, die, 10, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index,
              UINT32_MAX);

    const auto add_pin = [&](double x, double y, double size) {
        const LeHelloPinId pin = le_create_hello_pin(h, layout);
        const double rect[] = {x, y, x + size, y + size};
        return le_create_shape(h, le_shape_owner_hello_pin(pin), LeLayerId{UINT32_MAX, 0}, nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, rect, 4, 0, 0.0, 0, 0.0, 0);
    };
    // 6000 small pins in the left half (three tiles' worth), and one big
    // one alone at (73..77, 73..77) um.
    for (int i = 0; i < 6000; ++i)
        add_pin(0.8 * (i % 60), (i / 60) * 1.0, 0.4);
    const LeShapeId lone = add_pin(73, 73, 4);

    ASSERT_EQ(le_set_current_design_layout_by_id(h, design), 0);
    le_set_viewport_size(h, 400, 400);
    le_fit_scene(h, 0); // 4 px per um: (x, y) um is pixel (4x, 400 - 4y)
    const auto lit = [&](int x, int y) {
        const LePixelBuffer buffer = le_render_pixel_buffer(h);
        if (!buffer.data || x >= buffer.width || y >= buffer.height)
            return false;
        const uint8_t *px = buffer.data + y * buffer.row_bytes + x * 4;
        return px[0] + px[1] + px[2] > 30;
    };
    EXPECT_TRUE(lit(300, 100)) << "the lone pin at (75, 75) um";
    EXPECT_FALSE(lit(300, 300)) << "nothing at (75, 25) um yet";

    le_mouse_down(h, 300, 100);
    le_mouse_up(h, 300, 100);
    ASSERT_EQ(le_selection_count(h), 1);
    EXPECT_EQ(le_selected_object_ref(h, 0).index, lone.index);
    le_mouse_down(h, 10, 10); // click empty space: deselect, and move the cursor box away
    le_mouse_up(h, 10, 10);

    // Into another tile: drawn there, gone from where it was.
    const double moved[] = {73.0, 23.0, 77.0, 27.0};
    ASSERT_EQ(le_update_shape(h, lone, 0, LeLayerId{UINT32_MAX, 0}, nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, moved, 4, 0, 0.0, 0, 0.0, 0, 0), 0);
    EXPECT_TRUE(lit(300, 300));
    EXPECT_FALSE(lit(300, 100));
}

// HelloPin is also per_layer: a pin's shapes draw in the helloPin column of
// their layer's row, so hiding the layer or the purpose hides them and the
// layer's selectable toggle applies; a pin with no layer stays on the
// HELLO_PIN row.
TEST(HelloExt, PinsOnALayerDrawAndSelectInThatLayersRow)
{
    le::ext::register_all();
    Session session;
    LeHandle *h = session.handle;
    LeLayoutId layout{};
    LeDesignId design{};
    LeLayerId m1{};
    LeTechnologyId technology_id{};
    {
        le::ext::WriteView view = le::ext::ExtensionContext(h, "hello_ext").write();
        const le::TechnologyId technology = view.create_technology({.database_units_microns = 1000}).value();
        m1 = le::ext::to_c(view.create_layer({.technology = technology, .name = "M1", .type = "ROUTING"}).value());
        const le::DesignId top = view.create_design({.library = view.create_library({.name = "lib"}).value(), .name = "top"}).value();
        layout = le::ext::to_c(view.create_layout({.design = top}).value());
        design = le::ext::to_c(top);
        technology_id = le::ext::to_c(technology);
    } // releases the write lock: le_* calls take it themselves
    ASSERT_EQ(le_set_current_technology(h, technology_id), 0);
    // A 100 x 100 um die area, which the view fits.
    const double die[] = {1, 4, 0, 0, 100, 0, 100, 100, 0, 100};
    ASSERT_NE(le_create_shape(h, le_shape_owner_layout(layout), LeLayerId{UINT32_MAX, 0}, "BOUNDARY", 0, nullptr, 0, 1, die, 10, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index,
              UINT32_MAX);
    const auto add_pin = [&](LeLayerId layer, double x, double y) {
        const double rect[] = {x, y, x + 10, y + 10};
        return le_create_shape(h, le_shape_owner_hello_pin(le_create_hello_pin(h, layout)), layer, nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, rect, 4, 0, 0.0, 0, 0.0, 0);
    };
    const LeShapeId on_m1 = add_pin(m1, 20, 70);                       // pixels (80..120, 80..120)
    const LeShapeId no_layer = add_pin(LeLayerId{UINT32_MAX, 0}, 70, 70); // pixels (280..320, 80..120)
    ASSERT_NE(on_m1.index, UINT32_MAX);
    ASSERT_NE(no_layer.index, UINT32_MAX);

    int32_t hello_pin = -1;
    for (int32_t p = 0; p < le_purpose_kind_count(); ++p)
        if (std::string_view(le_purpose_name(p)) == "helloPin")
            hello_pin = p;
    ASSERT_GE(hello_pin, 0);

    ASSERT_EQ(le_set_current_design_layout_by_id(h, design), 0);
    le_set_viewport_size(h, 400, 400);
    le_fit_scene(h, 0); // 4 px per um: (x, y) um is pixel (4x, 400 - 4y)
    const auto lit = [&](int x, int y) {
        const LePixelBuffer buffer = le_render_pixel_buffer(h);
        if (!buffer.data || x >= buffer.width || y >= buffer.height)
            return false;
        const uint8_t *px = buffer.data + y * buffer.row_bytes + x * 4;
        return px[0] + px[1] + px[2] > 30;
    };
    EXPECT_TRUE(lit(100, 100)) << "the M1 pin";
    EXPECT_TRUE(lit(300, 100)) << "the pin with no layer";

    le_set_layer_name_visible(h, "M1", false);
    EXPECT_FALSE(lit(100, 100)) << "hidden with M1";
    EXPECT_TRUE(lit(300, 100)) << "on the HELLO_PIN row, not M1";
    le_set_layer_name_visible(h, "M1", true);
    le_set_purpose_visible(h, hello_pin, 0);
    EXPECT_FALSE(lit(100, 100)) << "hidden with the purpose";
    EXPECT_FALSE(lit(300, 100));
    le_set_purpose_visible(h, hello_pin, 1);

    le_mouse_down(h, 100, 100);
    le_mouse_up(h, 100, 100);
    ASSERT_EQ(le_selection_count(h), 1);
    EXPECT_EQ(le_selected_object_ref(h, 0).index, on_m1.index);
    le_mouse_down(h, 380, 380); // click empty space: deselect
    le_mouse_up(h, 380, 380);
    ASSERT_EQ(le_selection_count(h), 0);

    le_set_layer_name_selectable(h, "M1", 0);
    le_mouse_down(h, 100, 100);
    le_mouse_up(h, 100, 100);
    EXPECT_EQ(le_selection_count(h), 0) << "M1 isn't selectable";
    le_mouse_down(h, 300, 100);
    le_mouse_up(h, 300, 100);
    ASSERT_EQ(le_selection_count(h), 1);
    EXPECT_EQ(le_selected_object_ref(h, 0).index, no_layer.index);
}

// HelloMarker is labelled with its name (render=Render(..., label_field="name")):
// a named marker draws text over its fill that an unnamed one doesn't, and
// the label hides with the marker's row.
TEST(HelloExt, MarkersAreLabelledWithTheirNames)
{
    le::ext::register_all();
    Session session;
    LeHandle *h = session.handle;
    const LeTechnologyId technology = le_create_technology(h, 1000.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, 0, 0.0, nullptr, nullptr, 0, 0, 0, nullptr, 0, 0.0, 0,
                                                           0.0, 0, 0.0, nullptr, 0, 0, nullptr, nullptr, 0, 0.0, 0, 0.0, 0, 0.0);
    ASSERT_EQ(le_set_current_technology(h, technology), 0);
    const LeDesignId design = le_create_design(h, le_create_library(h, "lib"), "top");
    const LeLayoutId layout = le_create_layout(h, design);
    // A 100 x 100 um die area, which the view fits.
    const double die[] = {1, 4, 0, 0, 100, 0, 100, 100, 0, 100};
    ASSERT_NE(le_create_shape(h, le_shape_owner_layout(layout), LeLayerId{UINT32_MAX, 0}, "BOUNDARY", 0, nullptr, 0, 1, die, 10, 0, nullptr, 0, 0, 0.0, 0, 0.0, 0).index,
              UINT32_MAX);
    // Two identical 40 x 10 um markers, one named, one not.
    const auto add_marker = [&](const char *name, double y) {
        const double rect[] = {5.0, y, 45.0, y + 10.0};
        return le_create_shape(h, le_shape_owner_hello_marker(le_create_hello_marker(h, layout, name)), LeLayerId{UINT32_MAX, 0}, nullptr, 0, nullptr, 0, 0, nullptr, 0, 1, rect,
                               4, 0, 0.0, 0, 0.0, 0);
    };
    ASSERT_NE(add_marker("WWWWWWWW", 60.0).index, UINT32_MAX);
    ASSERT_NE(add_marker("", 20.0).index, UINT32_MAX);

    ASSERT_EQ(le_set_current_design_layout_by_id(h, design), 0);
    le_set_viewport_size(h, 400, 400);
    le_fit_scene(h, 0); // 4 px per um: (x, y) um is pixel (4x, 400 - 4y)
    // How many interior pixels differ between the named marker (pixels
    // y 120..160) and the unnamed one (y 280..320), at the same offsets.
    const auto differing_pixels = [&] {
        const LePixelBuffer buffer = le_render_pixel_buffer(h);
        if (!buffer.data)
            return -1;
        int differing = 0;
        for (int dy = 4; dy < 36; ++dy)
            for (int x = 24; x < 176; ++x)
            {
                const uint8_t *named = buffer.data + (120 + dy) * buffer.row_bytes + x * 4;
                const uint8_t *unnamed = buffer.data + (280 + dy) * buffer.row_bytes + x * 4;
                differing += std::memcmp(named, unnamed, 4) != 0;
            }
        return differing;
    };
    EXPECT_GT(differing_pixels(), 50) << "the name is drawn";

    int32_t hello_marker = -1;
    for (int32_t p = 0; p < le_purpose_kind_count(); ++p)
        if (std::string_view(le_purpose_name(p)) == "helloMarker")
            hello_marker = p;
    ASSERT_GE(hello_marker, 0);
    le_set_purpose_visible(h, hello_marker, 0);
    EXPECT_EQ(differing_pixels(), 0) << "hidden with the markers";
}
