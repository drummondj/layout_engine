#include "hello_ext/hello.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <variant>

namespace
{
    // A scratch file in the temp directory, unique to this test process:
    // ctest runs each test in its own process, in parallel, and other build
    // trees may run the same tests at the same time.
    std::filesystem::path scratch_path(const std::string &name)
    {
        return std::filesystem::temp_directory_path() / ("hello_ext_" + std::to_string(::getpid()) + "_" + name);
    }

    // Golden files: .led files written by each schema version of this
    // extension, loaded by every later build.
    const std::filesystem::path kGoldenDir = std::filesystem::path(__FILE__).parent_path() / "golden";

    // schema_ext.py's VERSION: the golden directory a new file goes in.
    constexpr const char *kSchemaVersion = "0.4.0";

    le::ext::ExtensionContext context(le::Session &session) { return le::ext::ExtensionContext(session.handle(), "hello_ext"); }

    // A HelloMarker owning one Shape (a 2x1 um DEBUG rect), in a new layout.
    le::HelloMarkerId add_marker(le::Session &session)
    {
        le::WriteView view = session.write();
        view.create_technology({.database_units_microns = 1000}).value(); // 1000 dbu per micron
        const le::LibraryId library = view.create_library({.name = "markers"}).value();
        const le::DesignId design = view.create_design({.library = library, .name = "top"}).value();
        const le::LayoutId layout = view.create_layout({.design = design}).value();
        const le::HelloMarkerId marker = view.create_hello_marker({.layout = layout, .name = "clock_root"}).value();
        view.build_shape(le::ShapeOwner::hello_marker(marker)).purpose(le::ShapePurpose::DEBUG).rect(0, 0, 2, 1).create().value();
        return marker;
    }

    // How many markers the session has, and how many shapes they own.
    std::pair<size_t, size_t> markers_and_shapes(le::Session &session)
    {
        const le::ReadView view = session.read();
        size_t shapes = 0;
        for (const le::HelloMarkerId marker : view.root().get_hello_marker_ids())
            shapes += view.root().get_hello_marker_shapes(marker).size();
        return {view.root().get_hello_marker_ids().size(), shapes};
    }

    le::LibraryId add_two_notes(le::Session &session)
    {
        le::WriteView view = session.write();
        const le::LibraryId library = view.create_library({.name = "lib1"}).value();
        view.create_hello_note({.library = library, .body = "route the clock first"}).value();
        view.create_hello_note({.library = library, .body = "then the resets"}).value();
        return library;
    }

    // A design "top" with a Layout, under a 1000 dbu/um Technology that's
    // current (as reading a LEF makes it), and that Layout open.
    struct OpenLayout
    {
        le::DesignId design;
        le::LayoutId layout;
    };

    OpenLayout open_new_layout(le::WriteView &view)
    {
        const le::TechnologyId technology = view.create_technology({.database_units_microns = 1000}).value();
        view.set_current_technology(technology).value();
        const le::DesignId design = view.create_design({.library = view.create_library({.name = "lib"}).value(), .name = "top"}).value();
        const le::LayoutId layout = view.create_layout({.design = design}).value();
        return {design, layout};
    }

    // A 100 x 100 um die area, which the view fits.
    void add_die_area(le::WriteView &view, le::LayoutId layout)
    {
        view.build_shape(le::ShapeOwner::layout(layout)).purpose(le::ShapePurpose::BOUNDARY).polygon({{0, 0}, {100, 0}, {100, 100}, {0, 100}}).create().value();
    }

    // Whether pixel (x, y) of a fresh render is drawn on.
    bool lit(le::Session &session, int x, int y)
    {
        const le::RenderedFrame frame = session.renderer().render();
        if (frame.empty() || x >= frame.width || y >= frame.height)
            return false;
        const uint8_t *px = frame.data + y * frame.row_bytes + x * 4;
        return px[0] + px[1] + px[2] > 30;
    }

    void click(le::Session &session, int x, int y)
    {
        session.write().mouse_down(x, y);
        session.write().mouse_up(x, y);
    }

