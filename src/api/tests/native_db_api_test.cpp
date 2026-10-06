#include "../api.hpp"
#include "../le_handle.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

// le_write_db / le_read_db / le_db_info - the native database file
// (docs/NATIVE_FILE_FORMAT_RESEARCH.md) at the API level: session state after a
// load, the empty-session rule, and the unsaved-changes flag.

namespace
{
    namespace fs = std::filesystem;

    std::string fixture_path(const std::string &name) { return std::string(API_TEST_FIXTURES_DIR) + "/" + name; }

    struct NativeDbApi : public ::testing::Test
    {
        void SetUp() override
        {
            dir = fs::temp_directory_path() / ("le_native_db_api_" + std::to_string(::getpid()));
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

    const std::string missing = le_db_info((dir / "absent.led").string().c_str());
    EXPECT_EQ(missing.rfind("error: ", 0), 0u) << missing;
}

// The session saved beside the database: the open view, viewport, current
// objects and layer/purpose/filter visibility come back with the file.
TEST_F(NativeDbApi, TheSessionComesBackWithTheFile)
{
    ASSERT_EQ(le_set_current_design_layout(source, le_design_count(source) - 1), 0);
    source->set_pan(le::Point{1234, -567});
    source->set_scale(0.125);
    le_set_layer_name_visible(source, "M1", false);
    le_set_layer_name_selectable(source, "M1", 0);
    const auto routes = le::purpose_from_label("route");
    ASSERT_TRUE(routes.has_value());
    le_set_purpose_visible(source, static_cast<int32_t>(*routes), 0);
    le_set_object_filter_value_visible(source, LE_OBJECT_FILTER_ROUTE_USE, "CLOCK", 0);
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
    EXPECT_FALSE(le_is_layer_name_visible(loaded, "M1"));
    EXPECT_EQ(le_is_layer_name_selectable(loaded, "M1"), 0);
    EXPECT_EQ(le_is_purpose_visible(loaded, static_cast<int32_t>(*routes)), 0);
    EXPECT_EQ(le_is_object_filter_value_visible(loaded, LE_OBJECT_FILTER_ROUTE_USE, "CLOCK"), 0);
    EXPECT_EQ(le_has_unsaved_database_changes(loaded), 0) << "restoring the session isn't an edit";
    le_destroy(loaded);

    // Read without the session: the defaults.
    loaded = le_create();
    ASSERT_EQ(le_read_db(loaded, path.c_str(), 0), 0);
    EXPECT_FALSE(loaded->root.get_layout(loaded->current_layout()));
    EXPECT_NE(le_is_purpose_visible(loaded, static_cast<int32_t>(*routes)), 0);
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
