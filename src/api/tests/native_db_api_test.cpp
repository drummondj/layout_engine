#include "../api.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

// le_write_db / le_read_db / le_db_info - the native database file
// (NATIVE_FILE_FORMAT_RESEARCH.md) at the API level: session state after a
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
    ASSERT_EQ(le_write_db(source, path.c_str()), 0);

    LeHandle *loaded = le_create();
    ASSERT_EQ(le_read_db(loaded, path.c_str()), 0);
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

TEST_F(NativeDbApi, ReadIntoANonEmptySessionFailsAndChangesNothing)
{
    ASSERT_EQ(le_write_db(source, path.c_str()), 0);
    const int32_t designs = le_design_count(source);
    EXPECT_NE(le_read_db(source, path.c_str()), 0);
    EXPECT_EQ(le_design_count(source), designs);
}

TEST_F(NativeDbApi, MissingOrCorruptFilesFail)
{
    LeHandle *loaded = le_create();
    EXPECT_NE(le_read_db(loaded, (dir / "absent.led").string().c_str()), 0);
    EXPECT_NE(le_read_db(loaded, fixture_path("testcell.def").c_str()), 0);
    EXPECT_NE(le_read_db(loaded, nullptr), 0);
    EXPECT_EQ(le_design_count(loaded), 0);
    le_destroy(loaded);
}

TEST_F(NativeDbApi, WriteFailureIsReported)
{
    EXPECT_NE(le_write_db(source, (dir / "no_such_dir" / "x.led").string().c_str()), 0);
    EXPECT_NE(le_write_db(source, ""), 0);
    EXPECT_NE(le_write_db(nullptr, path.c_str()), 0);
}

TEST_F(NativeDbApi, DbInfoDescribesTheFile)
{
    ASSERT_EQ(le_write_db(source, path.c_str()), 0);
    const std::string info = le_db_info(path.c_str());
    EXPECT_NE(info.find("same schema"), std::string::npos) << info;
    EXPECT_NE(info.find("Technology"), std::string::npos) << info;
    EXPECT_NE(info.find("Layout"), std::string::npos) << info;

    const std::string missing = le_db_info((dir / "absent.led").string().c_str());
    EXPECT_EQ(missing.rfind("error: ", 0), 0u) << missing;
}
