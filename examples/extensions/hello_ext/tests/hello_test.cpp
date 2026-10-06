#include "hello_ext/hello.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

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
    constexpr const char *kSchemaVersion = "0.2.0";

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

TEST(HelloExt, AFailedEditLeavesNothingToUndo)
{
    Session session;
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    EXPECT_FALSE(hello::add_library(ctx, ""));
    EXPECT_EQ(ctx.data<hello::State>().libraries_added, 0);
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
    EXPECT_NE(info.find("extension hello_ext 0.1.0 (schema 0.2.0): this build has schema 0.2.0"), std::string::npos) << info;

    Session session;
    ASSERT_EQ(le_read_db(session.handle, path.c_str(), 1), 0);
    le::ext::ExtensionContext ctx(session.handle, "hello_ext");
    EXPECT_EQ(hello::notes_on(ctx, "lib1"), (std::vector<std::string>{"route the clock first", "then the resets"}));
    std::filesystem::remove(path);
}

TEST(HelloExt, EverySchemaVersionsGoldenFileStillLoads)
{
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

// Run once per new schema version (--gtest_also_run_disabled_tests
// --gtest_filter='*WriteGolden*'), then commit the file.
TEST(HelloExt, DISABLED_WriteGoldenFileForThisSchemaVersion)
{
    Session session;
    add_two_notes(session.handle);
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
