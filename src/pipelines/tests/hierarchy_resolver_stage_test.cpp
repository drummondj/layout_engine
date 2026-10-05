#include "../stages/hierarchy_resolver_stage.hpp"
#include "synchronous_stage_runner.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

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

    // Every chunk's shapes of `data` merged into one map (a copy) - the
    // tests below check what a node draws, not which chunk holds it.
    ViewLayerShapes merged_shapes(const ViewData &data)
    {
        ViewLayerShapes merged;
        for (const ViewShapeChunk &chunk : data.chunks)
            if (chunk.shapes)
                for (const auto &[layer, shapes] : *chunk.shapes)
                    merged[layer].insert(merged[layer].end(), shapes.begin(), shapes.end());
        return merged;
    }

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
            root.create_shape(ShapeData{.owner = le::ShapeOwner::abstract(leaf_abstract), .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});
            const TerminalId leaf_terminal = root.create_terminal(TerminalData{.abstract = leaf_abstract, .name = "A", .direction = SignalDirection::INPUT});
            const TerminalPortId leaf_port = root.create_terminal_port(TerminalPortData{.terminal = leaf_terminal});
            root.create_shape(ShapeData{.owner = le::ShapeOwner::terminal_port(leaf_port), .layer = m1, .rects = {Rect{.ll = Point{1, 1}, .ur = Point{2, 2}}}});
            const ObstructionId leaf_obstruction = root.create_obstruction(ObstructionData{.abstract = leaf_abstract});
            root.create_shape(ShapeData{.owner = le::ShapeOwner::obstruction(leaf_obstruction), .layer = m1, .rects = {Rect{.ll = Point{3, 3}, .ur = Point{4, 4}}}});

            // BLOCK: both a Layout (containing 2 LEAF placements) and an Abstract.
            const DesignId block_design = root.create_design(DesignData{.library = library_id, .name = "BLOCK"});
            block_abstract = root.create_abstract(AbstractData{.design = block_design});
            root.create_shape(ShapeData{.owner = le::ShapeOwner::abstract(block_abstract), .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{1000, 1000}}}});
            block_layout = root.create_layout(LayoutData{.design = block_design});
            root.create_shape(ShapeData{.owner = le::ShapeOwner::layout(block_layout), .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{1000, 1000}}}}});
            root.create_placement(PlacementData{.layout = block_layout, .name = "leaf0", .reference_design = leaf_design, .placement_status = PlacementStatus::PLACED, .location = Point{10, 10}, .orientation = Orientation::N});
            root.create_placement(PlacementData{.layout = block_layout, .name = "leaf1", .reference_design = leaf_design, .placement_status = PlacementStatus::PLACED, .location = Point{500, 500}, .orientation = Orientation::N});

            // TOP: Layout only, places BLOCK once.
            const DesignId top_design = root.create_design(DesignData{.library = library_id, .name = "TOP"});
            top_layout = root.create_layout(LayoutData{.design = top_design});
            root.create_shape(ShapeData{.owner = le::ShapeOwner::layout(top_layout), .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{5000, 5000}}}}});
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
    EXPECT_TRUE((placement_count(top_data) == 0));

    // Still shows a PLACEMENT placeholder (outline + name) for block0 -
    // its own footprint (sized via resolve_design_target for bbox
    // purposes only) is real *data* about this Layout's own direct
    // content, unaffected by whether anything past it ever gets resolved.
    const ViewLayerShapes top_data_shapes = merged_shapes(top_data);
    const auto placement_group_it = top_data_shapes.find(view_layers.placement_view_layer());
    ASSERT_NE(placement_group_it, top_data_shapes.end());
    ASSERT_EQ(placement_group_it->second.size(), 1u);
    ASSERT_EQ(placement_group_it->second[0].rects.size(), 1u);
    ASSERT_EQ(placement_group_it->second[0].texts.size(), 1u);
    EXPECT_EQ(placement_group_it->second[0].texts[0].label, "block0");
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
    ASSERT_EQ(placement_count(top_data), 1u);
    EXPECT_EQ(placements_of(top_data)[0].id, HierarchyId{block_layout});

    const ViewData &block_data = output.view_data.at(HierarchyId{block_layout});
    EXPECT_TRUE((placement_count(block_data) == 0));
}

