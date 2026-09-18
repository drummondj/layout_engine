// Phase 5 mutation side-effects (LINKING_STRATEGY_RESEARCH.md section 5):
// le_delete_net_cascade/le_rename_net_propagate/le_rename_instance_propagate
// (api.hpp). Built directly against LeHandle (like le_handle_test.cpp -
// "constructed directly, no C API layer" for the *setup* side, since
// these fixtures need no Technology/LEF read at all - every object here
// is created via Root:: directly, sidestepping le_create_placement's own
// unconditional "no Technology has been read yet" gate, which nothing
// under test actually depends on), but exercising the real api.hpp
// mutation functions themselves (not Root:: directly) so the transaction-
// recording/locking wrapper is covered too, not just
// rename_propagation.hpp's own pure logic.

#include "../api.hpp"
#include "../le_handle.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    LeNetId to_c(NetId id) { return LeNetId{.index = id.index, .generation = id.generation}; }
    LeInstanceId to_c(InstanceId id) { return LeInstanceId{.index = id.index, .generation = id.generation}; }
}

TEST(MutationCascade, DeleteNetCascadesToRouteDeleteAndClearsDanglingReferences)
{
    LeHandle handle;
    Root &root = handle.root;

    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "TOP"});
    const SchematicId schematic = root.create_schematic(SchematicData{.design = design});
    const InstanceId instance =
        root.create_instance(InstanceData{.schematic = schematic, .name = "u1", .reference_design = design});
    const NetId net = root.create_net(NetData{.schematic = schematic, .name = "n1"});
    const PinId pin = root.create_pin(PinData{.instance = instance, .name = "A", .net = net});
    const PortId port = root.create_port(PortData{.schematic = schematic, .name = "n1_port", .net = net});

    const LayoutId layout = root.create_layout(LayoutData{.design = design});
    const RouteId route = root.create_route(RouteData{.layout = layout, .name = "n1", .net = net});
    const PhysicalPortId physical_port = root.create_physical_port(
        PhysicalPortData{.layout = layout, .name = "n1_pin", .net_name = "n1", .net = net});

    ASSERT_EQ(le_delete_net_cascade(&handle, to_c(net)), 0);

    EXPECT_EQ(root.get_net(net), nullptr);
    EXPECT_EQ(root.get_route(route), nullptr);

    const PinData *pin_after = root.get_pin(pin);
    ASSERT_NE(pin_after, nullptr);
    EXPECT_FALSE(pin_after->net.valid());

    const PortData *port_after = root.get_port(port);
    ASSERT_NE(port_after, nullptr);
    EXPECT_FALSE(port_after->net.valid());

    // PhysicalPort itself is *not* deleted - only its own .net link is
    // cleared (LINKING_STRATEGY_RESEARCH.md section 5a: a chip-boundary
    // pin can legitimately remain without a netlist-level Net).
    const PhysicalPortData *port_data_after = root.get_physical_port(physical_port);
    ASSERT_NE(port_data_after, nullptr);
    EXPECT_FALSE(port_data_after->net.valid());
    EXPECT_EQ(port_data_after->net_name, "n1");
}

TEST(MutationCascade, DeleteNetIsUndoable)
{
    LeHandle handle;
    Root &root = handle.root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "TOP"});
    const SchematicId schematic = root.create_schematic(SchematicData{.design = design});
    const NetId net = root.create_net(NetData{.schematic = schematic, .name = "n1"});
    const LayoutId layout = root.create_layout(LayoutData{.design = design});
    const RouteId route = root.create_route(RouteData{.layout = layout, .name = "n1", .net = net});

    ASSERT_EQ(le_delete_net_cascade(&handle, to_c(net)), 0);
    EXPECT_EQ(root.get_net(net), nullptr);
    EXPECT_EQ(root.get_route(route), nullptr);

    ASSERT_TRUE(handle.command_history.undo(root));
    // Both the recreated Net and Route get fresh ids (Pool::create()
    // never reuses the original one - see Transaction's own comment) -
    // find them by name/schematic instead of by the now-stale `net`/
    // `route` ids captured before the delete.
    NetId recreated_net;
    for (const NetId id : root.get_schematic_nets(schematic))
        if (const NetData *n = root.get_net(id); n && n->name == "n1")
            recreated_net = id;
    ASSERT_TRUE(recreated_net.valid());

    bool found_route_with_right_net = false;
    for (const RouteId id : root.get_layout_routes(layout))
        if (const RouteData *r = root.get_route(id); r && r->name == "n1")
            found_route_with_right_net = (r->net == recreated_net);
    EXPECT_TRUE(found_route_with_right_net);
}

