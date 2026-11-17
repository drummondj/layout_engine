// A failed read changes nothing: the database writes the same bytes
// before and after it.

#include "session_test.hpp"
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

    std::string api_fixture(const std::string &name) { return le::test::fixture_path(name); }
    std::string io_fixture(const std::string &name) { return std::string(IO_TEST_FIXTURES_DIR) + "/" + name; }

    struct ReadRollbackApi : le::test::SessionTest
    {
        void SetUp() override
        {
            dir = le::test::scratch_path("le_read_rollback");
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

        void read_testcell()
        {
            ASSERT_TRUE(read_lef(api_fixture("testcell.lef"), "cells"));
            ASSERT_TRUE(read_def(api_fixture("testcell.def"), "cells"));
        }

        fs::path dir;
        int snapshots = 0;
    };
}

TEST_F(ReadRollbackApi, ALefParseErrorAfterAMacroLeavesTheDatabaseAsItWas)
{
    read_testcell();
    const std::string before = database_bytes();
    const le::TechnologyData technology = *handle->root.get_technology(handle->root.get_technology_ids().front());

    EXPECT_FALSE(read_lef(api_fixture("fails_after_macro.lef"), "cells"));
    EXPECT_EQ(database_bytes(), before);
    // The Technology fields the read set in place are back too.
    const le::TechnologyData &after = *handle->root.get_technology(handle->root.get_technology_ids().front());
    EXPECT_EQ(after.database_units_microns, technology.database_units_microns);
    EXPECT_EQ(after.manufacturing_grid, technology.manufacturing_grid);
    EXPECT_EQ(after.capacitance_units_pf, technology.capacitance_units_pf);

    // And the next read works as if the failed one never happened.
    EXPECT_TRUE(read_lef(api_fixture("othercell.lef"), "cells"));
}

TEST_F(ReadRollbackApi, AnOldLefVersionLeavesTheDatabaseAsItWas)
{
    read_testcell();
    const std::string before = database_bytes();
    EXPECT_FALSE(read_lef(io_fixture("version_5_3.lef"), "cells"));
    EXPECT_EQ(database_bytes(), before);
}

TEST_F(ReadRollbackApi, LefGeometryWithoutUnitsLeavesAFreshSessionEmpty)
{
    const std::string before = database_bytes();
    EXPECT_FALSE(read_lef(io_fixture("no_units_with_geometry.lef"), "cells"));
    EXPECT_EQ(database_bytes(), before);
    EXPECT_TRUE(read().database_is_empty());
}

TEST_F(ReadRollbackApi, AMissingFileLeavesNoTechnologyBehind)
{
    EXPECT_FALSE(read_lef((dir / "missing.lef").string(), "cells"));
    EXPECT_FALSE(read_def((dir / "missing.def").string(), "cells"));
    EXPECT_TRUE(handle->root.get_technology_ids().empty());
    EXPECT_TRUE(read().database_is_empty());

    // So read_db, which needs an empty session, still works.
    le::Session source;
    ASSERT_TRUE(source.write().read_lef(api_fixture("testcell.lef"), "cells"));
    const std::string saved = (dir / "saved.led").string();
    ASSERT_TRUE(source.write().write_db(saved));
    EXPECT_TRUE(write().read_db(saved));
}

TEST_F(ReadRollbackApi, ADefParseErrorAfterComponentsLeavesTheDatabaseAsItWas)
{
    read_testcell();
    const std::string before = database_bytes();
    EXPECT_FALSE(read_def(api_fixture("fails_after_rows.def"), "cells"));
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
    const std::string paths[] = {verilog.string()};
    ASSERT_TRUE(write().read_verilog(paths, true, "top"));
    const std::string before = database_bytes();

    // TOP already has a Schematic in library top.
    EXPECT_FALSE(write().read_verilog(paths, true, "top"));
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

// A Verilog read links older unresolved Instances: rolled back, they are
// unresolved again; committed for undo, the record names them.
TEST(ReadRollback, TrackedLinksOfOlderInstancesRollBackAndAreRecorded)
{
    le::Root root;
    const le::LibraryId library = root.create_library(le::LibraryData{.name = "lib"});
    const le::DesignId top = root.create_design(le::DesignData{.library = library, .name = "TOP"});
    const le::SchematicId schematic = root.create_schematic(le::SchematicData{.design = top});
    const le::InstanceId older = root.create_instance(le::InstanceData{.schematic = schematic, .name = "u1", .reference_name = "LEAF"});

    auto read_leaf = [&]
    {
        const le::DesignId leaf = root.create_design(le::DesignData{.library = library, .name = "LEAF"});
        root.get_instance(older)->reference_design = leaf;
        return leaf;
    };

    {
        le::ReadRollback rollback(root, /*tracks_links=*/true);
        read_leaf();
    }
    EXPECT_FALSE(root.get_instance(older)->reference_design.valid());

    le::ReadRollback rollback(root, /*tracks_links=*/true);
    const le::DesignId leaf = read_leaf();
    const le::ReadRecord record = rollback.commit_for_undo();
    ASSERT_EQ(record.linked.size(), 1u);
    EXPECT_EQ(record.linked.front().first, older);
    EXPECT_EQ(record.linked.front().second, leaf);
    EXPECT_EQ(record.created.size(), 1u);
    EXPECT_TRUE(root.get_instance(older)->reference_design.valid());
}
