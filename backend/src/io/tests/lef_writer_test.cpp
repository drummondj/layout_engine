#include "../lef_reader.hpp"
#include "../lef_writer.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <gtest/gtest.h>

using namespace le;

// Deliberately does NOT use the generated to_string()/to_properties() free
// functions for comparison here - they call std::optional::value()
// unconditionally on every is_optional field (confirmed by reading
// generated/layer.hpp), so they'd throw on any layer that leaves one of
// Phase 1's new optional LAYER properties unset (e.g. V1 below, which has
// no PITCH/OFFSET/AREA/RESISTANCE/...). Direct field comparisons avoid
// that and give a clearer failure message anyway.
namespace
{
    std::string fixture_path(const std::string &name)
    {
        return std::string(IO_TEST_FIXTURES_DIR) + "/" + name;
    }

    std::string scratch_output_path(const std::string &name = "le_lef_writer_roundtrip.lef")
    {
        return (std::filesystem::temp_directory_path() / name).string();
    }

    void expect_shapes_equal(const Shape &original, const Shape &written)
    {
        EXPECT_EQ(original.layer_name, written.layer_name);

        ASSERT_EQ(original.rects.size(), written.rects.size());
        for (size_t i = 0; i < original.rects.size(); i++)
        {
            EXPECT_EQ(original.rects[i].ll.x, written.rects[i].ll.x);
            EXPECT_EQ(original.rects[i].ll.y, written.rects[i].ll.y);
            EXPECT_EQ(original.rects[i].ur.x, written.rects[i].ur.x);
            EXPECT_EQ(original.rects[i].ur.y, written.rects[i].ur.y);
        }
        EXPECT_EQ(original.rect_masks, written.rect_masks);

        ASSERT_EQ(original.polygons.size(), written.polygons.size());
        for (size_t i = 0; i < original.polygons.size(); i++)
        {
            ASSERT_EQ(original.polygons[i].points.size(), written.polygons[i].points.size());
            for (size_t j = 0; j < original.polygons[i].points.size(); j++)
            {
                EXPECT_EQ(original.polygons[i].points[j].x, written.polygons[i].points[j].x);
                EXPECT_EQ(original.polygons[i].points[j].y, written.polygons[i].points[j].y);
            }
        }
        EXPECT_EQ(original.polygon_masks, written.polygon_masks);

        ASSERT_EQ(original.paths.size(), written.paths.size());
        for (size_t i = 0; i < original.paths.size(); i++)
        {
            EXPECT_EQ(original.paths[i].width, written.paths[i].width);
            ASSERT_EQ(original.paths[i].polygon.points.size(), written.paths[i].polygon.points.size());
            for (size_t j = 0; j < original.paths[i].polygon.points.size(); j++)
            {
                EXPECT_EQ(original.paths[i].polygon.points[j].x, written.paths[i].polygon.points[j].x);
                EXPECT_EQ(original.paths[i].polygon.points[j].y, written.paths[i].polygon.points[j].y);
            }
        }
        EXPECT_EQ(original.path_masks, written.path_masks);

        ASSERT_EQ(original.rect_iterates.size(), written.rect_iterates.size());
        for (size_t i = 0; i < original.rect_iterates.size(); i++)
        {
            const RectIterate &o = original.rect_iterates[i];
            const RectIterate &w = written.rect_iterates[i];
            EXPECT_EQ(o.rect.ll.x, w.rect.ll.x);
            EXPECT_EQ(o.rect.ll.y, w.rect.ll.y);
            EXPECT_EQ(o.rect.ur.x, w.rect.ur.x);
            EXPECT_EQ(o.rect.ur.y, w.rect.ur.y);
            EXPECT_EQ(o.num_x, w.num_x);
            EXPECT_EQ(o.num_y, w.num_y);
            EXPECT_EQ(o.space_x, w.space_x);
            EXPECT_EQ(o.space_y, w.space_y);
        }

        ASSERT_EQ(original.path_iterates.size(), written.path_iterates.size());
        for (size_t i = 0; i < original.path_iterates.size(); i++)
        {
            const PathIterate &o = original.path_iterates[i];
            const PathIterate &w = written.path_iterates[i];
            EXPECT_EQ(o.path.width, w.path.width);
            ASSERT_EQ(o.path.polygon.points.size(), w.path.polygon.points.size());
            for (size_t j = 0; j < o.path.polygon.points.size(); j++)
            {
                EXPECT_EQ(o.path.polygon.points[j].x, w.path.polygon.points[j].x);
                EXPECT_EQ(o.path.polygon.points[j].y, w.path.polygon.points[j].y);
            }
            EXPECT_EQ(o.num_x, w.num_x);
            EXPECT_EQ(o.num_y, w.num_y);
            EXPECT_EQ(o.space_x, w.space_x);
            EXPECT_EQ(o.space_y, w.space_y);
        }
    }
}

class LEFWriterRoundtripFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_EQ(reader.read_lef(fixture_path("writer_roundtrip.lef"), original_root, "test_lib"), 0);

        const std::string out_path = scratch_output_path();
        ASSERT_EQ(writer.write_lef(out_path, original_root, original_root.get_design_abstract(original_root.get_design_by_name("WRITERTEST")), LEFWriter::LayerWriteMode::IncludeWithAbstract), 0)
            << (writer.messages().empty() ? "" : writer.messages().front());

        LEFReader reread;
        ASSERT_EQ(reread.read_lef(out_path, written_root, "test_lib"), 0)
            << (reread.messages().empty() ? "" : reread.messages().front());
    }

    Root original_root;
    Root written_root;
    LEFReader reader;
    LEFWriter writer;
};

