#include "../lef_reader.hpp"
#include <gtest/gtest.h>

using namespace le;

// src/lefdef/lef/TEST/complete.5.8.lef is the vendored LEF parser's own
// "everything" regression fixture. LEFReader only implements a subset of LEF
// (see lef_reader.cpp) - lefrSetRegisterUnusedCallbacks() means the parser
// still succeeds and just skips sections we haven't wired up a callback for.
// Assertions below cover only what LEFReader is expected to extract today;
// extend them as more LEF constructs get support.
namespace
{
    std::string complete_fixture_path()
    {
        return std::string(LEFDEF_TEST_DIR) + "/complete.5.8.lef";
    }

    std::string fixture_path(const std::string &name)
    {
        return std::string(IO_TEST_FIXTURES_DIR) + "/" + name;
    }

    TerminalId find_terminal_id(const Root &root, AbstractId abstract_id, const std::string &name)
    {
        for (auto terminal_id : root.get_abstract_terminals(abstract_id))
        {
            if (root.get_terminal(terminal_id)->name == name)
                return terminal_id;
        }
        return TerminalId{};
    }
}

class LEFReaderCompleteFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        result = reader.read_lef(complete_fixture_path(), root, "test_lib");
    }

    Root root;
    LEFReader reader;
    int result;
};

TEST_F(LEFReaderCompleteFixture, ParsesSuccessfullyAndReadsUnits)
{
    ASSERT_EQ(result, 0);
    ASSERT_EQ(root.get_technology_size(), 1u);

    TechnologyId technology_id = root.get_technology_ids().front();
    EXPECT_DOUBLE_EQ(root.get_technology(technology_id)->database_units_microns, 20000.0);
}

TEST_F(LEFReaderCompleteFixture, CreatesAllLayersWithTypeAndDirection)
{
    // 25 uppercase `LAYER ...` blocks plus one lowercase `layer OVERLAP`.
    EXPECT_EQ(root.get_layer_ids().size(), 26u);

    LayerId m1_id = root.get_layer_by_name("M1");
    ASSERT_TRUE(m1_id.valid());
    const LayerData *m1 = root.get_layer(m1_id);
    EXPECT_EQ(m1->type, "ROUTING");
    EXPECT_EQ(m1->direction, RoutingDirection::H);

    LayerId v1_id = root.get_layer_by_name("V1");
    ASSERT_TRUE(v1_id.valid());
    EXPECT_EQ(root.get_layer(v1_id)->type, "CUT");
    EXPECT_EQ(root.get_layer(v1_id)->direction, RoutingDirection::NONE);
}

TEST_F(LEFReaderCompleteFixture, ReadsBasicScalarLayerProperties)
{
    // LAYER M1: TYPE ROUTING ; WIDTH 1 ; WIREEXTENSION 7 ; PITCH 1.8 ;
    // DIRECTION HORIZONTAL ; RESISTANCE RPERSQ 0.103 ;
    // CAPACITANCE CPERSQDIST 0.000156 ; - no OFFSET/AREA/SPACING/HEIGHT/
    // THICKNESS statements, so those stay nullopt (UPDATES.md 12 Phase 1's
    // has*()-guarded basic scalar LAYER coverage).
    LayerId m1_id = root.get_layer_by_name("M1");
    ASSERT_TRUE(m1_id.valid());
    const LayerData *m1 = root.get_layer(m1_id);

    ASSERT_TRUE(m1->width.has_value());
    EXPECT_EQ(*m1->width, 20000); // 1um * 20000 dbu/um
    ASSERT_TRUE(m1->pitch.has_value());
    EXPECT_EQ(*m1->pitch, 36000); // 1.8um * 20000 dbu/um
    ASSERT_TRUE(m1->wire_extension.has_value());
    EXPECT_EQ(*m1->wire_extension, 140000); // 7um * 20000 dbu/um
    ASSERT_TRUE(m1->resistance.has_value());
    EXPECT_DOUBLE_EQ(*m1->resistance, 0.103);
    ASSERT_TRUE(m1->capacitance.has_value());
    EXPECT_DOUBLE_EQ(*m1->capacitance, 0.000156);

    EXPECT_FALSE(m1->offset.has_value());
    EXPECT_FALSE(m1->area.has_value());
    EXPECT_TRUE(root.get_layer_spacing_rules(m1_id).empty());
    EXPECT_FALSE(m1->height.has_value());
    EXPECT_FALSE(m1->thickness.has_value());
}

TEST_F(LEFReaderCompleteFixture, ReadsSpacingModifiersUnwritableByTheVendoredWriter)
{
    // ENDOFLINE/PARALLELEDGE/TWOEDGES (LAYER PC) and ROUTING CENTERTOCENTER
    // are both still fully readable, even though LEFWriter can't re-emit
    // them (lefwLayerRoutingSpacingEndOfLine's own premature-semicolon bug,
    // and lefwLayerSpacingCenterToCenter being obsoleted with no
    // replacement in this vendored writer version - see write_technology_layers's
    // own comments) - covered here via complete.5.8.lef directly rather
    // than through a round trip.
    const LayerId pc_id = root.get_layer_by_name("PC");
    const LayerData *pc = root.get_layer(pc_id);
    ASSERT_TRUE(pc != nullptr);
    const std::vector<SpacingRuleId> &pc_spacing_rule_ids = root.get_layer_spacing_rules(pc_id);
    ASSERT_EQ(pc_spacing_rule_ids.size(), 4u);

    // complete.5.8.lef: DATABASE MICRONS 20000.
    const SpacingRuleData &eol_only = *root.get_spacing_rule(pc_spacing_rule_ids[1]);
    ASSERT_TRUE(eol_only.end_of_line_width.has_value());
    ASSERT_TRUE(eol_only.end_of_line_within.has_value());
    EXPECT_EQ(*eol_only.end_of_line_width, 26000); // 1.3um
    EXPECT_EQ(*eol_only.end_of_line_within, 12000); // 0.6um
    EXPECT_FALSE(eol_only.parallel_edge_space.has_value());

    const SpacingRuleData &eol_with_two_edges = *root.get_spacing_rule(pc_spacing_rule_ids[2]);
    ASSERT_TRUE(eol_with_two_edges.parallel_edge_space.has_value());
    ASSERT_TRUE(eol_with_two_edges.parallel_edge_within.has_value());
    EXPECT_EQ(*eol_with_two_edges.parallel_edge_space, 22000); // 1.1um
    EXPECT_EQ(*eol_with_two_edges.parallel_edge_within, 10000); // 0.5um
    EXPECT_TRUE(eol_with_two_edges.two_edges);

    const SpacingRuleData &eol_without_two_edges = *root.get_spacing_rule(pc_spacing_rule_ids[3]);
    EXPECT_FALSE(eol_without_two_edges.two_edges);

    const LayerId via12_id = root.get_layer_by_name("via12");
    const LayerData *via12 = root.get_layer(via12_id);
    ASSERT_TRUE(via12 != nullptr);
    const std::vector<SpacingRuleId> &via12_spacing_rule_ids = root.get_layer_spacing_rules(via12_id);
    ASSERT_EQ(via12_spacing_rule_ids.size(), 1u);
    EXPECT_TRUE(root.get_spacing_rule(via12_spacing_rule_ids[0])->center_to_center);
}

TEST_F(LEFReaderCompleteFixture, ReadsMacroLevelDensityUnwritableByTheVendoredWriter)
{
    // MACRO-level DENSITY is fully readable but not writable via this
    // vendored version (lefwStartMacroDensity emits the wrong syntax
    // entirely - see write_macro's own comment) - covered here via
    // complete.5.8.lef directly rather than through a round trip.
    // complete.5.8.lef: DATABASE MICRONS 20000.
    const DesignId design_id = root.get_design_by_name("INV");
    ASSERT_TRUE(design_id.valid());
    const AbstractId abstract_id = root.get_design_abstract(design_id);
    const AbstractData *abstract = root.get_abstract(abstract_id);
    ASSERT_TRUE(abstract != nullptr);

    const std::vector<MacroDensityLayerId> &density_ids = root.get_abstract_densities(abstract_id);
    ASSERT_EQ(density_ids.size(), 3u);

    const MacroDensityLayerData &metal1 = *root.get_macro_density_layer(density_ids[0]);
    EXPECT_EQ(metal1.layer_name, "metal1");
    ASSERT_EQ(metal1.rects.size(), 2u);
    ASSERT_EQ(metal1.values.size(), 2u);
    EXPECT_EQ(metal1.rects[0].ll.x, 0);
    EXPECT_EQ(metal1.rects[0].ur.x, 2000000);
    EXPECT_DOUBLE_EQ(metal1.values[0], 45.5);
    EXPECT_DOUBLE_EQ(metal1.values[1], 42.2);

    const MacroDensityLayerData &metal3 = *root.get_macro_density_layer(density_ids[2]);
    EXPECT_EQ(metal3.layer_name, "metal3");
    ASSERT_EQ(metal3.rects.size(), 1u);
    EXPECT_EQ(metal3.rects[0].ll.x, 200000);
    EXPECT_EQ(metal3.rects[0].ll.y, 200000);
    EXPECT_EQ(metal3.rects[0].ur.x, 800000);
    EXPECT_EQ(metal3.rects[0].ur.y, 800000);
    EXPECT_DOUBLE_EQ(metal3.values[0], 4.5);
}