TEST_F(HierarchyResolverStageFixture, PlacementDataBboxMatchesPlacementBoundaryShapeRect)
{
    const ViewRenderOptions options = options_for(HierarchyId{top_layout}, 1);
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options);

    const ViewData &top_data = output.view_data.at(HierarchyId{top_layout});
    ASSERT_EQ(placement_count(top_data), 1u);

    // BLOCK's Layout declares a 1000x1000 diearea at depth 1 (BLOCK
    // resolves to its Layout, not its Abstract - see
    // DepthOneResolvesTopLevelPlacementsButNotTheirOwn), placed at
    // (100, 100) with orientation N (identity transform) - the world
    // bbox is that diearea translated by the placement's own location.
    const Rect &bbox = placements_of(top_data)[0].bbox;
    EXPECT_EQ(bbox.ll.x, 100);
    EXPECT_EQ(bbox.ll.y, 100);
    EXPECT_EQ(bbox.ur.x, 1100);
    EXPECT_EQ(bbox.ur.y, 1100);

    // Not just equal in value - the exact same bbox computed once and
    // reused for the PLACEMENT placeholder shape's own rect
    // (the whole point of folding this computation into one pass - see
    // the main compute() loop's own comment).
    const ViewLayerShapes top_data_shapes = merged_shapes(top_data);
    const auto boundary_group_it = top_data_shapes.find(view_layers.placement_view_layer());
    ASSERT_NE(boundary_group_it, top_data_shapes.end());
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
    ASSERT_EQ(placement_count(top_data), 1u);
    EXPECT_EQ(placements_of(top_data)[0].id, HierarchyId{block_layout}); // Layout this time, not Abstract

    const ViewData &block_data = output.view_data.at(HierarchyId{block_layout});
    ASSERT_EQ(placement_count(block_data), 2u);
    EXPECT_EQ(placements_of(block_data)[0].id, HierarchyId{leaf_abstract});
    EXPECT_EQ(placements_of(block_data)[1].id, HierarchyId{leaf_abstract});

    const ViewData &leaf_data = output.view_data.at(HierarchyId{leaf_abstract});
    const ViewLayerShapes leaf_data_shapes = merged_shapes(leaf_data);
    EXPECT_EQ(leaf_data_shapes.size(), 3u); // 3 distinct ViewLayer groups: terminal, obstruction, boundary - one shape each
    EXPECT_TRUE((placement_count(leaf_data) == 0));
}

TEST_F(HierarchyResolverStageFixture, ShapesResolveExpectedViewLayers)
{
    const ViewRenderOptions options = options_for(HierarchyId{top_layout}, 2); // depth 2 - see DepthTwoRecursesIntoLayoutAndDedupesRepeatedPlacements for why LEAF needs this now
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options);

    const ViewData &leaf_data = output.view_data.at(HierarchyId{leaf_abstract});
    const ViewLayerId expected_terminal_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const ViewLayerId expected_obstruction_layer = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);

    const ViewLayerShapes leaf_data_shapes = merged_shapes(leaf_data);
    const auto terminal_it = leaf_data_shapes.find(expected_terminal_layer);
    ASSERT_NE(terminal_it, leaf_data_shapes.end());
    EXPECT_FALSE(terminal_it->second.empty());
    EXPECT_FALSE(terminal_it->second.front().rects.empty());

    EXPECT_TRUE(leaf_data_shapes.contains(expected_obstruction_layer));
    EXPECT_TRUE(leaf_data_shapes.contains(view_layers.boundary_view_layer()));
}

TEST_F(HierarchyResolverStageFixture, AddsPlacementShapesWithOutlinesAndNameLabels)
{
    const ViewRenderOptions options = options_for(HierarchyId{top_layout}, 1);
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options);

    // Every placement in a Layout batches into a single PLACEMENT Shape
    // (one rect + one Text per placement, index-paired - draw_view_shapes'
    // own comment) rather than one Shape per placement - see the main
    // compute() loop's own comment for why batching matters (measured
    // allocation cost at real placement counts).
    const ViewLayerId placement_layer = view_layers.placement_view_layer();
    ASSERT_TRUE(placement_layer.valid());

    // TOP has one placement (block0) - its rect is block0's resolved world
    // bbox (BLOCK's own declared 1000x1000 size, translated by its
    // location), its Text is labeled "block0".
    const ViewData &top_data = output.view_data.at(HierarchyId{top_layout});
    const ViewLayerShapes top_data_shapes = merged_shapes(top_data);
    const auto top_it = top_data_shapes.find(placement_layer);
    ASSERT_NE(top_it, top_data_shapes.end());
    ASSERT_EQ(top_it->second.size(), 1u);
    const RenderShape &top_shape = top_it->second[0];
    ASSERT_EQ(top_shape.rects.size(), 1u);
    EXPECT_GT(top_shape.rects[0].ur.x, top_shape.rects[0].ll.x);
    EXPECT_GT(top_shape.rects[0].ur.y, top_shape.rects[0].ll.y);
    ASSERT_EQ(top_shape.texts.size(), 1u);
    EXPECT_EQ(top_shape.texts[0].label, "block0");

    // BLOCK's own Layout has two placements (leaf0/leaf1) - one shared
    // PLACEMENT shape with 2 rects/labels, not two shapes.
    const ViewData &block_data = output.view_data.at(HierarchyId{block_layout});
    const ViewLayerShapes block_data_shapes = merged_shapes(block_data);
    const auto block_name_it = block_data_shapes.find(placement_layer);
    ASSERT_NE(block_name_it, block_data_shapes.end());
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
    ASSERT_EQ(leaf_data.chunks.size(), 1u); // an Abstract is one chunk
    ASSERT_NE(leaf_data.chunks[0].shapes_index, nullptr);

    const ViewLayerId terminal_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const auto index_it = leaf_data.chunks[0].shapes_index->find(terminal_layer);
    ASSERT_NE(index_it, leaf_data.chunks[0].shapes_index->end());

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
    const auto obstruction_index_it = leaf_data.chunks[0].shapes_index->find(obstruction_layer);
    ASSERT_NE(obstruction_index_it, leaf_data.chunks[0].shapes_index->end());
    std::vector<ShapeIndexEntry> obstruction_hits;
    obstruction_index_it->second.query(boost::geometry::index::intersects(Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}), std::back_inserter(obstruction_hits));
    ASSERT_EQ(obstruction_hits.size(), 1u);
    const std::optional<Rect> obstruction_bbox = Geometry::bbox(leaf_data.chunks[0].shapes->at(obstruction_layer)[obstruction_hits.front().second]);
    ASSERT_TRUE(obstruction_bbox.has_value());
    EXPECT_EQ(obstruction_hits.front().first.ll.x, obstruction_bbox->ll.x);
    EXPECT_EQ(obstruction_hits.front().first.ll.y, obstruction_bbox->ll.y);
    EXPECT_EQ(obstruction_hits.front().first.ur.x, obstruction_bbox->ur.x);
    EXPECT_EQ(obstruction_hits.front().first.ur.y, obstruction_bbox->ur.y);
}

