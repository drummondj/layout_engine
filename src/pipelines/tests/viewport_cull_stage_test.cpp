#include "../stages/rasterize_blend2d_stage.hpp"
#include "../stages/viewport_cull_stage.hpp"
#include "synchronous_stage_runner.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

using namespace le;

namespace
{
    std::vector<ViewPlacementData> placements_of(const ViewData &data)
    {
        std::vector<ViewPlacementData> placements;
        for_each_placement(data, [&](const ViewPlacementData &placement)
                           { placements.push_back(placement); });
        return placements;
    }

    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using ViewportCullRunner = SynchronousStageRunner<ViewportCullStage, HierarchyResolverStage::OutputHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using RasterizeRunner = SynchronousStageRunner<RasterizeBlend2DStage, HierarchyResolverStage::OutputHandle, RasterizeOutput, ViewRenderOptions>;

    void expect_rect_eq(const std::optional<Rect> &actual, Rect expected)
    {
        ASSERT_TRUE(actual.has_value());
        EXPECT_EQ(actual->ll.x, expected.ll.x);
        EXPECT_EQ(actual->ll.y, expected.ll.y);
        EXPECT_EQ(actual->ur.x, expected.ur.x);
        EXPECT_EQ(actual->ur.y, expected.ur.y);
    }

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
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING", .width = 4});
            view_layers_handle = std::make_shared<const ViewLayerSet>(ViewLayerSet::build_for_technology(root, technology_id));

            const LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});

            const DesignId leaf_design = root.create_design(DesignData{.library = library_id, .name = "LEAF"});
            leaf_abstract = root.create_abstract(AbstractData{.design = leaf_design});
            root.create_shape(ShapeData{.owner = le::ShapeOwner::abstract(leaf_abstract), .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});

            const DesignId block_design = root.create_design(DesignData{.library = library_id, .name = "BLOCK"});
            block_layout = root.create_layout(LayoutData{.design = block_design});
            root.create_shape(ShapeData{.owner = le::ShapeOwner::layout(block_layout), .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{1000, 1000}}}}});
            root.create_placement(PlacementData{.layout = block_layout, .name = "leaf0", .reference_design = leaf_design, .placement_status = PlacementStatus::PLACED, .location = Point{10, 10}, .orientation = Orientation::N});
            root.create_placement(PlacementData{.layout = block_layout, .name = "leaf1", .reference_design = leaf_design, .placement_status = PlacementStatus::PLACED, .location = Point{500, 500}, .orientation = Orientation::N});

            const DesignId top_design = root.create_design(DesignData{.library = library_id, .name = "TOP"});
            top_layout = root.create_layout(LayoutData{.design = top_design});
            root.create_shape(ShapeData{.owner = le::ShapeOwner::layout(top_layout), .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{5000, 5000}}}}});
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
        LayerId m1;
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
    EXPECT_EQ(placement_count(culled.view_data.at(HierarchyId{top_layout})), 1u);
    EXPECT_EQ(placement_count(culled.view_data.at(HierarchyId{block_layout})), 2u);
}