    // The Shape or Wire of each selected piece.
    std::vector<le::GeometryId> selected_shapes(le::Session &session)
    {
        std::vector<le::GeometryId> shapes;
        for (const le::SelectedObject &entry : session.read().selection())
            if (const le::ShapePiece *piece = std::get_if<le::ShapePiece>(&entry))
                shapes.push_back(piece->owner());
        return shapes;
    }

    // A purpose by its label: an extension's purposes are named after it.
    std::optional<le::ViewLayerPurpose> purpose_named(std::string_view label)
    {
        for (const le::ViewLayerPurposeInfo &info : le::kViewLayerPurposes)
            if (info.label == label)
                return info.purpose;
        return std::nullopt;
    }
}

TEST(HelloExt, IsRegistered)
{
    le::ext::register_all();
    le::ext::register_all(); // idempotent
    const auto &extensions = le::ext::registry().extensions();
    const auto hello = std::ranges::find(extensions, std::string("hello_ext"), &le::ext::ExtensionInfo::name);
    ASSERT_NE(hello, extensions.end());
    EXPECT_EQ(hello->version, "0.1.0");
    EXPECT_EQ(std::ranges::count(extensions, std::string("hello_ext"), &le::ext::ExtensionInfo::name), 1);
}

TEST(HelloExt, ReadsTheDatabase)
{
    le::Session session;
    le::ext::ExtensionContext ctx = context(session);
    EXPECT_EQ(hello::library_count(ctx), 0);
    session.write().create_library({.name = "lib1"}).value();
    EXPECT_EQ(hello::library_count(ctx), 1);
}

TEST(HelloExt, AnEditIsOneUndoStep)
{
    le::Session session;
    le::ext::ExtensionContext ctx = context(session);
    ASSERT_TRUE(hello::add_library(ctx, "world"));
    EXPECT_EQ(hello::library_count(ctx), 1);
    ASSERT_EQ(session.write().undo(), true);
    EXPECT_EQ(hello::library_count(ctx), 0);
}

TEST(HelloExt, StateIsPerSessionAndPerExtension)
{
    le::Session a, b;
    le::ext::ExtensionContext in_a = context(a);
    le::ext::ExtensionContext in_b = context(b);
    le::ext::ExtensionContext other_extension(a.handle(), "other_ext");
    ASSERT_TRUE(hello::add_library(in_a, "one"));
    ASSERT_TRUE(hello::add_library(in_a, "two"));
    EXPECT_EQ(in_a.data<hello::State>().libraries_added, 2);
    EXPECT_EQ(in_b.data<hello::State>().libraries_added, 0);
    EXPECT_EQ(other_extension.data<hello::State>().libraries_added, 0);
}

TEST(HelloExt, AnEditIsRecalledByItsLabel)
{
    le::Session session;
    le::ext::ExtensionContext ctx = context(session);
    ASSERT_TRUE(hello::add_library(ctx, "world"));
    const le::ReadView view = session.read();
    ASSERT_EQ(view.command_history_size(), 1u);
    EXPECT_EQ(view.command_history_at(0), "hello_add_library world");
}

TEST(HelloExt, AFailedEditLeavesNothingToUndo)
{
    le::Session session;
    le::ext::ExtensionContext ctx = context(session);
    EXPECT_FALSE(hello::add_library(ctx, ""));
    EXPECT_EQ(ctx.data<hello::State>().libraries_added, 0);
    EXPECT_EQ(session.write().undo(), false);
}

TEST(HelloExt, AddsAMarkerToTheCurrentLayoutAsOneUndoStep)
{
    le::Session session;
    le::ext::ExtensionContext ctx = context(session);
    le::LayoutId layout;
    {
        le::WriteView view = session.write();
        const le::DesignId design = view.create_design({.library = view.create_library({.name = "lib"}).value(), .name = "top"}).value();
        layout = view.create_layout({.design = design}).value();
        ASSERT_TRUE(view.set_current_layout(layout));
    }

    ASSERT_TRUE(hello::add_marker(ctx, "clock_root"));
    EXPECT_EQ(ctx.read().root().get_layout_hello_markers(layout).size(), 1u);
    ASSERT_EQ(session.write().undo(), true);
    EXPECT_EQ(ctx.read().root().get_layout_hello_markers(layout).size(), 0u);
}