// --- Free-standing shapes (Abstract/Layout.free_shapes) ---

namespace
{
    size_t shapes_on(const ViewData &data, ViewLayerId view_layer)
    {
        return view_data_shapes(data, view_layer).size();
    }
}

TEST_F(HierarchyResolverStageFixture, FreeShapesDrawOnTheirLayersCustomShapeColumnOrTheDebugRow)
{
    root.create_shape(ShapeData{.owner = le::ShapeOwner::in_abstract(leaf_abstract), .layer = m1, .rects = {Rect{.ll = Point{5, 5}, .ur = Point{6, 6}}}});
    root.create_shape(ShapeData{.owner = le::ShapeOwner::in_abstract(leaf_abstract), .purpose = ShapePurpose::DEBUG, .rects = {Rect{.ll = Point{7, 7}, .ur = Point{8, 8}}}});
    // Layer-less and not DEBUG: not drawn at all - in particular not on the
    // BOUNDARY row its own purpose would otherwise resolve to.
    root.create_shape(ShapeData{.owner = le::ShapeOwner::in_abstract(leaf_abstract), .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{9, 9}}}});

    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options_for(HierarchyId{leaf_abstract}, 0));
    const ViewData &leaf = output.view_data.at(HierarchyId{leaf_abstract});

    const ViewLayerId custom = view_layers.find(m1, ViewLayerPurpose::CUSTOM_SHAPE);
    ASSERT_EQ(shapes_on(leaf, custom), 1u);
    const ViewLayerShapes leaf_shapes = merged_shapes(leaf);
    EXPECT_EQ(leaf_shapes.at(custom)[0].rects[0].ll.x, 5);
    EXPECT_EQ(shapes_on(leaf, view_layers.find(LayerId{}, ViewLayerPurpose::DEBUG)), 1u);
    EXPECT_EQ(shapes_on(leaf, view_layers.boundary_view_layer()), 1u); // only LEAF's real boundary
    // The terminal/obstruction shapes on M1 stay on their own columns.
    EXPECT_EQ(shapes_on(leaf, view_layers.find(m1, ViewLayerPurpose::TERMINAL)), 1u);
    EXPECT_EQ(shapes_on(leaf, view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION)), 1u);
}

TEST_F(HierarchyResolverStageFixture, ALayoutsFreeShapesAreDrawn)
{
    root.create_shape(ShapeData{.owner = le::ShapeOwner::in_layout(top_layout), .layer = m1, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{50, 50}}}});

    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 0));

    EXPECT_EQ(shapes_on(output.view_data.at(HierarchyId{top_layout}), view_layers.find(m1, ViewLayerPurpose::CUSTOM_SHAPE)), 1u);
}

TEST_F(HierarchyResolverStageFixture, AnAbstractsFreeShapesAppearInEveryPlacementOfIt)
{
    root.create_shape(ShapeData{.owner = le::ShapeOwner::in_abstract(leaf_abstract), .layer = m1, .rects = {Rect{.ll = Point{5, 5}, .ur = Point{6, 6}}}});

    // Depth 2 resolves TOP -> BLOCK's Layout -> both LEAF placements, each
    // reusing LEAF's one collected content.
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));

    ASSERT_TRUE(output.view_data.contains(HierarchyId{leaf_abstract}));
    EXPECT_EQ(shapes_on(output.view_data.at(HierarchyId{leaf_abstract}), view_layers.find(m1, ViewLayerPurpose::CUSTOM_SHAPE)), 1u);
    const ViewData &block = output.view_data.at(HierarchyId{block_layout});
    const std::vector<ViewPlacementData> block_placements = placements_of(block);
    EXPECT_EQ(std::count_if(block_placements.begin(), block_placements.end(), [&](const auto &placement)
                            { return placement.id == HierarchyId{leaf_abstract}; }),
              2);
}