TEST_F(LEFWriterRoundtripFixture, RoundTripsUnits)
{
    const TechnologyId original_id = original_root.get_technology_ids().front();
    const TechnologyId written_id = written_root.get_technology_ids().front();
    EXPECT_DOUBLE_EQ(original_root.get_technology(original_id)->database_units_microns, written_root.get_technology(written_id)->database_units_microns);
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsRoutingLayerProperties)
{
    const LayerId original_id = original_root.get_layer_by_name("M1");
    const LayerId written_id = written_root.get_layer_by_name("M1");
    const LayerData *original = original_root.get_layer(original_id);
    const LayerData *written = written_root.get_layer(written_id);
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_EQ(original->type, written->type);
    EXPECT_EQ(original->direction, written->direction);

    ASSERT_TRUE(original->width.has_value());
    ASSERT_TRUE(written->width.has_value());
    EXPECT_EQ(*original->width, *written->width);

    ASSERT_TRUE(original->pitch.has_value());
    ASSERT_TRUE(written->pitch.has_value());
    EXPECT_EQ(*original->pitch, *written->pitch);

    ASSERT_TRUE(original->offset.has_value());
    ASSERT_TRUE(written->offset.has_value());
    EXPECT_EQ(*original->offset, *written->offset);

    const std::vector<SpacingRuleId> &original_spacing_rule_ids = original_root.get_layer_spacing_rules(original_id);
    const std::vector<SpacingRuleId> &written_spacing_rule_ids = written_root.get_layer_spacing_rules(written_id);
    ASSERT_EQ(original_spacing_rule_ids.size(), 5u);
    ASSERT_EQ(written_spacing_rule_ids.size(), 5u);
    for (size_t i = 0; i < original_spacing_rule_ids.size(); i++)
        EXPECT_EQ(original_root.get_spacing_rule(original_spacing_rule_ids[i])->distance, written_root.get_spacing_rule(written_spacing_rule_ids[i])->distance) << "rule " << i;

    // SPACING 0.16 SAMENET PGONLY
    const SpacingRuleData &original_rule1 = *original_root.get_spacing_rule(original_spacing_rule_ids[1]);
    const SpacingRuleData &written_rule1 = *written_root.get_spacing_rule(written_spacing_rule_ids[1]);
    EXPECT_TRUE(original_rule1.same_net);
    EXPECT_TRUE(written_rule1.same_net);
    EXPECT_TRUE(original_rule1.same_net_pg_only);
    EXPECT_TRUE(written_rule1.same_net_pg_only);

    // SPACING 0.17 LENGTHTHRESHOLD 0.4
    const SpacingRuleData &original_rule2 = *original_root.get_spacing_rule(original_spacing_rule_ids[2]);
    const SpacingRuleData &written_rule2 = *written_root.get_spacing_rule(written_spacing_rule_ids[2]);
    ASSERT_TRUE(original_rule2.length_threshold.has_value());
    ASSERT_TRUE(written_rule2.length_threshold.has_value());
    EXPECT_EQ(*original_rule2.length_threshold, *written_rule2.length_threshold);

    // SPACING 0.18 RANGE 0.1 0.3
    const SpacingRuleData &original_rule3 = *original_root.get_spacing_rule(original_spacing_rule_ids[3]);
    const SpacingRuleData &written_rule3 = *written_root.get_spacing_rule(written_spacing_rule_ids[3]);
    ASSERT_TRUE(original_rule3.range_min.has_value());
    ASSERT_TRUE(written_rule3.range_min.has_value());
    EXPECT_EQ(*original_rule3.range_min, *written_rule3.range_min);
    ASSERT_TRUE(original_rule3.range_max.has_value());
    ASSERT_TRUE(written_rule3.range_max.has_value());
    EXPECT_EQ(*original_rule3.range_max, *written_rule3.range_max);

    // SPACING 0.19 RANGE 0.1 0.3 INFLUENCE 0.05 RANGE 0.2 0.4
    const SpacingRuleData &original_rule4 = *original_root.get_spacing_rule(original_spacing_rule_ids[4]);
    const SpacingRuleData &written_rule4 = *written_root.get_spacing_rule(written_spacing_rule_ids[4]);
    ASSERT_TRUE(original_rule4.range_influence.has_value());
    ASSERT_TRUE(written_rule4.range_influence.has_value());
    EXPECT_EQ(*original_rule4.range_influence, *written_rule4.range_influence);
    ASSERT_TRUE(original_rule4.range_influence_range_min.has_value());
    ASSERT_TRUE(written_rule4.range_influence_range_min.has_value());
    EXPECT_EQ(*original_rule4.range_influence_range_min, *written_rule4.range_influence_range_min);

    const std::vector<MinimumCutId> &original_min_cut_ids = original_root.get_layer_minimum_cuts(original_id);
    const std::vector<MinimumCutId> &written_min_cut_ids = written_root.get_layer_minimum_cuts(written_id);
    ASSERT_EQ(original_min_cut_ids.size(), 2u);
    ASSERT_EQ(written_min_cut_ids.size(), 2u);
    const MinimumCutData &original_min_cut0 = *original_root.get_minimum_cut(original_min_cut_ids[0]);
    const MinimumCutData &written_min_cut0 = *written_root.get_minimum_cut(written_min_cut_ids[0]);
    EXPECT_EQ(original_min_cut0.cuts, written_min_cut0.cuts);
    EXPECT_EQ(original_min_cut0.width, written_min_cut0.width);
    const MinimumCutData &original_min_cut1 = *original_root.get_minimum_cut(original_min_cut_ids[1]);
    const MinimumCutData &written_min_cut1 = *written_root.get_minimum_cut(written_min_cut_ids[1]);
    ASSERT_TRUE(original_min_cut1.within.has_value());
    ASSERT_TRUE(written_min_cut1.within.has_value());
    EXPECT_EQ(*original_min_cut1.within, *written_min_cut1.within);
    EXPECT_EQ(original_min_cut1.connection, written_min_cut1.connection);
    ASSERT_TRUE(original_min_cut1.length.has_value());
    ASSERT_TRUE(written_min_cut1.length.has_value());
    EXPECT_EQ(*original_min_cut1.length, *written_min_cut1.length);

    const std::vector<MinStepId> &original_min_step_ids = original_root.get_layer_min_steps(original_id);
    const std::vector<MinStepId> &written_min_step_ids = written_root.get_layer_min_steps(written_id);
    ASSERT_EQ(original_min_step_ids.size(), 3u);
    ASSERT_EQ(written_min_step_ids.size(), 3u);
    EXPECT_FALSE(original_root.get_min_step(original_min_step_ids[0])->min_step_type.has_value());
    EXPECT_FALSE(written_root.get_min_step(written_min_step_ids[0])->min_step_type.has_value());
    const MinStepData &original_min_step1 = *original_root.get_min_step(original_min_step_ids[1]);
    const MinStepData &written_min_step1 = *written_root.get_min_step(written_min_step_ids[1]);
    EXPECT_EQ(original_min_step1.min_step_type, "INSIDECORNER");
    EXPECT_EQ(written_min_step1.min_step_type, "INSIDECORNER");
    ASSERT_TRUE(original_min_step1.lengthsum.has_value());
    ASSERT_TRUE(written_min_step1.lengthsum.has_value());
    EXPECT_EQ(*original_min_step1.lengthsum, *written_min_step1.lengthsum);
    const MinStepData &original_min_step2 = *original_root.get_min_step(original_min_step_ids[2]);
    const MinStepData &written_min_step2 = *written_root.get_min_step(written_min_step_ids[2]);
    ASSERT_TRUE(original_min_step2.max_edges.has_value());
    ASSERT_TRUE(written_min_step2.max_edges.has_value());
    EXPECT_EQ(*original_min_step2.max_edges, *written_min_step2.max_edges);

    ASSERT_TRUE(original->spacing_table_parallel_run_length.has_value());
    ASSERT_TRUE(written->spacing_table_parallel_run_length.has_value());
    EXPECT_EQ(original->spacing_table_parallel_run_length->lengths, written->spacing_table_parallel_run_length->lengths);
    EXPECT_EQ(original->spacing_table_parallel_run_length->widths, written->spacing_table_parallel_run_length->widths);
    EXPECT_EQ(original->spacing_table_parallel_run_length->spacings, written->spacing_table_parallel_run_length->spacings);

    const std::vector<InfluenceSpacingEntryId> &original_influence_ids = original_root.get_layer_spacing_table_influence(original_id);
    const std::vector<InfluenceSpacingEntryId> &written_influence_ids = written_root.get_layer_spacing_table_influence(written_id);
    ASSERT_EQ(original_influence_ids.size(), 2u);
    ASSERT_EQ(written_influence_ids.size(), 2u);
    for (size_t i = 0; i < original_influence_ids.size(); i++)
    {
        const InfluenceSpacingEntryData &o = *original_root.get_influence_spacing_entry(original_influence_ids[i]);
        const InfluenceSpacingEntryData &w = *written_root.get_influence_spacing_entry(written_influence_ids[i]);
        EXPECT_EQ(o.width, w.width);
        EXPECT_EQ(o.distance, w.distance);
        EXPECT_EQ(o.spacing, w.spacing);
    }

    ASSERT_TRUE(original->resistance.has_value());
    ASSERT_TRUE(written->resistance.has_value());
    EXPECT_DOUBLE_EQ(*original->resistance, *written->resistance);

    ASSERT_TRUE(original->capacitance.has_value());
    ASSERT_TRUE(written->capacitance.has_value());
    EXPECT_DOUBLE_EQ(*original->capacitance, *written->capacitance);

    ASSERT_TRUE(original->height.has_value());
    ASSERT_TRUE(written->height.has_value());
    EXPECT_EQ(*original->height, *written->height);

    ASSERT_TRUE(original->thickness.has_value());
    ASSERT_TRUE(written->thickness.has_value());
    EXPECT_EQ(*original->thickness, *written->thickness);

    ASSERT_TRUE(original->wire_extension.has_value());
    ASSERT_TRUE(written->wire_extension.has_value());
    EXPECT_EQ(*original->wire_extension, *written->wire_extension);

    // area/antenna_length are stored in dbu/dbu^2, not the LEF file's own
    // raw microns value - checked against the actual expected number
    // (not just original == written), since a reader/writer pair that
    // both forgot the unit conversion would still "round-trip" to the
    // same wrong value. writer_roundtrip.lef: DATABASE MICRONS 1000,
    // AREA 0.5 (microns^2) -> 0.5 * 1000^2 = 500000 dbu^2;
    // ANTENNALENGTHFACTOR 2.0 (microns) -> 2.0 * 1000 = 2000 dbu.
    ASSERT_TRUE(original->area.has_value());
    ASSERT_TRUE(written->area.has_value());
    EXPECT_EQ(*original->area, 500000);
    EXPECT_EQ(*written->area, 500000);

    ASSERT_TRUE(original->antenna_length.has_value());
    ASSERT_TRUE(written->antenna_length.has_value());
    EXPECT_EQ(*original->antenna_length, 2000);
    EXPECT_EQ(*written->antenna_length, 2000);
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsCutLayerWidthAndSpacing)
{
    const LayerId original_id = original_root.get_layer_by_name("V1");
    const LayerId written_id = written_root.get_layer_by_name("V1");
    const LayerData *original = original_root.get_layer(original_id);
    const LayerData *written = written_root.get_layer(written_id);
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_EQ(original->type, written->type);

    ASSERT_TRUE(original->width.has_value());
    ASSERT_TRUE(written->width.has_value());
    EXPECT_EQ(*original->width, *written->width);

    const std::vector<SpacingRuleId> &original_spacing_rule_ids = original_root.get_layer_spacing_rules(original_id);
    const std::vector<SpacingRuleId> &written_spacing_rule_ids = written_root.get_layer_spacing_rules(written_id);
    ASSERT_EQ(original_spacing_rule_ids.size(), 7u);
    ASSERT_EQ(written_spacing_rule_ids.size(), 7u);
    for (size_t i = 0; i < original_spacing_rule_ids.size(); i++)
        EXPECT_EQ(original_root.get_spacing_rule(original_spacing_rule_ids[i])->distance, written_root.get_spacing_rule(written_spacing_rule_ids[i])->distance) << "rule " << i;

    // SPACING 0.22 ADJACENTCUTS 3 WITHIN 0.25
    const SpacingRuleData &original_rule1 = *original_root.get_spacing_rule(original_spacing_rule_ids[1]);
    const SpacingRuleData &written_rule1 = *written_root.get_spacing_rule(written_spacing_rule_ids[1]);
    ASSERT_TRUE(original_rule1.adjacent_cuts.has_value());
    ASSERT_TRUE(written_rule1.adjacent_cuts.has_value());
    EXPECT_EQ(*original_rule1.adjacent_cuts, *written_rule1.adjacent_cuts);
    ASSERT_TRUE(original_rule1.adjacent_within.has_value());
    ASSERT_TRUE(written_rule1.adjacent_within.has_value());
    EXPECT_EQ(*original_rule1.adjacent_within, *written_rule1.adjacent_within);
    EXPECT_FALSE(original_rule1.adjacent_except_same_pg_net);
    EXPECT_FALSE(written_rule1.adjacent_except_same_pg_net);

    // SPACING 0.23 ADJACENTCUTS 2 WITHIN 0.3 EXCEPTSAMEPGNET
    EXPECT_TRUE(original_root.get_spacing_rule(original_spacing_rule_ids[2])->adjacent_except_same_pg_net);
    EXPECT_TRUE(written_root.get_spacing_rule(written_spacing_rule_ids[2])->adjacent_except_same_pg_net);

    // SPACING 0.6 LAYER M1
    EXPECT_EQ(original_root.get_spacing_rule(original_spacing_rule_ids[3])->second_layer_name, "M1");
    EXPECT_EQ(written_root.get_spacing_rule(written_spacing_rule_ids[3])->second_layer_name, "M1");

    // SPACING 1.5 PARALLELOVERLAP
    EXPECT_TRUE(original_root.get_spacing_rule(original_spacing_rule_ids[4])->parallel_overlap);
    EXPECT_TRUE(written_root.get_spacing_rule(written_spacing_rule_ids[4])->parallel_overlap);

    // SPACING 0.24 CENTERTOCENTER
    EXPECT_TRUE(original_root.get_spacing_rule(original_spacing_rule_ids[5])->center_to_center);
    EXPECT_TRUE(written_root.get_spacing_rule(written_spacing_rule_ids[5])->center_to_center);

    // SPACING 0.25 SAMENET
    const SpacingRuleData &original_rule6 = *original_root.get_spacing_rule(original_spacing_rule_ids[6]);
    const SpacingRuleData &written_rule6 = *written_root.get_spacing_rule(written_spacing_rule_ids[6]);
    EXPECT_TRUE(original_rule6.same_net);
    EXPECT_TRUE(written_rule6.same_net);
    EXPECT_FALSE(original_rule6.same_net_pg_only);
    EXPECT_FALSE(written_rule6.same_net_pg_only);

    ASSERT_EQ(original->spacing_table_orthogonal.size(), 2u);
    ASSERT_EQ(written->spacing_table_orthogonal.size(), 2u);
    for (size_t i = 0; i < original->spacing_table_orthogonal.size(); i++)
    {
        EXPECT_EQ(original->spacing_table_orthogonal[i].cut_within, written->spacing_table_orthogonal[i].cut_within);
        EXPECT_EQ(original->spacing_table_orthogonal[i].ortho_spacing, written->spacing_table_orthogonal[i].ortho_spacing);
    }

    // V1 has no PITCH/OFFSET/AREA/RESISTANCE/... - proving they stay unset
    // through a round trip too, not accidentally defaulted to 0/written as
    // a spurious statement.
    EXPECT_FALSE(original->pitch.has_value());
    EXPECT_FALSE(written->pitch.has_value());
    EXPECT_FALSE(original->resistance.has_value());
    EXPECT_FALSE(written->resistance.has_value());
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsAPlainViaWithLayerGeometryAndResistance)
{
    const ViaId original_id = original_root.get_via_by_name("VIA1");
    const ViaId written_id = written_root.get_via_by_name("VIA1");
    const ViaData *original = original_root.get_via(original_id);
    const ViaData *written = written_root.get_via(written_id);
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_EQ(original->is_default, written->is_default);
    ASSERT_TRUE(original->resistance.has_value());
    ASSERT_TRUE(written->resistance.has_value());
    EXPECT_DOUBLE_EQ(*original->resistance, *written->resistance);
    EXPECT_EQ(original_root.get_via_rule_reference(original_root.get_via_via_rule(original_id)), nullptr);
    EXPECT_EQ(written_root.get_via_rule_reference(written_root.get_via_via_rule(written_id)), nullptr);

    const std::vector<ViaLayerId> &original_layer_ids = original_root.get_via_layers(original_id);
    const std::vector<ViaLayerId> &written_layer_ids = written_root.get_via_layers(written_id);
    ASSERT_EQ(original_layer_ids.size(), written_layer_ids.size());
    for (size_t i = 0; i < original_layer_ids.size(); i++)
    {
        const ViaLayerData &o = *original_root.get_via_layer(original_layer_ids[i]);
        const ViaLayerData &w = *written_root.get_via_layer(written_layer_ids[i]);
        EXPECT_EQ(o.layer_name, w.layer_name);
        ASSERT_EQ(o.rects.size(), w.rects.size());
        for (size_t j = 0; j < o.rects.size(); j++)
        {
            EXPECT_EQ(o.rects[j].ll.x, w.rects[j].ll.x);
            EXPECT_EQ(o.rects[j].ll.y, w.rects[j].ll.y);
            EXPECT_EQ(o.rects[j].ur.x, w.rects[j].ur.x);
            EXPECT_EQ(o.rects[j].ur.y, w.rects[j].ur.y);
        }
    }
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsAGenerateViaRuleWithEnclosureAndACutLayer)
{
    const ViaRuleId original_id = original_root.get_via_rule_by_name("VIARULE1");
    const ViaRuleId written_id = written_root.get_via_rule_by_name("VIARULE1");
    const ViaRuleData *original = original_root.get_via_rule(original_id);
    const ViaRuleData *written = written_root.get_via_rule(written_id);
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_TRUE(original->is_generate);
    EXPECT_TRUE(written->is_generate);
    const std::vector<ViaRuleLayerId> &original_layer_ids = original_root.get_via_rule_layers(original_id);
    const std::vector<ViaRuleLayerId> &written_layer_ids = written_root.get_via_rule_layers(written_id);
    ASSERT_EQ(original_layer_ids.size(), written_layer_ids.size());
    ASSERT_EQ(original_layer_ids.size(), 3u);

    for (size_t i = 0; i < 2; i++)
    {
        const ViaRuleLayerData &o = *original_root.get_via_rule_layer(original_layer_ids[i]);
        const ViaRuleLayerData &w = *written_root.get_via_rule_layer(written_layer_ids[i]);
        EXPECT_EQ(o.layer_name, w.layer_name);
        ASSERT_TRUE(o.enclosure_overhang1.has_value());
        ASSERT_TRUE(w.enclosure_overhang1.has_value());
        EXPECT_EQ(*o.enclosure_overhang1, *w.enclosure_overhang1);
        ASSERT_TRUE(o.enclosure_overhang2.has_value());
        ASSERT_TRUE(w.enclosure_overhang2.has_value());
        EXPECT_EQ(*o.enclosure_overhang2, *w.enclosure_overhang2);
        ASSERT_TRUE(o.width_min.has_value());
        ASSERT_TRUE(w.width_min.has_value());
        EXPECT_EQ(*o.width_min, *w.width_min);
        ASSERT_TRUE(o.width_max.has_value());
        ASSERT_TRUE(w.width_max.has_value());
        EXPECT_EQ(*o.width_max, *w.width_max);
    }

    const ViaRuleLayerData &original_cut = *original_root.get_via_rule_layer(original_layer_ids[2]);
    const ViaRuleLayerData &written_cut = *written_root.get_via_rule_layer(written_layer_ids[2]);
    EXPECT_EQ(original_cut.layer_name, written_cut.layer_name);
    ASSERT_TRUE(original_cut.rect.has_value());
    ASSERT_TRUE(written_cut.rect.has_value());
    EXPECT_EQ(original_cut.rect->ll.x, written_cut.rect->ll.x);
    EXPECT_EQ(original_cut.rect->ur.x, written_cut.rect->ur.x);
    ASSERT_TRUE(original_cut.resistance.has_value());
    ASSERT_TRUE(written_cut.resistance.has_value());
    EXPECT_DOUBLE_EQ(*original_cut.resistance, *written_cut.resistance);
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsSiteDefinitions)
{
    const SiteData *original_core = original_root.get_site(original_root.get_site_by_name("CORE"));
    const SiteData *written_core = written_root.get_site(written_root.get_site_by_name("CORE"));
    ASSERT_TRUE(original_core != nullptr);
    ASSERT_TRUE(written_core != nullptr);

    EXPECT_EQ(original_core->site_class, written_core->site_class);
    ASSERT_TRUE(original_core->size.has_value());
    ASSERT_TRUE(written_core->size.has_value());
    EXPECT_EQ(original_core->size->x, written_core->size->x);
    EXPECT_EQ(original_core->size->y, written_core->size->y);
    ASSERT_TRUE(original_core->symmetry.has_value());
    ASSERT_TRUE(written_core->symmetry.has_value());
    EXPECT_EQ(original_core->symmetry->x, written_core->symmetry->x);
    EXPECT_EQ(original_core->symmetry->y, written_core->symmetry->y);

    const SiteData *original_dblcore = original_root.get_site(original_root.get_site_by_name("DBLCORE"));
    const SiteData *written_dblcore = written_root.get_site(written_root.get_site_by_name("DBLCORE"));
    ASSERT_TRUE(original_dblcore != nullptr);
    ASSERT_TRUE(written_dblcore != nullptr);

    ASSERT_EQ(original_dblcore->row_pattern.size(), written_dblcore->row_pattern.size());
    for (size_t i = 0; i < original_dblcore->row_pattern.size(); i++)
    {
        EXPECT_EQ(original_dblcore->row_pattern[i].site_name, written_dblcore->row_pattern[i].site_name);
        EXPECT_EQ(original_dblcore->row_pattern[i].orient, written_dblcore->row_pattern[i].orient);
    }
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsANonDefaultRuleWithAnEmbeddedViaAndUseStatements)
{
    const NonDefaultRuleId original_id = original_root.get_non_default_rule_by_name("WIDE_M1");
    const NonDefaultRuleId written_id = written_root.get_non_default_rule_by_name("WIDE_M1");
    const NonDefaultRuleData *original = original_root.get_non_default_rule(original_id);
    const NonDefaultRuleData *written = written_root.get_non_default_rule(written_id);
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_EQ(original->hard_spacing, written->hard_spacing);

    const std::vector<NonDefaultRuleLayerId> &original_nd_layer_ids = original_root.get_non_default_rule_layers(original_id);
    const std::vector<NonDefaultRuleLayerId> &written_nd_layer_ids = written_root.get_non_default_rule_layers(written_id);
    ASSERT_EQ(original_nd_layer_ids.size(), written_nd_layer_ids.size());
    ASSERT_EQ(original_nd_layer_ids.size(), 1u);
    const NonDefaultRuleLayerData &original_nd_layer = *original_root.get_non_default_rule_layer(original_nd_layer_ids[0]);
    const NonDefaultRuleLayerData &written_nd_layer = *written_root.get_non_default_rule_layer(written_nd_layer_ids[0]);
    EXPECT_EQ(original_nd_layer.layer_name, written_nd_layer.layer_name);
    ASSERT_TRUE(original_nd_layer.width.has_value());
    ASSERT_TRUE(written_nd_layer.width.has_value());
    EXPECT_EQ(*original_nd_layer.width, *written_nd_layer.width);
    ASSERT_TRUE(original_nd_layer.spacing.has_value());
    ASSERT_TRUE(written_nd_layer.spacing.has_value());
    EXPECT_EQ(*original_nd_layer.spacing, *written_nd_layer.spacing);

    const std::vector<NonDefaultRuleViaId> &original_nd_via_ids = original_root.get_non_default_rule_vias(original_id);
    const std::vector<NonDefaultRuleViaId> &written_nd_via_ids = written_root.get_non_default_rule_vias(written_id);
    ASSERT_EQ(original_nd_via_ids.size(), written_nd_via_ids.size());
    ASSERT_EQ(original_nd_via_ids.size(), 1u);
    const NonDefaultRuleViaId original_via_id = original_nd_via_ids[0];
    const NonDefaultRuleViaId written_via_id = written_nd_via_ids[0];
    const NonDefaultRuleViaData &original_via = *original_root.get_non_default_rule_via(original_via_id);
    const NonDefaultRuleViaData &written_via = *written_root.get_non_default_rule_via(written_via_id);
    EXPECT_EQ(original_via.name, written_via.name);
    ASSERT_TRUE(original_via.resistance.has_value());
    ASSERT_TRUE(written_via.resistance.has_value());
    EXPECT_DOUBLE_EQ(*original_via.resistance, *written_via.resistance);
    ASSERT_EQ(original_root.get_non_default_rule_via_layers(original_via_id).size(), written_root.get_non_default_rule_via_layers(written_via_id).size());
    // UPDATES.md 12 Phase 7 - PROPERTY on a NONDEFAULTRULE-embedded VIA.
    ASSERT_EQ(original_via.properties.size(), written_via.properties.size());
    ASSERT_EQ(original_via.properties.size(), 1u);
    EXPECT_EQ(original_via.properties[0].name, written_via.properties[0].name);
    EXPECT_DOUBLE_EQ(original_via.properties[0].number_value, written_via.properties[0].number_value);

    EXPECT_EQ(original->use_via_names, written->use_via_names);

    ASSERT_EQ(original->min_cuts.size(), written->min_cuts.size());
    ASSERT_EQ(original->min_cuts.size(), 1u);
    EXPECT_EQ(original->min_cuts[0].cut_layer_name, written->min_cuts[0].cut_layer_name);
    EXPECT_EQ(original->min_cuts[0].num_cuts, written->min_cuts[0].num_cuts);
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsPropertyDefinitionsAndPerConstructPropertyAttachments)
{
    const TechnologyId original_tech_id = original_root.get_technology_ids().front();
    const TechnologyId written_tech_id = written_root.get_technology_ids().front();
    const TechnologyData *original_tech = original_root.get_technology(original_tech_id);
    const TechnologyData *written_tech = written_root.get_technology(written_tech_id);
    ASSERT_TRUE(original_tech != nullptr);
    ASSERT_TRUE(written_tech != nullptr);

    const std::vector<PropertyDefinitionId> &original_property_definition_ids = original_root.get_technology_property_definitions(original_tech_id);
    const std::vector<PropertyDefinitionId> &written_property_definition_ids = written_root.get_technology_property_definitions(written_tech_id);
    ASSERT_EQ(original_property_definition_ids.size(), written_property_definition_ids.size());
    for (size_t i = 0; i < original_property_definition_ids.size(); i++)
    {
        const PropertyDefinitionData &o = *original_root.get_property_definition(original_property_definition_ids[i]);
        const PropertyDefinitionData &w = *written_root.get_property_definition(written_property_definition_ids[i]);
        EXPECT_EQ(o.owner_type, w.owner_type) << "definition " << i;
        EXPECT_EQ(o.name, w.name) << "definition " << i;
        EXPECT_EQ(o.data_type, w.data_type) << "definition " << i;
        EXPECT_EQ(o.range_min.has_value(), w.range_min.has_value()) << "definition " << i;
        if (o.range_min.has_value() && w.range_min.has_value())
            EXPECT_DOUBLE_EQ(*o.range_min, *w.range_min) << "definition " << i;
    }

    // M1 (ROUTING) only round-trips its STRING property - lefwRealProperty/
    // lefwIntProperty are missing LEFW_LAYERROUTING(_START) from their own
    // accepted-state list (see write_properties's own comment), so the
    // numeric ones are readable on the original but never written.
    const LayerData *original_m1 = original_root.get_layer(original_root.get_layer_by_name("M1"));
    const LayerData *written_m1 = written_root.get_layer(written_root.get_layer_by_name("M1"));
    ASSERT_TRUE(original_m1 != nullptr);
    ASSERT_TRUE(written_m1 != nullptr);
    ASSERT_EQ(original_m1->properties.size(), 1u);
    ASSERT_EQ(written_m1->properties.size(), 1u);
    EXPECT_EQ(original_m1->properties[0].name, written_m1->properties[0].name);
    EXPECT_EQ(original_m1->properties[0].string_value, written_m1->properties[0].string_value);

    // V1 (CUT) round-trips both numeric properties fully.
    const LayerData *original_v1 = original_root.get_layer(original_root.get_layer_by_name("V1"));
    const LayerData *written_v1 = written_root.get_layer(written_root.get_layer_by_name("V1"));
    ASSERT_TRUE(original_v1 != nullptr);
    ASSERT_TRUE(written_v1 != nullptr);
    ASSERT_EQ(original_v1->properties.size(), written_v1->properties.size());
    ASSERT_EQ(original_v1->properties.size(), 2u);
    for (size_t i = 0; i < original_v1->properties.size(); i++)
    {
        EXPECT_EQ(original_v1->properties[i].name, written_v1->properties[i].name) << "property " << i;
        EXPECT_DOUBLE_EQ(original_v1->properties[i].number_value, written_v1->properties[i].number_value) << "property " << i;
    }

    const ViaData *original_via1 = original_root.get_via(original_root.get_via_by_name("VIA1"));
    const ViaData *written_via1 = written_root.get_via(written_root.get_via_by_name("VIA1"));
    ASSERT_TRUE(original_via1 != nullptr);
    ASSERT_TRUE(written_via1 != nullptr);
    ASSERT_EQ(original_via1->properties.size(), written_via1->properties.size());
    ASSERT_EQ(original_via1->properties.size(), 1u);
    EXPECT_EQ(original_via1->properties[0].name, written_via1->properties[0].name);
    EXPECT_DOUBLE_EQ(original_via1->properties[0].number_value, written_via1->properties[0].number_value);

    const AbstractId original_abstract_id = original_root.get_design_abstract(original_root.get_design_by_name("WRITERTEST"));
    const AbstractId written_abstract_id = written_root.get_design_abstract(written_root.get_design_by_name("WRITERTEST"));
    const AbstractData *original_abstract = original_root.get_abstract(original_abstract_id);
    const AbstractData *written_abstract = written_root.get_abstract(written_abstract_id);
    ASSERT_TRUE(original_abstract != nullptr);
    ASSERT_TRUE(written_abstract != nullptr);
    ASSERT_EQ(original_abstract->properties.size(), written_abstract->properties.size());
    ASSERT_EQ(original_abstract->properties.size(), 1u);
    EXPECT_EQ(original_abstract->properties[0].name, written_abstract->properties[0].name);
    EXPECT_DOUBLE_EQ(original_abstract->properties[0].number_value, written_abstract->properties[0].number_value);

    const TerminalId original_pin_id = original_root.get_abstract_terminals(original_abstract_id).front();
    const TerminalId written_pin_id = written_root.get_abstract_terminals(written_abstract_id).front();
    const TerminalData *original_pin = original_root.get_terminal(original_pin_id);
    const TerminalData *written_pin = written_root.get_terminal(written_pin_id);
    ASSERT_TRUE(original_pin != nullptr);
    ASSERT_TRUE(written_pin != nullptr);
    ASSERT_EQ(original_pin->properties.size(), written_pin->properties.size());
    ASSERT_EQ(original_pin->properties.size(), 1u);
    EXPECT_EQ(original_pin->properties[0].name, written_pin->properties[0].name);
    EXPECT_EQ(original_pin->properties[0].string_value, written_pin->properties[0].string_value);
}

