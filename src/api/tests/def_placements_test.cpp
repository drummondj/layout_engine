// read_def_placements: an existing Layout's Placements take their DEF
// components' status, location and orientation, as one undo step that
// the change log sees, leaving everything else alone.

#include "session_test.hpp"
#include "../le_handle.hpp"
#include "../../test_support/scratch_dir.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    using le::test::fixture_path;

    struct DefPlacements : le::test::SessionTest
    {
        void SetUp() override
        {
            dir = le::test::scratch_path("le_def_placements");
            fs::create_directories(dir);
            ASSERT_TRUE(read_lef(fixture_path("testcell.lef"), "cells"));
            ASSERT_TRUE(read_def(def("layout.def", "UNITS DISTANCE MICRONS 1000 ;\n"
                                                   "COMPONENTS 2 ;\n"
                                                   "- a TESTCELL + PLACED ( 0 0 ) N ;\n"
                                                   "- b TESTCELL + FIXED ( 100 200 ) S ;\n"
                                                   "END COMPONENTS\n"),
                                 "top"));
            const le::DesignId top = design_named("top");
            layout = read().root().get_design_layout(top);
            ASSERT_TRUE(layout.valid());
        }
        void TearDown() override { fs::remove_all(dir); }

        // A DEF file of design `top` with `body` after its header.
        std::string def(const std::string &name, const std::string &body)
        {
            const fs::path path = dir / name;
            std::ofstream(path) << "VERSION 5.8 ;\nDESIGN top ;\n" << body << "END DESIGN\n";
            return path.string();
        }

        le::PlacementData placement(const std::string &name)
        {
            const le::ReadView view = read();
            const le::PlacementData *data = view.root().get_placement(view.root().get_placement_by_name(layout, name));
            return data ? *data : le::PlacementData{};
        }

        // read_def_placements as a typed command: one undo step.
        std::expected<le::DefPlacementsReport, std::string> read_placements(const std::string &path, le::LayoutId target)
        {
            write().begin_command("read_def");
            auto report = write().read_def_placements(path, target);
            write().end_command(report.has_value());
            return report;
        }

        fs::path dir;
        le::LayoutId layout;
    };
}

TEST_F(DefPlacements, MatchingPlacementsMoveOthersStayAndUnknownsAreReported)
{
    const le::PlacementData b_before = placement("b");
    const auto report = read_placements(def("placements.def", "UNITS DISTANCE MICRONS 1000 ;\n"
                                                              "COMPONENTS 2 ;\n"
                                                              "- a TESTCELL + PLACED ( 5000 6000 ) FS ;\n"
                                                              "- c TESTCELL + PLACED ( 1 1 ) N ;\n"
                                                              "END COMPONENTS\n"),
                                        layout);
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->updated, 1u);
    EXPECT_EQ(report->unmatched, std::vector<std::string>{"c"});
    EXPECT_TRUE(report->cell_mismatches.empty());

    const le::PlacementData a = placement("a");
    ASSERT_TRUE(a.location);
    EXPECT_EQ(a.location->x, 5000);
    EXPECT_EQ(a.location->y, 6000);
    EXPECT_EQ(a.orientation, le::Orientation::FS);
    EXPECT_EQ(a.placement_status, le::PlacementStatus::PLACED);

    const le::PlacementData b = placement("b");
    EXPECT_EQ(b.location, b_before.location);
    EXPECT_EQ(b.orientation, b_before.orientation);
    EXPECT_EQ(b.placement_status, b_before.placement_status);
    EXPECT_FALSE(read().root().get_placement_by_name(layout, "c").valid());
}

TEST_F(DefPlacements, DifferentUnitsAreRescaled)
{
    const auto report = read_placements(def("placements.def", "UNITS DISTANCE MICRONS 2000 ;\n"
                                                              "COMPONENTS 1 ;\n"
                                                              "- a TESTCELL + PLACED ( 10000 4000 ) N ;\n"
                                                              "END COMPONENTS\n"),
                                        layout);
    ASSERT_TRUE(report) << report.error();
    const le::PlacementData a = placement("a");
    ASSERT_TRUE(a.location);
    EXPECT_EQ(a.location->x, 5000);
    EXPECT_EQ(a.location->y, 2000);
}