// A Layout's PhysicalPorts draw their
// shapes on TERMINAL with the port's name as a label, plus one direction
// marker per port on the PORT_MARKER row, beside its outer edge.
TEST_F(HierarchyResolverStageFixture, PhysicalPortsDrawShapesLabelsAndDirectionMarkers)
{
    // TOP's die is (0,0)-(5000,5000); IN sits on its left edge, OUT on its top.
    const PhysicalPortId in_port = root.create_physical_port(PhysicalPortData{.layout = top_layout, .name = "IN", .direction = SignalDirection::INPUT});
    const PhysicalPortSegmentId in_segment = root.create_physical_port_segment(PhysicalPortSegmentData{.physical_port = in_port});
    root.create_shape(ShapeData{.owner = le::ShapeOwner::physical_port_segment(in_segment), .layer = m1, .rects = {Rect{.ll = Point{0, 2000}, .ur = Point{100, 2040}}}});
    const PhysicalPortId out_port = root.create_physical_port(PhysicalPortData{.layout = top_layout, .name = "OUT", .direction = SignalDirection::OUTPUT});
    const PhysicalPortSegmentId out_segment = root.create_physical_port_segment(PhysicalPortSegmentData{.physical_port = out_port});
    root.create_shape(ShapeData{.owner = le::ShapeOwner::physical_port_segment(out_segment), .layer = m1, .rects = {Rect{.ll = Point{3000, 4900}, .ur = Point{3040, 5000}}}});

    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 0));
    const ViewData &top = output.view_data.at(HierarchyId{top_layout});

    const ViewLayerId terminal = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    ASSERT_EQ(shapes_on(top, terminal), 2u);
    std::vector<std::string> labels;
    const ViewLayerShapes top_shapes = merged_shapes(top);
    for (const RenderShape &shape : top_shapes.at(terminal))
        for (const Text &text : shape.texts)
            labels.push_back(text.label);
    std::ranges::sort(labels);
    EXPECT_EQ(labels, (std::vector<std::string>{"IN", "OUT"}));

    // One RenderShape per port (so each can be enlarged about its own
    // anchor), one triangle each, both outside the die beyond the port's
    // outer edge.
    ASSERT_EQ(shapes_on(top, view_layers.port_marker_view_layer()), 2u);
    for (const RenderShape &marker : top_shapes.at(view_layers.port_marker_view_layer()))
    {
        ASSERT_EQ(marker.polygons.size(), 1u);
        for (const Point &p : marker.polygons[0].points)
            EXPECT_TRUE(p.x <= 0 || p.y >= 5000) << p.x << "," << p.y;
    }
}

// ViewData::extent / ViewPlacementData::extent cover everything a node
// draws, not just its declared boundary.
TEST_F(HierarchyResolverStageFixture, ExtentsGrowToCoverContentOutsideTheBoundary)
{
    // LEAF's boundary is (0,0)-(10,10); this obstruction overhangs it.
    const ObstructionId overhang = root.create_obstruction(ObstructionData{.abstract = leaf_abstract});
    root.create_shape(ShapeData{.owner = le::ShapeOwner::obstruction(overhang), .layer = m1, .rects = {Rect{.ll = Point{8, -5}, .ur = Point{15, 12}}}});

    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));

    const Rect leaf = output.view_data.at(HierarchyId{leaf_abstract}).extent;
    EXPECT_EQ(leaf.ll.x, 0);
    EXPECT_EQ(leaf.ll.y, -5);
    EXPECT_EQ(leaf.ur.x, 15);
    EXPECT_EQ(leaf.ur.y, 12);

    // leaf1 sits at (500,500) in BLOCK: its declared bbox is unchanged,
    // its extent carries the overhang.
    const ViewData &block = output.view_data.at(HierarchyId{block_layout});
    const std::vector<ViewPlacementData> block_placements = placements_of(block);
    const auto leaf1 = std::ranges::find_if(block_placements, [](const ViewPlacementData &p)
                                            { return p.location.x == 500; });
    ASSERT_NE(leaf1, block_placements.end());
    EXPECT_EQ(leaf1->bbox.ur.x, 510);
    EXPECT_EQ(leaf1->extent.ll.y, 495);
    EXPECT_EQ(leaf1->extent.ur.x, 515);
    EXPECT_EQ(leaf1->extent.ur.y, 512);

    // BLOCK's own extent is its (0,0)-(1000,1000) diearea - both leaves
    // fit inside it - and TOP's covers its (0,0)-(5000,5000) diearea.
    const Rect block_extent = block.extent;
    EXPECT_EQ(block_extent.ll.x, 0);
    EXPECT_EQ(block_extent.ur.x, 1000);
    EXPECT_EQ(output.view_data.at(HierarchyId{top_layout}).extent.ur.x, 5000);
}

