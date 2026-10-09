#include "../verilog_stub_writer.hpp"
#include "../../database/database.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    TerminalId add_terminal(Root &root, AbstractId abstract_id, const std::string &name, SignalDirection direction)
    {
        return root.create_terminal(TerminalData{.abstract = abstract_id, .name = name, .direction = direction});
    }
}

TEST(VerilogStubWriter, SkipsADesignWithNoAbstractAtAll)
{
    Root root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    root.create_design(DesignData{.library = library, .name = "EMPTY"});

    EXPECT_EQ(generate_verilog_stubs(root, library), "");
}

TEST(VerilogStubWriter, SkipsADesignThatAlreadyHasASchematic)
{
    Root root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "REAL_RTL"});
    const AbstractId abstract_id = root.create_abstract(AbstractData{.design = design});
    add_terminal(root, abstract_id, "A", SignalDirection::INPUT);
    root.create_schematic(SchematicData{.design = design});

    EXPECT_EQ(generate_verilog_stubs(root, library), "");
}

TEST(VerilogStubWriter, ScalarPortsEmitOnePortEach)
{
    Root root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "BUF_X1"});
    const AbstractId abstract_id = root.create_abstract(AbstractData{.design = design});
    add_terminal(root, abstract_id, "A", SignalDirection::INPUT);
    add_terminal(root, abstract_id, "Z", SignalDirection::OUTPUT);

    const std::string stub = generate_verilog_stubs(root, library);
    EXPECT_NE(stub.find("module BUF_X1 (A, Z);"), std::string::npos) << stub;
    EXPECT_NE(stub.find("input A;"), std::string::npos) << stub;
    EXPECT_NE(stub.find("output Z;"), std::string::npos) << stub;
    EXPECT_NE(stub.find("endmodule"), std::string::npos) << stub;
}

TEST(VerilogStubWriter, ContiguousBracketedBitsCombineIntoOneBusPort)
{
    Root root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "SRAM"});
    const AbstractId abstract_id = root.create_abstract(AbstractData{.design = design});
    for (int i = 0; i < 8; ++i)
        add_terminal(root, abstract_id, "addr_in[" + std::to_string(i) + "]", SignalDirection::INPUT);
    add_terminal(root, abstract_id, "clk", SignalDirection::INPUT);

    const std::string stub = generate_verilog_stubs(root, library);
    EXPECT_NE(stub.find("input [7:0] addr_in;"), std::string::npos) << stub;
    // The bus is declared once, not per-bit.
    EXPECT_EQ(stub.find("addr_in[0]"), std::string::npos) << stub;
    EXPECT_NE(stub.find("module SRAM (addr_in, clk);"), std::string::npos) << stub;
}

TEST(VerilogStubWriter, NonContiguousBracketedBitsFallBackToOnePortPerBit)
{
    Root root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "SPARSE"});
    const AbstractId abstract_id = root.create_abstract(AbstractData{.design = design});
    add_terminal(root, abstract_id, "d[0]", SignalDirection::INPUT);
    add_terminal(root, abstract_id, "d[2]", SignalDirection::INPUT); // gap at d[1]

    const std::string stub = generate_verilog_stubs(root, library);
    EXPECT_NE(stub.find("\\d[0] "), std::string::npos) << stub;
    EXPECT_NE(stub.find("\\d[2] "), std::string::npos) << stub;
    EXPECT_EQ(stub.find("[2:0]"), std::string::npos) << stub;
}

TEST(VerilogStubWriter, DirectionInconsistentBracketedBitsFallBackToOnePortPerBit)
{
    Root root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "MIXED_DIR"});
    const AbstractId abstract_id = root.create_abstract(AbstractData{.design = design});
    add_terminal(root, abstract_id, "b[0]", SignalDirection::INPUT);
    add_terminal(root, abstract_id, "b[1]", SignalDirection::OUTPUT);

    const std::string stub = generate_verilog_stubs(root, library);
    EXPECT_NE(stub.find("\\b[0] "), std::string::npos) << stub;
    EXPECT_NE(stub.find("\\b[1] "), std::string::npos) << stub;
    EXPECT_EQ(stub.find("[1:0]"), std::string::npos) << stub;
}

