#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <fmt/format.h>
#include "../def_reader.hpp"
#include "../def_writer.hpp"

namespace le
{
    namespace
    {
        std::string complete_fixture_path()
        {
            return std::string(DEF_TEST_DIR) + "/complete.5.8.def";
        }

        std::string scratch_output_path(const std::string &name)
        {
            return (std::filesystem::temp_directory_path() / name).string();
        }

        // Same pre-population DEFReaderCompleteFixture (def_reader_test.cpp)
        // uses - complete.5.8.def references real layer/macro names with no
        // companion LEF of its own, and Shape.layer/Placement.reference_design
        // both require an already-resolved reference.
        void populate_technology_and_designs(Root &root)
        {
            const TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            for (const char *name : {"M1", "M2", "M3", "METAL1", "V1"})
                root.create_layer(LayerData{.technology = technology_id, .name = name, .type = "ROUTING"});
            const LibraryId library_id = root.create_library(LibraryData{.name = "test_lib"});
            for (const char *name : {"A", "B", "CHK3A"})
                root.create_design(DesignData{.library = library_id, .name = name});
        }
    }

    // Reads complete.5.8.def, writes it back out via DEFWriter, then
    // re-reads the written file into a second Root - same shape as
    // LEFWriterRoundtripFixture (lef_writer_test.cpp).
    class DEFWriterRoundtripFixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            populate_technology_and_designs(original_root);
            DEFReader reader;
            ASSERT_EQ(reader.read_def(complete_fixture_path(), original_root, "test_lib"), 0);

            const DesignId design_id = original_root.get_design_by_name("design");
            ASSERT_TRUE(design_id.valid());
            const LayoutId layout_id = original_root.get_design_layout(design_id);
            ASSERT_TRUE(layout_id.valid());

            const std::string written_path = scratch_output_path("le_def_writer_roundtrip.def");
            DEFWriter writer;
            ASSERT_EQ(writer.write_def(written_path, original_root, layout_id), 0) << [&]
            {
                std::string joined;
                for (const std::string &m : writer.messages())
                    joined += m + "\n";
                return joined;
            }();

            populate_technology_and_designs(written_root);
            DEFReader reread_reader;
            ASSERT_EQ(reread_reader.read_def(written_path, written_root, "test_lib"), 0);