// --- Incremental updates after an edit (Root change log) ---
//
// After each kind of edit, the stage must update its previous output from
// the change log - rebuilding only the touched chunks and sharing the rest
// - and the result must match a fresh full resolve exactly.

namespace
{
    // Everything a node draws and places, as comparable text - each rect/
    // polygon/path/label separately and sorted, since an incremental update
    // may list a moved object at a different place in its tile than a
    // full resolve would (draw order within one layer isn't significant).
    std::string describe(const ViewData &data)
    {
        std::map<std::uint32_t, std::vector<std::string>> by_layer;
        for (const ViewShapeChunk &chunk : data.chunks)
            if (chunk.shapes)
                for (const auto &[layer, shapes] : *chunk.shapes)
                    for (const RenderShape &shape : shapes)
                    {
                        std::vector<std::string> &out = by_layer[layer.index];
                        for (const Rect &r : shape.rects)
                            out.push_back("R" + to_string(r));
                        for (const Polygon &p : shape.polygons)
                            out.push_back("P" + to_string(p));
                        for (const Path &p : shape.paths)
                            out.push_back("W" + to_string(p));
                        for (const Text &t : shape.texts)
                            out.push_back("T" + to_string(t));
                    }
        std::string out;
        for (auto &[layer, elements] : by_layer)
        {
            std::ranges::sort(elements);
            out += "layer " + std::to_string(layer) + ":";
            for (const std::string &element : elements)
                out += " " + element;
            out += "\n";
        }
        std::vector<std::string> placements;
        for_each_placement(data, [&](const ViewPlacementData &placement)
                           { placements.push_back("placement " + to_string(placement.location) + " " + to_string(placement.bbox) + " " + to_string(placement.extent)); });
        std::ranges::sort(placements);
        for (const std::string &placement : placements)
            out += placement + "\n";
        out += "extent " + to_string(data.extent) + " depth " + std::to_string(data.remaining_depth) + "\n";
        return out;
    }

    void expect_same_output(const HierarchyResolverOutput &actual, const HierarchyResolverOutput &expected)
    {
        ASSERT_EQ(actual.view_data.size(), expected.view_data.size());
        for (const auto &[id, data] : expected.view_data)
        {
            ASSERT_TRUE(actual.view_data.contains(id));
            EXPECT_EQ(describe(actual.view_data.at(id)), describe(data));
        }
    }

    // How many of `id`'s chunks / placement tiles in `after` aren't shared
    // with `before` - i.e. were rebuilt.
    std::size_t rebuilt_chunks(const HierarchyResolverOutput &before, const HierarchyResolverOutput &after, HierarchyId id)
    {
        std::set<const ViewLayerShapes *> old;
        for (const ViewShapeChunk &chunk : before.view_data.at(id).chunks)
            old.insert(chunk.shapes.get());
        return std::ranges::count_if(after.view_data.at(id).chunks, [&](const ViewShapeChunk &chunk)
                                     { return !old.contains(chunk.shapes.get()); });
    }
    std::size_t rebuilt_placement_tiles(const HierarchyResolverOutput &before, const HierarchyResolverOutput &after, HierarchyId id)
    {
        std::set<const ViewPlacementTile *> old;
        for (const ViewPlacements &tile : before.view_data.at(id).placement_tiles)
            old.insert(tile.get());
        return std::ranges::count_if(after.view_data.at(id).placement_tiles, [&](const ViewPlacements &tile)
                                     { return !old.contains(tile.get()); });
    }

    struct IncrementalFixture : public HierarchyResolverStageFixture
    {
        // Runs the stage (it keeps its previous output), then a fresh one,
        // and checks they agree.
        const HierarchyResolverOutput &rerun_and_compare(int depth = 2)
        {
            root.bump_mutation_version();
            const HierarchyResolverOutput &updated = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, depth));
            HierarchyResolverRunner fresh{"fresh"};
            expect_same_output(updated, fresh.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, depth)));
            return updated;
        }

        PlacementId placement_named(LayoutId layout, const std::string &name)
        {
            for (const PlacementId id : root.get_layout_placements(layout))
                if (root.get_placement(id)->name == name)
                    return id;
            return PlacementId{};
        }
    };
}

TEST_F(IncrementalFixture, MovingAPlacementRebuildsOnlyItsLayoutsPlacements)
{
    const RouteId route = root.create_route(RouteData{.layout = block_layout});
    root.create_shape(ShapeData{.owner = le::ShapeOwner::route(route), .layer = m1, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{50, 5}}}});
    const HierarchyResolverOutput before = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));

    const PlacementId leaf1 = placement_named(block_layout, "leaf1");
    ASSERT_TRUE(root.update_placement(leaf1, block_layout, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, Point{600, 700}, std::nullopt, std::nullopt, std::nullopt));
    const HierarchyResolverOutput &after = rerun_and_compare();

    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
    // BLOCK's one placement tile (its rects/labels chunk and its
    // placements) is rebuilt; everything else is shared.
    EXPECT_EQ(rebuilt_chunks(before, after, HierarchyId{block_layout}), 1u);
    EXPECT_EQ(rebuilt_placement_tiles(before, after, HierarchyId{block_layout}), 1u);
    EXPECT_EQ(rebuilt_chunks(before, after, HierarchyId{leaf_abstract}), 0u);
    EXPECT_EQ(rebuilt_chunks(before, after, HierarchyId{top_layout}), 0u);
    EXPECT_EQ(rebuilt_placement_tiles(before, after, HierarchyId{top_layout}), 0u);
}