TEST(MutationCascade, DeleteNetFailsForUnknownId)
{
    LeHandle handle;
    EXPECT_NE(le_delete_net_cascade(&handle, LeNetId{.index = UINT32_MAX, .generation = 0}), 0);
    EXPECT_FALSE(handle.messages.empty());
}

TEST(MutationCascade, RenameNetPropagatesToLinkedRouteAndPhysicalPort)
{
    LeHandle handle;
    Root &root = handle.root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "TOP"});
    const SchematicId schematic = root.create_schematic(SchematicData{.design = design});
    const NetId net = root.create_net(NetData{.schematic = schematic, .name = "n1"});
    const LayoutId layout = root.create_layout(LayoutData{.design = design});
    const RouteId route = root.create_route(RouteData{.layout = layout, .name = "n1", .net = net});
    const PhysicalPortId physical_port =
        root.create_physical_port(PhysicalPortData{.layout = layout, .name = "n1", .net_name = "n1", .net = net});

    ASSERT_EQ(le_rename_net_propagate(&handle, to_c(net), "n1_renamed"), 0);

    EXPECT_EQ(root.get_net(net)->name, "n1_renamed");
    EXPECT_EQ(root.get_route(route)->name, "n1_renamed");
    EXPECT_EQ(root.get_physical_port(physical_port)->name, "n1_renamed");
    // net_name (the raw DEF-read string) is untouched - only the
    // resolved link's own display name changes.
    EXPECT_EQ(root.get_physical_port(physical_port)->net_name, "n1");
}

TEST(MutationCascade, RenameNetRejectsEmptyName)
{
    LeHandle handle;
    Root &root = handle.root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "TOP"});
    const SchematicId schematic = root.create_schematic(SchematicData{.design = design});
    const NetId net = root.create_net(NetData{.schematic = schematic, .name = "n1"});

    EXPECT_NE(le_rename_net_propagate(&handle, to_c(net), ""), 0);
    EXPECT_NE(le_rename_net_propagate(&handle, to_c(net), nullptr), 0);
    EXPECT_EQ(root.get_net(net)->name, "n1");
}

// Builds:
//   TOP.schematic: Instance "a" (-> A)
//     A.schematic: Instance "b" (-> LEAF, a hard macro), Net "n1"
//   TOP.layout: Placement "a" (linked to Instance "a"),
//               Placement "a/b" (linked to Instance "b"),
//               Route "a/n1" (linked to Net "n1")
// Renaming "a" to "a_renamed" should rename Placement "a" ->
// "a_renamed", Placement "a/b" -> "a_renamed/b", and Route "a/n1" ->
// "a_renamed/n1".
namespace
{
    struct RenameInstanceFixture
    {
        LeHandle handle;
        Root &root = handle.root;
        DesignId leaf_design, a_design, top_design;
        SchematicId top_schematic, a_schematic;
        LayoutId top_layout;
        InstanceId inst_a, inst_b;
        NetId net_n1;
        PlacementId placement_a, placement_ab;
        RouteId route_n1;

