#include "../stages/hierarchy_resolver_stage.hpp"
#include "synchronous_stage_runner.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace le;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, HierarchyResolverInput, HierarchyResolverOutput, ColdPipelineOptions>;

    // Fixture hierarchy:
    //   TOP (Layout only) - diearea, 1 placement of BLOCK
    //     BLOCK (Layout + Abstract) - diearea, 2 placements of LEAF
    //       LEAF (Abstract only) - boundary, 1 Terminal (1 Shape), 1 Obstruction (1 Shape)
    //
    // BLOCK having both a Layout and an Abstract is what exercises
    // resolve_design_target's own depth-based dispatch: placed at
    // hierarchy_depth 0 it resolves to BLOCK's Abstract (a flat leaf, no
    // further recursion into its own placements); placed at depth >= 1 it
    // resolves to BLOCK's Layout instead, and traversal continues into
    // BLOCK's own placements of LEAF.
    struct HierarchyResolverStageFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);

            const LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});

            // LEAF: Abstract only.
            const DesignId leaf_design = root.create_design(DesignData{.library = library_id, .name = "LEAF"});
            leaf_abstract = root.create_abstract(AbstractData{.design = leaf_design});
            root.create_shape(ShapeData{.abstract = leaf_abstract, .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});
            const TerminalId leaf_terminal = root.create_terminal(TerminalData{.abstract = leaf_abstract, .name = "A", .direction = SignalDirection::INPUT});
            const TerminalPortId leaf_port = root.create_terminal_port(TerminalPortData{.terminal = leaf_terminal});
            root.create_shape(ShapeData{.terminal_port = leaf_port, .layer = m1, .rects = {Rect{.ll = Point{1, 1}, .ur = Point{2, 2}}}});
            const ObstructionId leaf_obstruction = root.create_obstruction(ObstructionData{.abstract = leaf_abstract});
            root.create_shape(ShapeData{.obstruction = leaf_obstruction, .layer = m1, .rects = {Rect{.ll = Point{3, 3}, .ur = Point{4, 4}}}});

            // BLOCK: both a Layout (containing 2 LEAF placements) and an Abstract.
            const DesignId block_design = root.create_design(DesignData{.library = library_id, .name = "BLOCK"});
            block_abstract = root.create_abstract(AbstractData{.design = block_design});
            root.create_shape(ShapeData{.abstract = block_abstract, .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{1000, 1000}}}});
            block_layout = root.create_layout(LayoutData{.design = block_design});
            root.create_shape(ShapeData{.layout = block_layout, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{1000, 1000}}}}});
            root.create_placement(PlacementData{.layout = block_layout, .name = "leaf0", .reference_design = leaf_design, .placement_status = PlacementStatus::PLACED, .location = Point{10, 10}, .orientation = Orientation::N});
            root.create_placement(PlacementData{.layout = block_layout, .name = "leaf1", .reference_design = leaf_design, .placement_status = PlacementStatus::PLACED, .location = Point{500, 500}, .orientation = Orientation::N});

            // TOP: Layout only, places BLOCK once.
            const DesignId top_design = root.create_design(DesignData{.library = library_id, .name = "TOP"});
            top_layout = root.create_layout(LayoutData{.design = top_design});
            root.create_shape(ShapeData{.layout = top_layout, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{5000, 5000}}}}});
            root.create_placement(PlacementData{.layout = top_layout, .name = "block0", .reference_design = block_design, .placement_status = PlacementStatus::PLACED, .location = Point{100, 100}, .orientation = Orientation::N});
        }

        ColdPipelineOptions options_for(HierarchyId top_level, int hierarchy_depth) const
        {
            return ColdPipelineOptions{.root_mutation_version = root.mutation_version(), .top_level = top_level, .hierarchy_depth = hierarchy_depth};
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        ViewLayerSet view_layers;
        AbstractId leaf_abstract;
        AbstractId block_abstract;
        LayoutId block_layout;
        LayoutId top_layout;
        HierarchyResolverRunner runner{"HierarchyResolver"};
    };
}

