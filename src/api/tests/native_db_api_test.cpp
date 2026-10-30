#include "../api.hpp"
#include "../le_handle.hpp"
#include "../../io/native_format.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include "../../test_support/scratch_dir.hpp"
#include <atomic>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

// le_write_db / le_read_db / le_db_info - the native database file
// (plans/NATIVE_FILE_FORMAT_RESEARCH.md) at the API level: session state after a
// load, the empty-session rule, and the unsaved-changes flag.

namespace
{
    namespace fs = std::filesystem;

    std::string fixture_path(const std::string &name) { return std::string(API_TEST_FIXTURES_DIR) + "/" + name; }

    struct NativeDbApi : public ::testing::Test
    {
        void SetUp() override
        {
            dir = le::test::scratch_path("le_native_db_api");
            fs::create_directories(dir);
            path = (dir / "testcell.led").string();
            source = le_create();
            ASSERT_EQ(le_read_lef(source, fixture_path("testcell.lef").c_str(), "testcell"), 0);
            ASSERT_EQ(le_read_def(source, fixture_path("testcell.def").c_str(), "testcell"), 0);
        }
        void TearDown() override
        {
            le_destroy(source);
            fs::remove_all(dir);
        }

        fs::path dir;
        std::string path;
        LeHandle *source = nullptr;
    };
}

TEST_F(NativeDbApi, WriteThenReadIntoAFreshSessionRestoresTheDesign)
{
    ASSERT_EQ(le_write_db(source, path.c_str(), 1), 0);

    LeHandle *loaded = le_create();
    ASSERT_EQ(le_read_db(loaded, path.c_str(), 1), 0);
    EXPECT_EQ(le_design_count(loaded), le_design_count(source));
    EXPECT_EQ(le_layer_count(loaded), le_layer_count(source));
    ASSERT_EQ(le_library_design_count(loaded, 0), 1);
    const LeDesignInfo design = le_library_design_at(loaded, 0, 0);
    EXPECT_NE(design.abstract_id.index, UINT32_MAX);
    EXPECT_NE(design.layout_id.index, UINT32_MAX);

    // A freshly loaded file is saved, with nothing to undo.
    EXPECT_EQ(le_has_unsaved_database_changes(loaded), 0);
    EXPECT_EQ(le_can_undo(loaded), 0);
    le_destroy(loaded);
}

TEST_F(NativeDbApi, WriteSavesPendingEdits)
{
    le_create_library(source, "EDITLIB");
    EXPECT_EQ(le_has_unsaved_database_changes(source), 1);
    EXPECT_NE(le_write_db(source, (dir / "no_such_dir" / "x.led").string().c_str(), 1), 0);
    EXPECT_EQ(le_has_unsaved_database_changes(source), 1); // a failed write saves nothing
    ASSERT_EQ(le_write_db(source, path.c_str(), 1), 0);
    EXPECT_EQ(le_has_unsaved_database_changes(source), 0);
}

TEST_F(NativeDbApi, ReadIntoANonEmptySessionFailsAndChangesNothing)
{
    ASSERT_EQ(le_write_db(source, path.c_str(), 1), 0);
    const int32_t designs = le_design_count(source);
    EXPECT_NE(le_read_db(source, path.c_str(), 1), 0);
    EXPECT_EQ(le_design_count(source), designs);
}

TEST_F(NativeDbApi, MissingOrCorruptFilesFail)
{
    LeHandle *loaded = le_create();
    EXPECT_NE(le_read_db(loaded, (dir / "absent.led").string().c_str(), 1), 0);
    EXPECT_NE(le_read_db(loaded, fixture_path("testcell.def").c_str(), 1), 0);
    EXPECT_NE(le_read_db(loaded, nullptr, 1), 0);
    EXPECT_EQ(le_design_count(loaded), 0);
    le_destroy(loaded);
}

TEST_F(NativeDbApi, WriteFailureIsReported)
{
    EXPECT_NE(le_write_db(source, (dir / "no_such_dir" / "x.led").string().c_str(), 1), 0);
    EXPECT_NE(le_write_db(source, "", 1), 0);
    EXPECT_NE(le_write_db(nullptr, path.c_str(), 1), 0);
}

TEST_F(NativeDbApi, DbInfoDescribesTheFile)
{
    ASSERT_EQ(le_write_db(source, path.c_str(), 1), 0);
    const std::string info = le_db_info(path.c_str());
    EXPECT_NE(info.find("same schema"), std::string::npos) << info;
    EXPECT_NE(info.find("Technology"), std::string::npos) << info;
    EXPECT_NE(info.find("Layout"), std::string::npos) << info;
    EXPECT_NE(info.find("migrations to run: none"), std::string::npos) << info;

    const std::string missing = le_db_info((dir / "absent.led").string().c_str());
    EXPECT_EQ(missing.rfind("error: ", 0), 0u) << missing;
}