TEST(HelloExt, AddingAMarkerWithNoCurrentLayoutFails)
{
    le::Session session;
    le::ext::ExtensionContext ctx = context(session);
    {
        le::WriteView view = session.write();
        const le::DesignId design = view.create_design({.library = view.create_library({.name = "lib"}).value(), .name = "top"}).value();
        view.create_layout({.design = design}).value();
    }
    EXPECT_FALSE(hello::add_marker(ctx, "clock_root"));
    EXPECT_EQ(markers_and_shapes(session).first, 0u);
    EXPECT_EQ(session.write().undo(), false);
}

TEST(HelloExt, AWriteViewEditIsSeenByReaders)
{
    le::Session session;
    le::ext::ExtensionContext ctx = context(session);
    {
        le::ext::WriteView view = ctx.write();
        view.unrecorded_root().create_library(le::LibraryData{.name = "bulk"});
    }
    EXPECT_EQ(hello::library_count(ctx), 1);
}

TEST(HelloExt, NotesBelongToTheirLibraryAndGoWithIt)
{
    le::Session session;
    le::ext::ExtensionContext ctx = context(session);
    const le::LibraryId library = add_two_notes(session);
    EXPECT_EQ(hello::notes_on(ctx, "lib1"), (std::vector<std::string>{"route the clock first", "then the resets"}));
    EXPECT_TRUE(hello::notes_on(ctx, "nope").empty());

    ASSERT_TRUE(session.write("delete_library lib1").delete_library(library));
    EXPECT_EQ(ctx.read().root().get_hello_note_ids().size(), 0u);
    ASSERT_EQ(session.write().undo(), true);
    EXPECT_EQ(hello::notes_on(ctx, "lib1").size(), 2u);
}

TEST(HelloExt, NotesRoundTripThroughANativeFile)
{
    const std::filesystem::path path = scratch_path("notes_test.led");
    {
        le::Session session;
        add_two_notes(session);
        ASSERT_TRUE(session.write().write_db(path, true));
    }
    const std::string info = le::db_info(path).value();
    const std::string schema = kSchemaVersion;
    EXPECT_NE(info.find("extension hello_ext 0.1.0 (schema " + schema + "): this build has schema " + schema), std::string::npos) << info;

    le::Session session;
    ASSERT_TRUE(session.write().read_db(path, true));
    le::ext::ExtensionContext ctx = context(session);
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
        le::Session session;
        const std::filesystem::path file = version.path() / "notes.led";
        ASSERT_TRUE(session.write().read_db(file, true)) << file;
        le::ext::ExtensionContext ctx = context(session);
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
    const std::string info = le::db_info(oldest).value();
    EXPECT_NE(info.find("hello_ext 0.2.0: HelloNote.text renamed to body"), std::string::npos) << info;
    EXPECT_NE(info.find("hello_ext 0.4.0: HelloPin added, owning Shapes"), std::string::npos) << info;

    const std::string path = scratch_path("migrated.led").string();
    const std::expected<std::string, std::string> summary = le::migrate_db(oldest, path);
    ASSERT_TRUE(summary) << summary.error();
    EXPECT_NE(summary->find("applied hello_ext migration to 0.2.0"), std::string::npos) << *summary;
    EXPECT_NE(le::db_info(path).value().find("migrations to run: none"), std::string::npos);
    le::Session session;
    ASSERT_TRUE(session.write().read_db(path, true));
    le::ext::ExtensionContext ctx = context(session);
    EXPECT_EQ(hello::notes_on(ctx, "lib1"), (std::vector<std::string>{"route the clock first", "then the resets"}));
    std::filesystem::remove(path);
}

// Run once per new schema version (--gtest_also_run_disabled_tests
// --gtest_filter='*WriteGolden*'), then commit the file.
TEST(HelloExt, DISABLED_WriteGoldenFileForThisSchemaVersion)
{
    le::Session session;
    add_two_notes(session);
    add_marker(session);
    {
        // And a HelloPin owning a shape, in a layout of its own.
        le::WriteView view = session.write();
        const le::DesignId design = view.create_design({.library = view.create_library({.name = "pins"}).value(), .name = "top"}).value();
        const le::LayoutId pins = view.create_layout({.design = design}).value();
        const le::HelloPinId pin = view.create_hello_pin({.layout = pins}).value();
        view.build_shape(le::ShapeOwner::hello_pin(pin)).rect(1.0, 1.0, 2.0, 2.0).create().value();
    }
    std::filesystem::create_directories(kGoldenDir / kSchemaVersion);
    ASSERT_TRUE(session.write().write_db(kGoldenDir / kSchemaVersion / "notes.led", true));
}

