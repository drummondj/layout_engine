#include "../sv_reader.hpp"
#include <gtest/gtest.h>
#include <string>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"

using namespace le;

namespace
{
    std::string fixture_path(const std::string &name)
    {
        return std::string(SV_TEST_FIXTURES_DIR) + "/" + name;
    }
}


// Milestone 3 spike (see the linked plan's own "Required spike" section):
// confirms slang's per-module diagnostic-location filtering design
// actually works before SVReader's real reader logic is built on top of
// it. rtl_invalid_body.sv has a module with a real parameter + generate-
// for loop + gate instantiations, plus a deliberately broken always_ff
// block in the same module (a missing right-hand-side expression).
TEST(SVReader, NetlistFlavorElaboratesGenerateInstancesDespiteBrokenAlwaysBlock)
{
    auto tree = slang::syntax::SyntaxTree::fromFile(fixture_path("rtl_invalid_body.sv"));
    ASSERT_TRUE(tree.has_value()) << "failed to open fixture";

    slang::ast::Compilation compilation;
    compilation.addSyntaxTree(tree.value());
    const auto &root = compilation.getRoot();

    // The broken always_ff block must actually have been flagged - a
    // test that didn't reproduce a diagnostic at all would prove nothing
    // about the filtering design.
    EXPECT_FALSE(compilation.getAllDiagnostics().empty());

    struct InstanceCountVisitor : public slang::ast::ASTVisitor<InstanceCountVisitor>
    {
        int bufx1_count = 0;
        void handle(const slang::ast::InstanceSymbol &symbol)
        {
            if (symbol.getDefinition().name == "BUFX1")
                ++bufx1_count;
            visitDefault(symbol);
        }
    };
    InstanceCountVisitor visitor;
    root.visit(visitor);

    // WIDTH defaults to 2 in the fixture - the generate-for loop must
    // still have elaborated both instances despite the broken always_ff
    // block elsewhere in the same module.
    EXPECT_EQ(visitor.bufx1_count, 2);
}

TEST(SVReader, RtlFlavorExtractsStructureAndConfinesDiagnosticsToTheBrokenBlock)
{
    auto tree = slang::syntax::SyntaxTree::fromFile(fixture_path("rtl_invalid_body.sv"));
    ASSERT_TRUE(tree.has_value()) << "failed to open fixture";

    const slang::syntax::ModuleDeclarationSyntax *spike_mixed_syntax = nullptr;
    const slang::syntax::HierarchyInstantiationSyntax *instantiation_syntax = nullptr;
    const slang::syntax::ProceduralBlockSyntax *always_syntax = nullptr;

    auto visitor = slang::syntax::makeSyntaxVisitor(
        [&](auto &self, const slang::syntax::ModuleDeclarationSyntax &node)
        {
            if (node.header->name.valueText() == "spike_mixed")
                spike_mixed_syntax = &node;
            self.visitDefault(node);
        },
        [&](auto &self, const slang::syntax::HierarchyInstantiationSyntax &node)
        {
            instantiation_syntax = &node;
            self.visitDefault(node);
        },
        [&](auto &self, const slang::syntax::ProceduralBlockSyntax &node)
        {
            always_syntax = &node;
            self.visitDefault(node);
        });
    tree.value()->root().visit(visitor);

    // The port list and the gate instantiation are exactly the kind of
    // structural content SVReader's own per-module fallback decision
    // (extract structured Port/Instance/Pin data vs. fall back to a
    // logic-cloud Instance) depends on - both must have parsed intact.
    ASSERT_NE(spike_mixed_syntax, nullptr);
    ASSERT_NE(spike_mixed_syntax->header->ports, nullptr);
    ASSERT_NE(instantiation_syntax, nullptr);
    EXPECT_EQ(instantiation_syntax->type.valueText(), "BUFX1");
    ASSERT_NE(always_syntax, nullptr);

    // The broken block must actually have been flagged.
    ASSERT_FALSE(tree.value()->diagnostics().empty());

    // Diagnostics from the broken always_ff block must not bleed into
    // the port list or the (structurally clean) gate instantiation.
    const auto port_list_range = spike_mixed_syntax->header->ports->sourceRange();
    const auto instantiation_range = instantiation_syntax->sourceRange();
    const auto always_range = always_syntax->sourceRange();

    bool any_diagnostic_in_always_block = false;
    for (const auto &diag : tree.value()->diagnostics())
    {
        EXPECT_FALSE(port_list_range.contains(diag.location)) << "diagnostic leaked into the port list";
        EXPECT_FALSE(instantiation_range.contains(diag.location)) << "diagnostic leaked into the gate instantiation";
        if (always_range.contains(diag.location))
            any_diagnostic_in_always_block = true;
    }
    EXPECT_TRUE(any_diagnostic_in_always_block);
}