// migrate_db rewrites a file without touching any session's database.
TEST_F(NativeDbApi, MigrateDbRewritesAFile)
{
    ASSERT_EQ(le_write_db(source, path.c_str(), 1), 0);
    const std::string out = (dir / "migrated.led").string();
    const std::string summary = le_migrate_db(path.c_str(), out.c_str());
    EXPECT_EQ(summary.rfind("migrated " + path, 0), 0u) << summary;
    EXPECT_NE(std::string(le_db_info(out.c_str())).find("session: yes"), std::string::npos);

    LeHandle *loaded = le_create();
    EXPECT_EQ(le_read_db(loaded, out.c_str(), 1), 0);
    EXPECT_EQ(le_design_count(loaded), le_design_count(source));
    le_destroy(loaded);

    EXPECT_EQ(std::string(le_migrate_db((dir / "absent.led").string().c_str(), out.c_str())).rfind("error: ", 0), 0u);
    EXPECT_EQ(std::string(le_migrate_db(path.c_str(), "")).rfind("error: ", 0), 0u);
    EXPECT_EQ(std::string(le_migrate_db(nullptr, out.c_str())).rfind("error: ", 0), 0u);
}

// The session saved beside the database: the open view, viewport, current
// objects and layer/purpose/filter visibility come back with the file.
TEST_F(NativeDbApi, TheSessionComesBackWithTheFile)
{
    ASSERT_EQ(le_set_current_design_layout(source, le_design_count(source) - 1), 0);
    source->set_pan(le::Point{1234, -567});
    source->set_scale(0.125);
    ASSERT_EQ(le_set_view_flip(source, LE_VIEW_FLIP_VERTICAL), 0);
    le_set_layer_name_visible(source, "M1", false);
    le_set_layer_name_selectable(source, "M1", 0);
    const auto routes = le::purpose_from_label("route");
    ASSERT_TRUE(routes.has_value());
    le_set_purpose_visible(source, static_cast<int32_t>(*routes), 0);
    le_set_object_filter_value_visible(source, LE_OBJECT_FILTER_ROUTE_USE, "CLOCK", 0);
    le_set_object_filter_value_visible(source, LE_OBJECT_FILTER_ROUTE_SHAPE, "FOLLOWPIN", 0);
    le_set_object_filter_value_selectable(source, LE_OBJECT_FILTER_ROUTING_STATUS, "FIXED", 0);
    le_set_object_filter_value_visible(source, LE_OBJECT_FILTER_PLACEMENT_STATUS, "COVER", 0);
    const le::LayoutId layout = source->current_layout();
    ASSERT_TRUE(source->root.get_layout(layout));
    ASSERT_EQ(le_write_db(source, path.c_str(), 1), 0);
    EXPECT_EQ(std::string(le_db_path(source)), path);

    LeHandle *loaded = le_create();
    EXPECT_EQ(le_database_is_empty(loaded), 1);
    ASSERT_EQ(le_read_db(loaded, path.c_str(), 1), 0);
    EXPECT_EQ(le_database_is_empty(loaded), 0);
    EXPECT_EQ(std::string(le_db_path(loaded)), path);
    ASSERT_TRUE(loaded->root.get_layout(loaded->current_layout()));
    EXPECT_EQ(loaded->root.get_layout(loaded->current_layout())->design, source->root.get_layout(layout)->design);
    EXPECT_EQ(loaded->current_layout_id, loaded->current_layout());
    EXPECT_EQ(loaded->pan().x, 1234);
    EXPECT_EQ(loaded->pan().y, -567);
    EXPECT_DOUBLE_EQ(loaded->scale(), 0.125);
    EXPECT_EQ(le_view_flip(loaded), LE_VIEW_FLIP_VERTICAL);
    EXPECT_FALSE(le_is_layer_name_visible(loaded, "M1"));
    EXPECT_EQ(le_is_layer_name_selectable(loaded, "M1"), 0);
    EXPECT_EQ(le_is_purpose_visible(loaded, static_cast<int32_t>(*routes)), 0);
    EXPECT_EQ(le_is_object_filter_value_visible(loaded, LE_OBJECT_FILTER_ROUTE_USE, "CLOCK"), 0);
    EXPECT_EQ(le_is_object_filter_value_visible(loaded, LE_OBJECT_FILTER_ROUTE_SHAPE, "FOLLOWPIN"), 0);
    EXPECT_EQ(le_is_object_filter_value_selectable(loaded, LE_OBJECT_FILTER_ROUTING_STATUS, "FIXED"), 0);
    EXPECT_EQ(le_is_object_filter_value_visible(loaded, LE_OBJECT_FILTER_PLACEMENT_STATUS, "COVER"), 0);
    EXPECT_EQ(le_has_unsaved_database_changes(loaded), 0) << "restoring the session isn't an edit";
    le_destroy(loaded);

    // Read without the session: the defaults.
    loaded = le_create();
    ASSERT_EQ(le_read_db(loaded, path.c_str(), 0), 0);
    EXPECT_FALSE(loaded->root.get_layout(loaded->current_layout()));
    EXPECT_NE(le_is_purpose_visible(loaded, static_cast<int32_t>(*routes)), 0);
    EXPECT_EQ(le_view_flip(loaded), LE_VIEW_FLIP_NONE);
    le_destroy(loaded);
}

