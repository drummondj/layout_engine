#include "../stages/viewport_cull_stage.hpp"
#include "synchronous_stage_runner.hpp"
#include <gtest/gtest.h>

#include <memory>

using namespace le;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using ViewportCullRunner = SynchronousStageRunner<ViewportCullStage, HierarchyResolverStage::OutputHandle, HierarchyResolverOutput, ViewRenderOptions>;

    // Same LEAF/BLOCK/TOP fixture as hierarchy_resolver_stage_test.cpp -
    // TOP places BLOCK once at (100, 100); BLOCK (a Layout) places LEAF
    // twice, at (10, 10) ("leaf0") and (500, 500) ("leaf1"). Run through
    // the real HierarchyResolverStage first (depth 2, so LEAF is
    // reachable - see that file's own depth-semantics comment) to get a
    // genuine Cold output, rather than hand-building one.
    //
    // World-space bboxes (BLOCK's own placement composes an identity
    // linear + (100, 100) translation onto TOP's own identity):
    //   BLOCK:  (100, 100) - (1100, 1100)
    //   leaf0:  (110, 110) - (120, 120)   [BLOCK's local (10,10)-(20,20) + (100,100)]
    //   leaf1:  (600, 600) - (610, 610)   [BLOCK's local (500,500)-(510,510) + (100,100)]
    struct ViewportCullStageFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            view_layers_handle = std::make_shared<const ViewLayerSet>(ViewLayerSet::build_for_technology(root, technology_id));

            const LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});

            const DesignId leaf_design = root.create_design(DesignData{.library = library_id, .name = "LEAF"});
            leaf_abstract = root.create_abstract(AbstractData{.design = leaf_design});
            root.create_shape(ShapeData{.abstract = leaf_abstract, .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});

            const DesignId block_design = root.create_design(DesignData{.library = library_id, .name = "BLOCK"});
            block_layout = root.create_layout(LayoutData{.design = block_design});
            root.create_shape(ShapeData{.layout = block_layout, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{1000, 1000}}}}});
            root.create_placement(PlacementData{.layout = block_layout, .name = "leaf0", .reference_design = leaf_design, .placement_status = PlacementStatus::PLACED, .location = Point{10, 10}, .orientation = Orientation::N});
            root.create_placement(PlacementData{.layout = block_layout, .name = "leaf1", .reference_design = leaf_design, .placement_status = PlacementStatus::PLACED, .location = Point{500, 500}, .orientation = Orientation::N});

            const DesignId top_design = root.create_design(DesignData{.library = library_id, .name = "TOP"});
            top_layout = root.create_layout(LayoutData{.design = top_design});
            root.create_shape(ShapeData{.layout = top_layout, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{5000, 5000}}}}});
            root.create_placement(PlacementData{.layout = top_layout, .name = "block0", .reference_design = block_design, .placement_status = PlacementStatus::PLACED, .location = Point{100, 100}, .orientation = Orientation::N});

            const ViewRenderOptions cold_options{.root = &root, .root_mutation_version = root.mutation_version(), .top_level = HierarchyId{top_layout}, .hierarchy_depth = 2};
            hierarchy_resolver_runner.run(view_layers_handle, 0, cold_options);
            cold_output = hierarchy_resolver_runner.last_handle();
        }

        ViewRenderOptions options_with_viewport(Rect viewport) const
        {
            return ViewRenderOptions{.root = &root, .root_mutation_version = root.mutation_version(), .top_level = HierarchyId{top_layout}, .hierarchy_depth = 2, .viewport = viewport};
        }

        Root root;
        TechnologyId technology_id;
        ViewLayerSetHandle view_layers_handle;
        LayoutId block_layout;
        LayoutId top_layout;
        AbstractId leaf_abstract;
        HierarchyResolverRunner hierarchy_resolver_runner{"HierarchyResolver"};
        HierarchyResolverStage::OutputHandle cold_output;
        ViewportCullRunner cull_runner{"ViewportCull"};
    };
}

TEST_F(ViewportCullStageFixture, ViewportCoveringEverythingCullsNothing)
{
    const ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}});
    const HierarchyResolverOutput &culled = cull_runner.run(cold_output, 0, options);

    ASSERT_EQ(culled.view_data.size(), cold_output->view_data.size());
    ASSERT_TRUE(culled.view_data.contains(HierarchyId{top_layout}));
    ASSERT_TRUE(culled.view_data.contains(HierarchyId{block_layout}));
    ASSERT_TRUE(culled.view_data.contains(HierarchyId{leaf_abstract}));
    EXPECT_EQ(culled.view_data.at(HierarchyId{top_layout}).placement_data.size(), 1u);
    EXPECT_EQ(culled.view_data.at(HierarchyId{block_layout}).placement_data.size(), 2u);
}

