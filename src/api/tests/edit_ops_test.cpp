// The generated le::edit create and update ops: a refused edit changes
// nothing and records nothing.

#include "../edit_ops.hpp"
#include "../le_handle.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    // Two Layouts, a placement `p` in l1 and `q` in l2.
    struct TwoLayouts : ::testing::Test
    {
        LeHandle handle;
        Root &root = handle.root;
        LibraryId library = root.create_library(LibraryData{.name = "lib"});
        DesignId cell = root.create_design(DesignData{.library = library, .name = "cell"});
        LayoutId l1 = root.create_layout(LayoutData{.design = root.create_design(DesignData{.library = library, .name = "top1"})});
        LayoutId l2 = root.create_layout(LayoutData{.design = root.create_design(DesignData{.library = library, .name = "top2"})});
        PlacementId p = root.create_placement(PlacementData{.layout = l1, .name = "p", .reference_design = cell});
        PlacementId q = root.create_placement(PlacementData{.layout = l2, .name = "q", .reference_design = cell});
    };
}

TEST_F(TwoLayouts, AReparentOntoATakenNameIsRefusedAndChangesNothing)
{
    // An earlier command, to check undo still undoes it afterwards.
    handle.command_history.begin("rename q");
    ASSERT_TRUE(edit::update_placement(handle, q, PlacementChanges{.name = "q2"}));
    handle.command_history.end(true);

    const uint64_t version = root.mutation_version();
    handle.command_history.begin("move p");
    const auto moved = edit::update_placement(handle, p, PlacementChanges{.layout = l2, .name = "q2"});
    handle.command_history.end(false);
    ASSERT_FALSE(moved);
    EXPECT_EQ(moved.error(), "a sibling Placement with this name ('q2') already exists");

    EXPECT_EQ(root.get_placement(p)->layout, l1);
    EXPECT_EQ(root.get_placement(p)->name, "p");
    EXPECT_EQ(root.get_layout_placements(l1), std::vector<PlacementId>{p});
    EXPECT_EQ(root.get_layout_placements(l2), std::vector<PlacementId>{q});
    EXPECT_EQ(root.mutation_version(), version);

    const editing::ReplayResult undone = handle.command_history.undo();
    EXPECT_EQ(undone.label, "rename q");
    EXPECT_EQ(root.get_placement(q)->name, "q");
}

TEST_F(TwoLayouts, AReparentPlusRenameToAFreshNameSucceeds)
{
    // l2 holds a placement named like p: only p's new name matters.
    ASSERT_TRUE(edit::update_placement(handle, q, PlacementChanges{.name = "p"}));

    ASSERT_TRUE(edit::update_placement(handle, p, PlacementChanges{.layout = l2, .name = "fresh"}));
    EXPECT_EQ(root.get_placement(p)->layout, l2);
    EXPECT_EQ(root.get_placement_by_name(l2, "fresh"), p);
    EXPECT_EQ(root.get_placement_by_name(l2, "p"), q);
    EXPECT_TRUE(root.get_layout_placements(l1).empty());
}

TEST_F(TwoLayouts, ARefusedReferenceUpdateChangesNothing)
{
    const auto updated = edit::update_placement(handle, p, PlacementChanges{.reference_design = DesignId{999, 0}, .name = "renamed"});
    ASSERT_FALSE(updated);
    EXPECT_EQ(root.get_placement(p)->reference_design, cell);
    EXPECT_EQ(root.get_placement(p)->name, "p");
}

namespace
{
    // Design a instantiates b (u_b); b's and c's Schematics are empty.
    struct Hierarchy : ::testing::Test
    {
        LeHandle handle;
        Root &root = handle.root;
        LibraryId library = root.create_library(LibraryData{.name = "lib"});
        DesignId a = root.create_design(DesignData{.library = library, .name = "a"});
        DesignId b = root.create_design(DesignData{.library = library, .name = "b"});
        DesignId c = root.create_design(DesignData{.library = library, .name = "c"});
        SchematicId sa = root.create_schematic(SchematicData{.design = a});
        SchematicId sb = root.create_schematic(SchematicData{.design = b});
        SchematicId sc = root.create_schematic(SchematicData{.design = c});
        InstanceId u_b = root.create_instance(InstanceData{.schematic = sa, .name = "u_b", .reference_design = b});
    };
}

TEST_F(Hierarchy, CreatingARecursiveInstanceIsRefused)
{
    const auto self = edit::create_instance(handle, InstanceData{.schematic = sa, .name = "u_a", .reference_design = a});
    ASSERT_FALSE(self);
    EXPECT_EQ(self.error(), "instantiating design 'a' in design 'a' would make the hierarchy recursive");
    const auto through_b = edit::create_instance(handle, InstanceData{.schematic = sb, .name = "u_a", .reference_design = a});
    ASSERT_FALSE(through_b);
    EXPECT_EQ(through_b.error(), "instantiating design 'a' in design 'b' would make the hierarchy recursive");
    EXPECT_TRUE(root.get_schematic_instances(sb).empty());

    EXPECT_TRUE(edit::create_instance(handle, InstanceData{.schematic = sb, .name = "u_c", .reference_design = c}));
}