TEST_F(NativeDbApi, AMistypedSessionLoadsTheDesignAndSkipsWhatItCantRead)
{
    // Not object references, so the save stores them as they are.
    const std::string session = R"({"format": "one",
        "current": {"layout": {"index": 0, "generation": "g"}},
        "view": {"layout": {"index": "zero"}, "abstract": {"index": 99999999999}},
        "viewport": {"pan": ["x", 1], "scale": "big", "flip": 3},
        "layers": {"visible": {"M1": "no"}}})";
    ASSERT_TRUE(le::persistence::save_native(source->root, path, le::persistence::SaveOptions{.session_json = session}).ok());

    LeHandle *loaded = le_create();
    ASSERT_EQ(le_read_db(loaded, path.c_str(), 1), 0);
    EXPECT_EQ(le_design_count(loaded), le_design_count(source));
    EXPECT_FALSE(loaded->root.get_layout(loaded->current_layout()));
    EXPECT_TRUE(le_is_layer_name_visible(loaded, "M1"));
    EXPECT_EQ(le_view_flip(loaded), LE_VIEW_FLIP_NONE);
    le_destroy(loaded);
}

TEST_F(NativeDbApi, NoSessionSavesTheDatabaseOnly)
{
    ASSERT_EQ(le_set_current_design_layout(source, le_design_count(source) - 1), 0);
    le_set_layer_name_visible(source, "M1", false);
    ASSERT_EQ(le_write_db(source, path.c_str(), 0), 0);
    EXPECT_NE(std::string(le_db_info(path.c_str())).find("session: no"), std::string::npos);

    LeHandle *loaded = le_create();
    ASSERT_EQ(le_read_db(loaded, path.c_str(), 1), 0);
    EXPECT_FALSE(loaded->root.get_layout(loaded->current_layout()));
    EXPECT_TRUE(le_is_layer_name_visible(loaded, "M1"));
    le_destroy(loaded);
}

// The vendored LEF/DEF parsers and writers keep process-wide state: two
// handles reading and writing at once must give what one at a time does.
TEST(LefDefAcrossHandles, ConcurrentReadsAndWritesMatchSequentialOnes)
{
    const fs::path dir = le::test::scratch_path("le_lefdef_threads");
    fs::create_directories(dir);
    const auto read_text = [](const fs::path &path) {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    // Reads the testcell fixtures into a fresh handle and writes them back.
    const auto round_trip = [&](const std::string &tag) {
        LeHandle *handle = le_create();
        bool ok = le_read_lef(handle, fixture_path("testcell.lef").c_str(), "testcell") == 0 &&
                  le_read_def(handle, fixture_path("testcell.def").c_str(), "testcell") == 0;
        const LeDesignInfo design = le_library_design_at(handle, 0, 0);
        ok = ok && le_write_lef(handle, (dir / (tag + ".lef")).string().c_str(), nullptr, 0, le_library_at(handle, 0).id,
                                LE_LEF_LAYER_WRITE_MODE_INCLUDE_WITH_ABSTRACT) == 0;
        ok = ok && le_write_def(handle, (dir / (tag + ".def")).string().c_str(), design.layout_id) == 0;
        le_destroy(handle);
        return ok;
    };
    ASSERT_TRUE(round_trip("expected"));

    constexpr int kThreads = 4, kRounds = 10;
    std::vector<std::thread> threads;
    std::atomic<int> failures = 0;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int r = 0; r < kRounds; ++r)
                if (!round_trip("t" + std::to_string(t) + "_" + std::to_string(r)))
                    ++failures;
        });
    for (std::thread &thread : threads)
        thread.join();
    EXPECT_EQ(failures.load(), 0);
    const std::string lef = read_text(dir / "expected.lef"), def = read_text(dir / "expected.def");
    for (int t = 0; t < kThreads; ++t)
        for (int r = 0; r < kRounds; ++r)
        {
            const std::string tag = "t" + std::to_string(t) + "_" + std::to_string(r);
            EXPECT_EQ(read_text(dir / (tag + ".lef")), lef) << tag;
            EXPECT_EQ(read_text(dir / (tag + ".def")), def) << tag;
        }
    fs::remove_all(dir);
}
