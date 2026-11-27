#include "../database.hpp"
#include "../signal_use_inference.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    // INV: a cell with terminals A, Z (no use: SIGNAL), VDD (POWER), VSS (GROUND).
    // SUB.schematic: Port P on net p; INV i0 with VDD -> p, VSS -> g.
    // TOP.schematic: SUB s0 with P -> top_vdd; INV i1 with VDD -> top_vdd,
    //   A -> sig, VSS and Z -> mixed; INV i2 with VDD -> preset (CLOCK),
    //   VSS -> unknown, X (no such terminal) -> unknown.
    struct Fixture
    {
        Root root;
        SchematicId sub, top;
        PortId port_p;

        Fixture()
        {
            const LibraryId library = root.create_library(LibraryData{.name = "lib"});
            const DesignId inv = root.create_design(DesignData{.library = library, .name = "INV"});
            const AbstractId inv_abstract = root.create_abstract(AbstractData{.design = inv});
            root.create_terminal(TerminalData{.abstract = inv_abstract, .name = "A", .direction = SignalDirection::INPUT});
            root.create_terminal(TerminalData{.abstract = inv_abstract, .name = "Z", .direction = SignalDirection::OUTPUT});
            root.create_terminal(TerminalData{.abstract = inv_abstract, .name = "VDD", .direction = SignalDirection::INOUT, .use = SignalUse::POWER});
            root.create_terminal(TerminalData{.abstract = inv_abstract, .name = "VSS", .direction = SignalDirection::INOUT, .use = SignalUse::GROUND});

            const DesignId sub_design = root.create_design(DesignData{.library = library, .name = "SUB"});
            sub = root.create_schematic(SchematicData{.design = sub_design});
            const NetId p = net(sub, "p");
            port_p = root.create_port(PortData{.schematic = sub, .name = "P", .direction = SignalDirection::INOUT, .net = p});
            instance(sub, "i0", inv, {{"VDD", p}, {"VSS", net(sub, "g")}});

            const DesignId top_design = root.create_design(DesignData{.library = library, .name = "TOP"});
            top = root.create_schematic(SchematicData{.design = top_design});
            const NetId top_vdd = net(top, "top_vdd"), mixed = net(top, "mixed"), unknown = net(top, "unknown");
            instance(top, "s0", sub_design, {{"P", top_vdd}});
            instance(top, "i1", inv, {{"VDD", top_vdd}, {"A", net(top, "sig")}, {"VSS", mixed}, {"Z", mixed}});
            const NetId preset = root.create_net(NetData{.schematic = top, .name = "preset", .use = SignalUse::CLOCK});
            instance(top, "i2", inv, {{"VDD", preset}, {"VSS", unknown}, {"X", unknown}});
        }

        NetId net(SchematicId schematic, const std::string &name) { return root.create_net(NetData{.schematic = schematic, .name = name}); }

        void instance(SchematicId schematic, const std::string &name, DesignId design, std::vector<std::pair<std::string, NetId>> pins)
        {
            const InstanceId id = root.create_instance(InstanceData{.schematic = schematic, .name = name, .reference_design = design});
            for (const auto &[pin, net] : pins)
                root.create_pin(PinData{.instance = id, .name = pin, .net = net});
        }

        std::optional<SignalUse> use_of(SchematicId schematic, const std::string &name) const
        {
            return root.get_net(root.get_net_by_name(schematic, name))->use;
        }
    };
}

TEST(SignalUseInference, ANetReachingOnlySupplyTerminalsTakesTheirUse)
{
    Fixture f;
    infer_signal_uses(f.root);
    EXPECT_EQ(f.use_of(f.sub, "p"), SignalUse::POWER);
    EXPECT_EQ(f.use_of(f.sub, "g"), SignalUse::GROUND);
    EXPECT_EQ(f.root.get_port(f.port_p)->use, SignalUse::POWER);
}

TEST(SignalUseInference, AChildPortsInferredUseReachesItsParentsNet)
{
    Fixture f;
    infer_signal_uses(f.root);
    EXPECT_EQ(f.use_of(f.top, "top_vdd"), SignalUse::POWER);
}

TEST(SignalUseInference, MixedSignalOrUnknownNetsStayUnset)
{
    Fixture f;
    infer_signal_uses(f.root);
    EXPECT_EQ(f.use_of(f.top, "sig"), std::nullopt);     // only SIGNAL terminals
    EXPECT_EQ(f.use_of(f.top, "mixed"), std::nullopt);   // GROUND and SIGNAL
    EXPECT_EQ(f.use_of(f.top, "unknown"), std::nullopt); // a pin with no terminal
}

TEST(SignalUseInference, ASetUseIsKeptAndARerunChangesNothing)
{
    Fixture f;
    EXPECT_EQ(infer_signal_uses(f.root), 4u); // p, g, P, top_vdd
    EXPECT_EQ(f.use_of(f.top, "preset"), SignalUse::CLOCK);
    EXPECT_EQ(infer_signal_uses(f.root), 0u);
}