// hello_ext's section of settings.json: saved with the core settings, its
// own version beside it; another extension's section survives a build
// without that extension.
TEST(HelloExt, ItsSettingsSectionRoundTripsAndOthersAreKept)
{
    le::ext::register_all();
    const std::filesystem::path path = scratch_path("settings_test.json");
    {
        le::Session session;
        context(session).data<hello::State>().library_name = "mine";
        ASSERT_TRUE(session.write().save_settings(path));
    }
    std::ifstream in(path);
    nlohmann::json saved = nlohmann::json::parse(in);
    EXPECT_EQ(saved["extensions"]["hello_ext"], (nlohmann::json{{"library_name", "mine"}, {"version", 1}}));

    saved["extensions"]["other_ext"] = {{"version", 3}, {"colour", "red"}};
    std::ofstream(path) << saved.dump();
    le::Session session;
    ASSERT_TRUE(session.write().load_settings(path));
    EXPECT_EQ(context(session).data<hello::State>().library_name, "mine");
    ASSERT_TRUE(session.write().save_settings(path));
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
    le::Session session;
    le::HelloMarkerId marker;
    {
        le::Transaction transaction = session.transaction("markers");
        marker = add_marker(session);
    }
    EXPECT_EQ(markers_and_shapes(session), (std::pair<size_t, size_t>{1, 1}));

    ASSERT_TRUE(session.write("delete marker").delete_hello_marker(marker));
    EXPECT_EQ(markers_and_shapes(session), (std::pair<size_t, size_t>{0, 0}));
    EXPECT_EQ(session.read().root().get_shape_ids().size(), 0u) << "the shape goes with its marker";
    ASSERT_EQ(session.write().undo(), true);
    EXPECT_EQ(markers_and_shapes(session), (std::pair<size_t, size_t>{1, 1}));

    const std::filesystem::path path = scratch_path("markers_test.led");
    ASSERT_TRUE(session.write().write_db(path, true));
    le::Session loaded;
    ASSERT_TRUE(loaded.write().read_db(path, true));
    EXPECT_EQ(markers_and_shapes(loaded), (std::pair<size_t, size_t>{1, 1}));
    std::filesystem::remove(path);
}

// HelloMarker has render=: its shapes draw on the HELLO_MARKER row of the
// Layout view and are selected by a click, and an edit redraws them.
TEST(HelloExt, MarkersDrawAndSelectInTheLayoutView)
{
    le::ext::register_all();
    le::Session session;
    le::ShapeId shape;
    {
        le::WriteView view = session.write();
        const OpenLayout top = open_new_layout(view);
        const le::HelloMarkerId marker = view.create_hello_marker({.layout = top.layout, .name = "clock_root"}).value();
        shape = view.build_shape(le::ShapeOwner::hello_marker(marker)).rect(0.0, 0.0, 2.0, 1.0).create().value();
        ASSERT_TRUE(view.open_layout(top.design));
        view.set_viewport_size(200, 100);
        view.fit_view(10);
    }
    EXPECT_TRUE(lit(session, 60, 30)) << "the marker's rect fills the view";

    click(session, 100, 50);
    EXPECT_EQ(selected_shapes(session), std::vector<le::GeometryId>{le::GeometryId(shape)});

    // Moving the marker's shape out of the way redraws its chunk.
    {
        le::WriteView view = session.write();
        const le::Units units = view.units().value();
        ASSERT_TRUE(view.update_shape(shape, {.rects = le::CompactVector<le::Rect>{units.to_dbu(le::RectUm{{10.0, 10.0}, {11.0, 11.0}})}}));
    }
    EXPECT_FALSE(lit(session, 60, 30)) << "away from the cursor box the click left at (100, 50)";
}