// Milestone 4 (netlist flavor): gate_netlist_clean.v has a parameter-sized
// generate-for loop instantiating INV by name, plus AND2 instantiated
// once with named and once with positional connections, including a bus
// net (mid) connected via bit-select in every case.
TEST(SVReader, NetlistReadPopulatesPortsInstancesAndPins)
{
    Root root;
    SVReader reader;
    ASSERT_EQ(reader.read_netlist({fixture_path("gate_netlist_clean.v")}, root, "test_lib"), 0);

    const DesignId top_id = root.get_design_by_name("top");
    ASSERT_TRUE(top_id.valid());
    const SchematicId schematic_id = root.get_design_schematic(top_id);
    ASSERT_TRUE(schematic_id.valid());

    ASSERT_TRUE(root.get_port_by_name(schematic_id, "clk").valid());
    // "in"/"out" are both [WIDTH-1:0] = [1:0] buses (WIDTH defaults to 2) -
    // no scalar "in"/"out" Port exists anymore, replaced by one Port per
    // bit plus a PortBus grouping (see schema.py's own PortBus comment).
    const PortBusId out_bus = root.get_port_bus_by_name(schematic_id, "out");
    ASSERT_TRUE(out_bus.valid());
    const PortBusData *out_bus_data = root.get_port_bus(out_bus);
    ASSERT_NE(out_bus_data, nullptr);
    EXPECT_EQ(out_bus_data->msb, 1);
    EXPECT_EQ(out_bus_data->lsb, 0);

    const PortId out0_port = root.get_port_by_name(schematic_id, "out[0]");
    ASSERT_TRUE(out0_port.valid());
    const PortData *out0_data = root.get_port(out0_port);
    ASSERT_NE(out0_data, nullptr);
    EXPECT_EQ(out0_data->bus, out_bus);
    ASSERT_TRUE(out0_data->bit_index.has_value());
    EXPECT_EQ(*out0_data->bit_index, 0);
    ASSERT_TRUE(root.get_port_by_name(schematic_id, "out[1]").valid());

    // WIDTH defaults to 2 - the generate-for loop must have elaborated
    // both INV instances, plus the two hand-written AND2 instances.
    const auto instance_ids = root.get_schematic_instances(schematic_id);
    ASSERT_EQ(instance_ids.size(), 4u);

    int inv_count = 0;
    int and2_count = 0;
    InstanceId first_inv_id;
    for (const auto id : instance_ids)
    {
        const InstanceData *data = root.get_instance(id);
        ASSERT_NE(data, nullptr);
        ASSERT_TRUE(data->reference_name.has_value());
        EXPECT_TRUE(data->reference_design.valid());
        if (*data->reference_name == "INV")
        {
            if (!first_inv_id.valid())
                first_inv_id = id;
            ++inv_count;
        }
        else if (*data->reference_name == "AND2")
        {
            ++and2_count;
        }
    }
    EXPECT_EQ(inv_count, 2);
    EXPECT_EQ(and2_count, 2);

    // The two generate-created INV instances must be distinguishable by
    // name (both share the bare name "u_inv" in source - see
    // qualified_instance_name's own comment in sv_reader.cpp).
    ASSERT_TRUE(first_inv_id.valid());
    EXPECT_EQ(root.get_instance(first_inv_id)->name, "gen_inv[0].u_inv");

    // The first INV's own "A" pin connects to in[0] - a bus bit-select,
    // so it must resolve directly to the per-bit Net "in[0]" (see
    // schema.py's own Net.bus/.bit_index comment - the specific bit is
    // encoded in which Net this points at, not a separate field on Pin).
    const auto pin_ids = root.get_instance_pins(first_inv_id);
    ASSERT_EQ(pin_ids.size(), 2u);
    bool found_a_pin = false;
    for (const auto pin_id : pin_ids)
    {
        const PinData *pin = root.get_pin(pin_id);
        ASSERT_NE(pin, nullptr);
        if (pin->name == "A")
        {
            found_a_pin = true;
            ASSERT_TRUE(pin->net.valid());
            EXPECT_EQ(root.get_net(pin->net)->name, "in[0]");
        }
    }
    EXPECT_TRUE(found_a_pin);
}