TEST_F(LEFReaderCompleteFixture, CreatesOneLibraryAndOneDesignPerMacro)
{
    ASSERT_EQ(root.get_library_size(), 1u);
    EXPECT_TRUE(root.get_library_by_name("test_lib").valid());

    // 14 `MACRO ...` blocks in the fixture, each becomes a Design + Abstract.
    EXPECT_EQ(root.get_design_size(), 14u);
    EXPECT_EQ(root.get_abstract_size(), 14u);

    for (auto design_id : root.get_design_ids())
        EXPECT_TRUE(root.get_design_abstract(design_id).valid()) << root.get_design(design_id)->name;
}

TEST_F(LEFReaderCompleteFixture, ConvertsMacroSizeAndOriginToDbu)
{
    // MACRO INV has SIZE 67.2 BY 24, no ORIGIN, at DATABASE MICRONS 20000.
    DesignId design_id = root.get_design_by_name("INV");
    ASSERT_TRUE(design_id.valid());
    AbstractId abstract_id = root.get_design_abstract(design_id);
    const AbstractData *abstract = root.get_abstract(abstract_id);

    ASSERT_TRUE(abstract->size.has_value());
    EXPECT_EQ(abstract->size->x, 1344000);
    EXPECT_EQ(abstract->size->y, 480000);
    EXPECT_EQ(abstract->type, "CORE");

    // No OVERLAP obstruction on this macro, so the boundary falls back to a
    // single rect polygon built from origin (default {0,0}) + size.
    ASSERT_EQ(abstract->boundary.size(), 1u);
    const Polygon &boundary = abstract->boundary.front();
    ASSERT_EQ(boundary.points.size(), 5u);
    EXPECT_EQ(boundary.points[0].x, 0);
    EXPECT_EQ(boundary.points[0].y, 0);
    EXPECT_EQ(boundary.points[2].x, 1344000);
    EXPECT_EQ(boundary.points[2].y, 480000);
}

TEST_F(LEFReaderCompleteFixture, CreatesPinsWithPortShapes)
{
    // MACRO INV has 4 PINs: Z, A, VDD, VSS.
    DesignId design_id = root.get_design_by_name("INV");
    AbstractId abstract_id = root.get_design_abstract(design_id);
    EXPECT_EQ(root.get_abstract_terminals(abstract_id).size(), 4u);

    // PIN A DIRECTION INPUT; PORT LAYER M1 ; PATH 25.2 15 ; END
    TerminalId pin_a_id = find_terminal_id(root, abstract_id, "A");
    ASSERT_TRUE(pin_a_id.valid());
    EXPECT_EQ(root.get_terminal(pin_a_id)->direction, SignalDirection::INPUT);

    auto pin_a_ports = root.get_terminal_ports(pin_a_id);
    ASSERT_EQ(pin_a_ports.size(), 1u);
    const std::vector<ShapeId> &pin_a_shapes = root.get_terminal_port_shapes(pin_a_ports.front());

    ASSERT_EQ(pin_a_shapes.size(), 1u);
    const Shape &shape = *root.get_shape(pin_a_shapes.front());
    EXPECT_EQ(shape.layer_name, "M1");
    ASSERT_EQ(shape.paths.size(), 1u);
    EXPECT_EQ(shape.paths.front().width, 0u); // no WIDTH statement precedes this PATH
}

TEST_F(LEFReaderCompleteFixture, ObstructionCollectsRectsAndPathsButIgnoresVias)
{
    // MACRO INV's OBS block on LAYER M1: 1 RECT, a 2x1 RECT ITERATE, a 1x2
    // PATH ITERATE, 2 more PATHs, 4 VIAs (unsupported, must be ignored - not
    // counted as rects/paths/iterates), then a final RECT. Width 0.1um.
    // ITERATE statements are stored raw (UPDATES.md 12 Phase 1's ITERATE
    // rework), not pre-expanded - see rect_iterates/path_iterates below,
    // and Pipeline::generate_shapes for where they're expanded.
    DesignId design_id = root.get_design_by_name("INV");
    AbstractId abstract_id = root.get_design_abstract(design_id);

    ASSERT_EQ(root.get_abstract_obstructions(abstract_id).size(), 1u);
    const std::vector<ShapeId> &obstruction_shapes = root.get_obstruction_shapes(root.get_abstract_obstructions(abstract_id).front());

    ASSERT_EQ(obstruction_shapes.size(), 1u);
    const Shape &shape = *root.get_shape(obstruction_shapes.front());
    EXPECT_EQ(shape.layer_name, "M1");
    EXPECT_EQ(shape.rects.size(), 2u); // 1 + 1 final, VIAs excluded, ITERATE stored separately
    EXPECT_EQ(shape.paths.size(), 2u); // 2 singles, VIAs excluded, ITERATE stored separately
    EXPECT_EQ(shape.polygons.size(), 0u);

    ASSERT_EQ(shape.rect_iterates.size(), 1u);
    EXPECT_EQ(shape.rect_iterates.front().num_x, 2);
    EXPECT_EQ(shape.rect_iterates.front().num_y, 1);
    EXPECT_EQ(shape.rect_iterates.front().space_x, 400000); // 20.0um * 20000 dbu/um
    EXPECT_EQ(shape.rect_iterates.front().space_y, 0);

    ASSERT_EQ(shape.path_iterates.size(), 1u);
    EXPECT_EQ(shape.path_iterates.front().num_x, 1);
    EXPECT_EQ(shape.path_iterates.front().num_y, 2);
    EXPECT_EQ(shape.path_iterates.front().space_x, 0);
    EXPECT_EQ(shape.path_iterates.front().space_y, 28920000); // 1446um * 20000 dbu/um
    EXPECT_EQ(shape.path_iterates.front().path.width, 2000);  // WIDTH 0.1um * 20000 dbu/um

    for (const auto &path : shape.paths)
        EXPECT_EQ(path.width, 2000); // WIDTH 0.1um * 20000 dbu/um
}

// Two overlapping bugs made every FOREIGN past the first one silently lose
// its origin: (1) lef_reader.cpp called hasForeignOrigin()/hasForeignOrient()
// with no index, always checking foreign #0's flags regardless of which
// foreign was being processed; (2) the vendored parser's hasForeignOrigin_ is
// actually populated from the orientation code, not a real "has a point"
// flag (lefiMacro::addForeign) - hasForeignPoint(i) is the field that's
// genuinely wired to it. FOREIGNTEST's F0 (index 0, so bug #1 alone wouldn't
// expose it) has an explicit ORIENT N (code 0, falsy) alongside a real
// origin, which bug #2 alone was enough to misread as "no origin".
TEST(LEFReaderForeignIndex, EachForeignKeepsItsOwnOriginAndOrient)
{
    Root root;
    LEFReader reader;
    int result = reader.read_lef(fixture_path("foreign_index.lef"), root, "test_lib");
    ASSERT_EQ(result, 0);

    DesignId design_id = root.get_design_by_name("FOREIGNTEST");
    ASSERT_TRUE(design_id.valid());
    const AbstractId abstract_id = root.get_design_abstract(design_id);
    const AbstractData *abstract = root.get_abstract(abstract_id);
    ASSERT_TRUE(abstract != nullptr);

    const std::vector<ForeignId> &foreign_ids = root.get_abstract_foreigns(abstract_id);
    ASSERT_EQ(foreign_ids.size(), 4u);

    const ForeignData &f0 = *root.get_foreign(foreign_ids[0]); // FOREIGN F0 ( 1 2 ) N ;
    ASSERT_TRUE(f0.origin.has_value());
    EXPECT_EQ(f0.origin->x, 1000);
    EXPECT_EQ(f0.origin->y, 2000);
    ASSERT_TRUE(f0.orient.has_value());
    EXPECT_EQ(*f0.orient, Orientation::N);

    const ForeignData &f1 = *root.get_foreign(foreign_ids[1]); // FOREIGN F1 ( 3 4 ) ;
    ASSERT_TRUE(f1.origin.has_value());
    EXPECT_EQ(f1.origin->x, 3000);
    EXPECT_EQ(f1.origin->y, 4000);
    // UPDATES.md item 12: no orient specified in the LEF source means
    // genuinely unset now, not defaulted to N - a real ORIENT N and "no
    // ORIENT at all" must stay distinguishable.
    EXPECT_FALSE(f1.orient.has_value());

    const ForeignData &f2 = *root.get_foreign(foreign_ids[2]); // FOREIGN F2 ;
    // No point in the LEF source either - unset, not (0,0).
    EXPECT_FALSE(f2.origin.has_value());
    EXPECT_FALSE(f2.orient.has_value());

    const ForeignData &f3 = *root.get_foreign(foreign_ids[3]); // FOREIGN F3 ( 5 6 ) E ;
    ASSERT_TRUE(f3.origin.has_value());
    EXPECT_EQ(f3.origin->x, 5000);
    EXPECT_EQ(f3.origin->y, 6000);
    ASSERT_TRUE(f3.orient.has_value());
    EXPECT_EQ(*f3.orient, Orientation::E);
}