TEST_F(Hierarchy, AnUpdateMakingTheHierarchyRecursiveIsRefused)
{
    const InstanceId u_c = root.create_instance(InstanceData{.schematic = sb, .name = "u_c", .reference_design = c});
    const uint64_t version = root.mutation_version();

    const auto retargeted = edit::update_instance(handle, u_c, InstanceChanges{.reference_design = a});
    ASSERT_FALSE(retargeted);
    EXPECT_EQ(retargeted.error(), "instantiating design 'a' in design 'b' would make the hierarchy recursive");
    // Moving u_b (an instance of b) into b itself.
    EXPECT_FALSE(edit::update_instance(handle, u_b, InstanceChanges{.schematic = sb}));
    EXPECT_EQ(root.get_instance(u_c)->reference_design, c);
    EXPECT_EQ(root.get_instance(u_b)->schematic, sa);
    EXPECT_EQ(root.mutation_version(), version);

    EXPECT_TRUE(edit::update_instance(handle, u_c, InstanceChanges{.name = "renamed"}));
}

namespace
{
    // A Layout with n Routes (each with a Wire) and n Placements.
    struct BigLayout
    {
        LeHandle handle;
        Root &root = handle.root;
        LibraryId library = root.create_library(LibraryData{.name = "lib"});
        DesignId cell = root.create_design(DesignData{.library = library, .name = "cell"});
        LayoutId layout = root.create_layout(LayoutData{.design = root.create_design(DesignData{.library = library, .name = "top"})});
        std::vector<RouteId> routes;

        explicit BigLayout(int n)
        {
            for (int i = 0; i < n; ++i)
            {
                routes.push_back(root.create_route(RouteData{.layout = layout, .name = "n" + std::to_string(i)}));
                root.create_wire(WireData{.route = routes.back()});
                root.create_placement(PlacementData{.layout = layout, .name = "u" + std::to_string(i), .reference_design = cell});
            }
        }

        std::vector<std::string> route_names() const
        {
            std::vector<std::string> names;
            for (const RouteId id : root.get_layout_routes(layout))
                names.push_back(root.get_route(id)->name);
            return names;
        }
    };
}

TEST(EditOpsDelete, ACascadeLeavesNoChildListKeysAndUndoRestoresTheOrder)
{
    BigLayout d(50);
    const std::vector<std::string> names = d.route_names();
    const std::vector<RouteId> routes = d.root.get_layout_routes(d.layout);

    d.handle.command_history.begin("delete layout");
    ASSERT_TRUE(edit::delete_layout(d.handle, d.layout));
    d.handle.command_history.end(true);
    EXPECT_TRUE(d.root.get_route_ids().empty());
    EXPECT_TRUE(d.root.get_wire_ids().empty());
    EXPECT_TRUE(d.root.get_placement_ids().empty());
    // Nothing is left keyed by the deleted objects.
    EXPECT_FALSE(d.root.index().layout_routes.contains(d.layout));
    EXPECT_FALSE(d.root.index().layout_placements.contains(d.layout));
    EXPECT_FALSE(d.root.index().placement_by_name.contains(d.layout));
    EXPECT_FALSE(d.root.index().route_wires.contains(routes.front()));

    ASSERT_TRUE(d.handle.command_history.undo().replayed);
    EXPECT_EQ(d.root.get_layout_routes(d.layout), routes);
    EXPECT_EQ(d.route_names(), names);
    EXPECT_EQ(d.root.get_route_wires(routes.back()).size(), 1u);
    EXPECT_EQ(d.root.get_placement_by_name(d.layout, "u7").valid(), true);
}

TEST(EditOpsDelete, DeletingChildrenInAnyOrderMatchesTheCascade)
{
    BigLayout cascade(20), oldest_first(20), scattered(20);
    ASSERT_TRUE(edit::delete_layout(cascade.handle, cascade.layout));

    for (const RouteId id : oldest_first.routes)
        ASSERT_TRUE(edit::delete_route(oldest_first.handle, id));
    for (size_t i = 0; i < scattered.routes.size(); i += 2)
        ASSERT_TRUE(edit::delete_route(scattered.handle, scattered.routes[i]));
    for (size_t i = 1; i < scattered.routes.size(); i += 2)
        ASSERT_TRUE(edit::delete_route(scattered.handle, scattered.routes[i]));
    for (BigLayout *d : {&oldest_first, &scattered})
    {
        ASSERT_TRUE(edit::delete_layout(d->handle, d->layout));
        EXPECT_EQ(d->root.get_route_ids().size(), cascade.root.get_route_ids().size());
        EXPECT_EQ(d->root.get_wire_ids().size(), cascade.root.get_wire_ids().size());
        EXPECT_EQ(d->root.get_placement_ids().size(), cascade.root.get_placement_ids().size());
        EXPECT_EQ(d->root.index().layout_routes.size(), cascade.root.index().layout_routes.size());
        EXPECT_EQ(d->root.index().route_wires.size(), cascade.root.index().route_wires.size());
    }
    EXPECT_TRUE(cascade.root.index().route_wires.empty());
}