// UPDATES.md 12 Phase 5 (antenna modeling) - a dedicated fixture file,
// same reasoning as LEFAntennaFixture in lef_reader_test.cpp: the vendored
// parser's use5_3 flag is set once for the WHOLE FILE (see lef.y's VERSION
// rule), so writer_roundtrip.lef's own M1 ANTENNALENGTHFACTOR (5.3 syntax,
// Phase 1) can't coexist in the same file as this phase's 5.4+
// ANTENNAMODEL/ANTENNAAREARATIO/etc.
class LEFAntennaRoundtripFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_EQ(reader.read_lef(fixture_path("antenna_roundtrip.lef"), original_root, "test_lib"), 0);

        const std::string out_path = scratch_output_path("le_lef_antenna_roundtrip.lef");
        ASSERT_EQ(writer.write_lef(out_path, original_root, original_root.get_design_abstract(original_root.get_design_by_name("ANTENNATEST")), LEFWriter::LayerWriteMode::IncludeWithAbstract), 0)
            << (writer.messages().empty() ? "" : writer.messages().front());

        LEFReader reread;
        ASSERT_EQ(reread.read_lef(out_path, written_root, "test_lib"), 0)
            << (reread.messages().empty() ? "" : reread.messages().front());
    }

    Root original_root;
    Root written_root;
    LEFReader reader;
    LEFWriter writer;
};