// src/lefdef/lef/TEST/complete.5.8.lef has no MACRO with an OBS on the
// OVERLAP layer, so post_process()'s union-of-OVERLAP-shapes boundary path
// (as opposed to the SIZE/ORIGIN fallback) was never exercised. This
// hand-written fixture covers it.
TEST(LEFReaderOverlapBoundary, BoundaryComesFromOverlapObsNotMacroSize)
{
    Root root;
    LEFReader reader;
    int result = reader.read_lef(fixture_path("overlap_boundary.lef"), root, "test_lib");
    ASSERT_EQ(result, 0);

    DesignId design_id = root.get_design_by_name("OVERLAPTEST");
    ASSERT_TRUE(design_id.valid());
    AbstractId abstract_id = root.get_design_abstract(design_id);
    const AbstractData *abstract = root.get_abstract(abstract_id);

    // SIZE 10 BY 10 at DATABASE MICRONS 1000 would give a (0,0)-(10000,10000)
    // fallback boundary; OBS has an OVERLAP RECT 1 1 9 9 (-> (1000,1000)-
    // (9000,9000) dbu) plus an M1 RECT 0 0 2 2 that must NOT be included.
    ASSERT_EQ(abstract->boundary.size(), 1u);
    const Polygon &boundary = abstract->boundary.front();
    ASSERT_GE(boundary.points.size(), 4u);

    int64_t min_x = boundary.points.front().x, max_x = min_x;
    int64_t min_y = boundary.points.front().y, max_y = min_y;
    for (const auto &point : boundary.points)
    {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }

    EXPECT_EQ(min_x, 1000);
    EXPECT_EQ(min_y, 1000);
    EXPECT_EQ(max_x, 9000);
    EXPECT_EQ(max_y, 9000);
}

TEST(LEFReaderErrors, FileNotFoundReturnsOne)
{
    Root root;
    LEFReader reader;
    EXPECT_EQ(reader.read_lef("/no/such/file.lef", root, "test_lib"), 1);
    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().front().find("/no/such/file.lef"), std::string::npos);
}

TEST(LEFReaderErrors, MalformedFileReturnsTwo)
{
    Root root;
    LEFReader reader;
    EXPECT_EQ(reader.read_lef(fixture_path("malformed.lef"), root, "test_lib"), 2);
    EXPECT_FALSE(reader.messages().empty());
}

// LEFReader::messages() - the queue api.cpp's le_read_lef drains into a
// persistent, ever-growing handle-owned list for the GUI (UPDATES.md
// item 3). A distinct suite from LEFReaderErrors above since these
// tests are about message *content*, not read_lef's own return code.
TEST(LEFReaderMessages, SuccessfulReadWithNoDiagnosticsLeavesMessagesEmpty)
{
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("units_1000.lef"), root, "test_lib"), 0);
    EXPECT_TRUE(reader.messages().empty());
}

// CURRENTDEN inside a LAYER block is unconditionally flagged obsolete by
// the vendored parser on any LEF version >= 5.2 (lef.tab.cpp's
// `layer_option: K_CURRENTDEN int_number ';'` rule calls
// lefWarning(2079, ...) regardless of layer type once past that version
// check) - a reliable way to exercise the warning path (routed through
// lefrSetWarningLogFunction, same registration lefInfo also uses)
// without the parse itself failing.
TEST(LEFReaderMessages, ObsoleteCurrentdenStatementProducesAWarningOnAnOtherwiseSuccessfulRead)
{
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("warning_currentden.lef"), root, "test_lib"), 0);
    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().front().find("WARNING"), std::string::npos);
    EXPECT_NE(reader.messages().front().find("obsolete"), std::string::npos);
}

// messages_ is cleared at the top of every read_lef() call (not
// accumulated across calls on a reused instance) - api.cpp's
// le_read_lef is what accumulates these into a persistent queue, not
// LEFReader itself.
TEST(LEFReaderMessages, MessagesAreClearedNotAccumulatedAcrossReusedReaderCalls)
{
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("malformed.lef"), root, "test_lib"), 2);
    ASSERT_FALSE(reader.messages().empty());

    Root root2;
    ASSERT_EQ(reader.read_lef(fixture_path("units_1000.lef"), root2, "test_lib"), 0);
    EXPECT_TRUE(reader.messages().empty());
}

// shapes_from_parser() has an `if (!shape.has_value())` guard before every
// RECT/RECT-ITERATE/PATH/PATH-ITERATE/POLYGON/POLYGON-ITERATE case, in case
// one appears before any LAYER. This confirms the grammar itself already
// rejects that (a parse failure, before our callback ever runs) - so those
// guards are unreachable in practice, but kept anyway as defense-in-depth
// at a file-parsing boundary rather than removed like pure-internal dead
// code (see geometry.hpp's union_shapes/get_label_location cleanup).
TEST(LEFReaderErrors, GeometryWithoutPrecedingLayerFailsAtTheGrammarLevel)
{
    Root root;
    LEFReader reader;
    EXPECT_EQ(reader.read_lef(fixture_path("rect_without_layer.lef"), root, "test_lib"), 2);
}

TEST(LEFReaderErrors, DuplicateLayerNameIsIgnored)
{
    // Second `LAYER M1` (DIRECTION VERTICAL) must be dropped, keeping the
    // first (DIRECTION HORIZONTAL) - not overwritten, not duplicated.
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("duplicate_layer.lef"), root, "test_lib"), 0);

    EXPECT_EQ(root.get_layer_ids().size(), 1u);
    LayerId m1_id = root.get_layer_by_name("M1");
    ASSERT_TRUE(m1_id.valid());
    EXPECT_EQ(root.get_layer(m1_id)->direction, RoutingDirection::H);

    // log_warning (UPDATES.md item 3) - internal diagnostics, not just
    // the vendored parser's own, reach messages() too.
    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().front().find("WARNING"), std::string::npos);
    EXPECT_NE(reader.messages().front().find("M1"), std::string::npos);
}

TEST(LEFReaderErrors, PinWithoutDirectionDoesNotInheritThePreviousPinsDirection)
{
    // Regression: the vendored parser reuses one scratch lefiPin across
    // every PIN statement and never resets direction_ between them
    // (same hazard as lefiLayer's direction_) - PIN B declares no
    // DIRECTION at all and must read back NONE, not silently inherit
    // PIN A's DIRECTION INPUT.
    Root root;
    LEFReader reader;
    ASSERT_EQ(reader.read_lef(fixture_path("pin_without_direction.lef"), root, "test_lib"), 0);

    ASSERT_EQ(root.get_design_size(), 1u);
    AbstractId abstract_id = root.get_design_abstract(root.get_design_ids().front());

    TerminalId pin_a_id = find_terminal_id(root, abstract_id, "A");
    ASSERT_TRUE(pin_a_id.valid());
    EXPECT_EQ(root.get_terminal(pin_a_id)->direction, SignalDirection::INPUT);

    TerminalId pin_b_id = find_terminal_id(root, abstract_id, "B");
    ASSERT_TRUE(pin_b_id.valid());
    EXPECT_EQ(root.get_terminal(pin_b_id)->direction, SignalDirection::NONE);
}

TEST(LEFReaderErrors, SecondReadWithDifferentUnitsIsIgnored)
{
    // Reading a second LEF into the same Root with a different DATABASE
    // MICRONS must warn and keep the technology's original value.
    Root root;
    LEFReader reader1, reader2;
    ASSERT_EQ(reader1.read_lef(fixture_path("units_1000.lef"), root, "test_lib"), 0);
    ASSERT_EQ(reader2.read_lef(fixture_path("units_2000.lef"), root, "test_lib"), 0);

    ASSERT_EQ(root.get_technology_size(), 1u);
    EXPECT_DOUBLE_EQ(root.get_technology(root.get_technology_ids().front())->database_units_microns, 1000.0);

    // The warning fires on the second read (reader2) - log_warning
    // reaches messages() there, not on the first, unaffected read.
    ASSERT_FALSE(reader2.messages().empty());
    EXPECT_NE(reader2.messages().front().find("WARNING"), std::string::npos);
    EXPECT_TRUE(reader1.messages().empty());
}

