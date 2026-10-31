// Schematic<->Layout link mutation side-effects:
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
    // cleared (a chip-boundary pin can legitimately remain without a
    // netlist-level Net).
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
    const WireId wire = root.create_wire(WireData{.route = route});
    const ShapeId shape = root.create_shape(ShapeData{.owner = ShapeOwner::route(route)});

    ASSERT_EQ(le_delete_net_cascade(&handle, to_c(net)), 0);
    EXPECT_EQ(root.get_net(net), nullptr);
    EXPECT_EQ(root.get_route(route), nullptr);
    EXPECT_EQ(root.get_wire(wire), nullptr);
    EXPECT_EQ(root.get_shape(shape), nullptr);

    // Everything comes back at its own id, under its own parent.
    ASSERT_TRUE(handle.command_history.undo().replayed);
    ASSERT_NE(root.get_net(net), nullptr);
    EXPECT_EQ(root.get_schematic_nets(schematic), std::vector<NetId>{net});
    ASSERT_NE(root.get_route(route), nullptr);
    EXPECT_EQ(root.get_route(route)->net, net);
    EXPECT_EQ(root.get_layout_routes(layout), std::vector<RouteId>{route});
    EXPECT_EQ(root.get_route_wires(route), std::vector<WireId>{wire});
    EXPECT_EQ(root.get_route_shapes(route), std::vector<ShapeId>{shape});

    ASSERT_TRUE(handle.command_history.redo().replayed);
    EXPECT_EQ(root.get_route(route), nullptr);
    EXPECT_TRUE(root.get_layout_routes(layout).empty());
}