TEST_F(LEFAntennaRoundtripFixture, RoundTripsAntennaModelsOnRoutingAndCutLayersAndOnAPin)
{
    // M1 (ROUTING) round-trips every antenna field the fixture sets,
    // including the ROUTING-only SideAreaRatio (see
    // write_layer_antenna_models's own comment on the CUT-layer gap).
    const LayerId original_m1_id = original_root.get_layer_by_name("M1");
    const LayerId written_m1_id = written_root.get_layer_by_name("M1");
    const LayerData *original_m1 = original_root.get_layer(original_m1_id);
    const LayerData *written_m1 = written_root.get_layer(written_m1_id);
    ASSERT_TRUE(original_m1 != nullptr);
    ASSERT_TRUE(written_m1 != nullptr);
    const std::vector<AntennaModelId> &original_m1_antenna_ids = original_root.get_layer_antenna_models(original_m1_id);
    const std::vector<AntennaModelId> &written_m1_antenna_ids = written_root.get_layer_antenna_models(written_m1_id);
    ASSERT_EQ(original_m1_antenna_ids.size(), written_m1_antenna_ids.size());
    ASSERT_EQ(original_m1_antenna_ids.size(), 1u);
    const AntennaModelData &original_m1_model = *original_root.get_antenna_model(original_m1_antenna_ids[0]);
    const AntennaModelData &written_m1_model = *written_root.get_antenna_model(written_m1_antenna_ids[0]);
    EXPECT_EQ(original_m1_model.oxide, written_m1_model.oxide);
    ASSERT_EQ(original_m1_model.area_ratio.has_value(), written_m1_model.area_ratio.has_value());
    EXPECT_DOUBLE_EQ(*original_m1_model.area_ratio, *written_m1_model.area_ratio);
    ASSERT_EQ(original_m1_model.diff_area_ratio_pwl.size(), written_m1_model.diff_area_ratio_pwl.size());
    for (size_t i = 0; i < original_m1_model.diff_area_ratio_pwl.size(); i++)
    {
        EXPECT_DOUBLE_EQ(original_m1_model.diff_area_ratio_pwl[i].diffusion, written_m1_model.diff_area_ratio_pwl[i].diffusion) << "pwl " << i;
        EXPECT_DOUBLE_EQ(original_m1_model.diff_area_ratio_pwl[i].ratio, written_m1_model.diff_area_ratio_pwl[i].ratio) << "pwl " << i;
    }
    ASSERT_EQ(original_m1_model.side_area_ratio.has_value(), written_m1_model.side_area_ratio.has_value());
    EXPECT_DOUBLE_EQ(*original_m1_model.side_area_ratio, *written_m1_model.side_area_ratio);

    // V1 (CUT) round-trips its plain AREARATIO field (the vendored writer
    // accepts CUT layers for this one - see write_layer_antenna_models).
    const LayerId original_v1_id = original_root.get_layer_by_name("V1");
    const LayerId written_v1_id = written_root.get_layer_by_name("V1");
    const LayerData *original_v1 = original_root.get_layer(original_v1_id);
    const LayerData *written_v1 = written_root.get_layer(written_v1_id);
    ASSERT_TRUE(original_v1 != nullptr);
    ASSERT_TRUE(written_v1 != nullptr);
    const std::vector<AntennaModelId> &original_v1_antenna_ids = original_root.get_layer_antenna_models(original_v1_id);
    const std::vector<AntennaModelId> &written_v1_antenna_ids = written_root.get_layer_antenna_models(written_v1_id);
    ASSERT_EQ(original_v1_antenna_ids.size(), written_v1_antenna_ids.size());
    ASSERT_EQ(original_v1_antenna_ids.size(), 1u);
    const AntennaModelData &original_v1_model = *original_root.get_antenna_model(original_v1_antenna_ids[0]);
    const AntennaModelData &written_v1_model = *written_root.get_antenna_model(written_v1_antenna_ids[0]);
    ASSERT_EQ(original_v1_model.area_ratio.has_value(), written_v1_model.area_ratio.has_value());
    EXPECT_DOUBLE_EQ(*original_v1_model.area_ratio, *written_v1_model.area_ratio);

    const AbstractId original_abstract_id = original_root.get_design_abstract(original_root.get_design_by_name("ANTENNATEST"));
    const AbstractId written_abstract_id = written_root.get_design_abstract(written_root.get_design_by_name("ANTENNATEST"));
    const TerminalId original_pin_id = original_root.get_abstract_terminals(original_abstract_id).front();
    const TerminalId written_pin_id = written_root.get_abstract_terminals(written_abstract_id).front();
    const TerminalData *original_pin = original_root.get_terminal(original_pin_id);
    const TerminalData *written_pin = written_root.get_terminal(written_pin_id);
    ASSERT_TRUE(original_pin != nullptr);
    ASSERT_TRUE(written_pin != nullptr);

    ASSERT_EQ(original_pin->antenna_partial_metal_area.size(), written_pin->antenna_partial_metal_area.size());
    ASSERT_EQ(original_pin->antenna_partial_metal_area.size(), 1u);
    EXPECT_DOUBLE_EQ(original_pin->antenna_partial_metal_area[0].value, written_pin->antenna_partial_metal_area[0].value);
    EXPECT_EQ(original_pin->antenna_partial_metal_area[0].layer_name, written_pin->antenna_partial_metal_area[0].layer_name);

    const std::vector<PinAntennaModelId> &original_pin_antenna_ids = original_root.get_terminal_antenna_models(original_pin_id);
    const std::vector<PinAntennaModelId> &written_pin_antenna_ids = written_root.get_terminal_antenna_models(written_pin_id);
    ASSERT_EQ(original_pin_antenna_ids.size(), written_pin_antenna_ids.size());
    ASSERT_EQ(original_pin_antenna_ids.size(), 1u);
    const PinAntennaModelData &original_pin_model = *original_root.get_pin_antenna_model(original_pin_antenna_ids[0]);
    const PinAntennaModelData &written_pin_model = *written_root.get_pin_antenna_model(written_pin_antenna_ids[0]);
    EXPECT_EQ(original_pin_model.oxide, written_pin_model.oxide);
    ASSERT_EQ(original_pin_model.gate_area.size(), written_pin_model.gate_area.size());
    ASSERT_EQ(original_pin_model.gate_area.size(), 1u);
    EXPECT_DOUBLE_EQ(original_pin_model.gate_area[0].value, written_pin_model.gate_area[0].value);
    EXPECT_EQ(original_pin_model.gate_area[0].layer_name, written_pin_model.gate_area[0].layer_name);
}

