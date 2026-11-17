// A read made inside a recording transaction (a typed command) is one
// undo step: undo leaves the database as it was before the read, redo
// as it was after, byte for byte.

#include "session_test.hpp"
#include "../le_handle.hpp"
#include "../../test_support/scratch_dir.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <span>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    using le::test::fixture_path;

    struct ReadUndo : le::test::SessionTest
    {
        void SetUp() override
        {
            dir = le::test::scratch_path("le_read_undo");
            fs::create_directories(dir);
        }
        void TearDown() override { fs::remove_all(dir); }

        // The database as write_db -no_session stores it.
        std::string database_bytes()
        {
            const fs::path path = dir / ("snapshot" + std::to_string(snapshots++) + ".led");
            EXPECT_TRUE(write().write_db(path.string()));
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        }

        // What a typed Tcl command does around a read: one undo step.
        template <typename Fn>
        bool typed(const std::string &label, Fn &&read)
        {
            write().begin_command(label);
            const bool succeeded = read();
            write().end_command(succeeded);
            return succeeded;
        }

        bool read_cells(const std::string &fixture)
        {
            return typed("read_lef", [&] { return write().read_lef(fixture_path(fixture), "cells").has_value(); });
        }

        bool read_verilog(const std::string &name, const std::string &source, bool netlist, const std::string &library)
        {
            const fs::path path = dir / name;
            std::ofstream(path) << source;
            const std::string paths[] = {path.string()};
            return typed("read_verilog", [&] { return write().read_verilog(paths, netlist, library).has_value(); });
        }

        fs::path dir;
        int snapshots = 0;
    };
}

TEST_F(ReadUndo, UndoAndRedoStepThroughLefAndDefReads)
{
    const std::string empty = database_bytes();
    ASSERT_TRUE(read_cells("testcell.lef"));
    const std::string after_lef = database_bytes();
    ASSERT_TRUE(typed("read_def", [&] { return write().read_def(fixture_path("testcell.def"), "cells").has_value(); }));
    const std::string after_def = database_bytes();

    EXPECT_EQ(write().undo(), true);
    EXPECT_EQ(database_bytes(), after_lef);
    EXPECT_EQ(write().undo(), true);
    EXPECT_EQ(database_bytes(), empty);
    EXPECT_TRUE(read().database_is_empty());

    EXPECT_EQ(write().redo(), true);
    EXPECT_EQ(database_bytes(), after_lef);
    EXPECT_EQ(write().redo(), true);
    EXPECT_EQ(database_bytes(), after_def);

    // Ids are kept, so undoing again after the redo works the same.
    EXPECT_EQ(write().undo(), true);
    EXPECT_EQ(database_bytes(), after_lef);
}

TEST_F(ReadUndo, UndoPutsBackTheTechnologyASecondLefEditedInPlace)
{
    ASSERT_TRUE(read_cells("testcell.lef"));
    const std::string before = database_bytes();
    const le::TechnologyId technology_id = handle->root.get_technology_ids().front();
    const size_t layers_before = handle->root.get_technology_layers(technology_id).size();

    ASSERT_TRUE(read_cells("othercell.lef"));
    const std::string after = database_bytes();
    ASSERT_NE(after, before);

    EXPECT_EQ(write().undo(), true);
    EXPECT_EQ(database_bytes(), before);
    EXPECT_EQ(handle->root.get_technology_layers(technology_id).size(), layers_before);
    EXPECT_EQ(write().redo(), true);
    EXPECT_EQ(database_bytes(), after);
}

TEST_F(ReadUndo, UndoUnlinksOlderInstancesTheReadResolved)
{
    // TOP's u1 stays unresolved until a later read defines LEAF.
    ASSERT_TRUE(read_verilog("top.v", "module TOP();\n  LEAF u1();\nendmodule\n", /*is_netlist=*/0, "rtl"));
    const std::string unresolved = database_bytes();
    ASSERT_TRUE(read_verilog("leaf.v", "module LEAF();\nendmodule\n", /*is_netlist=*/0, "rtl"));
    const std::string linked = database_bytes();

    const auto instance_ids = handle->root.get_instance_ids();
    ASSERT_EQ(instance_ids.size(), 1u);
    ASSERT_TRUE(handle->root.get_instance(instance_ids.front())->reference_design.valid());

    EXPECT_EQ(write().undo(), true);
    EXPECT_FALSE(handle->root.get_instance(instance_ids.front())->reference_design.valid());
    EXPECT_EQ(database_bytes(), unresolved);
    EXPECT_EQ(write().redo(), true);
    EXPECT_TRUE(handle->root.get_instance(instance_ids.front())->reference_design.valid());
    EXPECT_EQ(database_bytes(), linked);
}

TEST_F(ReadUndo, AFailedTypedReadRecordsNothing)
{
    ASSERT_TRUE(read_cells("testcell.lef"));
    const std::string before = database_bytes();
    EXPECT_FALSE(read_cells("fails_after_macro.lef"));
    EXPECT_EQ(database_bytes(), before);

    // The one undo step is the first read.
    EXPECT_EQ(write().undo(), true);
    EXPECT_TRUE(read().database_is_empty());
    EXPECT_FALSE(read().can_undo());
}

TEST_F(ReadUndo, AReadOutsideATypedCommandIsNotAnUndoStep)
{
    ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "cells"));
    EXPECT_FALSE(read().can_undo());
}

TEST_F(ReadUndo, AnUndoneReadLeavingTheHistoryFreesItsSlotsForTheNextRead)
{
    ASSERT_TRUE(read_cells("testcell.lef"));
    const std::string after = database_bytes();
    EXPECT_EQ(write().undo(), true);

    // A new step clears the redo stack, releasing the undone read's ids.
    ASSERT_TRUE(read_cells("testcell.lef"));
    EXPECT_FALSE(read().can_redo());
    EXPECT_EQ(database_bytes(), after);
}