TEST_F(ViewportCullStageFixture, ViewportCoveringNothingLeavesOnlyTopLevel)
{
    const ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{-1000, -1000}, .ur = Point{-500, -500}});
    const HierarchyResolverOutput &culled = cull_runner.run(cold_output, 0, options);

    ASSERT_EQ(culled.view_data.size(), 1u);
    ASSERT_TRUE(culled.view_data.contains(HierarchyId{top_layout}));
    EXPECT_TRUE((placement_count(culled.view_data.at(HierarchyId{top_layout})) == 0));

    // shapes are carried through unchanged regardless of culling - not
    // just equal in content, but the exact same ViewShapesHandle (a
    // shared_ptr copy, not a fresh vector) - this stage prunes
    // placements, not a node's own direct content.
    EXPECT_EQ(culled.view_data.at(HierarchyId{top_layout}).chunks[0].shapes.get(), cold_output->view_data.at(HierarchyId{top_layout}).chunks[0].shapes.get());
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
    ASSERT_EQ(placement_count(top_data), 1u);
    EXPECT_EQ(placements_of(top_data)[0].id, HierarchyId{block_layout});

    const ViewData &block_data = culled.view_data.at(HierarchyId{block_layout});
    ASSERT_EQ(placement_count(block_data), 1u); // leaf0 culled, leaf1 survives
    EXPECT_EQ(placements_of(block_data)[0].location.x, 500);
    EXPECT_EQ(placements_of(block_data)[0].location.y, 500);
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
    EXPECT_EQ(placement_count(full.view_data.at(HierarchyId{block_layout})), 2u);

    const ViewRenderOptions narrow = options_with_viewport(Rect{.ll = Point{550, 550}, .ur = Point{650, 650}});
    const HierarchyResolverOutput &narrowed = cull_runner.run(cold_output, 0, narrow);
    ASSERT_EQ(placement_count(narrowed.view_data.at(HierarchyId{block_layout})), 1u);
    EXPECT_EQ(placements_of(narrowed.view_data.at(HierarchyId{block_layout}))[0].location.x, 500);

    const HierarchyResolverOutput &full_again = cull_runner.run(cold_output, 0, everything);
    EXPECT_EQ(placement_count(full_again.view_data.at(HierarchyId{block_layout})), 2u);
}

TEST_F(ViewportCullStageFixture, SubPixelPlacementIsCulledEvenWhenItOverlapsTheViewport)
{
    // leaf0's own world bbox is (110,110)-(120,120) - a real 10x10 dbu
    // footprint, comfortably non-sub-pixel at scale 1.0 (every other test
    // here uses that default). At scale 0.05, that same placement's own
    // on-screen size is 10*0.05 = 0.5px in both dimensions - genuinely
    // sub-pixel (bbox_is_sub_pixel's own threshold) - even though its own
    // bbox still fully overlaps a viewport that covers everything, it
    // must not survive, and BLOCK's own leaf1 sibling (same size, same
    // scale) must not either.
    ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}});
    options.scale = 0.05;
    const HierarchyResolverOutput &culled = cull_runner.run(cold_output, 0, options);

    ASSERT_TRUE(culled.view_data.contains(HierarchyId{top_layout}));
    ASSERT_TRUE(culled.view_data.contains(HierarchyId{block_layout}));
    // Neither leaf0 nor leaf1 survived, so LEAF itself is unreachable -
    // the same "no surviving placement anywhere means never visited"
    // principle this class's own doc comment already documents for
    // viewport-overlap culling now applies to sub-pixel culling too.
    EXPECT_FALSE(culled.view_data.contains(HierarchyId{leaf_abstract}));

    EXPECT_TRUE((placement_count(culled.view_data.at(HierarchyId{block_layout})) == 0));
}

TEST_F(ViewportCullStageFixture, NonSubPixelPlacementSurvivesTheSameSubPixelCheck)
{
    // BLOCK's own world bbox (100,100)-(1100,1100), 1000x1000 dbu, is
    // nowhere near sub-pixel even at the same aggressively-zoomed-out
    // scale (0.05) the sibling test above uses to cull the much smaller
    // LEAF placements - confirms the new check is a real size threshold,
    // not an accidental blanket cull under a small scale.
    ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}});
    options.scale = 0.05;
    const HierarchyResolverOutput &culled = cull_runner.run(cold_output, 0, options);

    ASSERT_TRUE(culled.view_data.contains(HierarchyId{top_layout}));
    ASSERT_EQ(placement_count(culled.view_data.at(HierarchyId{top_layout})), 1u);
    EXPECT_EQ(placements_of(culled.view_data.at(HierarchyId{top_layout}))[0].id, HierarchyId{block_layout});
}

TEST_F(ViewportCullStageFixture, NullInputProducesEmptyOutput)
{
    const ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}});
    const HierarchyResolverOutput &culled = cull_runner.run(nullptr, 0, options);
    EXPECT_TRUE(culled.view_data.empty());
}