TEST_F(LEFAntennaRoundtripFixture, RoundTripsRoutingLayerMiscScalarAndTableFieldsAddedInPhase6)
{
    const LayerId original_m1_id = original_root.get_layer_by_name("M1");
    const LayerId written_m1_id = written_root.get_layer_by_name("M1");
    const LayerData *original_m1 = original_root.get_layer(original_m1_id);
    const LayerData *written_m1 = written_root.get_layer(written_m1_id);
    ASSERT_TRUE(original_m1 != nullptr);
    ASSERT_TRUE(written_m1 != nullptr);

    EXPECT_EQ(original_m1->direction, written_m1->direction);
    ASSERT_EQ(original_m1->default_mask.has_value(), written_m1->default_mask.has_value());
    EXPECT_EQ(*original_m1->default_mask, *written_m1->default_mask);

    ASSERT_EQ(original_m1->pitch_xy.has_value(), written_m1->pitch_xy.has_value());
    EXPECT_EQ(original_m1->pitch_xy->x, written_m1->pitch_xy->x);
    EXPECT_EQ(original_m1->pitch_xy->y, written_m1->pitch_xy->y);
    ASSERT_EQ(original_m1->offset_xy.has_value(), written_m1->offset_xy.has_value());
    EXPECT_EQ(original_m1->offset_xy->x, written_m1->offset_xy->x);
    EXPECT_EQ(original_m1->offset_xy->y, written_m1->offset_xy->y);

    ASSERT_EQ(original_m1->diag_pitch.has_value(), written_m1->diag_pitch.has_value());
    EXPECT_EQ(*original_m1->diag_pitch, *written_m1->diag_pitch);
    ASSERT_EQ(original_m1->diag_spacing.has_value(), written_m1->diag_spacing.has_value());
    EXPECT_EQ(*original_m1->diag_spacing, *written_m1->diag_spacing);
    ASSERT_EQ(original_m1->diag_width.has_value(), written_m1->diag_width.has_value());
    EXPECT_EQ(*original_m1->diag_width, *written_m1->diag_width);
    ASSERT_EQ(original_m1->diag_min_edge_length.has_value(), written_m1->diag_min_edge_length.has_value());
    EXPECT_EQ(*original_m1->diag_min_edge_length, *written_m1->diag_min_edge_length);

    ASSERT_EQ(original_m1->max_width.has_value(), written_m1->max_width.has_value());
    EXPECT_EQ(*original_m1->max_width, *written_m1->max_width);
    ASSERT_EQ(original_m1->min_width.has_value(), written_m1->min_width.has_value());
    EXPECT_EQ(*original_m1->min_width, *written_m1->min_width);

    ASSERT_EQ(original_m1->min_sizes.size(), written_m1->min_sizes.size());
    for (size_t i = 0; i < original_m1->min_sizes.size(); i++)
    {
        EXPECT_EQ(original_m1->min_sizes[i].width, written_m1->min_sizes[i].width) << "entry " << i;
        EXPECT_EQ(original_m1->min_sizes[i].length, written_m1->min_sizes[i].length) << "entry " << i;
    }

    ASSERT_EQ(original_m1->min_enclosed_areas.size(), written_m1->min_enclosed_areas.size());
    for (size_t i = 0; i < original_m1->min_enclosed_areas.size(); i++)
    {
        EXPECT_EQ(original_m1->min_enclosed_areas[i].area, written_m1->min_enclosed_areas[i].area) << "entry " << i;
        EXPECT_EQ(original_m1->min_enclosed_areas[i].width.has_value(), written_m1->min_enclosed_areas[i].width.has_value()) << "entry " << i;
        if (original_m1->min_enclosed_areas[i].width)
            EXPECT_EQ(*original_m1->min_enclosed_areas[i].width, *written_m1->min_enclosed_areas[i].width) << "entry " << i;
    }

    ASSERT_EQ(original_m1->protrusion_width1.has_value(), written_m1->protrusion_width1.has_value());
    EXPECT_EQ(*original_m1->protrusion_width1, *written_m1->protrusion_width1);
    EXPECT_EQ(*original_m1->protrusion_length, *written_m1->protrusion_length);
    EXPECT_EQ(*original_m1->protrusion_width2, *written_m1->protrusion_width2);

    const std::vector<TwoWidthsSpacingEntryId> &original_two_widths_ids = original_root.get_layer_spacing_table_two_widths(original_m1_id);
    const std::vector<TwoWidthsSpacingEntryId> &written_two_widths_ids = written_root.get_layer_spacing_table_two_widths(written_m1_id);
    ASSERT_EQ(original_two_widths_ids.size(), written_two_widths_ids.size());
    for (size_t i = 0; i < original_two_widths_ids.size(); i++)
    {
        const TwoWidthsSpacingEntryData &o = *original_root.get_two_widths_spacing_entry(original_two_widths_ids[i]);
        const TwoWidthsSpacingEntryData &w = *written_root.get_two_widths_spacing_entry(written_two_widths_ids[i]);
        EXPECT_EQ(o.width, w.width) << "row " << i;
        // Row 1's PRL is nonzero (150) and round-trips fine; row 0 has no
        // PRL at all, also unaffected - the documented PRL==0 vendored-
        // writer edge case (write_technology_layers's own comment) isn't
        // exercised by this fixture's own values, deliberately.
        EXPECT_EQ(o.prl.has_value(), w.prl.has_value()) << "row " << i;
        if (o.prl)
            EXPECT_EQ(*o.prl, *w.prl) << "row " << i;
        ASSERT_EQ(o.spacings.size(), w.spacings.size()) << "row " << i;
        for (size_t j = 0; j < o.spacings.size(); j++)
            EXPECT_EQ(o.spacings[j], w.spacings[j]) << "row " << i << " col " << j;
    }

    // split_wire_width is deliberately excluded here - read-only, no
    // vendored writer function exists (see write_technology_layers's own
    // comment); written_m1->split_wire_width stays unset.
    EXPECT_FALSE(written_m1->split_wire_width.has_value());

    ASSERT_EQ(original_m1->minimum_density.has_value(), written_m1->minimum_density.has_value());
    EXPECT_DOUBLE_EQ(*original_m1->minimum_density, *written_m1->minimum_density);
    ASSERT_EQ(original_m1->maximum_density.has_value(), written_m1->maximum_density.has_value());
    EXPECT_DOUBLE_EQ(*original_m1->maximum_density, *written_m1->maximum_density);
    ASSERT_EQ(original_m1->density_check_step.has_value(), written_m1->density_check_step.has_value());
    EXPECT_EQ(*original_m1->density_check_step, *written_m1->density_check_step);
    ASSERT_EQ(original_m1->density_check_window.has_value(), written_m1->density_check_window.has_value());
    EXPECT_EQ(original_m1->density_check_window->length, written_m1->density_check_window->length);
    EXPECT_EQ(original_m1->density_check_window->width, written_m1->density_check_window->width);
    ASSERT_EQ(original_m1->fill_active_spacing.has_value(), written_m1->fill_active_spacing.has_value());
    EXPECT_EQ(*original_m1->fill_active_spacing, *written_m1->fill_active_spacing);

    const std::vector<LayerDensityEntryId> &original_ac_ids = original_root.get_layer_ac_current_density(original_m1_id);
    const std::vector<LayerDensityEntryId> &written_ac_ids = written_root.get_layer_ac_current_density(written_m1_id);
    ASSERT_EQ(original_ac_ids.size(), written_ac_ids.size());
    ASSERT_EQ(original_ac_ids.size(), 2u);
    for (size_t i = 0; i < 2; i++)
    {
        const LayerDensityEntryData &o = *original_root.get_layer_density_entry(original_ac_ids[i]);
        const LayerDensityEntryData &w = *written_root.get_layer_density_entry(written_ac_ids[i]);
        EXPECT_EQ(o.type, w.type) << "entry " << i;
        EXPECT_EQ(o.one_entry.has_value(), w.one_entry.has_value()) << "entry " << i;
        if (o.one_entry)
            EXPECT_DOUBLE_EQ(*o.one_entry, *w.one_entry) << "entry " << i;
        ASSERT_EQ(o.frequency.size(), w.frequency.size()) << "entry " << i;
        for (size_t j = 0; j < o.frequency.size(); j++)
            EXPECT_DOUBLE_EQ(o.frequency[j], w.frequency[j]) << "entry " << i << " freq " << j;
        ASSERT_EQ(o.width.size(), w.width.size()) << "entry " << i;
        for (size_t j = 0; j < o.width.size(); j++)
            EXPECT_EQ(o.width[j], w.width[j]) << "entry " << i << " width " << j;
        ASSERT_EQ(o.table_entries.size(), w.table_entries.size()) << "entry " << i;
        for (size_t j = 0; j < o.table_entries.size(); j++)
            EXPECT_DOUBLE_EQ(o.table_entries[j], w.table_entries[j]) << "entry " << i << " table " << j;
    }

    const std::vector<LayerDensityEntryId> &original_dc_ids = original_root.get_layer_dc_current_density(original_m1_id);
    const std::vector<LayerDensityEntryId> &written_dc_ids = written_root.get_layer_dc_current_density(written_m1_id);
    ASSERT_EQ(original_dc_ids.size(), written_dc_ids.size());
    ASSERT_EQ(original_dc_ids.size(), 1u);
    const LayerDensityEntryData *original_dc0 = original_root.get_layer_density_entry(original_dc_ids[0]);
    const LayerDensityEntryData *written_dc0 = written_root.get_layer_density_entry(written_dc_ids[0]);
    ASSERT_EQ(original_dc0->one_entry.has_value(), written_dc0->one_entry.has_value());
    EXPECT_DOUBLE_EQ(*original_dc0->one_entry, *written_dc0->one_entry);
}