TEST_F(DefPlacements, UndoRestoresEveryPlacementInOneStepIncludingALockedOne)
{
    const le::PlacementData a_before = placement("a");
    const le::PlacementData b_before = placement("b");
    const auto report = read_placements(def("placements.def", "UNITS DISTANCE MICRONS 1000 ;\n"
                                                              "COMPONENTS 2 ;\n"
                                                              "- a TESTCELL + FIXED ( 300 400 ) E ;\n"
                                                              "- b TESTCELL + FIXED ( 700 800 ) W ;\n"
                                                              "END COMPONENTS\n"),
                                        layout);
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->updated, 2u);
    EXPECT_EQ(placement("b").location->x, 700) << "a FIXED placement takes the DEF's location";
    EXPECT_EQ(placement("b").placement_status, le::PlacementStatus::FIXED);
    EXPECT_EQ(placement("a").placement_status, le::PlacementStatus::FIXED);

    ASSERT_EQ(write().undo(), true);
    for (const auto &[name, before] : {std::pair{"a", a_before}, std::pair{"b", b_before}})
    {
        const le::PlacementData after = placement(name);
        EXPECT_EQ(after.location, before.location) << name;
        EXPECT_EQ(after.orientation, before.orientation) << name;
        EXPECT_EQ(after.placement_status, before.placement_status) << name;
    }
    EXPECT_TRUE(read().root().get_layout(layout)) << "the layout read before stays";

    ASSERT_EQ(write().redo(), true);
    EXPECT_EQ(placement("a").location->x, 300);
}

TEST_F(DefPlacements, ACellMismatchIsAnErrorAndLeavesThatPlacementUnchanged)
{
    const auto report = read_placements(def("placements.def", "UNITS DISTANCE MICRONS 1000 ;\n"
                                                              "COMPONENTS 2 ;\n"
                                                              "- a OTHERCELL + PLACED ( 10 20 ) N ;\n"
                                                              "- b TESTCELL + FIXED ( 30 40 ) S ;\n"
                                                              "END COMPONENTS\n"),
                                        layout);
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->cell_mismatches, std::vector<std::string>{"a"});
    EXPECT_EQ(report->updated, 1u);
    EXPECT_EQ(placement("a").location->x, 0);
    EXPECT_EQ(placement("a").reference_design, design_named("TESTCELL"));
    EXPECT_EQ(placement("b").location->x, 30) << "the other components still apply";
}

TEST_F(DefPlacements, TheChangeLogSeesEachUpdateWithoutSaturating)
{
    const uint64_t since = read().root().change_log().end_sequence();
    const auto report = read_placements(def("placements.def", "UNITS DISTANCE MICRONS 1000 ;\n"
                                                              "COMPONENTS 1 ;\n"
                                                              "- a TESTCELL + PLACED ( 10 20 ) N ;\n"
                                                              "END COMPONENTS\n"),
                                        le::LayoutId{});
    ASSERT_FALSE(report) << "no layout given and none current";

    const le::DesignId top = read().root().get_layout(layout)->design;
    ASSERT_TRUE(write().open_layout(top));
    const auto current = read_placements(def("placements.def", "UNITS DISTANCE MICRONS 1000 ;\n"
                                                               "COMPONENTS 1 ;\n"
                                                               "- a TESTCELL + PLACED ( 10 20 ) N ;\n"
                                                               "END COMPONENTS\n"),
                                         le::LayoutId{});
    ASSERT_TRUE(current) << current.error();
    const le::ReadView view = read();
    const le::ChangeLog &log = view.root().change_log();
    ASSERT_TRUE(log.covers(since));
    size_t placement_updates = 0;
    log.for_each_since(since, [&](const le::ChangeLogEntry &entry)
                       { placement_updates += entry.klass == le::ChangeKlass::Placement && entry.op == le::ChangeOp::UPDATE; });
    EXPECT_GE(placement_updates, 1u);
}

TEST_F(DefPlacements, AnUnchangedPlacementIsNotUpdated)
{
    const auto report = read_placements(def("placements.def", "UNITS DISTANCE MICRONS 1000 ;\n"
                                                              "COMPONENTS 1 ;\n"
                                                              "- a TESTCELL + PLACED ( 0 0 ) N ;\n"
                                                              "END COMPONENTS\n"),
                                        layout);
    ASSERT_TRUE(report) << report.error();
    EXPECT_EQ(report->updated, 0u);
}