// Milestone 4/5: an instance referencing a module never defined in the
// file being read (the common case for a real gate-level netlist
// referencing standard cells) stays unresolved until
// link_unresolved_instances() is told about a matching Design - e.g.
// after a later read_lef supplies the leaf cell.
TEST(SVReader, UndefinedLeafCellStaysUnresolvedUntilLinked)
{
    Root root;
    SVReader reader;
    ASSERT_EQ(reader.read_netlist({fixture_path("gate_netlist_undefined_leaf.v")}, root, "test_lib"), 0);

    const DesignId top_id = root.get_design_by_name("top");
    ASSERT_TRUE(top_id.valid());
    const SchematicId schematic_id = root.get_design_schematic(top_id);
    const auto instance_ids = root.get_schematic_instances(schematic_id);
    ASSERT_EQ(instance_ids.size(), 1u);

    const InstanceId instance_id = instance_ids[0];
    const InstanceData *instance = root.get_instance(instance_id);
    ASSERT_NE(instance, nullptr);
    ASSERT_TRUE(instance->reference_name.has_value());
    EXPECT_EQ(*instance->reference_name, "BUFX1");
    EXPECT_FALSE(instance->reference_design.valid());

    // No matching Design exists yet - linking finds nothing to resolve.
    EXPECT_EQ(SVReader::link_unresolved_instances(root), 0u);
    EXPECT_FALSE(root.get_instance(instance_id)->reference_design.valid());

    // Simulate a later read_lef supplying the leaf cell's own Design.
    const LibraryId library_id = root.get_library_by_name("test_lib");
    const DesignId bufx1_id = root.create_design(DesignData{.library = library_id, .name = "BUFX1"});

    EXPECT_EQ(SVReader::link_unresolved_instances(root), 1u);
    EXPECT_EQ(root.get_instance(instance_id)->reference_design, bufx1_id);
}

// A synthesizer-flattened unpacked-array element's connection uses a
// Verilog escaped identifier (`\name`, terminated by whitespace) - found
// while diagnosing why real synthesized-netlist DEF Route/Net linking
// (LINKING_STRATEGY_RESEARCH.md) still failed on ~2300 routes even after
// per-bit bus Nets existed: classify_simple_connection_text (used for
// undefined leaf cells like BUFX1 here, same as real standard cells)
// must recognize the escaped form and produce the same bracketed Net
// name a plain bus bit-select would (see that function's own comment for
// why slang's own source-range trivia rules mean a *plain* escaped
// identifier's connection text carries no embedded whitespace at all,
// while one immediately followed by a real bit-select does).
TEST(SVReader, EscapedIdentifierConnectionsResolveToTheSameBracketedNetNameAsAPlainBitSelect)
{
    Root root;
    SVReader reader;
    ASSERT_EQ(reader.read_netlist({fixture_path("gate_netlist_escaped_identifiers.v")}, root, "test_lib"), 0);

    const DesignId top_id = root.get_design_by_name("top");
    ASSERT_TRUE(top_id.valid());
    const SchematicId schematic_id = root.get_design_schematic(top_id);

    // A plain escaped identifier with no further bit-select - the whole
    // net name is exactly the escaped text, brackets included literally.
    const NetId rkey_net = root.get_net_by_name(schematic_id, "rkey[1]");
    ASSERT_TRUE(rkey_net.valid());

    // An escaped identifier (itself a real 32-bit vector) plus a real
    // bit-select on top - resolves to the fully bracketed per-bit name,
    // not left as raw_expression.
    const NetId block_reg_bit_net = root.get_net_by_name(schematic_id, "block_reg[0][31]");
    ASSERT_TRUE(block_reg_bit_net.valid());

    bool found_scalar_pin = false;
    bool found_vector_bit_pin = false;
    for (const auto instance_id : root.get_schematic_instances(schematic_id))
    {
        for (const auto pin_id : root.get_instance_pins(instance_id))
        {
            const PinData *pin = root.get_pin(pin_id);
            ASSERT_NE(pin, nullptr);
            if (pin->name != "Z")
                continue;
            if (pin->net == rkey_net)
                found_scalar_pin = true;
            else if (pin->net == block_reg_bit_net)
                found_vector_bit_pin = true;
        }
    }
    EXPECT_TRUE(found_scalar_pin);
    EXPECT_TRUE(found_vector_bit_pin);
}