TEST_F(LEFAntennaRoundtripFixture, RoundTripsCutLayerArraySpacingEnclosureAndCurrentDensityAddedInPhase6)
{
    const LayerId original_v1_id = original_root.get_layer_by_name("V1");
    const LayerId written_v1_id = written_root.get_layer_by_name("V1");
    const LayerData *original_v1 = original_root.get_layer(original_v1_id);
    const LayerData *written_v1 = written_root.get_layer(written_v1_id);
    ASSERT_TRUE(original_v1 != nullptr);
    ASSERT_TRUE(written_v1 != nullptr);

    ASSERT_EQ(original_v1->array_cuts.size(), written_v1->array_cuts.size());
    for (size_t i = 0; i < original_v1->array_cuts.size(); i++)
    {
        EXPECT_EQ(original_v1->array_cuts[i].cuts, written_v1->array_cuts[i].cuts) << "entry " << i;
        EXPECT_EQ(original_v1->array_cuts[i].spacing, written_v1->array_cuts[i].spacing) << "entry " << i;
    }
    const ArraySpacingData *original_array_spacing = original_root.get_array_spacing(original_root.get_layer_array_spacing(original_v1_id));
    const ArraySpacingData *written_array_spacing = written_root.get_array_spacing(written_root.get_layer_array_spacing(written_v1_id));
    ASSERT_EQ(original_array_spacing != nullptr, written_array_spacing != nullptr);
    EXPECT_EQ(original_array_spacing->long_array, written_array_spacing->long_array);
    ASSERT_EQ(original_array_spacing->via_width.has_value(), written_array_spacing->via_width.has_value());
    EXPECT_EQ(*original_array_spacing->via_width, *written_array_spacing->via_width);
    EXPECT_EQ(original_array_spacing->cut_spacing, written_array_spacing->cut_spacing);

    const std::vector<PreferEnclosureEntryId> &original_prefer_enclosure_ids = original_root.get_layer_prefer_enclosures(original_v1_id);
    const std::vector<PreferEnclosureEntryId> &written_prefer_enclosure_ids = written_root.get_layer_prefer_enclosures(written_v1_id);
    ASSERT_EQ(original_prefer_enclosure_ids.size(), written_prefer_enclosure_ids.size());
    ASSERT_EQ(original_prefer_enclosure_ids.size(), 1u);
    const PreferEnclosureEntryData &original_prefer_enclosure0 = *original_root.get_prefer_enclosure_entry(original_prefer_enclosure_ids[0]);
    const PreferEnclosureEntryData &written_prefer_enclosure0 = *written_root.get_prefer_enclosure_entry(written_prefer_enclosure_ids[0]);
    EXPECT_EQ(original_prefer_enclosure0.location, written_prefer_enclosure0.location);
    EXPECT_EQ(original_prefer_enclosure0.overhang1, written_prefer_enclosure0.overhang1);
    EXPECT_EQ(original_prefer_enclosure0.overhang2, written_prefer_enclosure0.overhang2);

    const std::vector<EnclosureEntryId> &original_enclosure_ids = original_root.get_layer_enclosures(original_v1_id);
    const std::vector<EnclosureEntryId> &written_enclosure_ids = written_root.get_layer_enclosures(written_v1_id);
    ASSERT_EQ(original_enclosure_ids.size(), written_enclosure_ids.size());
    for (size_t i = 0; i < original_enclosure_ids.size(); i++)
    {
        const EnclosureEntryData &o = *original_root.get_enclosure_entry(original_enclosure_ids[i]);
        const EnclosureEntryData &w = *written_root.get_enclosure_entry(written_enclosure_ids[i]);
        EXPECT_EQ(o.location, w.location) << "entry " << i;
        EXPECT_EQ(o.overhang1, w.overhang1) << "entry " << i;
        EXPECT_EQ(o.overhang2, w.overhang2) << "entry " << i;
        EXPECT_EQ(o.width.has_value(), w.width.has_value()) << "entry " << i;
        if (o.width)
            EXPECT_EQ(*o.width, *w.width) << "entry " << i;
    }

    // KNOWN VENDORED-LIBRARY BUG: lefwLayerResistancePerCut writes the
    // wrong keyword ("RESISTANCEPERCUT", not a real lef.y token - see
    // write_technology_layers's own comment) - CUT-layer resistance is
    // read-only, so written_v1->resistance stays unset even though
    // original_v1->resistance is populated from the fixture's own
    // "RESISTANCE 10.0 ;".
    ASSERT_TRUE(original_v1->resistance.has_value());
    EXPECT_FALSE(written_v1->resistance.has_value());

    const std::vector<LayerDensityEntryId> &original_v1_dc_ids = original_root.get_layer_dc_current_density(original_v1_id);
    const std::vector<LayerDensityEntryId> &written_v1_dc_ids = written_root.get_layer_dc_current_density(written_v1_id);
    ASSERT_EQ(original_v1_dc_ids.size(), written_v1_dc_ids.size());
    ASSERT_EQ(original_v1_dc_ids.size(), 1u);
    const LayerDensityEntryData &o_dc = *original_root.get_layer_density_entry(original_v1_dc_ids[0]);
    const LayerDensityEntryData &w_dc = *written_root.get_layer_density_entry(written_v1_dc_ids[0]);
    ASSERT_EQ(o_dc.cutarea.size(), w_dc.cutarea.size());
    for (size_t i = 0; i < o_dc.cutarea.size(); i++)
        EXPECT_EQ(o_dc.cutarea[i], w_dc.cutarea[i]) << "cutarea " << i;
    ASSERT_EQ(o_dc.table_entries.size(), w_dc.table_entries.size());
    for (size_t i = 0; i < o_dc.table_entries.size(); i++)
        EXPECT_DOUBLE_EQ(o_dc.table_entries[i], w_dc.table_entries[i]) << "table " << i;
}