        RenameInstanceFixture()
        {
            const LibraryId library = root.create_library(LibraryData{.name = "lib"});
            leaf_design = root.create_design(DesignData{.library = library, .name = "LEAF"});

            a_design = root.create_design(DesignData{.library = library, .name = "A"});
            a_schematic = root.create_schematic(SchematicData{.design = a_design});
            inst_b = root.create_instance(
                InstanceData{.schematic = a_schematic, .name = "b", .reference_design = leaf_design});
            net_n1 = root.create_net(NetData{.schematic = a_schematic, .name = "n1"});

            top_design = root.create_design(DesignData{.library = library, .name = "TOP"});
            top_schematic = root.create_schematic(SchematicData{.design = top_design});
            inst_a = root.create_instance(
                InstanceData{.schematic = top_schematic, .name = "a", .reference_design = a_design});

            top_layout = root.create_layout(LayoutData{.design = top_design});
            placement_a = root.create_placement(PlacementData{.layout = top_layout, .name = "a", .instance = inst_a});
            placement_ab =
                root.create_placement(PlacementData{.layout = top_layout, .name = "a/b", .instance = inst_b});
            route_n1 = root.create_route(RouteData{.layout = top_layout, .name = "a/n1", .net = net_n1});
        }
    };
}

TEST(MutationCascade, RenameInstancePropagatesToOwnAndDescendantPlacementsAndRoutes)
{
    RenameInstanceFixture f;

    ASSERT_EQ(le_rename_instance_propagate(&f.handle, to_c(f.inst_a), "a_renamed"), 0);

    EXPECT_EQ(f.root.get_instance(f.inst_a)->name, "a_renamed");
    EXPECT_EQ(f.root.get_placement(f.placement_a)->name, "a_renamed");
    EXPECT_EQ(f.root.get_placement(f.placement_ab)->name, "a_renamed/b");
    EXPECT_EQ(f.root.get_route(f.route_n1)->name, "a_renamed/n1");
}

TEST(MutationCascade, RenameInstanceIsUndoable)
{
    RenameInstanceFixture f;
    ASSERT_EQ(le_rename_instance_propagate(&f.handle, to_c(f.inst_a), "a_renamed"), 0);
    ASSERT_TRUE(f.handle.command_history.undo(f.root));

    EXPECT_EQ(f.root.get_instance(f.inst_a)->name, "a");
    EXPECT_EQ(f.root.get_placement(f.placement_a)->name, "a");
    EXPECT_EQ(f.root.get_placement(f.placement_ab)->name, "a/b");
    EXPECT_EQ(f.root.get_route(f.route_n1)->name, "a/n1");
}

TEST(MutationCascade, RenameInstanceWithNoLinkedPlacementSkipsDescendantPropagation)
{
    // Known limitation (rename_propagation.hpp's own doc comment): an
    // Instance with no linked Placement yet has no recoverable path
    // prefix, so descendant propagation is skipped entirely rather than
    // guessing a wrong one - the Instance itself still renames.
    LeHandle handle;
    Root &root = handle.root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "TOP"});
    const SchematicId schematic = root.create_schematic(SchematicData{.design = design});
    const InstanceId instance =
        root.create_instance(InstanceData{.schematic = schematic, .name = "a", .reference_design = design});

    ASSERT_EQ(le_rename_instance_propagate(&handle, to_c(instance), "a_renamed"), 0);
    EXPECT_EQ(root.get_instance(instance)->name, "a_renamed");
}

TEST(MutationCascade, RenameInstanceRejectsEmptyName)
{
    LeHandle handle;
    Root &root = handle.root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId design = root.create_design(DesignData{.library = library, .name = "TOP"});
    const SchematicId schematic = root.create_schematic(SchematicData{.design = design});
    const InstanceId instance =
        root.create_instance(InstanceData{.schematic = schematic, .name = "a", .reference_design = design});

    EXPECT_NE(le_rename_instance_propagate(&handle, to_c(instance), ""), 0);
    EXPECT_EQ(root.get_instance(instance)->name, "a");
}