TEST_F(IncrementalFixture, EditingAndDeletingARouteRebuildsOnlyTheRoutesChunk)
{
    const RouteId route = root.create_route(RouteData{.layout = top_layout});
    const ShapeId shape = root.create_shape(ShapeData{.owner = le::ShapeOwner::route(route), .layer = m1, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{50, 5}}}});
    const HierarchyResolverOutput before = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));

    ASSERT_TRUE(root.update_shape(shape, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::vector<Rect>{Rect{.ll = Point{0, 0}, .ur = Point{80, 5}}}, std::nullopt, std::nullopt, std::nullopt));
    const HierarchyResolverOutput edited = rerun_and_compare();
    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
    EXPECT_EQ(rebuilt_chunks(before, edited, HierarchyId{top_layout}), 1u); // its route tile
    EXPECT_EQ(rebuilt_placement_tiles(before, edited, HierarchyId{top_layout}), 0u);

    // Deleted shape then route: found through the deleted route's logged owner.
    ASSERT_TRUE(root.delete_shape(shape));
    ASSERT_TRUE(root.delete_route(route));
    rerun_and_compare();
    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
}

TEST_F(IncrementalFixture, EditingACellsTerminalRebuildsOnlyThatCell)
{
    const HierarchyResolverOutput before = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));

    ShapeId terminal_shape;
    root.for_each_shape_id([&](ShapeId id)
                           { if (root.get_shape(id)->terminal_port().valid()) terminal_shape = id; });
    ASSERT_TRUE(root.update_shape(terminal_shape, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::vector<Rect>{Rect{.ll = Point{1, 1}, .ur = Point{3, 3}}}, std::nullopt, std::nullopt, std::nullopt));
    const HierarchyResolverOutput &after = rerun_and_compare();

    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
    EXPECT_NE(after.view_data.at(HierarchyId{leaf_abstract}).chunks[0].shapes, before.view_data.at(HierarchyId{leaf_abstract}).chunks[0].shapes);
    EXPECT_EQ(rebuilt_chunks(before, after, HierarchyId{block_layout}), 0u);
}

TEST_F(IncrementalFixture, ChangingACellsBoundaryRebuildsThePlacementsOfIt)
{
    runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));

    ShapeId boundary;
    root.for_each_shape_id([&](ShapeId id)
                           { if (root.get_shape(id)->abstract() == leaf_abstract) boundary = id; });
    ASSERT_TRUE(root.update_shape(boundary, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::vector<Rect>{Rect{.ll = Point{0, 0}, .ur = Point{20, 30}}}, std::nullopt, std::nullopt, std::nullopt));
    rerun_and_compare();
    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
}

TEST_F(IncrementalFixture, RepointingAPlacementResolvesTheNewCellAndDropsUnreachableOnes)
{
    // A second cell, created before the first run (creating an Abstract
    // resolves everything - what a design resolves to changes).
    const LibraryId library_id = root.get_design(root.get_abstract(leaf_abstract)->design)->library;
    const DesignId other_design = root.create_design(DesignData{.library = library_id, .name = "OTHER"});
    const AbstractId other_abstract = root.create_abstract(AbstractData{.design = other_design});
    root.create_shape(ShapeData{.owner = le::ShapeOwner::abstract(other_abstract), .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{40, 40}}}});
    runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));

    for (const char *name : {"leaf0", "leaf1"})
        ASSERT_TRUE(root.update_placement(placement_named(block_layout, name), block_layout, other_design, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt));
    const HierarchyResolverOutput &after = rerun_and_compare();

    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
    EXPECT_TRUE(after.view_data.contains(HierarchyId{other_abstract}));
    EXPECT_FALSE(after.view_data.contains(HierarchyId{leaf_abstract}));
}

TEST_F(IncrementalFixture, ALogicalEditSharesEveryChunk)
{
    const HierarchyResolverOutput before = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));
    const DesignId top_design = root.get_layout(top_layout)->design;
    const SchematicId schematic = root.create_schematic(SchematicData{.design = top_design});
    root.create_net(NetData{.schematic = schematic, .name = "n1"});
    const HierarchyResolverOutput &after = rerun_and_compare();

    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
    for (const auto &[id, data] : before.view_data)
        for (std::size_t c = 0; c < data.chunks.size(); ++c)
            EXPECT_EQ(after.view_data.at(id).chunks[c].shapes, data.chunks[c].shapes);
}