TEST_F(LEFAntennaRoundtripFixture, RoundTripsPinScalarFieldsDirectionEnumPortClassViaAndSiteArrayPlacementsAddedInPhase7)
{
    const AbstractId original_abstract_id = original_root.get_design_abstract(original_root.get_design_by_name("ANTENNATEST"));
    const AbstractId written_abstract_id = written_root.get_design_abstract(written_root.get_design_by_name("ANTENNATEST"));
    const AbstractData *original_abstract = original_root.get_abstract(original_abstract_id);
    const AbstractData *written_abstract = written_root.get_abstract(written_abstract_id);
    ASSERT_TRUE(original_abstract != nullptr);
    ASSERT_TRUE(written_abstract != nullptr);

    const std::vector<MacroSitePlacementId> &original_site_placement_ids = original_root.get_abstract_site_placements(original_abstract_id);
    const std::vector<MacroSitePlacementId> &written_site_placement_ids = written_root.get_abstract_site_placements(written_abstract_id);
    ASSERT_EQ(original_site_placement_ids.size(), written_site_placement_ids.size());
    ASSERT_EQ(original_site_placement_ids.size(), 2u);
    for (size_t i = 0; i < 2; i++)
    {
        const MacroSitePlacementData &o = *original_root.get_macro_site_placement(original_site_placement_ids[i]);
        const MacroSitePlacementData &w = *written_root.get_macro_site_placement(written_site_placement_ids[i]);
        EXPECT_EQ(o.site_name, w.site_name) << "entry " << i;
        EXPECT_EQ(o.orient, w.orient) << "entry " << i;
        EXPECT_EQ(o.num_x.has_value(), w.num_x.has_value()) << "entry " << i;
        if (o.num_x)
        {
            EXPECT_EQ(*o.num_x, *w.num_x) << "entry " << i;
            EXPECT_EQ(*o.step_x, *w.step_x) << "entry " << i;
        }
    }

    const std::vector<TerminalId> &original_terminal_ids = original_root.get_abstract_terminals(original_abstract_id);
    const std::vector<TerminalId> &written_terminal_ids = written_root.get_abstract_terminals(written_abstract_id);
    ASSERT_EQ(original_terminal_ids.size(), written_terminal_ids.size());
    ASSERT_EQ(original_terminal_ids.size(), 2u);

    const TerminalData *original_pin_a = original_root.get_terminal(original_terminal_ids[0]);
    const TerminalData *written_pin_a = written_root.get_terminal(written_terminal_ids[0]);
    ASSERT_TRUE(original_pin_a != nullptr);
    ASSERT_TRUE(written_pin_a != nullptr);
    EXPECT_EQ(original_pin_a->taper_rule, written_pin_a->taper_rule);
    EXPECT_EQ(original_pin_a->supply_sensitivity, written_pin_a->supply_sensitivity);
    EXPECT_EQ(original_pin_a->ground_sensitivity, written_pin_a->ground_sensitivity);
    // rise_slew_limit/fall_slew_limit/max_load are read-only - no vendored
    // writer function exists (see write_terminal's own comment) -
    // written_pin_a stays unset even though original_pin_a has a real
    // value from the fixture.
    ASSERT_TRUE(original_pin_a->rise_slew_limit.has_value());
    EXPECT_DOUBLE_EQ(*original_pin_a->rise_slew_limit, 0.01);
    EXPECT_FALSE(written_pin_a->rise_slew_limit.has_value());
    EXPECT_FALSE(written_pin_a->fall_slew_limit.has_value());
    EXPECT_FALSE(written_pin_a->max_load.has_value());

    const std::vector<TerminalPortId> &original_port_ids = original_root.get_terminal_ports(original_terminal_ids[0]);
    const std::vector<TerminalPortId> &written_port_ids = written_root.get_terminal_ports(written_terminal_ids[0]);
    ASSERT_EQ(original_port_ids.size(), written_port_ids.size());
    ASSERT_EQ(original_port_ids.size(), 1u);
    const TerminalPortData *original_port_a = original_root.get_terminal_port(original_port_ids[0]);
    const TerminalPortData *written_port_a = written_root.get_terminal_port(written_port_ids[0]);
    ASSERT_TRUE(original_port_a != nullptr);
    ASSERT_TRUE(written_port_a != nullptr);
    EXPECT_EQ(original_port_a->port_class, written_port_a->port_class);
    const std::vector<ShapeId> &original_port_a_shapes = original_root.get_terminal_port_shapes(original_port_ids[0]);
    const std::vector<ShapeId> &written_port_a_shapes = written_root.get_terminal_port_shapes(written_port_ids[0]);
    ASSERT_EQ(original_port_a_shapes.size(), written_port_a_shapes.size());
    ASSERT_EQ(original_port_a_shapes.size(), 1u);
    const Shape &original_port_a_shape = *original_root.get_shape(original_port_a_shapes[0]);
    const Shape &written_port_a_shape = *written_root.get_shape(written_port_a_shapes[0]);
    EXPECT_EQ(original_port_a_shape.spacing, written_port_a_shape.spacing);
    ASSERT_EQ(original_port_a_shape.vias.size(), written_port_a_shape.vias.size());
    ASSERT_EQ(original_port_a_shape.vias.size(), 1u);
    EXPECT_EQ(original_port_a_shape.vias[0].via_name, written_port_a_shape.vias[0].via_name);
    EXPECT_EQ(original_port_a_shape.vias[0].origin.x, written_port_a_shape.vias[0].origin.x);

    const TerminalData *original_pin_b = original_root.get_terminal(original_terminal_ids[1]);
    const TerminalData *written_pin_b = written_root.get_terminal(written_terminal_ids[1]);
    ASSERT_TRUE(original_pin_b != nullptr);
    ASSERT_TRUE(written_pin_b != nullptr);
    EXPECT_EQ(original_pin_b->direction, SignalDirection::FEEDTHRU);
    EXPECT_EQ(original_pin_b->direction, written_pin_b->direction);

    const std::vector<ObstructionId> &original_obs_ids = original_root.get_abstract_obstructions(original_abstract_id);
    const std::vector<ObstructionId> &written_obs_ids = written_root.get_abstract_obstructions(written_abstract_id);
    ASSERT_EQ(original_obs_ids.size(), written_obs_ids.size());
    ASSERT_EQ(original_obs_ids.size(), 1u);
    const ObstructionData *original_obs = original_root.get_obstruction(original_obs_ids.front());
    const ObstructionData *written_obs = written_root.get_obstruction(written_obs_ids.front());
    ASSERT_TRUE(original_obs != nullptr);
    ASSERT_TRUE(written_obs != nullptr);
    const std::vector<ShapeId> &original_obs_shapes = original_root.get_obstruction_shapes(original_obs_ids.front());
    const std::vector<ShapeId> &written_obs_shapes = written_root.get_obstruction_shapes(written_obs_ids.front());
    ASSERT_EQ(original_obs_shapes.size(), written_obs_shapes.size());
    ASSERT_EQ(original_obs_shapes.size(), 1u);
    const Shape &original_obs_shape = *original_root.get_shape(original_obs_shapes[0]);
    const Shape &written_obs_shape = *written_root.get_shape(written_obs_shapes[0]);
    EXPECT_TRUE(original_obs_shape.except_pg_net);
    EXPECT_EQ(original_obs_shape.except_pg_net, written_obs_shape.except_pg_net);
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsMacroClassOriginSizeSymmetryAndSite)
{
    const DesignId original_design_id = original_root.get_design_by_name("WRITERTEST");
    const DesignId written_design_id = written_root.get_design_by_name("WRITERTEST");
    ASSERT_TRUE(original_design_id.valid());
    ASSERT_TRUE(written_design_id.valid());

    const AbstractData *original = original_root.get_abstract(original_root.get_design_abstract(original_design_id));
    const AbstractData *written = written_root.get_abstract(written_root.get_design_abstract(written_design_id));
    ASSERT_TRUE(original != nullptr);
    ASSERT_TRUE(written != nullptr);

    EXPECT_EQ(original->type, written->type);
    ASSERT_TRUE(original->origin.has_value());
    ASSERT_TRUE(written->origin.has_value());
    EXPECT_EQ(original->origin->x, written->origin->x);
    EXPECT_EQ(original->origin->y, written->origin->y);
    ASSERT_TRUE(original->size.has_value());
    ASSERT_TRUE(written->size.has_value());
    EXPECT_EQ(original->size->x, written->size->x);
    EXPECT_EQ(original->size->y, written->size->y);
    ASSERT_TRUE(original->symmetry.has_value());
    ASSERT_TRUE(written->symmetry.has_value());
    EXPECT_EQ(original->symmetry->x, written->symmetry->x);
    EXPECT_EQ(original->symmetry->y, written->symmetry->y);
    EXPECT_EQ(original->symmetry->r90, written->symmetry->r90);
    EXPECT_EQ(original->site, written->site);
    EXPECT_EQ(original->eeq, written->eeq);
    EXPECT_EQ(original->is_fixed_mask, written->is_fixed_mask);
    // leq/power/source are never populated in the first place for a
    // VERSION 5.8 file (lef.y's own grammar drops all three silently on
    // read at that version) - see write_macro's own comment.
    EXPECT_FALSE(original->leq.has_value());
    EXPECT_FALSE(written->leq.has_value());
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsPinDirectionAndPortGeometryIncludingAMixedRectPolygonPath)
{
    const DesignId original_design_id = original_root.get_design_by_name("WRITERTEST");
    const DesignId written_design_id = written_root.get_design_by_name("WRITERTEST");
    const AbstractId original_abstract_id = original_root.get_design_abstract(original_design_id);
    const AbstractId written_abstract_id = written_root.get_design_abstract(written_design_id);

    ASSERT_EQ(original_root.get_abstract_terminals(original_abstract_id).size(), 1u);
    ASSERT_EQ(written_root.get_abstract_terminals(written_abstract_id).size(), 1u);

    const TerminalId original_terminal_id = original_root.get_abstract_terminals(original_abstract_id).front();
    const TerminalId written_terminal_id = written_root.get_abstract_terminals(written_abstract_id).front();

    const TerminalData *original_terminal = original_root.get_terminal(original_terminal_id);
    const TerminalData *written_terminal = written_root.get_terminal(written_terminal_id);
    EXPECT_EQ(original_terminal->direction, written_terminal->direction);
    EXPECT_EQ(original_terminal->use, written_terminal->use);
    EXPECT_EQ(original_terminal->shape, written_terminal->shape);
    EXPECT_EQ(original_terminal->must_join, written_terminal->must_join);
    EXPECT_EQ(original_terminal->net_expr, written_terminal->net_expr);
    // leq is never populated at VERSION 5.8 (obsolete on both read and
    // write - see write_terminal's own comment).
    EXPECT_FALSE(original_terminal->leq.has_value());
    EXPECT_FALSE(written_terminal->leq.has_value());

    ASSERT_EQ(original_root.get_terminal_ports(original_terminal_id).size(), 1u);
    ASSERT_EQ(written_root.get_terminal_ports(written_terminal_id).size(), 1u);

    const TerminalPortId original_port_id = original_root.get_terminal_ports(original_terminal_id).front();
    const TerminalPortId written_port_id = written_root.get_terminal_ports(written_terminal_id).front();
    const std::vector<ShapeId> &original_port_shapes = original_root.get_terminal_port_shapes(original_port_id);
    const std::vector<ShapeId> &written_port_shapes = written_root.get_terminal_port_shapes(written_port_id);

    ASSERT_EQ(original_port_shapes.size(), written_port_shapes.size());
    for (size_t i = 0; i < original_port_shapes.size(); i++)
        expect_shapes_equal(*original_root.get_shape(original_port_shapes[i]), *written_root.get_shape(written_port_shapes[i]));
}

