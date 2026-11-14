// A read made inside a recording transaction (a typed command) is one
// undo step: undo leaves the database as it was before the read, redo
// as it was after, byte for byte.

#include "../api.hpp"
#include "../le_handle.hpp"
#include "../../test_support/scratch_dir.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    std::string api_fixture(const std::string &name) { return std::string(API_TEST_FIXTURES_DIR) + "/" + name; }

    struct ReadUndo : ::testing::Test
    {
        void SetUp() override
        {
            dir = le::test::scratch_path("le_read_undo");
            fs::create_directories(dir);
            handle = le_create();
        }
        void TearDown() override
        {
            le_destroy(handle);
            fs::remove_all(dir);
        }

        // The database as write_db -no_session stores it.
        std::string database_bytes()
        {
            const fs::path path = dir / ("snapshot" + std::to_string(snapshots++) + ".led");
            EXPECT_EQ(le_write_db(handle, path.string().c_str(), 0), 0);
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        }

        // What le_repl_eval does around a typed command.
        template <typename Fn>
        int typed(const std::string &label, Fn &&read)
        {
            handle->command_history.begin(label);
            const int result = read();
            handle->command_history.end(result == 0);
            return result;
        }

        int read_lef(const std::string &fixture)
        {
            return typed("read_lef", [&] { return le_read_lef(handle, api_fixture(fixture).c_str(), "cells"); });
        }

        int read_verilog(const std::string &name, const std::string &source, int32_t is_netlist, const char *library)
        {
            const fs::path path = dir / name;
            std::ofstream(path) << source;
            const std::string path_string = path.string();
            const char *paths[] = {path_string.c_str()};
            return typed("read_verilog", [&] { return le_read_verilog(handle, paths, 1, is_netlist, library); });
        }

        fs::path dir;
        LeHandle *handle = nullptr;
        int snapshots = 0;
    };
}

TEST_F(ReadUndo, UndoAndRedoStepThroughLefAndDefReads)
{
    const std::string empty = database_bytes();
    ASSERT_EQ(read_lef("testcell.lef"), 0);
    const std::string after_lef = database_bytes();
    ASSERT_EQ(typed("read_def", [&] { return le_read_def(handle, api_fixture("testcell.def").c_str(), "cells"); }), 0);
    const std::string after_def = database_bytes();

    EXPECT_EQ(le_undo(handle), 1);
    EXPECT_EQ(database_bytes(), after_lef);
    EXPECT_EQ(le_undo(handle), 1);
    EXPECT_EQ(database_bytes(), empty);
    EXPECT_EQ(le_database_is_empty(handle), 1);

    EXPECT_EQ(le_redo(handle), 1);
    EXPECT_EQ(database_bytes(), after_lef);
    EXPECT_EQ(le_redo(handle), 1);
    EXPECT_EQ(database_bytes(), after_def);

    // Ids are kept, so undoing again after the redo works the same.
    EXPECT_EQ(le_undo(handle), 1);
    EXPECT_EQ(database_bytes(), after_lef);
}

TEST_F(ReadUndo, UndoPutsBackTheTechnologyASecondLefEditedInPlace)
{
    ASSERT_EQ(read_lef("testcell.lef"), 0);
    const std::string before = database_bytes();
    const le::TechnologyId technology_id = handle->root.get_technology_ids().front();
    const size_t layers_before = handle->root.get_technology_layers(technology_id).size();

    ASSERT_EQ(read_lef("othercell.lef"), 0);
    const std::string after = database_bytes();
    ASSERT_NE(after, before);

    EXPECT_EQ(le_undo(handle), 1);
    EXPECT_EQ(database_bytes(), before);
    EXPECT_EQ(handle->root.get_technology_layers(technology_id).size(), layers_before);
    EXPECT_EQ(le_redo(handle), 1);
    EXPECT_EQ(database_bytes(), after);
}

TEST_F(ReadUndo, UndoUnlinksOlderInstancesTheReadResolved)
{
    // TOP's u1 stays unresolved until a later read defines LEAF.
    ASSERT_EQ(read_verilog("top.v", "module TOP();\n  LEAF u1();\nendmodule\n", /*is_netlist=*/0, "rtl"), 0);
    const std::string unresolved = database_bytes();
    ASSERT_EQ(read_verilog("leaf.v", "module LEAF();\nendmodule\n", /*is_netlist=*/0, "rtl"), 0);
    const std::string linked = database_bytes();

    const auto instance_ids = handle->root.get_instance_ids();
    ASSERT_EQ(instance_ids.size(), 1u);
    ASSERT_TRUE(handle->root.get_instance(instance_ids.front())->reference_design.valid());

    EXPECT_EQ(le_undo(handle), 1);
    EXPECT_FALSE(handle->root.get_instance(instance_ids.front())->reference_design.valid());
    EXPECT_EQ(database_bytes(), unresolved);
    EXPECT_EQ(le_redo(handle), 1);
    EXPECT_TRUE(handle->root.get_instance(instance_ids.front())->reference_design.valid());
    EXPECT_EQ(database_bytes(), linked);
}

TEST_F(ReadUndo, AFailedTypedReadRecordsNothing)
{
    ASSERT_EQ(read_lef("testcell.lef"), 0);
    const std::string before = database_bytes();
    EXPECT_NE(read_lef("fails_after_macro.lef"), 0);
    EXPECT_EQ(database_bytes(), before);

    // The one undo step is the first read.
    EXPECT_EQ(le_undo(handle), 1);
    EXPECT_EQ(le_database_is_empty(handle), 1);
    EXPECT_EQ(le_can_undo(handle), 0);
}

TEST_F(ReadUndo, AReadOutsideATypedCommandIsNotAnUndoStep)
{
    ASSERT_EQ(le_read_lef(handle, api_fixture("testcell.lef").c_str(), "cells"), 0);
    EXPECT_EQ(le_can_undo(handle), 0);
}

TEST_F(ReadUndo, AnUndoneReadLeavingTheHistoryFreesItsSlotsForTheNextRead)
{
    ASSERT_EQ(read_lef("testcell.lef"), 0);
    const std::string after = database_bytes();
    EXPECT_EQ(le_undo(handle), 1);

    // A new step clears the redo stack, releasing the undone read's ids.
    ASSERT_EQ(read_lef("testcell.lef"), 0);
    EXPECT_EQ(le_can_redo(handle), 0);
    EXPECT_EQ(database_bytes(), after);
}