TEST(LEFReaderErrors, GeometryBeforeDatabaseMicronsEverDeclaredIsAnError)
{
    // Every microns_to_dbu()/microns_squared_to_dbu() call silently
    // multiplies by 0 (the "unset" sentinel database_units_microns
    // defaults to) if DATABASE MICRONS was never declared - previously
    // undetected, producing silently-wrong all-zero coordinates. Caught
    // once at the end of read_lef (not mid-parse) via
    // used_dbu_before_units_declared_.
    Root root;
    LEFReader reader;
    const int result = reader.read_lef(fixture_path("no_units_with_geometry.lef"), root, "test_lib");
    EXPECT_EQ(result, 3);

    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().back().find("ERROR"), std::string::npos);
    EXPECT_NE(reader.messages().back().find("DATABASE MICRONS"), std::string::npos);
}

TEST(LEFReaderErrors, VersionBelow5_4IsAnError)
{
    // UPDATES.md item 12: this project only supports LEF >= 5.4 - several
    // PIN-level statements (POWER/LEAKAGE/CAPACITANCE/etc., not modeled at
    // all - see LEFDEF_BUGS.md's "Reader-side: intentional version-
    // obsolescence") are silently discarded by the vendored reader itself
    // at >= 5.4, so accepting an older file would mean this project's own
    // database silently disagrees with what a real pre-5.4-reading tool
    // would see for those fields. Caught once at the end of read_lef (not
    // mid-parse), same convention as used_dbu_before_units_declared_.
    Root root;
    LEFReader reader;
    const int result = reader.read_lef(fixture_path("version_5_3.lef"), root, "test_lib");
    EXPECT_EQ(result, 4);

    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().back().find("ERROR"), std::string::npos);
    EXPECT_NE(reader.messages().back().find("VERSION"), std::string::npos);
}

TEST(LEFReaderErrors, GeometryAfterAnEarlierReadAlreadyDeclaredDatabaseMicronsIsFine)
{
    // The same no-UNITS macro file, but read into a Root that already has
    // DATABASE MICRONS from an earlier read (the realistic "tech file
    // first, macro file second" pattern) - not an error, since
    // database_units_microns is no longer the unset sentinel by the time
    // this file's own geometry gets converted.
    Root root;
    LEFReader tech_reader;
    ASSERT_EQ(tech_reader.read_lef(fixture_path("units_1000.lef"), root, "test_lib"), 0);

    LEFReader macro_reader;
    EXPECT_EQ(macro_reader.read_lef(fixture_path("no_units_with_geometry.lef"), root, "test_lib"), 0);
}

TEST(LEFReaderErrors, DuplicateMacroNameIsRejected)
{
    // The second `MACRO DUPTEST` must be rejected (an abstract already
    // exists for that design name); only the first's Design/Abstract stick.
    Root root;
    LEFReader reader;
    EXPECT_EQ(reader.read_lef(fixture_path("duplicate_macro.lef"), root, "test_lib"), 2);
    EXPECT_EQ(root.get_design_ids().size(), 1u);

    // log_error (UPDATES.md item 3) - this failure comes from our own
    // lefrMacroBeginCbkFn, not the vendored parser's own log callback,
    // and still reaches messages().
    ASSERT_FALSE(reader.messages().empty());
    EXPECT_NE(reader.messages().front().find("ERROR"), std::string::npos);
    EXPECT_NE(reader.messages().front().find("DUPTEST"), std::string::npos);
}

// UPDATES.md 12 Phase 2 (VIA/VIARULE) - writer_roundtrip.lef (shared with
// lef_writer_test.cpp) exercises a plain VIA (rects on M1/V1 + RESISTANCE),
// a GENERATE VIARULE (2 M1 layers + a V1 cut layer with RECT/SPACING/
// RESISTANCE), a non-GENERATE VIARULE (2 M1 layers + a VIA name list), and
// a VIA referencing that VIARULE via the 5.6 VIARULE-inside-VIA syntax
// (CUTSIZE/LAYERS/CUTSPACING/ENCLOSURE). DATABASE MICRONS 1000 throughout.
class LEFReaderViaFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_EQ(reader.read_lef(fixture_path("writer_roundtrip.lef"), root, "test_lib"), 0);
    }

    Root root;
    LEFReader reader;
};

TEST_F(LEFReaderViaFixture, ReadsAPlainViaWithLayerGeometryAndResistance)
{
    const ViaId via_id = root.get_via_by_name("VIA1");
    ASSERT_TRUE(via_id.valid());
    const ViaData *via = root.get_via(via_id);
    ASSERT_TRUE(via != nullptr);

    EXPECT_FALSE(via->is_default);
    ASSERT_TRUE(via->resistance.has_value());
    EXPECT_DOUBLE_EQ(*via->resistance, 1.5);
    EXPECT_EQ(root.get_foreign(root.get_via_foreign(via_id)), nullptr);
    EXPECT_EQ(root.get_via_rule_reference(root.get_via_via_rule(via_id)), nullptr);

    const std::vector<ViaLayerId> &via_layer_ids = root.get_via_layers(via_id);
    ASSERT_EQ(via_layer_ids.size(), 2u);
    const ViaLayerData &via_layer0 = *root.get_via_layer(via_layer_ids[0]);
    EXPECT_EQ(via_layer0.layer_name, "M1");
    ASSERT_EQ(via_layer0.rects.size(), 1u);
    EXPECT_EQ(via_layer0.rects[0].ll.x, -1000);
    EXPECT_EQ(via_layer0.rects[0].ll.y, -1000);
    EXPECT_EQ(via_layer0.rects[0].ur.x, 1000);
    EXPECT_EQ(via_layer0.rects[0].ur.y, 1000);

    const ViaLayerData &via_layer1 = *root.get_via_layer(via_layer_ids[1]);
    EXPECT_EQ(via_layer1.layer_name, "V1");
    ASSERT_EQ(via_layer1.rects.size(), 1u);
    EXPECT_EQ(via_layer1.rects[0].ll.x, -500);
    EXPECT_EQ(via_layer1.rects[0].ll.y, -500);
    EXPECT_EQ(via_layer1.rects[0].ur.x, 500);
    EXPECT_EQ(via_layer1.rects[0].ur.y, 500);
}

TEST_F(LEFReaderViaFixture, ReadsAGenerateViaRuleWithDirectionsWidthsAndACutLayer)
{
    const ViaRuleId via_rule_id = root.get_via_rule_by_name("VIARULE1");
    ASSERT_TRUE(via_rule_id.valid());
    const ViaRuleData *via_rule = root.get_via_rule(via_rule_id);
    ASSERT_TRUE(via_rule != nullptr);

    EXPECT_TRUE(via_rule->is_generate);
    EXPECT_FALSE(via_rule->is_default);
    EXPECT_TRUE(via_rule->via_names.empty()); // GENERATE - no VIA name list

    const std::vector<ViaRuleLayerId> &via_rule_layer_ids = root.get_via_rule_layers(via_rule_id);
    ASSERT_EQ(via_rule_layer_ids.size(), 3u);

    // DIRECTION and OVERHANG/METALOVERHANG are obsolete for VIARULE
    // GENERATE at LEF >= 5.6 (the vendored parser silently ignores
    // DIRECTION - see lef.y's K_DIRECTION rule under viarule_layer_option
    // - and translates OVERHANG/METALOVERHANG into ENCLOSURE instead of
    // setOverhang()/setMetalOverhang() - see the K_OVERHANG rule's own
    // "In 5.6 & later, set it to either ENCLOSURE overhang1 or overhang2"
    // comment), so the fixture uses ENCLOSURE directly and direction is
    // never set for a GENERATE rule's layers.
    const ViaRuleLayerData &m1_horizontal = *root.get_via_rule_layer(via_rule_layer_ids[0]);
    EXPECT_EQ(m1_horizontal.layer_name, "M1");
    EXPECT_EQ(m1_horizontal.direction, RoutingDirection::NONE);
    ASSERT_TRUE(m1_horizontal.width_min.has_value());
    ASSERT_TRUE(m1_horizontal.width_max.has_value());
    EXPECT_EQ(*m1_horizontal.width_min, 100);
    EXPECT_EQ(*m1_horizontal.width_max, 1900);
    EXPECT_FALSE(m1_horizontal.overhang.has_value());
    EXPECT_FALSE(m1_horizontal.metal_overhang.has_value());
    ASSERT_TRUE(m1_horizontal.enclosure_overhang1.has_value());
    ASSERT_TRUE(m1_horizontal.enclosure_overhang2.has_value());
    EXPECT_EQ(*m1_horizontal.enclosure_overhang1, 100);
    EXPECT_EQ(*m1_horizontal.enclosure_overhang2, 150);

    const ViaRuleLayerData &m1_vertical = *root.get_via_rule_layer(via_rule_layer_ids[1]);
    EXPECT_EQ(m1_vertical.layer_name, "M1");
    EXPECT_EQ(m1_vertical.direction, RoutingDirection::NONE);

    const ViaRuleLayerData &cut = *root.get_via_rule_layer(via_rule_layer_ids[2]);
    EXPECT_EQ(cut.layer_name, "V1");
    ASSERT_TRUE(cut.rect.has_value());
    EXPECT_EQ(cut.rect->ll.x, -100);
    EXPECT_EQ(cut.rect->ll.y, -100);
    EXPECT_EQ(cut.rect->ur.x, 100);
    EXPECT_EQ(cut.rect->ur.y, 100);
    ASSERT_TRUE(cut.spacing_step_x.has_value());
    ASSERT_TRUE(cut.spacing_step_y.has_value());
    EXPECT_EQ(*cut.spacing_step_x, 500);
    EXPECT_EQ(*cut.spacing_step_y, 500);
    ASSERT_TRUE(cut.resistance.has_value());
    EXPECT_DOUBLE_EQ(*cut.resistance, 0.2);
}