TEST_F(LEFWriterRoundtripFixture, RoundTripsObstructionRectIterateAndPathIterateAsRawStatementsNotExpanded)
{
    const DesignId original_design_id = original_root.get_design_by_name("WRITERTEST");
    const DesignId written_design_id = written_root.get_design_by_name("WRITERTEST");
    const AbstractId original_abstract_id = original_root.get_design_abstract(original_design_id);
    const AbstractId written_abstract_id = written_root.get_design_abstract(written_design_id);

    ASSERT_EQ(original_root.get_abstract_obstructions(original_abstract_id).size(), 1u);
    ASSERT_EQ(written_root.get_abstract_obstructions(written_abstract_id).size(), 1u);

    const ObstructionId original_obstruction_id = original_root.get_abstract_obstructions(original_abstract_id).front();
    const ObstructionId written_obstruction_id = written_root.get_abstract_obstructions(written_abstract_id).front();
    const std::vector<ShapeId> &original_shapes = original_root.get_obstruction_shapes(original_obstruction_id);
    const std::vector<ShapeId> &written_shapes = written_root.get_obstruction_shapes(written_obstruction_id);

    ASSERT_EQ(original_shapes.size(), written_shapes.size());
    for (size_t i = 0; i < original_shapes.size(); i++)
        expect_shapes_equal(*original_root.get_shape(original_shapes[i]), *written_root.get_shape(written_shapes[i]));

    // Belt-and-suspenders on the specific claim in this test's name: both
    // the original read and the round-tripped re-read see the ITERATE as
    // one raw statement each (num_x*num_y == 4/2), not 4/2 separately
    // materialized rects/paths - proving LEFWriter re-emitted compact
    // ITERATE syntax rather than expanded individual RECT/PATH statements.
    ASSERT_EQ(original_root.get_shape(original_shapes.front())->rect_iterates.size(), 1u);
    ASSERT_EQ(written_root.get_shape(written_shapes.front())->rect_iterates.size(), 1u);
    ASSERT_EQ(original_root.get_shape(original_shapes.front())->path_iterates.size(), 1u);
    ASSERT_EQ(written_root.get_shape(written_shapes.front())->path_iterates.size(), 1u);
}

TEST_F(LEFWriterRoundtripFixture, LayerWriteModeNoneWritesNoLayers)
{
    const std::string out_path = scratch_output_path();
    LEFWriter local_writer;
    ASSERT_EQ(local_writer.write_lef(out_path, original_root, original_root.get_design_abstract(original_root.get_design_by_name("WRITERTEST")), LEFWriter::LayerWriteMode::None), 0);

    // LayerWriteMode::None deliberately omits UNITS too (see write_lef's
    // own comment - it's gated on the same `mode != None` check as the
    // layers), so this MACRO-only file has no DATABASE MICRONS of its
    // own - pre-seed reread_root's Technology with one, mirroring the
    // realistic "tech file read first, macro file second, same Root"
    // pattern this mode exists for (matches load-order convention in
    // lef_reader.cpp's own is_technology_empty()-gated create-vs-reuse).
    Root reread_root;
    reread_root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    LEFReader reread;
    ASSERT_EQ(reread.read_lef(out_path, reread_root, "test_lib"), 0);
    EXPECT_TRUE(reread_root.is_layer_empty());
    EXPECT_TRUE(reread_root.get_design_by_name("WRITERTEST").valid());
}

TEST_F(LEFWriterRoundtripFixture, LayerWriteModeTechnologyOnlyWritesNoMacro)
{
    const std::string out_path = scratch_output_path();
    LEFWriter local_writer;
    ASSERT_EQ(local_writer.write_lef(out_path, original_root, original_root.get_design_abstract(original_root.get_design_by_name("WRITERTEST")), LEFWriter::LayerWriteMode::TechnologyOnly), 0);

    Root reread_root;
    LEFReader reread;
    ASSERT_EQ(reread.read_lef(out_path, reread_root, "test_lib"), 0);
    EXPECT_TRUE(reread_root.get_layer_by_name("M1").valid());
    EXPECT_FALSE(reread_root.get_design_by_name("WRITERTEST").valid());
}

TEST(LEFWriterTwoWordMacroClass, SplitsATwoWordAbstractTypeIntoBaseAndSubtypeInsteadOfFailingTheWholeWrite)
{
    // LEF's CLASS statement is `CLASS <base> [<subtype>] ;` - lef.y's own
    // class_type grammar rule combines both into ONE space-joined string
    // (e.g. "CORE WELLTAP") in lefiMacro::macroClass(), which is exactly
    // what lef_reader.cpp stores verbatim into AbstractData::type. Found
    // by running the lef_roundtrip_diff dev tool against complete.5.8.lef -
    // passing that combined string straight to lefwMacroClass's first
    // argument (which validates it against a fixed known-base-class list)
    // silently failed the ENTIRE MACRO write with LEFW_BAD_DATA for every
    // macro using a two-word CLASS (mycorewelltap/myblackbox/mysoft/
    // mydriver in that fixture all hit this before the fix).
    Root root;
    const LibraryId library_id = root.create_library(LibraryData{.name = "test_lib"});
    const DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "WELLTAPTEST"});
    root.create_abstract(AbstractData{.design = design_id, .type = "CORE WELLTAP"});

    const std::string out_path = (std::filesystem::temp_directory_path() / "le_lef_writer_two_word_class.lef").string();
    LEFWriter writer;
    ASSERT_EQ(writer.write_lef(out_path, root, root.get_design_abstract(design_id), LEFWriter::LayerWriteMode::None), 0)
        << (writer.messages().empty() ? "" : writer.messages().front());

    // LayerWriteMode::None omits UNITS along with layers - this MACRO's
    // own ORIGIN/SIZE statements still need a database_units_microns to
    // convert against on re-read (see LEFWriterRoundtripFixture's
    // LayerWriteModeNoneWritesNoLayers test for the same pre-seeding).
    Root reread_root;
    reread_root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    LEFReader reread;
    ASSERT_EQ(reread.read_lef(out_path, reread_root, "test_lib"), 0);

    const DesignId reread_design_id = reread_root.get_design_by_name("WELLTAPTEST");
    ASSERT_TRUE(reread_design_id.valid());
    const AbstractData *reread_abstract = reread_root.get_abstract(reread_root.get_design_abstract(reread_design_id));
    ASSERT_TRUE(reread_abstract != nullptr);
    EXPECT_EQ(reread_abstract->type, "CORE WELLTAP");
}

TEST(LEFWriterErrorHandling, FailsGracefullyWhenTheOutputPathCannotBeOpened)
{
    Root root;
    LEFWriter writer;
    // A directory that doesn't exist - fopen(..., "w") fails.
    const int result = writer.write_lef("/nonexistent_directory_le_test/out.lef", root, AbstractId{}, LEFWriter::LayerWriteMode::None);
    EXPECT_NE(result, 0);
    ASSERT_FALSE(writer.messages().empty());
}

TEST(LEFWriterErrorHandling, WritesJustTheEndLibraryLineForAnEmptyRoot)
{
    Root root;
    LEFWriter writer;
    const std::string out_path = (std::filesystem::temp_directory_path() / "le_lef_writer_empty.lef").string();
    EXPECT_EQ(writer.write_lef(out_path, root, AbstractId{}, LEFWriter::LayerWriteMode::IncludeWithAbstract), 0);
}

namespace
{
    // lefdiff itself doesn't diff anything (despite the name) - its real
    // main() (differLef.cpp, confirmed by reading it) takes
    // `fileName1 fileName2 lefOut1 lefOut2` and dumps a normalized text
    // representation of each input to lefOut1/lefOut2; the caller is
    // expected to sort and compare those two dumps itself (see that
    // file's own "later these files will be sorted for comparison"
    // comment) - confirmed directly by the user, who'd hit this exact
    // "lefdiff doesn't work, it just writes sorted-looking output" gotcha
    // before.
    std::vector<std::string> sorted_lines(const std::string &path)
    {
        std::ifstream in(path);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
        std::sort(lines.begin(), lines.end());
        return lines;
    }
}

// This is the actual UPDATES.md item 12 verification loop, automated: read
// a LEF file, write it back out via LEFWriter, and confirm the vendored
// lefdiff tool's own normalized dumps of the two files match - i.e. the
// round trip lost nothing lefdiff itself can see. writer_roundtrip.lef is
// deliberately scoped to exactly what Phase 1 supports (see its own
// comment/the fixture file), so this is expected to pass cleanly today;
// it's the harness later phases reuse to verify their own added coverage
// (each new construct added to writer_roundtrip.lef either round-trips
// clean or shows up here as a real, specific diff).
TEST(LEFWriterLefdiffFidelity, WriterRoundtripFixtureMatchesTheOriginalPerLefdiff)
{
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("writer_roundtrip.lef"), root, "test_lib"), 0);

    const std::string written_path = (std::filesystem::temp_directory_path() / "le_lefdiff_written.lef").string();
    LEFWriter writer;
    ASSERT_EQ(writer.write_lef(written_path, root, root.get_design_abstract(root.get_design_by_name("WRITERTEST")), LEFWriter::LayerWriteMode::IncludeWithAbstract), 0);

    const std::string dump1_path = (std::filesystem::temp_directory_path() / "le_lefdiff_dump1.txt").string();
    const std::string dump2_path = (std::filesystem::temp_directory_path() / "le_lefdiff_dump2.txt").string();

    const std::string command = fmt::format(
        "\"{}\" \"{}\" \"{}\" \"{}\" \"{}\" > /dev/null 2>&1",
        LEFDIFF_BIN, fixture_path("writer_roundtrip.lef"), written_path, dump1_path, dump2_path);
    ASSERT_EQ(std::system(command.c_str()), 0) << "lefdiff itself failed to run: " << command;

    EXPECT_EQ(sorted_lines(dump1_path), sorted_lines(dump2_path));
}