// An edit that leaves a node's placements alone (a route edit, say) makes
// HierarchyResolverStage share that node's placement vector, so the
// per-node placement index is reused rather than rebuilt; moving a
// placement rebuilds only its own Layout's index.
TEST_F(ViewportCullStageFixture, PlacementIndicesSurviveEditsThatDontTouchThem)
{
    const ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}});
    cull_runner.run(cold_output, 0, options);
    const std::size_t initial_builds = cull_runner.stage().index_builds(); // TOP, BLOCK, LEAF

    const RouteId route = root.create_route(RouteData{.layout = top_layout});
    root.create_shape(ShapeData{.owner = le::ShapeOwner::route(route), .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});
    root.bump_mutation_version();
    hierarchy_resolver_runner.run(view_layers_handle, 0, options_with_viewport(Rect{}));
    ASSERT_TRUE(hierarchy_resolver_runner.stage().last_compute_was_incremental());
    cull_runner.run(hierarchy_resolver_runner.last_handle(), 1, options);
    EXPECT_EQ(cull_runner.stage().index_builds(), initial_builds);

    PlacementId leaf1;
    for (const PlacementId id : root.get_layout_placements(block_layout))
        if (root.get_placement(id)->name == "leaf1")
            leaf1 = id;
    ASSERT_TRUE(root.update_placement(leaf1, block_layout, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, Point{600, 600}, std::nullopt, std::nullopt, std::nullopt));
    root.bump_mutation_version();
    hierarchy_resolver_runner.run(view_layers_handle, 0, options_with_viewport(Rect{}));
    cull_runner.run(hierarchy_resolver_runner.last_handle(), 2, options);
    EXPECT_EQ(cull_runner.stage().index_builds(), initial_builds + 1); // BLOCK's only
}

namespace
{
    ViewRenderOptions with_hidden(ViewRenderOptions options, ObjectFilterSets hidden)
    {
        options.hidden_objects = std::move(hidden);
        return options;
    }

    // Every set bit across `data`'s chunk masks: hidden route shapes, then
    // hidden placements.
    std::pair<std::size_t, std::size_t> hidden_counts(const ViewData &data)
    {
        std::size_t shapes = 0;
        std::size_t placements = 0;
        for (const ChunkVisibilityHandle &visibility : data.chunk_visibility)
            if (visibility)
            {
                for (const auto &[layer, bits] : visibility->hidden_shapes)
                    shapes += static_cast<std::size_t>(std::ranges::count(bits, true));
                placements += static_cast<std::size_t>(std::ranges::count(visibility->hidden_placements, true));
            }
        return {shapes, placements};
    }
}

TEST_F(ViewportCullStageFixture, AHiddenPlacementTypeIsNeitherDescendedIntoNorDrawn)
{
    root.get_abstract(leaf_abstract)->type = "CORE";
    const ViewRenderOptions everything = options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}});

    const HierarchyResolverOutput &hidden = cull_runner.run(cold_output, 0, with_hidden(everything, {.placement_types = {"CORE"}}));
    ASSERT_TRUE(hidden.view_data.contains(HierarchyId{block_layout}));
    EXPECT_FALSE(hidden.view_data.contains(HierarchyId{leaf_abstract}));
    const ViewData &block = hidden.view_data.at(HierarchyId{block_layout});
    EXPECT_EQ(placement_count(block), 0u);
    ASSERT_EQ(block.chunk_visibility.size(), block.chunks.size());
    ASSERT_TRUE(block.chunk_visibility[block.placement_chunk_offset]);
    EXPECT_EQ(block.chunk_visibility[block.placement_chunk_offset]->hidden_placements, (std::vector<bool>{true, true})); // leaf0/leaf1 rects and labels
    EXPECT_EQ(placement_count(hidden.view_data.at(HierarchyId{top_layout})), 1u); // BLOCK is untyped

    const HierarchyResolverOutput &shown = cull_runner.run(cold_output, 0, everything);
    EXPECT_TRUE(shown.view_data.contains(HierarchyId{leaf_abstract}));
    EXPECT_TRUE(shown.view_data.at(HierarchyId{block_layout}).chunk_visibility.empty());
}