// VIARULE2/VIA2 (a non-GENERATE VIARULE, and a VIA referencing one) live in
// their own fixture file/class rather than writer_roundtrip.lef, since they
// can't round-trip through LEFWriter: lefwViaRuleLayer (the non-GENERATE
// writer call) rejects any non-null DIRECTION with LEFW_OBSOLETE for
// VERSION >= 5.6 (see lefwWriter.cpp's shared lefwViaRulePrtLayer helper),
// but unlike the GENERATE path (lefwViaRuleGenLayerEnclosure) has no
// ENCLOSURE-based alternative - so a non-GENERATE VIARULE's first two
// layers can never satisfy the reader's own "requires DIRECTION or
// ENCLOSURE" check (lef.y's viarule_layer rule) once re-read, at any
// version >= 5.6. A real gap in the vendored 6.0.62-p004 writer/reader
// pair, not something fixable on our side without hand-editing vendored
// code - reader-only coverage here, kept out of the writer round-trip
// fixture used by LEFWriterRoundtripFixture.
class LEFReaderViaRuleReferenceFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_EQ(reader.read_lef(fixture_path("via_rule_reference.lef"), root, "test_lib"), 0);
    }

    Root root;
    LEFReader reader;
};

TEST_F(LEFReaderViaRuleReferenceFixture, ReadsANonGenerateViaRuleWithAViaNameList)
{
    const ViaRuleId via_rule_id = root.get_via_rule_by_name("VIARULE2");
    ASSERT_TRUE(via_rule_id.valid());
    const ViaRuleData *via_rule = root.get_via_rule(via_rule_id);
    ASSERT_TRUE(via_rule != nullptr);

    EXPECT_FALSE(via_rule->is_generate);
    const std::vector<ViaRuleLayerId> &via_rule_layer_ids = root.get_via_rule_layers(via_rule_id);
    ASSERT_EQ(via_rule_layer_ids.size(), 2u);
    EXPECT_EQ(root.get_via_rule_layer(via_rule_layer_ids[0])->direction, RoutingDirection::H);
    EXPECT_EQ(root.get_via_rule_layer(via_rule_layer_ids[1])->direction, RoutingDirection::V);
    // Neither layer sets OVERHANG/METALOVERHANG in this VIARULE.
    EXPECT_FALSE(root.get_via_rule_layer(via_rule_layer_ids[0])->overhang.has_value());
    EXPECT_FALSE(root.get_via_rule_layer(via_rule_layer_ids[0])->metal_overhang.has_value());

    ASSERT_EQ(via_rule->via_names.size(), 1u);
    EXPECT_EQ(via_rule->via_names[0], "VIA1");
}

TEST_F(LEFReaderViaRuleReferenceFixture, ReadsAViaReferencingAViaRuleWithCutGeometry)
{
    const ViaId via_id = root.get_via_by_name("VIA2");
    ASSERT_TRUE(via_id.valid());
    const ViaData *via = root.get_via(via_id);
    ASSERT_TRUE(via != nullptr);

    EXPECT_FALSE(via->resistance.has_value()); // mutually exclusive with via_rule
    const ViaRuleReferenceData *vr_ptr = root.get_via_rule_reference(root.get_via_via_rule(via_id));
    ASSERT_TRUE(vr_ptr != nullptr);

    const ViaRuleReferenceData &vr = *vr_ptr;
    EXPECT_EQ(vr.via_rule_name, "VIARULE2");
    ASSERT_TRUE(vr.cut_size.has_value());
    EXPECT_EQ(vr.cut_size->x, 100);
    EXPECT_EQ(vr.cut_size->y, 100);
    EXPECT_EQ(vr.bot_layer_name, "M1");
    EXPECT_EQ(vr.cut_layer_name, "V1");
    EXPECT_EQ(vr.top_layer_name, "M1");
    ASSERT_TRUE(vr.cut_spacing.has_value());
    EXPECT_EQ(vr.cut_spacing->x, 100);
    EXPECT_EQ(vr.cut_spacing->y, 100);
    // ENCLOSURE 0.05 0.01 0.01 0.05 -> xBotEnc yBotEnc xTopEnc yTopEnc
    // (confirmed via lef.y's via_viarule grammar rule's own positional
    // setViaRule(...) call).
    ASSERT_TRUE(vr.bot_enclosure.has_value());
    EXPECT_EQ(vr.bot_enclosure->x, 50);
    EXPECT_EQ(vr.bot_enclosure->y, 10);
    ASSERT_TRUE(vr.top_enclosure.has_value());
    EXPECT_EQ(vr.top_enclosure->x, 10);
    EXPECT_EQ(vr.top_enclosure->y, 50);
}

TEST_F(LEFReaderViaFixture, ReadsSiteDefinitionsWithClassSizeSymmetryAndRowPattern)
{
    const SiteId core_id = root.get_site_by_name("CORE");
    ASSERT_TRUE(core_id.valid());
    const SiteData *core = root.get_site(core_id);
    ASSERT_TRUE(core != nullptr);

    EXPECT_EQ(core->site_class, "CORE");
    ASSERT_TRUE(core->size.has_value());
    EXPECT_EQ(core->size->x, 200);
    EXPECT_EQ(core->size->y, 2000);
    ASSERT_TRUE(core->symmetry.has_value());
    EXPECT_FALSE(core->symmetry->x);
    EXPECT_TRUE(core->symmetry->y);
    EXPECT_FALSE(core->symmetry->r90);
    EXPECT_TRUE(core->row_pattern.empty());

    const SiteId dblcore_id = root.get_site_by_name("DBLCORE");
    ASSERT_TRUE(dblcore_id.valid());
    const SiteData *dblcore = root.get_site(dblcore_id);
    ASSERT_TRUE(dblcore != nullptr);

    ASSERT_TRUE(dblcore->symmetry.has_value());
    EXPECT_TRUE(dblcore->symmetry->x);
    EXPECT_TRUE(dblcore->symmetry->y);
    ASSERT_EQ(dblcore->row_pattern.size(), 2u);
    EXPECT_EQ(dblcore->row_pattern[0].site_name, "CORE");
    EXPECT_EQ(dblcore->row_pattern[0].orient, Orientation::N);
    EXPECT_EQ(dblcore->row_pattern[1].site_name, "CORE");
    EXPECT_EQ(dblcore->row_pattern[1].orient, Orientation::FS);
}

TEST_F(LEFReaderViaFixture, ReadsANonDefaultRuleWithHardspacingLayerOverridesAnEmbeddedViaAndUseStatements)
{
    const NonDefaultRuleId rule_id = root.get_non_default_rule_by_name("WIDE_M1");
    ASSERT_TRUE(rule_id.valid());
    const NonDefaultRuleData *rule = root.get_non_default_rule(rule_id);
    ASSERT_TRUE(rule != nullptr);

    EXPECT_TRUE(rule->hard_spacing);

    const std::vector<NonDefaultRuleLayerId> &nd_layer_ids = root.get_non_default_rule_layers(rule_id);
    ASSERT_EQ(nd_layer_ids.size(), 1u);
    const NonDefaultRuleLayerData &layer = *root.get_non_default_rule_layer(nd_layer_ids[0]);
    EXPECT_EQ(layer.layer_name, "M1");
    ASSERT_TRUE(layer.width.has_value());
    EXPECT_EQ(*layer.width, 300);
    ASSERT_TRUE(layer.spacing.has_value());
    EXPECT_EQ(*layer.spacing, 250);
    ASSERT_TRUE(layer.wire_extension.has_value());
    EXPECT_EQ(*layer.wire_extension, 100);
    // RESISTANCE/CAPACITANCE/EDGECAPACITANCE on a NONDEFAULTRULE LAYER are
    // obsolete for VERSION >= 5.6 (lef.y's own nd_layer_stmt rule: "obsolete
    // in version 5.6 and later... will ignore this statement") - the
    // fixture (VERSION 5.8) deliberately doesn't set one.
    EXPECT_FALSE(layer.resistance.has_value());

    const std::vector<NonDefaultRuleViaId> &nd_via_ids = root.get_non_default_rule_vias(rule_id);
    ASSERT_EQ(nd_via_ids.size(), 1u);
    const NonDefaultRuleViaId via_id = nd_via_ids[0];
    const NonDefaultRuleViaData &via = *root.get_non_default_rule_via(via_id);
    EXPECT_EQ(via.name, "ND_VIA1");
    ASSERT_TRUE(via.resistance.has_value());
    EXPECT_DOUBLE_EQ(*via.resistance, 0.3);
    const std::vector<ViaLayerId> &nd_via_layer_ids = root.get_non_default_rule_via_layers(via_id);
    ASSERT_EQ(nd_via_layer_ids.size(), 2u);
    EXPECT_EQ(root.get_via_layer(nd_via_layer_ids[0])->layer_name, "M1");
    EXPECT_EQ(root.get_via_layer(nd_via_layer_ids[1])->layer_name, "V1");
    // UPDATES.md 12 Phase 7 - PROPERTY on a NONDEFAULTRULE-embedded VIA
    // (same LefProperty mechanism as the already-working top-level Via).
    ASSERT_EQ(via.properties.size(), 1u);
    EXPECT_EQ(via.properties[0].name, "vip");
    EXPECT_TRUE(via.properties[0].is_number);
    EXPECT_DOUBLE_EQ(via.properties[0].number_value, 7.0);

    ASSERT_EQ(rule->use_via_names.size(), 1u);
    EXPECT_EQ(rule->use_via_names[0], "VIA1");

    ASSERT_EQ(rule->min_cuts.size(), 1u);
    EXPECT_EQ(rule->min_cuts[0].cut_layer_name, "V1");
    EXPECT_EQ(rule->min_cuts[0].num_cuts, 2);
}