TEST(MutationCascade, DeleteNetFailsForUnknownId)
{
    LeHandle handle;
    EXPECT_NE(le_delete_net_cascade(&handle, LeNetId{.index = UINT32_MAX, .generation = 0}), 0);
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
    ASSERT_TRUE(f.handle.command_history.undo().replayed);

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

namespace
{
    // TOP instantiates MID twice (u1, u2); MID has Instance x (-> LEAF) and
    // Net n. TOP's Layout places each copy: u1, u1/x, u2, u2/x, with
    // Routes u1/n and u2/n - both copies' x and n are the same objects.
    struct SharedModule
    {
        LeHandle handle;
        Root &root = handle.root;
        LibraryId library = root.create_library(LibraryData{.name = "lib"});
        DesignId leaf = root.create_design(DesignData{.library = library, .name = "LEAF"});
        DesignId mid = root.create_design(DesignData{.library = library, .name = "MID"});
        SchematicId mid_schematic = root.create_schematic(SchematicData{.design = mid});
        InstanceId x = root.create_instance(InstanceData{.schematic = mid_schematic, .name = "x", .reference_design = leaf});
        NetId n = root.create_net(NetData{.schematic = mid_schematic, .name = "n"});
        DesignId top = root.create_design(DesignData{.library = library, .name = "TOP"});
        SchematicId top_schematic = root.create_schematic(SchematicData{.design = top});
        InstanceId u1 = root.create_instance(InstanceData{.schematic = top_schematic, .name = "u1", .reference_design = mid});
        InstanceId u2 = root.create_instance(InstanceData{.schematic = top_schematic, .name = "u2", .reference_design = mid});
        LayoutId layout = root.create_layout(LayoutData{.design = top});
        PlacementId p_u1 = place("u1", mid, u1);
        PlacementId p_u1x = place("u1/x", leaf, x);
        PlacementId p_u2 = place("u2", mid, u2);
        PlacementId p_u2x = place("u2/x", leaf, x);
        RouteId r_u1n = root.create_route(RouteData{.layout = layout, .name = "u1/n", .net = n});
        RouteId r_u2n = root.create_route(RouteData{.layout = layout, .name = "u2/n", .net = n});

        PlacementId place(const char *name, DesignId design, InstanceId instance)
        {
            return root.create_placement(PlacementData{.layout = layout, .name = name, .reference_design = design, .instance = instance});
        }
        const std::string &name(PlacementId id) { return root.get_placement(id)->name; }
    };
}

TEST(MutationCascade, RenamingAnInstanceOfAModuleInstantiatedTwiceRenamesBothCopies)
{
    SharedModule f;
    ASSERT_EQ(le_rename_instance_propagate(&f.handle, to_c(f.x), "y"), 0);
    EXPECT_EQ(f.name(f.p_u1x), "u1/y");
    EXPECT_EQ(f.name(f.p_u2x), "u2/y");
    EXPECT_EQ(f.name(f.p_u1), "u1");

    ASSERT_EQ(le_rename_instance_propagate(&f.handle, to_c(f.u2), "v2"), 0);
    EXPECT_EQ(f.name(f.p_u2), "v2");
    EXPECT_EQ(f.name(f.p_u2x), "v2/y");
    EXPECT_EQ(f.root.get_route(f.r_u2n)->name, "v2/n");
    EXPECT_EQ(f.name(f.p_u1x), "u1/y");
    EXPECT_EQ(f.root.get_route(f.r_u1n)->name, "u1/n");
}

TEST(MutationCascade, RenamingANetInAModuleInstantiatedTwiceKeepsEachRoutesPrefix)
{
    SharedModule f;
    ASSERT_EQ(le_rename_net_propagate(&f.handle, to_c(f.n), "m"), 0);
    EXPECT_EQ(f.root.get_route(f.r_u1n)->name, "u1/m");
    EXPECT_EQ(f.root.get_route(f.r_u2n)->name, "u2/m");
}

TEST(MutationCascade, RenamingKeepsEscapedSegmentsAndEscapesTheNewName)
{
    LeHandle handle;
    Root &root = handle.root;
    const LibraryId library = root.create_library(LibraryData{.name = "lib"});
    const DesignId leaf = root.create_design(DesignData{.library = library, .name = "LEAF"});
    const DesignId mid = root.create_design(DesignData{.library = library, .name = "MID"});
    const SchematicId mid_schematic = root.create_schematic(SchematicData{.design = mid});
    const InstanceId a = root.create_instance(InstanceData{.schematic = mid_schematic, .name = "a", .reference_design = leaf});
    const DesignId top = root.create_design(DesignData{.library = library, .name = "TOP"});
    const SchematicId top_schematic = root.create_schematic(SchematicData{.design = top});
    const InstanceId blk = root.create_instance(InstanceData{.schematic = top_schematic, .name = "blk[0]/q", .reference_design = mid});
    const LayoutId layout = root.create_layout(LayoutData{.design = top});
    // DEF escapes '[', ']' and a '/' inside a name.
    const PlacementId p_blk = root.create_placement(PlacementData{.layout = layout, .name = "blk\\[0\\]\\/q", .reference_design = mid, .instance = blk});
    const PlacementId p_a = root.create_placement(PlacementData{.layout = layout, .name = "blk\\[0\\]\\/q/a", .reference_design = leaf, .instance = a});
    const PlacementId p_deep = root.create_placement(PlacementData{.layout = layout, .name = "blk\\[0\\]\\/q/a/deep", .reference_design = leaf});
    const PlacementId p_sibling = root.create_placement(PlacementData{.layout = layout, .name = "blk\\[0\\]\\/q/ab", .reference_design = leaf});

    ASSERT_EQ(le_rename_instance_propagate(&handle, to_c(a), "b/c"), 0);
    EXPECT_EQ(root.get_placement(p_a)->name, "blk\\[0\\]\\/q/b\\/c");
    EXPECT_EQ(root.get_placement(p_deep)->name, "blk\\[0\\]\\/q/b\\/c/deep");
    EXPECT_EQ(root.get_placement(p_sibling)->name, "blk\\[0\\]\\/q/ab"); // whole segments only
    EXPECT_EQ(root.get_placement(p_blk)->name, "blk\\[0\\]\\/q");
}

TEST(MutationCascade, ARenameThatWouldClashIsRefusedAndChangesNothing)
{
    SharedModule f;
    const PlacementId taken = f.place("u2/y", f.leaf, InstanceId{});
    const uint64_t version = f.root.mutation_version();

    EXPECT_NE(le_rename_instance_propagate(&f.handle, to_c(f.x), "y"), 0);
    EXPECT_EQ(f.root.get_instance(f.x)->name, "x");
    EXPECT_EQ(f.name(f.p_u1x), "u1/x");
    EXPECT_EQ(f.name(f.p_u2x), "u2/x");
    EXPECT_EQ(f.name(taken), "u2/y");
    EXPECT_EQ(f.root.mutation_version(), version);
    EXPECT_EQ(f.handle.command_history.can_undo(), false);
}