TEST_F(ViewportCullStageFixture, ViewportCoveringNothingLeavesOnlyTopLevel)
{
    const ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{-1000, -1000}, .ur = Point{-500, -500}});
    const HierarchyResolverOutput &culled = cull_runner.run(cold_output, 0, options);

    ASSERT_EQ(culled.view_data.size(), 1u);
    ASSERT_TRUE(culled.view_data.contains(HierarchyId{top_layout}));
    EXPECT_TRUE(culled.view_data.at(HierarchyId{top_layout}).placement_data.empty());

    // shapes are carried through unchanged regardless of culling - not
    // just equal in content, but the exact same ViewShapesHandle (a
    // shared_ptr copy, not a fresh vector) - this stage prunes
    // placements, not a node's own direct content.
    EXPECT_EQ(culled.view_data.at(HierarchyId{top_layout}).shapes.get(), cold_output->view_data.at(HierarchyId{top_layout}).shapes.get());
}

TEST_F(ViewportCullStageFixture, CullingComposesAncestorTransformsNotJustLocalBbox)
{
    // Chosen precisely so a *correct* (composed-transform) culling keeps
    // leaf1 (world bbox (600,600)-(610,610)) and drops leaf0 (world bbox
    // (110,110)-(120,120)), while a *buggy* implementation testing each
    // placement's own bbox directly against the viewport (skipping
    // BLOCK's own (100,100) translation) would incorrectly drop both.
    const ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{550, 550}, .ur = Point{650, 650}});
    const HierarchyResolverOutput &culled = cull_runner.run(cold_output, 0, options);

    ASSERT_TRUE(culled.view_data.contains(HierarchyId{top_layout}));
    ASSERT_TRUE(culled.view_data.contains(HierarchyId{block_layout}));
    ASSERT_TRUE(culled.view_data.contains(HierarchyId{leaf_abstract})); // leaf1 survived, so LEAF is still reachable

    const ViewData &top_data = culled.view_data.at(HierarchyId{top_layout});
    ASSERT_EQ(top_data.placement_data.size(), 1u);
    EXPECT_EQ(top_data.placement_data[0].id, HierarchyId{block_layout});

    const ViewData &block_data = culled.view_data.at(HierarchyId{block_layout});
    ASSERT_EQ(block_data.placement_data.size(), 1u); // leaf0 culled, leaf1 survives
    EXPECT_EQ(block_data.placement_data[0].location.x, 500);
    EXPECT_EQ(block_data.placement_data[0].location.y, 500);
}

TEST_F(ViewportCullStageFixture, ReusingCachedIndexAcrossViewportOnlyChangesStaysCorrect)
{
    // ViewportCullStage caches a spatial index per node keyed on the Cold
    // input's own identity (viewport_cull_stage.hpp's own doc comment) -
    // reused across calls that share the same cold_output, rebuilt only
    // when that identity changes. Querying the SAME runner (so the SAME
    // cached index) with two different viewports in sequence, in either
    // order, must still answer each one correctly - a stale-cache bug
    // would show up here as the second call's own result still matching
    // the first viewport instead of its own.
    const ViewRenderOptions everything = options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}});
    const HierarchyResolverOutput &full = cull_runner.run(cold_output, 0, everything);
    EXPECT_EQ(full.view_data.at(HierarchyId{block_layout}).placement_data.size(), 2u);

    const ViewRenderOptions narrow = options_with_viewport(Rect{.ll = Point{550, 550}, .ur = Point{650, 650}});
    const HierarchyResolverOutput &narrowed = cull_runner.run(cold_output, 0, narrow);
    ASSERT_EQ(narrowed.view_data.at(HierarchyId{block_layout}).placement_data.size(), 1u);
    EXPECT_EQ(narrowed.view_data.at(HierarchyId{block_layout}).placement_data[0].location.x, 500);

    const HierarchyResolverOutput &full_again = cull_runner.run(cold_output, 0, everything);
    EXPECT_EQ(full_again.view_data.at(HierarchyId{block_layout}).placement_data.size(), 2u);
}

TEST_F(ViewportCullStageFixture, NullInputProducesEmptyOutput)
{
    const ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}});
    const HierarchyResolverOutput &culled = cull_runner.run(nullptr, 0, options);
    EXPECT_TRUE(culled.view_data.empty());
}