TEST_F(LEFReaderViaFixture, ReadsPropertyDefinitionsAndPerConstructPropertyAttachments)
{
    const TechnologyId technology_id = root.get_technology_ids().front();
    const TechnologyData *technology = root.get_technology(technology_id);
    ASSERT_TRUE(technology != nullptr);

    const std::vector<PropertyDefinitionId> &property_definition_ids = root.get_technology_property_definitions(technology_id);
    ASSERT_EQ(property_definition_ids.size(), 6u);
    const PropertyDefinitionData &lip_def = *root.get_property_definition(property_definition_ids[0]);
    // lefiProp::propType() (see lef.y's PROPERTYDEFINITIONS grammar) is
    // lowercase ("layer"/"via"/...), unlike the LEF keyword itself.
    EXPECT_EQ(lip_def.owner_type, "layer");
    EXPECT_EQ(lip_def.name, "lip");
    EXPECT_EQ(lip_def.data_type, "I");
    EXPECT_FALSE(lip_def.range_min.has_value());

    const PropertyDefinitionData &vip_def = *root.get_property_definition(property_definition_ids[3]);
    EXPECT_EQ(vip_def.owner_type, "via");
    EXPECT_EQ(vip_def.name, "vip");
    ASSERT_TRUE(vip_def.range_min.has_value());
    ASSERT_TRUE(vip_def.range_max.has_value());
    EXPECT_DOUBLE_EQ(*vip_def.range_min, 0.0);
    EXPECT_DOUBLE_EQ(*vip_def.range_max, 100.0);

    const LayerData *m1 = root.get_layer(root.get_layer_by_name("M1"));
    ASSERT_TRUE(m1 != nullptr);
    ASSERT_EQ(m1->properties.size(), 1u);
    EXPECT_EQ(m1->properties[0].name, "lsp");
    EXPECT_FALSE(m1->properties[0].is_number);
    EXPECT_EQ(m1->properties[0].string_value, "top");

    const LayerData *v1 = root.get_layer(root.get_layer_by_name("V1"));
    ASSERT_TRUE(v1 != nullptr);
    ASSERT_EQ(v1->properties.size(), 2u);
    EXPECT_EQ(v1->properties[0].name, "lip");
    EXPECT_TRUE(v1->properties[0].is_number);
    EXPECT_DOUBLE_EQ(v1->properties[0].number_value, 5.0);
    EXPECT_EQ(v1->properties[1].name, "lrp");
    EXPECT_DOUBLE_EQ(v1->properties[1].number_value, 2.3);

    const ViaData *via1 = root.get_via(root.get_via_by_name("VIA1"));
    ASSERT_TRUE(via1 != nullptr);
    ASSERT_EQ(via1->properties.size(), 1u);
    EXPECT_EQ(via1->properties[0].name, "vip");
    EXPECT_TRUE(via1->properties[0].is_number);
    EXPECT_DOUBLE_EQ(via1->properties[0].number_value, 42.0);

    const AbstractId abstract_id = root.get_design_abstract(root.get_design_by_name("WRITERTEST"));
    const AbstractData *abstract = root.get_abstract(abstract_id);
    ASSERT_TRUE(abstract != nullptr);
    ASSERT_EQ(abstract->properties.size(), 1u);
    EXPECT_EQ(abstract->properties[0].name, "mrp");
    EXPECT_DOUBLE_EQ(abstract->properties[0].number_value, 1.5);

    ASSERT_EQ(root.get_abstract_terminals(abstract_id).size(), 1u);
    const TerminalData *pin_a = root.get_terminal(root.get_abstract_terminals(abstract_id).front());
    ASSERT_TRUE(pin_a != nullptr);
    ASSERT_EQ(pin_a->properties.size(), 1u);
    EXPECT_EQ(pin_a->properties[0].name, "psp");
    EXPECT_FALSE(pin_a->properties[0].is_number);
    EXPECT_EQ(pin_a->properties[0].string_value, "signal");
}

// UPDATES.md 12 Phase 5 (antenna modeling) - a dedicated fixture file
// rather than reusing writer_roundtrip.lef: this vendored parser's
// use5_3/use5_4 flags (see lef.y's VERSION rule) are set once for the
// WHOLE FILE, not per-LAYER/PIN - M1's own ANTENNALENGTHFACTOR (5.3
// syntax, Phase 1) would make ANY 5.4+ ANTENNAMODEL/ANTENNAAREARATIO/etc.
// anywhere else in that same file a hard parse error ("has both old and
// new ANTENNAMODEL syntax"), confirmed by direct reader.messages()
// inspection when this was first attempted against writer_roundtrip.lef.
// Phase 6 (LAYER field completeness) reuses this same fixture/class for
// the same reason - its own new fields (MASK, two-value PITCH/OFFSET/
// DIAGPITCH, MINSIZE, TWOWIDTHS, AC/DC CURRENTDENSITY, CUT-layer
// ENCLOSURE/ARRAYSPACING/PREFERENCLOSURE, ...) have no such syntax
// conflict with writer_roundtrip.lef, but keeping everything in one
// dedicated non-legacy-syntax fixture avoids re-litigating that per field.
class LEFAntennaFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_EQ(reader.read_lef(fixture_path("antenna_roundtrip.lef"), root, "test_lib"), 0);
    }

    Root root;
    LEFReader reader;
};

TEST_F(LEFAntennaFixture, ReadsAntennaModelsOnRoutingAndCutLayersAndOnAPin)
{
    // Antenna ratios/areas are unitless or declared-unit values, not
    // geometric coordinates - no microns_to_dbu conversion, matching
    // resistance/capacitance's own treatment.
    const LayerId m1_id = root.get_layer_by_name("M1");
    const LayerData *m1 = root.get_layer(m1_id);
    ASSERT_TRUE(m1 != nullptr);
    const std::vector<AntennaModelId> &m1_antenna_model_ids = root.get_layer_antenna_models(m1_id);
    ASSERT_EQ(m1_antenna_model_ids.size(), 1u);
    const AntennaModelData &m1_model = *root.get_antenna_model(m1_antenna_model_ids[0]);
    EXPECT_EQ(m1_model.oxide, "OXIDE1");
    ASSERT_TRUE(m1_model.area_ratio.has_value());
    EXPECT_DOUBLE_EQ(*m1_model.area_ratio, 100.0);
    EXPECT_FALSE(m1_model.diff_area_ratio.has_value());
    ASSERT_EQ(m1_model.diff_area_ratio_pwl.size(), 2u);
    EXPECT_DOUBLE_EQ(m1_model.diff_area_ratio_pwl[0].diffusion, 0.1);
    EXPECT_DOUBLE_EQ(m1_model.diff_area_ratio_pwl[0].ratio, 1.0);
    EXPECT_DOUBLE_EQ(m1_model.diff_area_ratio_pwl[1].diffusion, 0.2);
    EXPECT_DOUBLE_EQ(m1_model.diff_area_ratio_pwl[1].ratio, 1.5);
    ASSERT_TRUE(m1_model.side_area_ratio.has_value());
    EXPECT_DOUBLE_EQ(*m1_model.side_area_ratio, 50.0);

    const LayerId v1_id = root.get_layer_by_name("V1");
    const LayerData *v1 = root.get_layer(v1_id);
    ASSERT_TRUE(v1 != nullptr);
    const std::vector<AntennaModelId> &v1_antenna_model_ids = root.get_layer_antenna_models(v1_id);
    ASSERT_EQ(v1_antenna_model_ids.size(), 1u);
    const AntennaModelData *v1_model = root.get_antenna_model(v1_antenna_model_ids[0]);
    EXPECT_EQ(v1_model->oxide, "OXIDE1");
    ASSERT_TRUE(v1_model->area_ratio.has_value());
    EXPECT_DOUBLE_EQ(*v1_model->area_ratio, 80.0);

    const AbstractId abstract_id = root.get_design_abstract(root.get_design_by_name("ANTENNATEST"));
    const TerminalId pin_a_id = root.get_abstract_terminals(abstract_id).front();
    const TerminalData *pin_a = root.get_terminal(pin_a_id);
    ASSERT_TRUE(pin_a != nullptr);
    ASSERT_EQ(pin_a->antenna_partial_metal_area.size(), 1u);
    EXPECT_DOUBLE_EQ(pin_a->antenna_partial_metal_area[0].value, 1.5);
    EXPECT_EQ(pin_a->antenna_partial_metal_area[0].layer_name, "M1");

    const std::vector<PinAntennaModelId> &pin_a_antenna_model_ids = root.get_terminal_antenna_models(pin_a_id);
    ASSERT_EQ(pin_a_antenna_model_ids.size(), 1u);
    const PinAntennaModelData *pin_a_model = root.get_pin_antenna_model(pin_a_antenna_model_ids[0]);
    EXPECT_EQ(pin_a_model->oxide, "OXIDE1");
    ASSERT_EQ(pin_a_model->gate_area.size(), 1u);
    EXPECT_DOUBLE_EQ(pin_a_model->gate_area[0].value, 2.0);
    EXPECT_EQ(pin_a_model->gate_area[0].layer_name, "M1");
}