TEST_F(HierarchyResolverStageFixture, DepthZeroFallsBackToAbstractWithoutRecursing)
{
    const ColdPipelineOptions options = options_for(HierarchyId{top_layout}, 0);
    const HierarchyResolverOutput &output = runner.run(HierarchyResolverInput{.root = &root, .view_layers = &view_layers}, 0, options);

    // TOP + BLOCK's Abstract only - depth 0 never reaches BLOCK's own
    // Layout, so LEAF is never discovered at all.
    EXPECT_EQ(output.view_data.size(), 2u);
    ASSERT_TRUE(output.view_data.contains(HierarchyId{top_layout}));
    ASSERT_TRUE(output.view_data.contains(HierarchyId{block_abstract}));
    EXPECT_FALSE(output.view_data.contains(HierarchyId{leaf_abstract}));

    const ViewData &top_data = output.view_data.at(HierarchyId{top_layout});
    ASSERT_EQ(top_data.placement_data.size(), 1u);
    EXPECT_EQ(top_data.placement_data[0].id, HierarchyId{block_abstract});
    EXPECT_EQ(top_data.placement_data[0].location.x, 100);
    EXPECT_EQ(top_data.placement_data[0].location.y, 100);

    const ViewData &block_data = output.view_data.at(HierarchyId{block_abstract});
    EXPECT_EQ(block_data.shapes.size(), 1u); // just its own boundary
    EXPECT_TRUE(block_data.placement_data.empty());
}

TEST_F(HierarchyResolverStageFixture, DepthOneRecursesIntoLayoutAndDedupesRepeatedPlacements)
{
    const ColdPipelineOptions options = options_for(HierarchyId{top_layout}, 1);
    const HierarchyResolverOutput &output = runner.run(HierarchyResolverInput{.root = &root, .view_layers = &view_layers}, 0, options);

    // TOP + BLOCK's Layout + LEAF's Abstract (visited once, not twice,
    // despite BLOCK placing it twice).
    EXPECT_EQ(output.view_data.size(), 3u);
    ASSERT_TRUE(output.view_data.contains(HierarchyId{block_layout}));
    ASSERT_TRUE(output.view_data.contains(HierarchyId{leaf_abstract}));

    const ViewData &top_data = output.view_data.at(HierarchyId{top_layout});
    ASSERT_EQ(top_data.placement_data.size(), 1u);
    EXPECT_EQ(top_data.placement_data[0].id, HierarchyId{block_layout}); // Layout this time, not Abstract

    const ViewData &block_data = output.view_data.at(HierarchyId{block_layout});
    ASSERT_EQ(block_data.placement_data.size(), 2u);
    EXPECT_EQ(block_data.placement_data[0].id, HierarchyId{leaf_abstract});
    EXPECT_EQ(block_data.placement_data[1].id, HierarchyId{leaf_abstract});

    const ViewData &leaf_data = output.view_data.at(HierarchyId{leaf_abstract});
    EXPECT_EQ(leaf_data.shapes.size(), 3u); // terminal shape + obstruction shape + boundary
    EXPECT_TRUE(leaf_data.placement_data.empty());
}

TEST_F(HierarchyResolverStageFixture, ShapesResolveExpectedViewLayers)
{
    const ColdPipelineOptions options = options_for(HierarchyId{top_layout}, 1);
    const HierarchyResolverOutput &output = runner.run(HierarchyResolverInput{.root = &root, .view_layers = &view_layers}, 0, options);

    const ViewData &leaf_data = output.view_data.at(HierarchyId{leaf_abstract});
    const ViewLayerId expected_terminal_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const ViewLayerId expected_obstruction_layer = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);

    bool found_terminal = false;
    bool found_obstruction = false;
    bool found_boundary = false;
    for (const ViewShape &view_shape : leaf_data.shapes)
    {
        if (view_shape.view_layer == expected_terminal_layer && !view_shape.shape.rects.empty())
            found_terminal = true;
        if (view_shape.view_layer == expected_obstruction_layer)
            found_obstruction = true;
        if (view_shape.view_layer == view_layers.boundary_view_layer())
            found_boundary = true;
    }
    EXPECT_TRUE(found_terminal);
    EXPECT_TRUE(found_obstruction);
    EXPECT_TRUE(found_boundary);
}

