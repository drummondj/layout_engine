#include "../stages/hierarchy_resolver_stage.hpp"
#include "synchronous_stage_runner.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace le;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;

    // Fixture hierarchy:
    //   TOP (Layout only) - diearea, 1 placement of BLOCK
    //     BLOCK (Layout + Abstract) - diearea, 2 placements of LEAF
    //       LEAF (Abstract only) - boundary, 1 Terminal (1 Shape), 1 Obstruction (1 Shape)
    //
    // hierarchy_depth semantics (HierarchyResolverStage::compute()'s own
    // doc comment): depth 0 shows only TOP's own direct content, nothing
    // resolved past it at all; each further unit of depth lets one more
    // Placement -> Layout hop actually resolve and get visited. So:
    //   depth 0: TOP only - block0 is never resolved to anything.
    //   depth 1: TOP resolves block0 -> BLOCK's Layout (remaining_depth 1
    //            > 0 and BLOCK has one), but BLOCK's own remaining_depth
    //            is now 0, so BLOCK's own placements (leaf0/leaf1) are
    //            never resolved either.
    //   depth 2: BLOCK's own remaining_depth is 1 > 0, so leaf0/leaf1
    //            resolve too - to LEAF's Abstract (LEAF has no Layout of
    //            its own, so resolve_design_target falls back to it
    //            regardless of remaining_depth - Abstracts are always
    //            leaves, never depth-gated).
    struct HierarchyResolverStageFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);
            view_layers_handle = std::make_shared<const ViewLayerSet>(view_layers);

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

        ViewRenderOptions options_for(HierarchyId top_level, int hierarchy_depth) const
        {
            return ViewRenderOptions{.root = &root, .root_mutation_version = root.mutation_version(), .top_level = top_level, .hierarchy_depth = hierarchy_depth};
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        ViewLayerSet view_layers;
        ViewLayerSetHandle view_layers_handle;
        AbstractId leaf_abstract;
        AbstractId block_abstract;
        LayoutId block_layout;
        LayoutId top_layout;
        HierarchyResolverRunner runner{"HierarchyResolver"};
    };
}

TEST_F(HierarchyResolverStageFixture, DepthZeroShowsOnlyTopLevelContent)
{
    const ViewRenderOptions options = options_for(HierarchyId{top_layout}, 0);
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options);

    // TOP only - depth 0 means "just the top level," full stop: block0
    // isn't resolved to anything at all, not even a fallback to its own
    // Abstract.
    EXPECT_EQ(output.view_data.size(), 1u);
    ASSERT_TRUE(output.view_data.contains(HierarchyId{top_layout}));
    EXPECT_FALSE(output.view_data.contains(HierarchyId{block_abstract}));
    EXPECT_FALSE(output.view_data.contains(HierarchyId{block_layout}));
    EXPECT_FALSE(output.view_data.contains(HierarchyId{leaf_abstract}));

    const ViewData &top_data = output.view_data.at(HierarchyId{top_layout});
    EXPECT_TRUE(top_data.placement_data.empty());

    // Still shows a PLACEMENT_BOUNDARY placeholder for block0 - its own
    // footprint (sized via resolve_design_target for bbox purposes only -
    // collect_layout_content's own placement-boundary comment) is real
    // *data* about this Layout's own direct content, unaffected by
    // whether anything past it ever gets resolved.
    const ViewLayerId placement_boundary_layer = view_layers.find(LayerId{}, ViewLayerPurpose::PLACEMENT_BOUNDARY);
    const auto boundary_group_it = top_data.shapes->find(placement_boundary_layer);
    ASSERT_NE(boundary_group_it, top_data.shapes->end());
    ASSERT_EQ(boundary_group_it->second.size(), 1u);
    EXPECT_TRUE(boundary_group_it->second[0].texts.empty()); // boundary Shape carries only the outline now - see placement_name_layer below

    // The name label itself lives on its own dedicated PLACEMENT_NAME
    // Shape/ViewLayer (view_style.hpp, BUGS_AND_ENHANCEMENTS.md E13) -
    // split out from PLACEMENT_BOUNDARY so its own color/visibility is
    // independently toggleable, matching pipelines.old's own
    // draw_placement_labels/PLACEMENT_NAME split.
    const auto name_group_it = top_data.shapes->find(view_layers.placement_name_view_layer());
    ASSERT_NE(name_group_it, top_data.shapes->end());
    ASSERT_EQ(name_group_it->second.size(), 1u);
    ASSERT_EQ(name_group_it->second[0].texts.size(), 1u);
    EXPECT_EQ(name_group_it->second[0].texts[0].label, "block0");
}