TEST_F(ViewportCullStageFixture, AHiddenRouteUseMasksExactlyItsRoutesShapes)
{
    const RouteId power = root.create_route(RouteData{.layout = top_layout, .name = "VDD", .use = std::string("POWER")});
    root.create_shape(ShapeData{.owner = le::ShapeOwner::route(power), .rects = {Rect{.ll = Point{0, 0}, .ur = Point{50, 5}}}});
    root.create_shape(ShapeData{.owner = le::ShapeOwner::route(power), .rects = {Rect{.ll = Point{0, 20}, .ur = Point{50, 25}}}});
    const RouteId signal = root.create_route(RouteData{.layout = top_layout, .name = "n1"});
    root.create_shape(ShapeData{.owner = le::ShapeOwner::route(signal), .rects = {Rect{.ll = Point{0, 10}, .ur = Point{50, 15}}}});
    root.bump_mutation_version();
    hierarchy_resolver_runner.run(view_layers_handle, 0, options_with_viewport(Rect{}));
    const ViewRenderOptions everything = options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}});

    const HierarchyResolverOutput &power_hidden = cull_runner.run(hierarchy_resolver_runner.last_handle(), 1, with_hidden(everything, {.route_uses = {"POWER"}}));
    EXPECT_EQ(hidden_counts(power_hidden.view_data.at(HierarchyId{top_layout})), (std::pair<std::size_t, std::size_t>{2, 0}));

    const HierarchyResolverOutput &unset_hidden = cull_runner.run(hierarchy_resolver_runner.last_handle(), 1, with_hidden(everything, {.route_uses = {"SIGNAL"}})); // n1 has no USE
    EXPECT_EQ(hidden_counts(unset_hidden.view_data.at(HierarchyId{top_layout})), (std::pair<std::size_t, std::size_t>{1, 0}));
}

TEST_F(ViewportCullStageFixture, AHiddenRouteUseMasksItsWiresToo)
{
    const RouteId power = root.create_route(RouteData{.layout = top_layout, .name = "VDD", .use = std::string("POWER")});
    WireBuilder builder(root, m1);
    ASSERT_TRUE(builder.add_path(Path{.width = 4, .polygon = {.points = {Point{0, 0}, Point{50, 0}}}}));
    root.create_wire(std::move(builder).build(power));
    const RouteId signal = root.create_route(RouteData{.layout = top_layout, .name = "n1", .use = std::string("SIGNAL")});
    WireBuilder signal_builder(root, m1);
    ASSERT_TRUE(signal_builder.add_path(Path{.width = 4, .polygon = {.points = {Point{0, 10}, Point{50, 10}}}}));
    root.create_wire(std::move(signal_builder).build(signal));
    root.bump_mutation_version();
    hierarchy_resolver_runner.run(view_layers_handle, 0, options_with_viewport(Rect{}));
    const ViewRenderOptions everything = options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}});

    const HierarchyResolverOutput &power_hidden = cull_runner.run(hierarchy_resolver_runner.last_handle(), 1, with_hidden(everything, {.route_uses = {"POWER"}}));
    EXPECT_EQ(hidden_counts(power_hidden.view_data.at(HierarchyId{top_layout})), (std::pair<std::size_t, std::size_t>{1, 0}));
}

TEST_F(ViewportCullStageFixture, MasksAreReusedAcrossPansAndEditsAndRebuiltWhenTheFilterChanges)
{
    root.get_abstract(leaf_abstract)->type = "CORE";
    const ObjectFilterSets filter{.placement_types = {"CORE"}};
    cull_runner.run(cold_output, 0, with_hidden(options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}}), filter));
    const std::size_t initial = cull_runner.stage().visibility_builds();
    ASSERT_GT(initial, 0u);

    cull_runner.run(cold_output, 0, with_hidden(options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{9000, 9000}}), filter)); // a pan
    EXPECT_EQ(cull_runner.stage().visibility_builds(), initial);

    // Moving TOP's BLOCK placement rebuilds TOP's placement tile only.
    const PlacementId block0 = root.get_layout_placements(top_layout).front();
    ASSERT_TRUE(root.update_placement(block0, top_layout, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, Point{200, 200}, std::nullopt, std::nullopt, std::nullopt));
    root.bump_mutation_version();
    hierarchy_resolver_runner.run(view_layers_handle, 0, options_with_viewport(Rect{}));
    ASSERT_TRUE(hierarchy_resolver_runner.stage().last_compute_was_incremental());
    cull_runner.run(hierarchy_resolver_runner.last_handle(), 1, with_hidden(options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}}), filter));
    EXPECT_EQ(cull_runner.stage().visibility_builds(), initial + 1);

    cull_runner.run(hierarchy_resolver_runner.last_handle(), 1, with_hidden(options_with_viewport(Rect{.ll = Point{0, 0}, .ur = Point{10000, 10000}}), {.route_uses = {"POWER"}}));
    EXPECT_GT(cull_runner.stage().visibility_builds(), initial + 1);
}