// HelloPin has render=Render(..., tiled=True): a layout's pins are split into
// spatial tiles. Enough pins for several tiles draw and select, and moving
// one into another tile redraws both tiles.
TEST(HelloExt, TiledPinsDrawSelectAndMoveBetweenTiles)
{
    le::ext::register_all();
    le::Session session;
    le::ShapeId lone;
    {
        le::WriteView view = session.write();
        const OpenLayout top = open_new_layout(view);
        add_die_area(view, top.layout);
        const auto add_pin = [&](double x, double y, double size)
        {
            const le::HelloPinId pin = view.create_hello_pin({.layout = top.layout}).value();
            return view.build_shape(le::ShapeOwner::hello_pin(pin)).rect(x, y, x + size, y + size).create().value();
        };
        // 6000 small pins in the left half (three tiles' worth), and one big
        // one alone at (73..77, 73..77) um.
        for (int i = 0; i < 6000; ++i)
            add_pin(0.8 * (i % 60), (i / 60) * 1.0, 0.4);
        lone = add_pin(73, 73, 4);

        ASSERT_TRUE(view.open_layout(top.design));
        view.set_viewport_size(400, 400);
        view.fit_view(0); // 4 px per um: (x, y) um is pixel (4x, 400 - 4y)
    }
    EXPECT_TRUE(lit(session, 300, 100)) << "the lone pin at (75, 75) um";
    EXPECT_FALSE(lit(session, 300, 300)) << "nothing at (75, 25) um yet";

    click(session, 300, 100);
    EXPECT_EQ(selected_shapes(session), std::vector<le::GeometryId>{le::GeometryId(lone)});
    click(session, 10, 10); // empty space: deselect, and move the cursor box away

    // Into another tile: drawn there, gone from where it was.
    {
        le::WriteView view = session.write();
        const le::Units units = view.units().value();
        ASSERT_TRUE(view.update_shape(lone, {.rects = le::CompactVector<le::Rect>{units.to_dbu(le::RectUm{{73.0, 23.0}, {77.0, 27.0}})}}));
    }
    EXPECT_TRUE(lit(session, 300, 300));
    EXPECT_FALSE(lit(session, 300, 100));
}

// HelloPin is also per_layer: a pin's shapes draw in the helloPin column of
// their layer's row, so hiding the layer or the purpose hides them and the
// layer's selectable toggle applies; a pin with no layer stays on the
// HELLO_PIN row.
TEST(HelloExt, PinsOnALayerDrawAndSelectInThatLayersRow)
{
    le::ext::register_all();
    const std::optional<le::ViewLayerPurpose> hello_pin = purpose_named("helloPin");
    ASSERT_TRUE(hello_pin.has_value());

    le::Session session;
    le::ShapeId on_m1;
    le::ShapeId no_layer;
    {
        le::WriteView view = session.write();
        const OpenLayout top = open_new_layout(view);
        const le::LayerId m1 = view.create_layer({.technology = view.current_technology(), .name = "M1", .type = "ROUTING"}).value();
        add_die_area(view, top.layout);
        const auto add_pin = [&](le::LayerId layer, double x, double y)
        {
            const le::HelloPinId pin = view.create_hello_pin({.layout = top.layout}).value();
            le::ShapeBuilder shape = view.build_shape(le::ShapeOwner::hello_pin(pin));
            if (layer.valid())
                shape.layer(layer);
            return shape.rect(x, y, x + 10, y + 10).create().value();
        };
        on_m1 = add_pin(m1, 20, 70);                // pixels (80..120, 80..120)
        no_layer = add_pin(le::LayerId{}, 70, 70);  // pixels (280..320, 80..120)

        ASSERT_TRUE(view.open_layout(top.design));
        view.set_viewport_size(400, 400);
        view.fit_view(0); // 4 px per um: (x, y) um is pixel (4x, 400 - 4y)
    }
    EXPECT_TRUE(lit(session, 100, 100)) << "the M1 pin";
    EXPECT_TRUE(lit(session, 300, 100)) << "the pin with no layer";

    session.write().set_layer_visible("M1", false);
    EXPECT_FALSE(lit(session, 100, 100)) << "hidden with M1";
    EXPECT_TRUE(lit(session, 300, 100)) << "on the HELLO_PIN row, not M1";
    session.write().set_layer_visible("M1", true);
    session.write().set_purpose_visible(*hello_pin, false);
    EXPECT_FALSE(lit(session, 100, 100)) << "hidden with the purpose";
    EXPECT_FALSE(lit(session, 300, 100));
    session.write().set_purpose_visible(*hello_pin, true);

    click(session, 100, 100);
    EXPECT_EQ(selected_shapes(session), std::vector<le::GeometryId>{le::GeometryId(on_m1)});
    click(session, 380, 380); // empty space: deselect
    ASSERT_TRUE(session.read().selection().empty());

    session.write().set_layer_selectable("M1", false);
    click(session, 100, 100);
    EXPECT_TRUE(session.read().selection().empty()) << "M1 isn't selectable";
    click(session, 300, 100);
    EXPECT_EQ(selected_shapes(session), std::vector<le::GeometryId>{le::GeometryId(no_layer)});
}