TEST_F(HierarchyResolverStageFixture, DepthOneResolvesTopLevelPlacementsButNotTheirOwn)
{
    const ViewRenderOptions options = options_for(HierarchyId{top_layout}, 1);
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options);

    // TOP + BLOCK's Layout - block0 resolves (remaining_depth 1 > 0 and
    // BLOCK has a Layout), but BLOCK's own remaining_depth is now 0, so
    // its own placements (leaf0/leaf1) don't resolve to anything either.
    EXPECT_EQ(output.view_data.size(), 2u);
    ASSERT_TRUE(output.view_data.contains(HierarchyId{block_layout}));
    EXPECT_FALSE(output.view_data.contains(HierarchyId{block_abstract}));
    EXPECT_FALSE(output.view_data.contains(HierarchyId{leaf_abstract}));

    const ViewData &top_data = output.view_data.at(HierarchyId{top_layout});
    ASSERT_EQ(top_data.placement_data.size(), 1u);
    EXPECT_EQ(top_data.placement_data[0].id, HierarchyId{block_layout});

    const ViewData &block_data = output.view_data.at(HierarchyId{block_layout});
    EXPECT_TRUE(block_data.placement_data.empty());
}

TEST_F(HierarchyResolverStageFixture, PlacementDataBboxMatchesPlacementBoundaryShapeRect)
{
    const ViewRenderOptions options = options_for(HierarchyId{top_layout}, 1);
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options);

    const ViewData &top_data = output.view_data.at(HierarchyId{top_layout});
    ASSERT_EQ(top_data.placement_data.size(), 1u);

    // BLOCK's Layout declares a 1000x1000 diearea at depth 1 (BLOCK
    // resolves to its Layout, not its Abstract - see
    // DepthOneResolvesTopLevelPlacementsButNotTheirOwn), placed at
    // (100, 100) with orientation N (identity transform) - the world
    // bbox is that diearea translated by the placement's own location.
    const Rect &bbox = top_data.placement_data[0].bbox;
    EXPECT_EQ(bbox.ll.x, 100);
    EXPECT_EQ(bbox.ll.y, 100);
    EXPECT_EQ(bbox.ur.x, 1100);
    EXPECT_EQ(bbox.ur.y, 1100);

    // Not just equal in value - the exact same bbox computed once and
    // reused for the PLACEMENT_BOUNDARY placeholder shape's own rect
    // (the whole point of folding this computation into one pass - see
    // the main compute() loop's own comment).
    const ViewLayerId placement_boundary_layer = view_layers.find(LayerId{}, ViewLayerPurpose::PLACEMENT_BOUNDARY);
    const auto boundary_group_it = top_data.shapes->find(placement_boundary_layer);
    ASSERT_NE(boundary_group_it, top_data.shapes->end());
    ASSERT_EQ(boundary_group_it->second.size(), 1u);
    const RenderShape &top_boundary_shape = boundary_group_it->second[0];
    ASSERT_EQ(top_boundary_shape.rects.size(), 1u);
    EXPECT_EQ(top_boundary_shape.rects[0].ll.x, bbox.ll.x);
    EXPECT_EQ(top_boundary_shape.rects[0].ll.y, bbox.ll.y);
    EXPECT_EQ(top_boundary_shape.rects[0].ur.x, bbox.ur.x);
    EXPECT_EQ(top_boundary_shape.rects[0].ur.y, bbox.ur.y);
}

TEST_F(HierarchyResolverStageFixture, DepthTwoRecursesIntoLayoutAndDedupesRepeatedPlacements)
{
    const ViewRenderOptions options = options_for(HierarchyId{top_layout}, 2);
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options);

    // TOP + BLOCK's Layout + LEAF's Abstract (visited once, not twice,
    // despite BLOCK placing it twice) - BLOCK's own remaining_depth is
    // now 1 > 0, so its own placements resolve too. LEAF has no Layout
    // of its own, so it falls back to its Abstract regardless of
    // remaining_depth (Abstracts are always leaves, never depth-gated).
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
    EXPECT_EQ(leaf_data.shapes->size(), 3u); // 3 distinct ViewLayer groups: terminal, obstruction, boundary - one shape each
    EXPECT_TRUE(leaf_data.placement_data.empty());
}