TEST_F(ViewportCullStageFixture, NestedVisibleRegionIsTheViewportInTheNodesOwnSpace)
{
    const ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{550, 550}, .ur = Point{650, 650}});
    const HierarchyResolverOutput &culled = cull_runner.run(cold_output, 0, options);

    expect_rect_eq(culled.view_data.at(HierarchyId{top_layout}).visible_region, Rect{.ll = Point{550, 550}, .ur = Point{650, 650}});
    // BLOCK is placed at (100,100); LEAF (leaf1 at BLOCK-local (500,500)) is wholly inside.
    expect_rect_eq(culled.view_data.at(HierarchyId{block_layout}).visible_region, Rect{.ll = Point{450, 450}, .ur = Point{550, 550}});
    expect_rect_eq(culled.view_data.at(HierarchyId{leaf_abstract}).visible_region, Rect{.ll = Point{0, 0}, .ur = Point{10, 10}});
}

TEST_F(ViewportCullStageFixture, SharedNodeKeepsChildrenAnyOfItsInstancesShows)
{
    // A second BLOCK at (2000,100). The viewport shows BLOCK-local
    // (900,0)-(1000,30) through block0 (no leaves there) and (0,0)-(30,30)
    // through block1, which holds leaf0 - culling BLOCK against only its
    // first instance's view would drop leaf0.
    root.create_placement(PlacementData{.layout = top_layout, .name = "block1", .reference_design = root.get_layout(block_layout)->design, .placement_status = PlacementStatus::PLACED, .location = Point{2000, 100}, .orientation = Orientation::N});
    const ViewRenderOptions cold_options{.root = &root, .root_mutation_version = root.mutation_version(), .top_level = HierarchyId{top_layout}, .hierarchy_depth = 2};
    hierarchy_resolver_runner.run(view_layers_handle, 1, cold_options);

    const ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{1000, 100}, .ur = Point{2030, 130}});
    const HierarchyResolverOutput &culled = cull_runner.run(hierarchy_resolver_runner.last_handle(), 1, options);

    ASSERT_TRUE(culled.view_data.contains(HierarchyId{block_layout}));
    const ViewData &block_data = culled.view_data.at(HierarchyId{block_layout});
    expect_rect_eq(block_data.visible_region, Rect{.ll = Point{0, 0}, .ur = Point{1000, 30}});
    ASSERT_EQ(placement_count(block_data), 1u);
    EXPECT_EQ(placements_of(block_data)[0].location.x, 10); // leaf0
    EXPECT_TRUE(culled.view_data.contains(HierarchyId{leaf_abstract}));
}

TEST_F(ViewportCullStageFixture, RasterizeSizesANestedNodeToItsVisibleRegion)
{
    // At scale 10 BLOCK's whole 1000-dbu extent would be 10000 px a side;
    // only the 100x100 dbu the viewport shows is rasterized.
    ViewRenderOptions options = options_with_viewport(Rect{.ll = Point{550, 550}, .ur = Point{650, 650}});
    options.scale = 10.0;
    cull_runner.run(cold_output, 0, options);
    RasterizeRunner rasterize_runner{"Rasterize"};
    const RasterizeOutput &rasterized = rasterize_runner.run(cull_runner.last_handle(), 0, options);

    const RasterizedImage &block_image = rasterized.images.at(HierarchyId{block_layout});
    EXPECT_EQ(block_image.image.width(), 1000);
    EXPECT_EQ(block_image.image.height(), 1000);
    EXPECT_EQ(block_image.local_origin.x, 450);
    EXPECT_EQ(block_image.local_origin.y, 450);
}
