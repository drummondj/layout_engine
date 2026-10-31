// A failed read changes nothing: the database writes the same bytes
// before and after it.

#include "../api.hpp"
#include "../le_handle.hpp"
#include "../../io/read_rollback.hpp"
#include "../../test_support/scratch_dir.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    std::string api_fixture(const std::string &name) { return std::string(API_TEST_FIXTURES_DIR) + "/" + name; }
    std::string io_fixture(const std::string &name) { return std::string(IO_TEST_FIXTURES_DIR) + "/" + name; }

    struct ReadRollbackApi : ::testing::Test
    {
        void SetUp() override
        {
            dir = le::test::scratch_path("le_read_rollback");
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

        void read_testcell()
        {
            ASSERT_EQ(le_read_lef(handle, api_fixture("testcell.lef").c_str(), "cells"), 0);
            ASSERT_EQ(le_read_def(handle, api_fixture("testcell.def").c_str(), "cells"), 0);
        }

        fs::path dir;
        LeHandle *handle = nullptr;
        int snapshots = 0;
    };
}

TEST_F(ReadRollbackApi, ALefParseErrorAfterAMacroLeavesTheDatabaseAsItWas)
{
    read_testcell();
    const std::string before = database_bytes();
    const le::TechnologyData technology = *handle->root.get_technology(handle->root.get_technology_ids().front());

    EXPECT_NE(le_read_lef(handle, api_fixture("fails_after_macro.lef").c_str(), "cells"), 0);
    EXPECT_EQ(database_bytes(), before);
    // The Technology fields the read set in place are back too.
    const le::TechnologyData &after = *handle->root.get_technology(handle->root.get_technology_ids().front());
    EXPECT_EQ(after.database_units_microns, technology.database_units_microns);
    EXPECT_EQ(after.manufacturing_grid, technology.manufacturing_grid);
    EXPECT_EQ(after.capacitance_units_pf, technology.capacitance_units_pf);

    // And the next read works as if the failed one never happened.
    EXPECT_EQ(le_read_lef(handle, api_fixture("othercell.lef").c_str(), "cells"), 0);
}

TEST_F(ReadRollbackApi, AnOldLefVersionLeavesTheDatabaseAsItWas)
{
    read_testcell();
    const std::string before = database_bytes();
    EXPECT_NE(le_read_lef(handle, io_fixture("version_5_3.lef").c_str(), "cells"), 0);
    EXPECT_EQ(database_bytes(), before);
}

TEST_F(ReadRollbackApi, LefGeometryWithoutUnitsLeavesAFreshSessionEmpty)
{
    const std::string before = database_bytes();
    EXPECT_NE(le_read_lef(handle, io_fixture("no_units_with_geometry.lef").c_str(), "cells"), 0);
    EXPECT_EQ(database_bytes(), before);
    EXPECT_EQ(le_database_is_empty(handle), 1);
}

TEST_F(ReadRollbackApi, AMissingFileLeavesNoTechnologyBehind)
{
    EXPECT_NE(le_read_lef(handle, (dir / "missing.lef").string().c_str(), "cells"), 0);
    EXPECT_NE(le_read_def(handle, (dir / "missing.def").string().c_str(), "cells"), 0);
    EXPECT_TRUE(handle->root.get_technology_ids().empty());
    EXPECT_EQ(le_database_is_empty(handle), 1);

    // So read_db, which needs an empty session, still works.
    LeHandle *source = le_create();
    ASSERT_EQ(le_read_lef(source, api_fixture("testcell.lef").c_str(), "cells"), 0);
    const std::string saved = (dir / "saved.led").string();
    ASSERT_EQ(le_write_db(source, saved.c_str(), 0), 0);
    le_destroy(source);
    EXPECT_EQ(le_read_db(handle, saved.c_str(), 0), 0);
}

TEST_F(ReadRollbackApi, ADefParseErrorAfterComponentsLeavesTheDatabaseAsItWas)
{
    read_testcell();
    const std::string before = database_bytes();
    EXPECT_NE(le_read_def(handle, api_fixture("fails_after_rows.def").c_str(), "cells"), 0);
    EXPECT_EQ(database_bytes(), before);
}

TEST_F(ReadRollbackApi, ARefusedNetlistReadLeavesTheDatabaseAsItWas)
{
    read_testcell();
    const fs::path verilog = dir / "top.v";
    {
        std::ofstream out(verilog);
        out << "module TOP();\n  wire n1;\n  TESTCELL u1(.A(n1));\nendmodule\n";
    }
    const std::string path = verilog.string();
    const char *paths[] = {path.c_str()};
    ASSERT_EQ(le_read_verilog(handle, paths, 1, /*is_netlist=*/1, "top"), 0);
    const std::string before = database_bytes();

    // TOP already has a Schematic in library top.
    EXPECT_NE(le_read_verilog(handle, paths, 1, /*is_netlist=*/1, "top"), 0);
    EXPECT_EQ(database_bytes(), before);
}

// An exception mid-read unwinds through the guard, which rolls back.
TEST(ReadRollback, AnExceptionRollsBackEverythingCreatedAndTechnologyEdits)
{
    le::Root root;
    const le::TechnologyId technology = root.create_technology(le::TechnologyData{.database_units_microns = 1000});
    const le::LayerId kept = root.create_layer(le::LayerData{.technology = technology, .name = "M1"});
    EXPECT_THROW(
        {
            le::ReadRollback rollback(root);
            root.get_technology(technology)->database_units_microns = 2000;
            const le::LibraryId library = root.create_library(le::LibraryData{.name = "lib"});
            root.create_design(le::DesignData{.library = library, .name = "d"});
            root.create_layer(le::LayerData{.technology = technology, .name = "M2"});
            throw std::runtime_error("callback failed");
        },
        std::runtime_error);
    EXPECT_EQ(root.get_technology(technology)->database_units_microns, 1000);
    EXPECT_TRUE(root.get_library_ids().empty());
    EXPECT_TRUE(root.get_design_ids().empty());
    EXPECT_EQ(root.get_technology_layers(technology), std::vector<le::LayerId>{kept});
    EXPECT_FALSE(root.get_layer_by_name("M2").valid());
    EXPECT_EQ(root.creation_journal(), nullptr);
}

TEST(ReadRollback, ACommittedReadKeepsItsObjectsAndAnInnerGuardDefersToTheOuter)
{
    le::Root root;
    {
        le::ReadRollback outer(root);
        {
            le::ReadRollback inner(root);
            root.create_library(le::LibraryData{.name = "inner"});
        } // not committed, but the outer guard owns the rollback
        EXPECT_EQ(root.get_library_ids().size(), 1u);
        outer.commit();
    }
    EXPECT_EQ(root.get_library_ids().size(), 1u);
    EXPECT_EQ(root.creation_journal(), nullptr);
}
