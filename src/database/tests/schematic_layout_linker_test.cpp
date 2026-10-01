#include "../database.hpp"
#include "../schematic_layout_linker.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    bool has_message_containing(const std::vector<std::string> &messages, std::string_view needle)
    {
        for (const auto &msg : messages)
            if (msg.find(needle) != std::string::npos)
                return true;
        return false;
    }

    // Builds:
    //   TOP.schematic: Instance "a" (-> A, no Placement yet - scenario 3),
    //                  Net "topnet" (no Route yet - scenario 4, a no-op)
    //     A.schematic:  Instance "b" (-> LEAF), Net "n1"
    //     LEAF: no Schematic (a hard macro)
    //   TOP.layout: Placement "a/b" (matches nested Instance "b"),
    //               Placement "filler0" (matches nothing - scenario 1),
    //               Route "n1" (should resolve through "a" to A.schematic's net),
    //               Route "unmatched_net" (matches nothing - scenario 2),
    //               PhysicalPort "clk" net_name "n1" (matches),
    //               PhysicalPort "vdd" net_name "unmatched_net" (matches nothing)
    struct Fixture
    {
        Root root;
        LibraryId library;
        DesignId leaf_design, a_design, top_design;
        SchematicId top_schematic, a_schematic;
        LayoutId top_layout;
        InstanceId inst_a, inst_b;
        NetId net_topnet, net_n1;
        PlacementId placement_ab, placement_filler;
        RouteId route_n1, route_unmatched;
        PhysicalPortId port_clk, port_vdd;

        Fixture()
        {
            library = root.create_library(LibraryData{.name = "lib"});

            leaf_design = root.create_design(DesignData{.library = library, .name = "LEAF"});

            a_design = root.create_design(DesignData{.library = library, .name = "A"});
            a_schematic = root.create_schematic(SchematicData{.design = a_design});
            inst_b = root.create_instance(InstanceData{.schematic = a_schematic, .name = "b", .reference_design = leaf_design});
            net_n1 = root.create_net(NetData{.schematic = a_schematic, .name = "n1"});

            top_design = root.create_design(DesignData{.library = library, .name = "TOP"});
            top_schematic = root.create_schematic(SchematicData{.design = top_design});
            inst_a = root.create_instance(InstanceData{.schematic = top_schematic, .name = "a", .reference_design = a_design});
            net_topnet = root.create_net(NetData{.schematic = top_schematic, .name = "topnet"});

            top_layout = root.create_layout(LayoutData{.design = top_design});
            placement_ab = root.create_placement(PlacementData{
                .layout = top_layout, .name = "a/b", .reference_design = leaf_design,
                .placement_status = PlacementStatus::PLACED});
            placement_filler = root.create_placement(PlacementData{
                .layout = top_layout, .name = "filler0", .reference_design = leaf_design,
                .placement_status = PlacementStatus::PLACED});
            route_n1 = root.create_route(RouteData{.layout = top_layout, .name = "a/n1", .is_special = false});
            route_unmatched = root.create_route(RouteData{.layout = top_layout, .name = "unmatched_net", .is_special = false});
            port_clk = root.create_physical_port(
                PhysicalPortData{.layout = top_layout, .name = "clk", .net_name = "topnet"});
            port_vdd = root.create_physical_port(
                PhysicalPortData{.layout = top_layout, .name = "vdd", .net_name = "unmatched_net"});
        }
    };
}

TEST(SchematicLayoutLinker, LinksAPlacementToItsMatchingNestedInstance)
{
    Fixture f;
    const PhysicalLinkResult result = link_physical(f.root);
    EXPECT_EQ(result.placements_linked, 1u);
    EXPECT_EQ(f.root.get_placement(f.placement_ab)->instance, f.inst_b);
    EXPECT_FALSE(f.root.get_placement(f.placement_ab)->physical_only);
}

TEST(SchematicLayoutLinker, MarksAnUnmatchedPlacementPhysicalOnlyAndWarns)
{
    Fixture f;
    const PhysicalLinkResult result = link_physical(f.root);
    EXPECT_EQ(result.placements_marked_physical_only, 1u);
    EXPECT_TRUE(f.root.get_placement(f.placement_filler)->physical_only);
    EXPECT_FALSE(f.root.get_placement(f.placement_filler)->instance.valid());
    EXPECT_TRUE(has_message_containing(result.messages, "WARNING: link: Placement 'filler0'"));
}