TEST_F(LEFAntennaFixture, ReadsRoutingLayerMiscScalarAndTableFieldsAddedInPhase6)
{
    // Lengths/areas go through microns_to_dbu like every other geometric
    // LAYER field; MINIMUMDENSITY/MAXIMUMDENSITY are percentages (no
    // conversion); AC CURRENTDENSITY's frequency/table_entries are
    // declared-unit values (no conversion) but width IS geometric.
    const LayerId m1_id = root.get_layer_by_name("M1");
    const LayerData *m1 = root.get_layer(m1_id);
    ASSERT_TRUE(m1 != nullptr);

    EXPECT_EQ(m1->direction, RoutingDirection::DIAG45);
    ASSERT_TRUE(m1->default_mask.has_value());
    EXPECT_EQ(*m1->default_mask, 2);

    ASSERT_TRUE(m1->pitch_xy.has_value());
    EXPECT_EQ(m1->pitch_xy->x, 400);
    EXPECT_EQ(m1->pitch_xy->y, 500);
    EXPECT_FALSE(m1->pitch.has_value());
    ASSERT_TRUE(m1->offset_xy.has_value());
    EXPECT_EQ(m1->offset_xy->x, 50);
    EXPECT_EQ(m1->offset_xy->y, 60);

    ASSERT_TRUE(m1->diag_pitch.has_value());
    EXPECT_EQ(*m1->diag_pitch, 300);
    ASSERT_TRUE(m1->diag_spacing.has_value());
    EXPECT_EQ(*m1->diag_spacing, 150);
    ASSERT_TRUE(m1->diag_width.has_value());
    EXPECT_EQ(*m1->diag_width, 100);
    ASSERT_TRUE(m1->diag_min_edge_length.has_value());
    EXPECT_EQ(*m1->diag_min_edge_length, 80);

    ASSERT_TRUE(m1->max_width.has_value());
    EXPECT_EQ(*m1->max_width, 2000);
    ASSERT_TRUE(m1->min_width.has_value());
    EXPECT_EQ(*m1->min_width, 100);

    ASSERT_EQ(m1->min_sizes.size(), 2u);
    EXPECT_EQ(m1->min_sizes[0].width, 100);
    EXPECT_EQ(m1->min_sizes[0].length, 200);
    EXPECT_EQ(m1->min_sizes[1].width, 300);
    EXPECT_EQ(m1->min_sizes[1].length, 400);

    ASSERT_EQ(m1->min_enclosed_areas.size(), 2u);
    EXPECT_EQ(m1->min_enclosed_areas[0].area, 400000);
    ASSERT_TRUE(m1->min_enclosed_areas[0].width.has_value());
    EXPECT_EQ(*m1->min_enclosed_areas[0].width, 150);
    EXPECT_EQ(m1->min_enclosed_areas[1].area, 800000);
    EXPECT_FALSE(m1->min_enclosed_areas[1].width.has_value());

    ASSERT_TRUE(m1->protrusion_width1.has_value());
    EXPECT_EQ(*m1->protrusion_width1, 300);
    ASSERT_TRUE(m1->protrusion_length.has_value());
    EXPECT_EQ(*m1->protrusion_length, 600);
    ASSERT_TRUE(m1->protrusion_width2.has_value());
    EXPECT_EQ(*m1->protrusion_width2, 1200);

    const std::vector<TwoWidthsSpacingEntryId> &two_widths_ids = root.get_layer_spacing_table_two_widths(m1_id);
    ASSERT_EQ(two_widths_ids.size(), 2u);
    const TwoWidthsSpacingEntryData &two_widths0 = *root.get_two_widths_spacing_entry(two_widths_ids[0]);
    EXPECT_EQ(two_widths0.width, 100);
    EXPECT_FALSE(two_widths0.prl.has_value());
    ASSERT_EQ(two_widths0.spacings.size(), 2u);
    EXPECT_EQ(two_widths0.spacings[0], 200);
    EXPECT_EQ(two_widths0.spacings[1], 300);
    const TwoWidthsSpacingEntryData &two_widths1 = *root.get_two_widths_spacing_entry(two_widths_ids[1]);
    EXPECT_EQ(two_widths1.width, 200);
    ASSERT_TRUE(two_widths1.prl.has_value());
    EXPECT_EQ(*two_widths1.prl, 150);

    ASSERT_TRUE(m1->split_wire_width.has_value());
    EXPECT_EQ(*m1->split_wire_width, 500);
    ASSERT_TRUE(m1->minimum_density.has_value());
    EXPECT_DOUBLE_EQ(*m1->minimum_density, 4.0);
    ASSERT_TRUE(m1->maximum_density.has_value());
    EXPECT_DOUBLE_EQ(*m1->maximum_density, 10.0);
    ASSERT_TRUE(m1->density_check_step.has_value());
    EXPECT_EQ(*m1->density_check_step, 200);
    ASSERT_TRUE(m1->density_check_window.has_value());
    EXPECT_EQ(m1->density_check_window->length, 1000);
    EXPECT_EQ(m1->density_check_window->width, 1000);
    ASSERT_TRUE(m1->fill_active_spacing.has_value());
    EXPECT_EQ(*m1->fill_active_spacing, 400);

    const std::vector<LayerDensityEntryId> &ac_ids = root.get_layer_ac_current_density(m1_id);
    ASSERT_EQ(ac_ids.size(), 2u);
    const LayerDensityEntryData &ac_peak = *root.get_layer_density_entry(ac_ids[0]);
    EXPECT_EQ(ac_peak.type, "PEAK");
    EXPECT_FALSE(ac_peak.one_entry.has_value());
    ASSERT_EQ(ac_peak.frequency.size(), 2u);
    EXPECT_DOUBLE_EQ(ac_peak.frequency[0], 1000000.0);
    EXPECT_DOUBLE_EQ(ac_peak.frequency[1], 100000000.0);
    ASSERT_EQ(ac_peak.width.size(), 2u);
    EXPECT_EQ(ac_peak.width[0], 100);
    EXPECT_EQ(ac_peak.width[1], 200);
    ASSERT_EQ(ac_peak.table_entries.size(), 2u);
    EXPECT_DOUBLE_EQ(ac_peak.table_entries[0], 0.0000005);
    const LayerDensityEntryData &ac_average = *root.get_layer_density_entry(ac_ids[1]);
    EXPECT_EQ(ac_average.type, "AVERAGE");
    ASSERT_TRUE(ac_average.one_entry.has_value());
    EXPECT_DOUBLE_EQ(*ac_average.one_entry, 5.5);

    const std::vector<LayerDensityEntryId> &dc_ids = root.get_layer_dc_current_density(m1_id);
    ASSERT_EQ(dc_ids.size(), 1u);
    const LayerDensityEntryData *dc0 = root.get_layer_density_entry(dc_ids[0]);
    ASSERT_TRUE(dc0->one_entry.has_value());
    EXPECT_DOUBLE_EQ(*dc0->one_entry, 4.9);
}