TEST_F(IncrementalFixture, TechnologyEditsAndASaturatedLogResolveEverything)
{
    runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));
    root.create_layer(LayerData{.technology = technology_id, .name = "M9", .type = "ROUTING"});
    rerun_and_compare();
    EXPECT_FALSE(runner.stage().last_compute_was_incremental());

    root.saturate_change_log();
    rerun_and_compare();
    EXPECT_FALSE(runner.stage().last_compute_was_incremental());
}

TEST_F(IncrementalFixture, ARebuiltViewLayerSetWithTheSameIdsStaysIncremental)
{
    const HierarchyResolverOutput before = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));

    // A layer color change: a new ViewLayerSet, same ids.
    ViewLayerSet recolored = ViewLayerSet::build_for_technology(root, technology_id);
    recolored.apply_color_overrides({{"M1", Color{1, 2, 3, 255}}});
    const ViewLayerSetHandle recolored_handle = std::make_shared<const ViewLayerSet>(recolored);
    const HierarchyResolverOutput &after = runner.run(recolored_handle, 1, options_for(HierarchyId{top_layout}, 2));

    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
    EXPECT_EQ(after.view_layers, recolored_handle);
    EXPECT_EQ(after.view_data.at(HierarchyId{leaf_abstract}).chunks[0].shapes, before.view_data.at(HierarchyId{leaf_abstract}).chunks[0].shapes);
}

// A Layout with thousands of routes and placements is split into tiles;
// an edit rebuilds only the tile(s) it touched.
TEST_F(IncrementalFixture, EditsInABigLayoutRebuildOnlyTheirTiles)
{
    // 5000 one-rect routes over TOP's 5000x5000 die, 5000 LEAF placements
    // over BLOCK's 1000x1000.
    std::vector<ShapeId> route_shapes;
    for (int i = 0; i < 5000; ++i)
    {
        const int64_t x = (i % 100) * 50;
        const int64_t y = (i / 100) * 100;
        const RouteId route = root.create_route(RouteData{.layout = top_layout, .name = "r" + std::to_string(i)}); // names are unique per Layout
        route_shapes.push_back(root.create_shape(ShapeData{.owner = le::ShapeOwner::route(route), .layer = m1, .rects = {Rect{.ll = Point{x, y}, .ur = Point{x + 20, y + 5}}}}));
    }
    const DesignId leaf_design = root.get_abstract(leaf_abstract)->design;
    std::vector<PlacementId> placements;
    for (int i = 0; i < 5000; ++i)
        placements.push_back(root.create_placement(PlacementData{.layout = block_layout, .name = "p" + std::to_string(i), .reference_design = leaf_design,
                                                                 .placement_status = PlacementStatus::PLACED,
                                                                 .location = Point{(i % 70) * 14, (i / 70) * 14}, .orientation = Orientation::N}));
    const HierarchyResolverOutput before = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));
    EXPECT_GT(before.view_data.at(HierarchyId{top_layout}).chunks.size(), kFixedLayoutChunkCount + 2); // several route tiles
    EXPECT_GT(before.view_data.at(HierarchyId{block_layout}).placement_tiles.size(), 1u);

    // One route shape: its tile only.
    ASSERT_TRUE(root.update_shape(route_shapes[2525], std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                  std::vector<Rect>{Rect{.ll = Point{1250, 2500}, .ur = Point{1290, 2505}}}, std::nullopt, std::nullopt, std::nullopt));
    const HierarchyResolverOutput route_edited = rerun_and_compare();
    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
    EXPECT_EQ(rebuilt_chunks(before, route_edited, HierarchyId{top_layout}), 1u);

    // A small placement move within its tile: that tile only.
    ASSERT_TRUE(root.update_placement(placements[1000], block_layout, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                      Point{(1000 % 70) * 14 + 1, (1000 / 70) * 14}, std::nullopt, std::nullopt, std::nullopt));
    const HierarchyResolverOutput moved = rerun_and_compare();
    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
    EXPECT_EQ(rebuilt_chunks(route_edited, moved, HierarchyId{block_layout}), 1u);
    EXPECT_EQ(rebuilt_placement_tiles(route_edited, moved, HierarchyId{block_layout}), 1u);

    // A move across the Layout: the old tile and the new one.
    ASSERT_TRUE(root.update_placement(placements[0], block_layout, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                      Point{980, 980}, std::nullopt, std::nullopt, std::nullopt));
    const HierarchyResolverOutput &far = rerun_and_compare();
    EXPECT_TRUE(runner.stage().last_compute_was_incremental());
    EXPECT_EQ(rebuilt_placement_tiles(moved, far, HierarchyId{block_layout}), 2u);

    // Deleting a route: its tile only.
    const HierarchyResolverOutput before_delete = far;
    const RouteId route = root.get_shape(route_shapes[10])->route();
    ASSERT_TRUE(root.delete_shape(route_shapes[10]));
    ASSERT_TRUE(root.delete_route(route));
    const HierarchyResolverOutput &deleted = rerun_and_compare();
    EXPECT_EQ(rebuilt_chunks(before_delete, deleted, HierarchyId{top_layout}), 1u);
}