TEST_F(HierarchyResolverStageFixture, ShapesResolveExpectedViewLayers)
{
    const ViewRenderOptions options = options_for(HierarchyId{top_layout}, 2); // depth 2 - see DepthTwoRecursesIntoLayoutAndDedupesRepeatedPlacements for why LEAF needs this now
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options);

    const ViewData &leaf_data = output.view_data.at(HierarchyId{leaf_abstract});
    const ViewLayerId expected_terminal_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const ViewLayerId expected_obstruction_layer = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);

    const auto terminal_it = leaf_data.shapes->find(expected_terminal_layer);
    ASSERT_NE(terminal_it, leaf_data.shapes->end());
    EXPECT_FALSE(terminal_it->second.empty());
    EXPECT_FALSE(terminal_it->second.front().rects.empty());

    EXPECT_TRUE(leaf_data.shapes->contains(expected_obstruction_layer));
    EXPECT_TRUE(leaf_data.shapes->contains(view_layers.boundary_view_layer()));
}

TEST_F(HierarchyResolverStageFixture, AddsPlacementBoundaryShapesWithNameLabels)
{
    const ViewRenderOptions options = options_for(HierarchyId{top_layout}, 1);
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options);

    const ViewLayerId placement_boundary_layer = view_layers.find(LayerId{}, ViewLayerPurpose::PLACEMENT_BOUNDARY);
    ASSERT_TRUE(placement_boundary_layer.valid());

    // Every placement in a Layout batches into a single PLACEMENT_BOUNDARY
    // Shape (one rect per placement) plus a single, separate
    // PLACEMENT_NAME Shape (one rect + one Text per placement, index-
    // paired with the boundary rects - draw_view_shapes' own comment)
    // rather than one Shape per placement - see the main compute() loop's
    // own comment for why batching matters (measured allocation cost at
    // real placement counts), and view_style.hpp's own PLACEMENT_NAME
    // purpose for why the label is its own Shape/ViewLayer rather than
    // living on the boundary Shape itself (independent color/visibility,
    // BUGS_AND_ENHANCEMENTS.md E13).
    const ViewLayerId placement_name_layer = view_layers.placement_name_view_layer();
    ASSERT_TRUE(placement_name_layer.valid());

    // TOP has one placement (block0) - its own PLACEMENT_BOUNDARY shape
    // holds block0's resolved world bbox (BLOCK's own declared 1000x1000
    // size, translated by its location); its own PLACEMENT_NAME shape
    // holds that same rect plus a Text labeled "block0".
    const ViewData &top_data = output.view_data.at(HierarchyId{top_layout});
    const auto top_boundary_it = top_data.shapes->find(placement_boundary_layer);
    ASSERT_NE(top_boundary_it, top_data.shapes->end());
    ASSERT_EQ(top_boundary_it->second.size(), 1u);
    const RenderShape &top_boundary_shape = top_boundary_it->second[0];
    ASSERT_EQ(top_boundary_shape.rects.size(), 1u);
    EXPECT_GT(top_boundary_shape.rects[0].ur.x, top_boundary_shape.rects[0].ll.x);
    EXPECT_GT(top_boundary_shape.rects[0].ur.y, top_boundary_shape.rects[0].ll.y);
    EXPECT_TRUE(top_boundary_shape.texts.empty());

    const auto top_name_it = top_data.shapes->find(placement_name_layer);
    ASSERT_NE(top_name_it, top_data.shapes->end());
    ASSERT_EQ(top_name_it->second.size(), 1u);
    const RenderShape &top_name_shape = top_name_it->second[0];
    ASSERT_EQ(top_name_shape.texts.size(), 1u);
    EXPECT_EQ(top_name_shape.texts[0].label, "block0");

    // BLOCK's own Layout has two placements (leaf0/leaf1) - one shared
    // PLACEMENT_NAME shape with 2 rects/labels, not two shapes.
    const ViewData &block_data = output.view_data.at(HierarchyId{block_layout});
    const auto block_name_it = block_data.shapes->find(placement_name_layer);
    ASSERT_NE(block_name_it, block_data.shapes->end());
    ASSERT_EQ(block_name_it->second.size(), 1u);
    const RenderShape &block_name_shape = block_name_it->second[0];
    ASSERT_EQ(block_name_shape.rects.size(), 2u);
    ASSERT_EQ(block_name_shape.texts.size(), 2u);

    std::vector<std::string> block_labels;
    for (const Text &text : block_name_shape.texts)
        block_labels.push_back(text.label);
    EXPECT_NE(std::find(block_labels.begin(), block_labels.end(), "leaf0"), block_labels.end());
    EXPECT_NE(std::find(block_labels.begin(), block_labels.end(), "leaf1"), block_labels.end());
}