TEST(SchematicLayoutLinker, CreatesADefaultPlacementForAnInstanceWithNoneAtTopLevel)
{
    Fixture f;
    // "a" itself has no Placement anywhere in the fixture (only its own
    // nested "b" does, via "a/b") - link_physical must auto-create one.
    const PhysicalLinkResult result = link_physical(f.root);
    EXPECT_EQ(result.default_placements_created, 1u);

    PlacementId created;
    for (const PlacementId id : f.root.get_layout_placements(f.top_layout))
        if (f.root.get_placement(id)->instance == f.inst_a)
            created = id;
    ASSERT_TRUE(created.valid());
    const PlacementData *data = f.root.get_placement(created);
    EXPECT_EQ(data->name, "a");
    EXPECT_EQ(data->reference_design, f.a_design);
    ASSERT_TRUE(data->location.has_value());
    EXPECT_EQ(data->location->x, 0);
    EXPECT_EQ(data->location->y, 0);
    ASSERT_TRUE(data->orientation.has_value());
    EXPECT_EQ(*data->orientation, Orientation::N);
}

TEST(SchematicLayoutLinker, GenuinelyResolvedRowsAreNeverReprocessedOnARepeatedCall)
{
    // A permanent resolution (Placement.instance/Route.net/
    // PhysicalPort.net already valid, or an Instance that already has a
    // default Placement) must never be redone or re-counted - unlike a
    // still-unresolved row, which legitimately IS re-attempted every
    // call (see the next test).
    Fixture f;
    link_physical(f.root);
    const PhysicalLinkResult second = link_physical(f.root);
    EXPECT_EQ(second.placements_linked, 0u);
    EXPECT_EQ(second.default_placements_created, 0u);
    EXPECT_EQ(second.routes_linked, 0u);
    EXPECT_EQ(second.physical_ports_linked, 0u);
}

TEST(SchematicLayoutLinker, AStillUnresolvedRowIsReattemptedAndSucceedsOnceTheFixIsMade)
{
    // The whole point of `link` being re-runnable: a Placement that
    // failed to match keeps being retried (and, until fixed, keeps being
    // reported - matching link_unresolved_instances' own "keyed on
    // current resolved state, not on a prior attempt" convention) rather
    // than being permanently given up on after the first miss.
    Fixture f;
    const PhysicalLinkResult first = link_physical(f.root);
    EXPECT_EQ(first.placements_marked_physical_only, 1u);
    ASSERT_TRUE(f.root.get_placement(f.placement_filler)->physical_only);

    // Add the instance "filler0" was actually named after, then re-link.
    const InstanceId inst_filler =
        f.root.create_instance(InstanceData{.schematic = f.top_schematic, .name = "filler0", .reference_design = f.leaf_design});

    const PhysicalLinkResult second = link_physical(f.root);
    EXPECT_EQ(second.placements_linked, 1u);
    EXPECT_EQ(f.root.get_placement(f.placement_filler)->instance, inst_filler);
    EXPECT_FALSE(f.root.get_placement(f.placement_filler)->physical_only);
}

TEST(SchematicLayoutLinker, LinksARouteToItsMatchingNetThroughAHierarchicalName)
{
    Fixture f;
    const PhysicalLinkResult result = link_physical(f.root);
    EXPECT_EQ(result.routes_linked, 1u);
    EXPECT_EQ(f.root.get_route(f.route_n1)->net, f.net_n1);
}

TEST(SchematicLayoutLinker, UnmatchedRouteLogsAnError)
{
    Fixture f;
    const PhysicalLinkResult result = link_physical(f.root);
    EXPECT_FALSE(f.root.get_route(f.route_unmatched)->net.valid());
    EXPECT_TRUE(has_message_containing(result.messages, "ERROR: link: Route 'unmatched_net'"));
}

TEST(SchematicLayoutLinker, LinksAPhysicalPortToItsMatchingNet)
{
    Fixture f;
    const PhysicalLinkResult result = link_physical(f.root);
    EXPECT_EQ(result.physical_ports_linked, 1u);
    EXPECT_EQ(f.root.get_physical_port(f.port_clk)->net, f.net_topnet);
}

TEST(SchematicLayoutLinker, UnmatchedPhysicalPortLogsAWarningNotAnError)
{
    Fixture f;
    const PhysicalLinkResult result = link_physical(f.root);
    EXPECT_FALSE(f.root.get_physical_port(f.port_vdd)->net.valid());
    EXPECT_TRUE(has_message_containing(result.messages, "WARNING: link: PhysicalPort 'vdd'"));
}

TEST(SchematicLayoutLinker, LayoutWithNoSchematicIsSkippedWithoutError)
{
    Root root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "NOSCHEM"});
    const LayoutId layout = root.create_layout(LayoutData{.design = design});
    root.create_placement(PlacementData{
        .layout = layout, .name = "u1", .reference_design = design, .placement_status = PlacementStatus::PLACED});

    const PhysicalLinkResult result = link_physical(root);
    EXPECT_EQ(result.placements_linked, 0u);
    EXPECT_EQ(result.placements_marked_physical_only, 0u);
    EXPECT_TRUE(result.messages.empty());
}