TEST(VerilogStubWriter, OutputTristateAndFeedthruAndNoneMapToPermissiveKeywords)
{
    Root root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "TRI"});
    const AbstractId abstract_id = root.create_abstract(AbstractData{.design = design});
    add_terminal(root, abstract_id, "TZ", SignalDirection::OUTPUT_TRISTATE);
    add_terminal(root, abstract_id, "FT", SignalDirection::FEEDTHRU);
    add_terminal(root, abstract_id, "UN", SignalDirection::NONE);

    const std::string stub = generate_verilog_stubs(root, library);
    EXPECT_NE(stub.find("output TZ;"), std::string::npos) << stub;
    EXPECT_NE(stub.find("inout FT;"), std::string::npos) << stub;
    EXPECT_NE(stub.find("inout UN;"), std::string::npos) << stub;
}

TEST(VerilogStubWriter, GeneratesOneModulePerAbstractOnlyDesignInTheLibrary)
{
    Root root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    for (const std::string &name : {"BUF_X1", "INV_X1"})
    {
        const DesignId design = root.create_design(DesignData{.library = library, .name = name});
        const AbstractId abstract_id = root.create_abstract(AbstractData{.design = design});
        add_terminal(root, abstract_id, "A", SignalDirection::INPUT);
        add_terminal(root, abstract_id, "Z", SignalDirection::OUTPUT);
    }

    const std::string stub = generate_verilog_stubs(root, library);
    EXPECT_NE(stub.find("module BUF_X1 ("), std::string::npos) << stub;
    EXPECT_NE(stub.find("module INV_X1 ("), std::string::npos) << stub;
}

namespace
{
    // An Abstract-only cell `name` in `library`, with one input pin `pin`.
    DesignId add_cell(Root &root, LibraryId library, const std::string &name, const std::string &pin)
    {
        const DesignId design = root.create_design(DesignData{.library = library, .name = name});
        add_terminal(root, root.create_abstract(AbstractData{.design = design}), pin, SignalDirection::INPUT);
        return design;
    }

    size_t count(const std::string &haystack, const std::string &needle)
    {
        size_t n = 0;
        for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1))
            ++n;
        return n;
    }
}

// Two libraries' INV can't both become `module INV`: the read's own
// library picks one, and with no pick neither is stubbed.
TEST(VerilogStubWriter, ForARead_OneModulePerNameResolvedLikeTheRead)
{
    Root root;
    const LibraryId lib_a = root.create_library(LibraryData{.name = "a"});
    const LibraryId lib_b = root.create_library(LibraryData{.name = "b"});
    add_cell(root, lib_a, "INV", "A_PIN");
    add_cell(root, lib_b, "INV", "B_PIN");
    add_cell(root, lib_a, "BUF", "I");

    std::vector<std::string> ambiguous;
    const std::string into_a = generate_verilog_stubs_for_read(root, lib_a, ambiguous);
    EXPECT_TRUE(ambiguous.empty());
    EXPECT_EQ(count(into_a, "module INV ("), 1u) << into_a;
    EXPECT_NE(into_a.find("module INV (A_PIN);"), std::string::npos) << into_a;
    EXPECT_NE(into_a.find("module BUF (I);"), std::string::npos) << into_a;

    const std::string into_new = generate_verilog_stubs_for_read(root, LibraryId{}, ambiguous);
    EXPECT_EQ(into_new.find("module INV"), std::string::npos) << into_new;
    EXPECT_NE(into_new.find("module BUF (I);"), std::string::npos) << into_new;
    EXPECT_EQ(ambiguous, std::vector<std::string>{"INV"});
}