TEST_F(HierarchyResolverStageFixture, AddsPlacementBoundaryShapesWithNameLabels)
{
    const ColdPipelineOptions options = options_for(HierarchyId{top_layout}, 1);
    const HierarchyResolverOutput &output = runner.run(HierarchyResolverInput{.root = &root, .view_layers = &view_layers}, 0, options);

    const ViewLayerId placement_boundary_layer = view_layers.find(LayerId{}, ViewLayerPurpose::PLACEMENT_BOUNDARY);
    ASSERT_TRUE(placement_boundary_layer.valid());

    // TOP has one placement (block0) - its own PLACEMENT_BOUNDARY shape is
    // block0's resolved world bbox (BLOCK's own declared 1000x1000 size,
    // translated by its location) plus a Text labeled "block0".
    const ViewData &top_data = output.view_data.at(HierarchyId{top_layout});
    std::vector<std::string> top_labels;
    for (const ViewShape &view_shape : top_data.shapes)
    {
        if (view_shape.view_layer != placement_boundary_layer)
            continue;
        ASSERT_EQ(view_shape.shape.rects.size(), 1u);
        EXPECT_GT(view_shape.shape.rects[0].ur.x, view_shape.shape.rects[0].ll.x);
        EXPECT_GT(view_shape.shape.rects[0].ur.y, view_shape.shape.rects[0].ll.y);
        ASSERT_EQ(view_shape.shape.texts.size(), 1u);
        top_labels.push_back(view_shape.shape.texts[0].label);
    }
    EXPECT_EQ(top_labels, (std::vector<std::string>{"block0"}));

    // BLOCK's own Layout has two placements (leaf0/leaf1) - one
    // PLACEMENT_BOUNDARY shape each.
    const ViewData &block_data = output.view_data.at(HierarchyId{block_layout});
    std::vector<std::string> block_labels;
    for (const ViewShape &view_shape : block_data.shapes)
    {
        if (view_shape.view_layer != placement_boundary_layer)
            continue;
        ASSERT_EQ(view_shape.shape.texts.size(), 1u);
        block_labels.push_back(view_shape.shape.texts[0].label);
    }
    ASSERT_EQ(block_labels.size(), 2u);
    EXPECT_NE(std::find(block_labels.begin(), block_labels.end(), "leaf0"), block_labels.end());
    EXPECT_NE(std::find(block_labels.begin(), block_labels.end(), "leaf1"), block_labels.end());
}

TEST_F(HierarchyResolverStageFixture, NullRootProducesEmptyOutput)
{
    const ColdPipelineOptions options = options_for(HierarchyId{top_layout}, 1);
    const HierarchyResolverOutput &output = runner.run(HierarchyResolverInput{.root = nullptr, .view_layers = nullptr}, 0, options);
    EXPECT_TRUE(output.view_data.empty());
}

TEST_F(HierarchyResolverStageFixture, RecomputesWhenHierarchyDepthChangesEvenIfMutationVersionDoesNot)
{
    const ColdPipelineOptions depth_zero = options_for(HierarchyId{top_layout}, 0);
    const HierarchyResolverOutput &first = runner.run(HierarchyResolverInput{.root = &root, .view_layers = &view_layers}, 0, depth_zero);
    EXPECT_EQ(first.view_data.size(), 2u);

    const ColdPipelineOptions depth_one = options_for(HierarchyId{top_layout}, 1);
    ASSERT_TRUE(runner.would_recompute(0, depth_one));

    const HierarchyResolverOutput &second = runner.run(HierarchyResolverInput{.root = &root, .view_layers = &view_layers}, 0, depth_one);
    EXPECT_EQ(second.view_data.size(), 3u);
}