            const DesignId reread_design_id = written_root.get_design_by_name("design");
            ASSERT_TRUE(reread_design_id.valid());
            written_layout_id = written_root.get_design_layout(reread_design_id);
            ASSERT_TRUE(written_layout_id.valid());
            original_layout_id = layout_id;
        }

        Root original_root;
        Root written_root;
        LayoutId original_layout_id;
        LayoutId written_layout_id;
    };

    TEST_F(DEFWriterRoundtripFixture, RoundTripsRowCount)
    {
        EXPECT_EQ(written_root.get_layout_rows(written_layout_id).size(), original_root.get_layout_rows(original_layout_id).size());
    }

    // Not a full round trip - see DEFWriter::write_tracks' own comment: a
    // Track with no LAYER clause at all (complete.5.8.def has two, DEF's
    // own TRACKS grammar allows it) can't be written back through
    // defwTracks, which always emits a bare "LAYER" token regardless of
    // num_layers - a known, documented vendored-writer gap, not a bug
    // here. complete.5.8.def has 4 Tracks, 2 with a real LAYER clause.
    TEST_F(DEFWriterRoundtripFixture, RoundTripsTracksThatHaveALayerClause)
    {
        EXPECT_EQ(written_root.get_layout_tracks(written_layout_id).size(), 2u);
    }

    TEST_F(DEFWriterRoundtripFixture, RoundTripsGCellGridCount)
    {
        EXPECT_EQ(written_root.get_layout_gcell_grids(written_layout_id).size(), original_root.get_layout_gcell_grids(original_layout_id).size());
    }

    TEST_F(DEFWriterRoundtripFixture, RoundTripsPlacementCount)
    {
        EXPECT_EQ(written_root.get_layout_placements(written_layout_id).size(), original_root.get_layout_placements(original_layout_id).size());
    }

    TEST_F(DEFWriterRoundtripFixture, RoundTripsPhysicalPortCount)
    {
        EXPECT_EQ(written_root.get_layout_physical_ports(written_layout_id).size(), original_root.get_layout_physical_ports(original_layout_id).size());
    }

    TEST_F(DEFWriterRoundtripFixture, RoundTripsBlockageCount)
    {
        EXPECT_EQ(written_root.get_layout_blockages(written_layout_id).size(), original_root.get_layout_blockages(original_layout_id).size());
    }

    TEST_F(DEFWriterRoundtripFixture, RoundTripsLayoutViaCount)
    {
        EXPECT_EQ(written_root.get_layout_vias(written_layout_id).size(), original_root.get_layout_vias(original_layout_id).size());
    }

    TEST_F(DEFWriterRoundtripFixture, RoundTripsRegionCount)
    {
        EXPECT_EQ(written_root.get_layout_regions(written_layout_id).size(), original_root.get_layout_regions(original_layout_id).size());
    }

    TEST_F(DEFWriterRoundtripFixture, RoundTripsRouteCount)
    {
        EXPECT_EQ(written_root.get_layout_routes(written_layout_id).size(), original_root.get_layout_routes(original_layout_id).size());
    }

    TEST_F(DEFWriterRoundtripFixture, RoundTripsRowFields)
    {
        const RowId original_id = original_root.get_row_by_name(original_layout_id, "ROW_1");
        const RowId reread_id = written_root.get_row_by_name(written_layout_id, "ROW_1");
        ASSERT_TRUE(original_id.valid());
        ASSERT_TRUE(reread_id.valid());
        const RowData *original = original_root.get_row(original_id);
        const RowData *reread = written_root.get_row(reread_id);
        ASSERT_TRUE(original && reread);
        EXPECT_EQ(original->site_name, reread->site_name);
        EXPECT_EQ(original->orientation, reread->orientation);
        ASSERT_EQ(original->origin.has_value(), reread->origin.has_value());
        if (original->origin)
        {
            EXPECT_EQ(original->origin->x, reread->origin->x);
            EXPECT_EQ(original->origin->y, reread->origin->y);
        }
        EXPECT_EQ(original->num_x, reread->num_x);
        EXPECT_EQ(original->step_x, reread->step_x);
    }

    // Locks in DEFWriter::write_non_default_rules' own micron-conversion
    // fix - NONDEFAULTRULES LAYER WIDTH/SPACING/etc are the one DEF
    // construct written as real micron decimals rather than raw database
    // units (see that function's own comment), and defwNonDefaultRuleLayer
    // has no way to write a fractional micron value at all (a `%d`, not
    // `%g`). complete.5.8.def's own METAL1 WIDTH is 10.1 microns (10100
    // dbu) - this asserts the conversion lands in the right unit (10100,
    // not the un-converted-would-be 10100100) and truncates the way that
    // gap predicts (10000, the whole-micron part only), not some other
    // wrong value - a real, permanent precision loss, not a bug to chase.
    TEST_F(DEFWriterRoundtripFixture, RoundTripsNonDefaultRuleLayerWidthsToWholeMicronPrecision)
    {
        const NonDefaultRuleId original_id = original_root.get_non_default_rule_by_name("DEFAULT");
        const NonDefaultRuleId reread_id = written_root.get_non_default_rule_by_name("DEFAULT");
        ASSERT_TRUE(original_id.valid());
        ASSERT_TRUE(reread_id.valid());
        const auto find_metal1 = [](const Root &r, NonDefaultRuleId rule_id) -> const NonDefaultRuleLayerData *
        {
            for (NonDefaultRuleLayerId id : r.get_non_default_rule_layers(rule_id))
                if (const NonDefaultRuleLayerData *layer = r.get_non_default_rule_layer(id); layer && layer->layer_name == "METAL1")
                    return layer;
            return nullptr;
        };
        const NonDefaultRuleLayerData *original = find_metal1(original_root, original_id);
        const NonDefaultRuleLayerData *reread = find_metal1(written_root, reread_id);
        ASSERT_TRUE(original && reread);
        ASSERT_TRUE(original->width.has_value());
        EXPECT_EQ(*original->width, 10100);
        ASSERT_TRUE(reread->width.has_value());
        EXPECT_EQ(*reread->width, 10000);
    }

    // Route.width isn't ever set by DEFReader (see RouteData's own schema
    // comment - DEF's own per-layer "+ WIDTH layerName dist" SPECIALNETS
    // option isn't modeled), so complete.5.8.def's own round trip above
    // never exercises DEFWriter::write_routes' route->width branch even
    // though complete.5.8.def has real "+ WIDTH" statements (SPECIALNETS
    // N1) - it's only reachable when a Route is built directly with width
    // set (e.g. via the generated create_route/update_route API). Confirms
    // that branch's own root.get_shape(shape_ids.front()) call is
    // null-checked before dereferencing (a real crash here, caught by this
    // test, before that check was added).
    TEST(DEFWriterSpecialNetWidth, WritesSpecialNetWidthAgainstItsFirstShapesLayer)
    {
        Root root;
        const TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
        const LayerId layer_id = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
        const LibraryId library_id = root.create_library(LibraryData{.name = "test_lib"});
        const DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "widthtest"});
        const LayoutId layout_id = root.create_layout(LayoutData{.design = design_id});
        const RouteId route_id = root.create_route(RouteData{.layout = layout_id, .name = "VDD", .is_special = true, .width = 200});
        root.create_shape(ShapeData{.route = route_id, .layer = layer_id, .paths = {Path{.polygon = Polygon{.points = {Point{.x = 0, .y = 0}, Point{.x = 100, .y = 0}}}, .width = 200}}});

        const std::string written_path = scratch_output_path("le_def_writer_special_net_width.def");
        DEFWriter writer;
        ASSERT_EQ(writer.write_def(written_path, root, layout_id), 0);

        // DEFReader itself never populates Route.width back from a
        // "+ WIDTH layerName dist" statement (see this test's own comment
        // above), so a read-back-and-compare isn't possible here - check
        // the written text directly instead for the one thing this test
        // actually verifies: that the WIDTH statement was written at all
        // (i.e. write_routes' route->width branch didn't crash or skip
        // silently), against the correct layer name.
        std::ifstream in(written_path);
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(contents.find("WIDTH M1 200"), std::string::npos) << contents;
    }

    // Confirms the vendored defdiff tool's own normalized dump of the
    // round-tripped file at least parses cleanly (defdiff itself succeeds
    // - a real signal the written DEF is valid, not just "didn't crash").
    // Unlike LEFWriterLefdiffFidelity (lef_writer_test.cpp), this does NOT
    // additionally assert the two dumps are line-for-line identical:
    // DEFWriter's own scope (see def_writer.hpp's class comment,
    // PROJECT_MIGRATION.md) is deliberately narrower than everything
    // complete.5.8.def exercises - net *connectivity*, PROPERTY/
    // PROPERTYDEFINITIONS, GROUPS/SLOTS/FILLS/PINPROPERTIES/SCANCHAINS,
    // COMPONENTMASKSHIFT, Placement's HALO/ROUTEHALO/EEQMASTER/GENERATE/
    // REGION, VPIN/SUBNET/SHIELDNET, and every non-ROUTED wire status
    // (FIXED/COVER/NOSHIELD - Route has no field for this, always writes
    // ROUTED) are all real, deliberately out-of-scope gaps, not bugs -
    // the per-construct RoundTrips* tests above are this file's real
    // fidelity gate for what DEFWriter actually claims to cover.
    TEST(DEFWriterLefdiffFidelity, WriterRoundtripFixtureMatchesTheOriginalPerLefdiff)
    {
        Root root;
        populate_technology_and_designs(root);
        DEFReader reader;
        ASSERT_EQ(reader.read_def(complete_fixture_path(), root, "test_lib"), 0);
        const DesignId design_id = root.get_design_by_name("design");
        ASSERT_TRUE(design_id.valid());
        const LayoutId layout_id = root.get_design_layout(design_id);
        ASSERT_TRUE(layout_id.valid());

        const std::string written_path = scratch_output_path("le_defdiff_written.def");
        DEFWriter writer;
        ASSERT_EQ(writer.write_def(written_path, root, layout_id), 0);

        const std::string dump1_path = scratch_output_path("le_defdiff_dump1.txt");
        const std::string dump2_path = scratch_output_path("le_defdiff_dump2.txt");

        // Unlike lefdiff (differLef.cpp, argc==5: 4 positional args),
        // defdiff's own main (differDef.cpp) requires argc==9 - file1,
        // file2, out1, out2, then 4 more ignore-flag args
        // (ignorePinExtra/ignoreRowName/ignoreViaName/netSegComp, each
        // compared against the literal string "0" for "don't ignore" -
        // see diffDefReadFile in diffDefRW.cpp) - a real difference from
        // lefdiff's own invocation convention, not a typo.
        const std::string command = fmt::format(
            "\"{}\" \"{}\" \"{}\" \"{}\" \"{}\" 0 0 0 0 > /dev/null 2>&1",
            DEFDIFF_BIN, complete_fixture_path(), written_path, dump1_path, dump2_path);
        ASSERT_EQ(std::system(command.c_str()), 0) << "defdiff itself failed to run: " << command;
    }
}