// Milestone 6 (RTL flavor): gate_netlist_clean.v's two AND2 instances sit
// directly at module scope (not nested in the generate-for loop like the
// INV instances), so the RTL flavor - which only extracts a top-level
// instantiation, not one nested inside a generate construct - should
// still find them as real Instances/Pins, while the generate region and
// genvar declaration each become their own logic-cloud Instance.
TEST(SVReader, RtlReadExtractsTopLevelInstantiationsAndFallsBackForGenerateRegions)
{
    Root root;
    SVReader reader;
    ASSERT_EQ(reader.read_rtl({fixture_path("gate_netlist_clean.v")}, root, "test_lib"), 0);

    const DesignId top_id = root.get_design_by_name("top");
    ASSERT_TRUE(top_id.valid());
    const SchematicId schematic_id = root.get_design_schematic(top_id);
    ASSERT_TRUE(root.get_port_by_name(schematic_id, "clk").valid());
    ASSERT_TRUE(root.get_port_by_name(schematic_id, "out").valid());

    int and2_count = 0;
    int logic_cloud_count = 0;
    InstanceId and2_with_bit_select;
    for (const auto id : root.get_schematic_instances(schematic_id))
    {
        const InstanceData *data = root.get_instance(id);
        ASSERT_NE(data, nullptr);
        if (data->rtl_text.has_value())
        {
            ++logic_cloud_count;
            continue;
        }
        ASSERT_TRUE(data->reference_name.has_value());
        EXPECT_EQ(*data->reference_name, "AND2");
        ++and2_count;
        if (!and2_with_bit_select.valid())
            and2_with_bit_select = id;
    }
    EXPECT_EQ(and2_count, 2);
    EXPECT_GE(logic_cloud_count, 1); // the generate region (and genvar decl).

    // AND2's own "A" pin connects to a bus bit-select (mid[0] or mid[1] -
    // RTL flavor classifies this from its own verbatim connection text,
    // not an elaborated expression).
    ASSERT_TRUE(and2_with_bit_select.valid());
    bool found_bit_select_pin = false;
    for (const auto pin_id : root.get_instance_pins(and2_with_bit_select))
    {
        const PinData *pin = root.get_pin(pin_id);
        ASSERT_NE(pin, nullptr);
        if (pin->name == "A" && pin->net.valid())
        {
            const std::string &net_name = root.get_net(pin->net)->name;
            if (net_name == "mid[0]" || net_name == "mid[1]")
                found_bit_select_pin = true;
        }
    }
    EXPECT_TRUE(found_bit_select_pin);
}

// Milestone 6: rtl_invalid_body.sv's BUFX1 has a clean, trivial header
// (real Ports, no body content at all); spike_mixed's own broken
// always_ff block must become a logic-cloud Instance carrying both the
// verbatim source text and a diagnostic summary, without disturbing
// BUFX1's own separate Design. RTL flavor doesn't evaluate bit widths
// (unlike read_netlist) - spike_mixed's own "out" port must come back
// scalar (no PortBus) despite being declared [WIDTH-1:0] in source.
TEST(SVReader, RtlReadCreatesLogicCloudForBrokenAlwaysBlockWithoutDisturbingOtherModules)
{
    Root root;
    SVReader reader;
    ASSERT_EQ(reader.read_rtl({fixture_path("rtl_invalid_body.sv")}, root, "test_lib"), 0);

    const DesignId bufx1_id = root.get_design_by_name("BUFX1");
    ASSERT_TRUE(bufx1_id.valid());
    const SchematicId bufx1_schematic = root.get_design_schematic(bufx1_id);
    EXPECT_TRUE(root.get_port_by_name(bufx1_schematic, "A").valid());
    EXPECT_TRUE(root.get_port_by_name(bufx1_schematic, "Z").valid());
    EXPECT_TRUE(root.get_schematic_instances(bufx1_schematic).empty());

    const DesignId spike_id = root.get_design_by_name("spike_mixed");
    ASSERT_TRUE(spike_id.valid());
    const SchematicId spike_schematic = root.get_design_schematic(spike_id);
    const PortId out_port = root.get_port_by_name(spike_schematic, "out");
    ASSERT_TRUE(out_port.valid());
    EXPECT_FALSE(root.get_port(out_port)->bus.valid());

    bool found_always_ff_logic_cloud = false;
    for (const auto id : root.get_schematic_instances(spike_schematic))
    {
        const InstanceData *data = root.get_instance(id);
        ASSERT_NE(data, nullptr);
        if (data->rtl_text.has_value() && data->rtl_text->find("always_ff") != std::string::npos)
        {
            found_always_ff_logic_cloud = true;
            EXPECT_TRUE(data->diagnostic_summary.has_value());
        }
    }
    EXPECT_TRUE(found_always_ff_logic_cloud);
}