TEST_F(HierarchyResolverStageFixture, NullRootProducesEmptyOutput)
{
    ViewRenderOptions options = options_for(HierarchyId{top_layout}, 1);
    options.root = nullptr;
    const HierarchyResolverOutput &output = runner.run(nullptr, 0, options);
    EXPECT_TRUE(output.view_data.empty());
}

TEST_F(HierarchyResolverStageFixture, RecomputesWhenHierarchyDepthChangesEvenIfMutationVersionDoesNot)
{
    const ViewRenderOptions depth_zero = options_for(HierarchyId{top_layout}, 0);
    const HierarchyResolverOutput &first = runner.run(view_layers_handle, 0, depth_zero);
    EXPECT_EQ(first.view_data.size(), 1u);

    const ViewRenderOptions depth_one = options_for(HierarchyId{top_layout}, 1);
    ASSERT_TRUE(runner.would_recompute(0, depth_one));

    const HierarchyResolverOutput &second = runner.run(view_layers_handle, 0, depth_one);
    EXPECT_EQ(second.view_data.size(), 2u);
}

TEST_F(HierarchyResolverStageFixture, ShapesIndexQueryFindsOnlyOverlappingShapesOnTheRightLayer)
{
    // LEAF's own TERMINAL shape sits at (1,1)-(2,2); its own OBSTRUCTION
    // shape sits at (3,3)-(4,4) - both real per-shape RasterizeBlend2DStage's
    // own draw_view_shapes_blend2d queries against ViewData::shapes_index
    // for per-shape viewport culling (the gap ViewportCullStage's own doc
    // comment names - it only culls placements, not shapes within one
    // node). Exercises the index directly rather than through a full
    // render, so a coordinate-space or off-by-one mistake in the query
    // itself is caught independent of anything the rasterizer's own
    // clipping might otherwise paper over.
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options_for(HierarchyId{leaf_abstract}, 0));
    const ViewData &leaf_data = output.view_data.at(HierarchyId{leaf_abstract});
    ASSERT_NE(leaf_data.shapes_index, nullptr);

    const ViewLayerId terminal_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const auto index_it = leaf_data.shapes_index->find(terminal_layer);
    ASSERT_NE(index_it, leaf_data.shapes_index->end());

    auto query = [&](Rect rect)
    {
        std::vector<ShapeIndexEntry> hits;
        index_it->second.query(boost::geometry::index::intersects(rect), std::back_inserter(hits));
        return hits.size();
    };

    // Tightly around the terminal's own (1,1)-(2,2) rect - a hit.
    EXPECT_EQ(query(Rect{.ll = Point{0, 0}, .ur = Point{3, 3}}), 1u);
    // Only partially overlaps the terminal's own rect (its own lower-left
    // corner region) - a real, if partial, overlap is still a hit.
    EXPECT_EQ(query(Rect{.ll = Point{-10, -10}, .ur = Point{1, 1}}), 1u);
    // Far from both the terminal and the obstruction - no hit.
    EXPECT_EQ(query(Rect{.ll = Point{500, 500}, .ur = Point{600, 600}}), 0u);

    // The OBSTRUCTION shape (3,3)-(4,4) lives on a *different* ViewLayerId
    // (same physical Layer, different purpose) - a query against the
    // TERMINAL layer's own index must not find it even though its own
    // bbox is close by.
    const ViewLayerId obstruction_layer = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    const auto obstruction_index_it = leaf_data.shapes_index->find(obstruction_layer);
    ASSERT_NE(obstruction_index_it, leaf_data.shapes_index->end());
    std::vector<ShapeIndexEntry> obstruction_hits;
    obstruction_index_it->second.query(boost::geometry::index::intersects(Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}), std::back_inserter(obstruction_hits));
    ASSERT_EQ(obstruction_hits.size(), 1u);
    const std::optional<Rect> obstruction_bbox = Geometry::bbox(leaf_data.shapes->at(obstruction_layer)[obstruction_hits.front().second]);
    ASSERT_TRUE(obstruction_bbox.has_value());
    EXPECT_EQ(obstruction_hits.front().first.ll.x, obstruction_bbox->ll.x);
    EXPECT_EQ(obstruction_hits.front().first.ll.y, obstruction_bbox->ll.y);
    EXPECT_EQ(obstruction_hits.front().first.ur.x, obstruction_bbox->ur.x);
    EXPECT_EQ(obstruction_hits.front().first.ur.y, obstruction_bbox->ur.y);
}
