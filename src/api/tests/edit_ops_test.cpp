// The generated le::edit update ops: a refused update changes nothing and
// records nothing.

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