// Click selection reads the render tree, so every render shape in a
// selectable chunk must name the database object behind it (ChunkSources):
// a route shape and its via geometry the route Shape, a port shape its
// Shape, a port marker nothing, and each placement rect its Placement.
TEST_F(HierarchyResolverStageFixture, ChunkSourcesNameTheObjectBehindEverySelectableRenderShape)
{
    const LayerId m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
    view_layers = ViewLayerSet::build_for_technology(root, technology_id);
    view_layers_handle = std::make_shared<const ViewLayerSet>(view_layers);
    const ViaId via = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via, .layer_name = "M2", .rects = {Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    const RouteId route = root.create_route(RouteData{.layout = top_layout, .name = "R1"});
    const ShapeId route_shape = root.create_shape(ShapeData{.owner = le::ShapeOwner::route(route), .layer = m1, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{100, 10}}},
                                                            .vias = {ShapeVia{.via_name = "VIA12", .origin = Point{50, 5}}}});
    const PhysicalPortId port = root.create_physical_port(PhysicalPortData{.layout = top_layout, .name = "IN", .direction = SignalDirection::INPUT});
    const PhysicalPortSegmentId segment = root.create_physical_port_segment(PhysicalPortSegmentData{.physical_port = port});
    const ShapeId port_shape = root.create_shape(ShapeData{.owner = le::ShapeOwner::physical_port_segment(segment), .layer = m1, .rects = {Rect{.ll = Point{0, 2000}, .ur = Point{100, 2040}}}});

    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 1));
    const ViewData &top = output.view_data.at(HierarchyId{top_layout});
    const ViewLayerId placement_layer = view_layers.placement_view_layer();

    std::map<std::pair<std::uint32_t, std::uint32_t>, std::set<std::uint32_t>> layers_of; // shape -> view layers it's behind
    std::size_t marker_entries = 0;
    std::vector<PlacementId> placements;
    for (const ViewShapeChunk &chunk : top.chunks)
    {
        if (!chunk.sources)
            continue;
        for (const auto &[layer, shapes] : *chunk.shapes)
        {
            if (layer == placement_layer)
            {
                ASSERT_EQ(shapes.size(), 1u); // one batched shape per tile
                ASSERT_EQ(chunk.sources->placements.size(), shapes[0].rects.size());
                placements.insert(placements.end(), chunk.sources->placements.begin(), chunk.sources->placements.end());
                continue;
            }
            ASSERT_TRUE(chunk.sources->shapes.contains(layer));
            const std::vector<ShapeId> &sources = chunk.sources->shapes.at(layer);
            ASSERT_EQ(sources.size(), shapes.size()); // index-parallel
            for (const ShapeId id : sources)
            {
                if (layer == view_layers.port_marker_view_layer())
                {
                    EXPECT_FALSE(id.valid());
                    ++marker_entries;
                }
                else
                    layers_of[{id.index, id.generation}].insert(layer.index);
            }
        }
    }

    const std::set<std::uint32_t> route_layers = layers_of[{route_shape.index, route_shape.generation}];
    EXPECT_TRUE(route_layers.contains(view_layers.find(m1, ViewLayerPurpose::ROUTE).index));
    EXPECT_TRUE(route_layers.contains(view_layers.find(m2, ViewLayerPurpose::ROUTE).index)); // its via
    const std::set<std::uint32_t> port_layers = layers_of[{port_shape.index, port_shape.generation}];
    EXPECT_TRUE(port_layers.contains(view_layers.find(m1, ViewLayerPurpose::TERMINAL).index));
    EXPECT_EQ(marker_entries, 1u);
    ASSERT_EQ(placements.size(), 1u);
    EXPECT_EQ(placements[0], root.get_layout_placements(top_layout).front());
}

TEST_F(IncrementalFixture, ChangingTheObjectFiltersDoesNotReResolve)
{
    const ViewRenderOptions options = options_for(HierarchyId{top_layout}, 2);
    const HierarchyResolverOutput *before = &runner.run(view_layers_handle, 0, options);

    ViewRenderOptions filtered = options;
    filtered.hidden_objects = ObjectFilterSets{.placement_types = {"CORE"}, .route_uses = {"POWER"}};
    EXPECT_EQ(&runner.run(view_layers_handle, 0, filtered), before); // memoized - same output, no compute
}

TEST_F(HierarchyResolverStageFixture, PlacementChunkOffsetPointsAtTheFirstPlacementTilesChunk)
{
    const HierarchyResolverOutput &output = runner.run(view_layers_handle, 0, options_for(HierarchyId{top_layout}, 2));
    const ViewData &block = output.view_data.at(HierarchyId{block_layout});
    ASSERT_LT(block.placement_chunk_offset, block.chunks.size());
    const ViewShapeChunk &chunk = block.chunks[block.placement_chunk_offset];
    ASSERT_TRUE(chunk.sources);
    EXPECT_EQ(chunk.sources->placements.size(), block.placement_tiles.front()->placements.size()); // leaf0, leaf1
    EXPECT_EQ(chunk.sources->placements.size(), 2u);
}