TEST_F(LEFAntennaFixture, ReadsCutLayerArraySpacingEnclosureAndCurrentDensityAddedInPhase6)
{
    const LayerId v1_id = root.get_layer_by_name("V1");
    const LayerData *v1 = root.get_layer(v1_id);
    ASSERT_TRUE(v1 != nullptr);

    ASSERT_EQ(v1->array_cuts.size(), 2u);
    EXPECT_EQ(v1->array_cuts[0].cuts, 3);
    EXPECT_EQ(v1->array_cuts[0].spacing, 1000);
    EXPECT_EQ(v1->array_cuts[1].cuts, 4);
    EXPECT_EQ(v1->array_cuts[1].spacing, 1500);
    const ArraySpacingData *array_spacing = root.get_array_spacing(root.get_layer_array_spacing(v1_id));
    ASSERT_TRUE(array_spacing != nullptr);
    EXPECT_TRUE(array_spacing->long_array);
    ASSERT_TRUE(array_spacing->via_width.has_value());
    EXPECT_EQ(*array_spacing->via_width, 500);
    EXPECT_EQ(array_spacing->cut_spacing, 200);

    const std::vector<PreferEnclosureEntryId> &prefer_enclosure_ids = root.get_layer_prefer_enclosures(v1_id);
    ASSERT_EQ(prefer_enclosure_ids.size(), 1u);
    const PreferEnclosureEntryData &prefer_enclosure0 = *root.get_prefer_enclosure_entry(prefer_enclosure_ids[0]);
    EXPECT_EQ(prefer_enclosure0.location, "BELOW");
    EXPECT_EQ(prefer_enclosure0.overhang1, 60);
    EXPECT_EQ(prefer_enclosure0.overhang2, 10);

    const std::vector<EnclosureEntryId> &enclosure_ids = root.get_layer_enclosures(v1_id);
    ASSERT_EQ(enclosure_ids.size(), 2u);
    const EnclosureEntryData &enclosure0 = *root.get_enclosure_entry(enclosure_ids[0]);
    EXPECT_EQ(enclosure0.location, "ABOVE");
    EXPECT_EQ(enclosure0.overhang1, 50);
    EXPECT_EQ(enclosure0.overhang2, 10);
    EXPECT_FALSE(enclosure0.width.has_value());
    const EnclosureEntryData &enclosure1 = *root.get_enclosure_entry(enclosure_ids[1]);
    EXPECT_FALSE(enclosure1.location.has_value());
    ASSERT_TRUE(enclosure1.width.has_value());
    EXPECT_EQ(*enclosure1.width, 1000);

    // CUT-layer RESISTANCE (resistancePerCut()) - readable but, per the
    // vendored writer's own RESISTANCEPERCUT-keyword bug (see
    // write_technology_layers's own comment), never re-written.
    ASSERT_TRUE(v1->resistance.has_value());
    EXPECT_DOUBLE_EQ(*v1->resistance, 10.0);

    const std::vector<LayerDensityEntryId> &v1_dc_ids = root.get_layer_dc_current_density(v1_id);
    ASSERT_EQ(v1_dc_ids.size(), 1u);
    const LayerDensityEntryData &dc = *root.get_layer_density_entry(v1_dc_ids[0]);
    EXPECT_FALSE(dc.one_entry.has_value());
    ASSERT_EQ(dc.cutarea.size(), 2u);
    EXPECT_EQ(dc.cutarea[0], 2000000);
    EXPECT_EQ(dc.cutarea[1], 5000000);
    ASSERT_EQ(dc.table_entries.size(), 2u);
    EXPECT_DOUBLE_EQ(dc.table_entries[0], 0.0000005);
}

TEST_F(LEFAntennaFixture, ReadsPinScalarFieldsDirectionEnumPortClassViaAndSiteArrayPlacementsAddedInPhase7)
{
    const AbstractId abstract_id = root.get_design_abstract(root.get_design_by_name("ANTENNATEST"));
    const AbstractData *abstract = root.get_abstract(abstract_id);
    ASSERT_TRUE(abstract != nullptr);

    // MACRO SITE array placements (distinct from the singular `site` name
    // form, which ANTENNATEST doesn't use).
    const std::vector<MacroSitePlacementId> &site_placement_ids = root.get_abstract_site_placements(abstract_id);
    ASSERT_EQ(site_placement_ids.size(), 2u);
    const MacroSitePlacementData &site_placement0 = *root.get_macro_site_placement(site_placement_ids[0]);
    EXPECT_EQ(site_placement0.site_name, "CORE");
    EXPECT_EQ(site_placement0.orient, Orientation::N);
    ASSERT_TRUE(site_placement0.num_x.has_value());
    EXPECT_EQ(*site_placement0.num_x, 2);
    EXPECT_EQ(*site_placement0.num_y, 1);
    EXPECT_EQ(*site_placement0.step_x, 10000);
    const MacroSitePlacementData &site_placement1 = *root.get_macro_site_placement(site_placement_ids[1]);
    EXPECT_EQ(site_placement1.orient, Orientation::FN);
    EXPECT_FALSE(site_placement1.num_x.has_value());

    const std::vector<TerminalId> &terminal_ids = root.get_abstract_terminals(abstract_id);
    const TerminalData *pin_a = root.get_terminal(terminal_ids[0]);
    ASSERT_TRUE(pin_a != nullptr);
    EXPECT_EQ(pin_a->name, "A");
    EXPECT_EQ(pin_a->taper_rule, "RULE1");
    EXPECT_EQ(pin_a->supply_sensitivity, "vddpin1");
    EXPECT_EQ(pin_a->ground_sensitivity, "gndpin");
    // rise_slew_limit/fall_slew_limit/max_load are declared-unit values -
    // no dbu conversion, matching resistance/capacitance's own treatment.
    ASSERT_TRUE(pin_a->rise_slew_limit.has_value());
    EXPECT_DOUBLE_EQ(*pin_a->rise_slew_limit, 0.01);
    ASSERT_TRUE(pin_a->fall_slew_limit.has_value());
    EXPECT_DOUBLE_EQ(*pin_a->fall_slew_limit, 0.02);
    ASSERT_TRUE(pin_a->max_load.has_value());
    EXPECT_DOUBLE_EQ(*pin_a->max_load, 0.1);

    const std::vector<TerminalPortId> &pin_a_port_ids = root.get_terminal_ports(terminal_ids[0]);
    ASSERT_EQ(pin_a_port_ids.size(), 1u);
    const TerminalPortData *port_a = root.get_terminal_port(pin_a_port_ids[0]);
    ASSERT_TRUE(port_a != nullptr);
    EXPECT_EQ(port_a->port_class, "CORE");
    const std::vector<ShapeId> &port_a_shapes = root.get_terminal_port_shapes(pin_a_port_ids[0]);
    ASSERT_EQ(port_a_shapes.size(), 1u);
    const Shape &shape_a = *root.get_shape(port_a_shapes[0]);
    EXPECT_EQ(shape_a.layer_name, "M1");
    EXPECT_EQ(shape_a.spacing, 50);
    ASSERT_EQ(shape_a.vias.size(), 1u);
    EXPECT_EQ(shape_a.vias[0].via_name, "MYVIA");
    EXPECT_EQ(shape_a.vias[0].origin.x, 200);
    EXPECT_EQ(shape_a.vias[0].origin.y, 200);

    const TerminalData *pin_b = root.get_terminal(terminal_ids[1]);
    ASSERT_TRUE(pin_b != nullptr);
    EXPECT_EQ(pin_b->name, "B");
    EXPECT_EQ(pin_b->direction, SignalDirection::FEEDTHRU);

    ASSERT_EQ(root.get_abstract_obstructions(abstract_id).size(), 1u);
    const ObstructionId obs_id = root.get_abstract_obstructions(abstract_id).front();
    const ObstructionData *obs = root.get_obstruction(obs_id);
    ASSERT_TRUE(obs != nullptr);
    const std::vector<ShapeId> &obs_shapes = root.get_obstruction_shapes(obs_id);
    ASSERT_EQ(obs_shapes.size(), 1u);
    const Shape &obs_shape = *root.get_shape(obs_shapes[0]);
    EXPECT_EQ(obs_shape.layer_name, "V1");
    EXPECT_TRUE(obs_shape.except_pg_net);
}

TEST_F(LEFReaderViaFixture, DuplicateViaAndViaRuleNamesAreIgnoredWithAWarning)
{
    // Re-reading the whole fixture into the same Root also re-declares
    // MACRO WRITERTEST, which DuplicateMacroNameIsRejected already covers
    // as its own hard failure (result 2) - VIA/VIARULE appear earlier in
    // the file than MACRO, so their own warnings are logged (and land in
    // messages(), moved from g_pending_lef_messages regardless of the
    // later abort - see read_lef's own structure) before that happens.
    LEFReader second_reader;
    ASSERT_EQ(second_reader.read_lef(fixture_path("writer_roundtrip.lef"), root, "test_lib"), 2);

    ASSERT_FALSE(second_reader.messages().empty());
    bool saw_via_warning = false;
    bool saw_via_rule_warning = false;
    for (const auto &msg : second_reader.messages())
    {
        if (msg.find("WARNING") != std::string::npos && msg.find("Via VIA1") != std::string::npos)
            saw_via_warning = true;
        if (msg.find("WARNING") != std::string::npos && msg.find("ViaRule VIARULE1") != std::string::npos)
            saw_via_rule_warning = true;
    }
    EXPECT_TRUE(saw_via_warning);
    EXPECT_TRUE(saw_via_rule_warning);
}