// HelloMarker is labelled with its name (render=Render(..., label_field="name")):
// a named marker draws text over its fill that an unnamed one doesn't, and
// the label hides with the marker's row.
TEST(HelloExt, MarkersAreLabelledWithTheirNames)
{
    le::ext::register_all();
    const std::optional<le::ViewLayerPurpose> hello_marker = purpose_named("helloMarker");
    ASSERT_TRUE(hello_marker.has_value());

    le::Session session;
    {
        le::WriteView view = session.write();
        const OpenLayout top = open_new_layout(view);
        add_die_area(view, top.layout);
        // Two identical 40 x 10 um markers, one named, one not.
        for (const auto &[name, y] : {std::pair{"WWWWWWWW", 60.0}, std::pair{"", 20.0}})
        {
            const le::HelloMarkerId marker = view.create_hello_marker({.layout = top.layout, .name = name}).value();
            view.build_shape(le::ShapeOwner::hello_marker(marker)).rect(5.0, y, 45.0, y + 10.0).create().value();
        }
        ASSERT_TRUE(view.open_layout(top.design));
        view.set_viewport_size(400, 400);
        view.fit_view(0); // 4 px per um: (x, y) um is pixel (4x, 400 - 4y)
    }
    // How many interior pixels differ between the named marker (pixels
    // y 120..160) and the unnamed one (y 280..320), at the same offsets.
    const auto differing_pixels = [&]
    {
        const le::RenderedFrame frame = session.renderer().render();
        if (frame.empty())
            return -1;
        int differing = 0;
        for (int dy = 4; dy < 36; ++dy)
            for (int x = 24; x < 176; ++x)
            {
                const uint8_t *named = frame.data + (120 + dy) * frame.row_bytes + x * 4;
                const uint8_t *unnamed = frame.data + (280 + dy) * frame.row_bytes + x * 4;
                differing += std::memcmp(named, unnamed, 4) != 0;
            }
        return differing;
    };
    EXPECT_GT(differing_pixels(), 50) << "the name is drawn";

    session.write().set_purpose_visible(*hello_marker, false);
    EXPECT_EQ(differing_pixels(), 0) << "hidden with the markers";
}

// The overlay reads show_origin on the render thread while the GUI's menu
// item toggles it: both go through with_data<State>().
TEST(HelloExt, TheOverlayReadsItsStateWhileAnotherThreadWritesIt)
{
    le::ext::register_all();
    le::Session session;
    {
        le::WriteView view = session.write();
        const OpenLayout top = open_new_layout(view);
        ASSERT_TRUE(view.open_layout(top.design));
        view.set_viewport_size(200, 100);
        view.fit_view(10);
    }

    std::atomic<bool> done = false;
    std::thread toggler([&]
                        {
                            le::ext::ExtensionContext ctx = context(session);
                            while (!done)
                                ctx.with_data<hello::State>([](hello::State &state) { state.show_origin = !state.show_origin; }); });
    for (int i = 0; i < 200; ++i)
    {
        session.write().set_mouse_position(i % 200, 50); // recomposes, so the overlay runs
        ASSERT_FALSE(session.renderer().render().empty());
    }
    done = true;
    toggler.join();
}
