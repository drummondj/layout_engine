// backend/ONETBB_INTEGRATION.md migration plan, Phase 4: HierarchyResolver -
// a representative subset of instancing_test.cpp's own InstancingFixture
// cases, covering both Phase B (resolve_design_picture/build_layout_picture)
// and Phase C (render_layout_frame) scope.
#include "../hierarchy_resolver.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include <gtest/gtest.h>

namespace
{
    struct HierarchyResolverFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(le::TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(le::LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            view_layers = le::ViewLayerSet::build_for_technology(root, technology_id);
            library_id = root.create_library(le::LibraryData{.name = "LIB"});
        }

        le::DesignId create_design(const std::string &name)
        {
            return root.create_design(le::DesignData{.library = library_id, .name = name});
        }

        le::DesignId create_leaf_design(const std::string &name, le::Point size)
        {
            le::DesignId design_id = create_design(name);
            le::AbstractId abstract_id = root.create_abstract(le::AbstractData{.design = design_id, .size = size});
            le::ObstructionId obstruction_id = root.create_obstruction(le::ObstructionData{.abstract = abstract_id});
            le::Shape shape;
            shape.obstruction = obstruction_id;
            shape.layer = m1;
            shape.rects.push_back(le::Rect{.ll = {0, 0}, .ur = size});
            root.create_shape(std::move(shape));
            return design_id;
        }

        std::pair<le::DesignId, le::LayoutId> create_layout_design(const std::string &name, le::Point die_size)
        {
            le::DesignId design_id = create_design(name);
            le::LayoutId layout_id = root.create_layout(le::LayoutData{.design = design_id});
            le::Shape diearea;
            diearea.layout = layout_id;
            diearea.purpose = le::ShapePurpose::BOUNDARY;
            diearea.rects.push_back(le::Rect{.ll = {0, 0}, .ur = die_size});
            root.create_shape(std::move(diearea));
            return {design_id, layout_id};
        }

        le::PlacementId add_placement(le::LayoutId layout_id, le::DesignId reference_design, le::Point location, le::Orientation orientation, const std::string &name)
        {
            return root.create_placement(le::PlacementData{
                .layout = layout_id,
                .name = name,
                .reference_design = reference_design,
                .location = location,
                .orientation = orientation,
            });
        }

        SkBitmap rasterize(const sk_sp<SkPicture> &picture, int width, int height)
        {
            sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
            surface->getCanvas()->clear(SK_ColorTRANSPARENT);
            surface->getCanvas()->drawPicture(picture);

            SkBitmap bitmap;
            bitmap.allocPixels(SkImageInfo::MakeN32Premul(width, height));
            surface->readPixels(bitmap, 0, 0);
            return bitmap;
        }

        bool region_has_opaque_pixel(const SkBitmap &bitmap, int x0, int y0, int x1, int y1)
        {
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (SkColorGetA(bitmap.getColor(x, y)) > 0)
                        return true;
            return false;
        }

        // Same region-scan reasoning as region_has_opaque_pixel above, but
        // reading a raw PixelBuffer (render_layout_frame's own return
        // type) directly - checks for a *colored* (non-grayscale) opaque
        // pixel specifically, not just any opacity, since render_layout_frame
        // also draws a background dot/axis grid across the whole viewport
        // (always gray/white). M1 (a ROUTING layer) never gets a
        // grayscale default color, so r != g reliably distinguishes real
        // M1 content from the grid.
        bool pixel_buffer_region_has_opaque_pixel(const le::PixelBuffer &buffer, int x0, int y0, int x1, int y1)
        {
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const uint8_t *p = buffer.data + static_cast<size_t>(y) * buffer.row_bytes + static_cast<size_t>(x) * 4;
                    if (p[3] > 0 && p[0] != p[1])
                        return true;
                }
            return false;
        }

        le::Root root;
        le::TechnologyId technology_id;
        le::LayerId m1;
        le::ViewLayerSet view_layers;
        le::LibraryId library_id;
        le::HierarchyResolver resolver;
        le::Scene scene;
    };
}

TEST_F(HierarchyResolverFixture, SinglePlacementDrawsLeafAtItsAbsoluteLocation)
{
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{500, 500}, le::Orientation::N, "U1");

    const sk_sp<SkPicture> picture = resolver.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 800, 800);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 510, 510, 590, 590));  // inside the leaf's world rect (500,500)-(600,600)
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 700, 700, 750, 750)); // outside both the leaf and sub's own diearea outline
}

TEST_F(HierarchyResolverFixture, TwoLevelHierarchyRecursesAndPlacesTheLeafRelativeToItsSubBlock)
{
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{0, 0}, le::Orientation::N, "U1");

    auto [top_design, top_layout] = create_layout_design("TOP", le::Point{3000, 3000});
    add_placement(top_layout, sub_design, le::Point{1000, 1000}, le::Orientation::N, "U1");

    const sk_sp<SkPicture> picture = resolver.build_layout_picture(root, top_layout, /*remaining_depth=*/2, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 4000, 4000);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 1010, 1010, 1090, 1090));  // inside the leaf
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 1250, 1250, 1290, 1290)); // inside sub's own diearea but outside the leaf
}

TEST_F(HierarchyResolverFixture, NestedLayoutRecursionDoesNotCorruptTheOuterLayoutsOwnDirectContent)
{
    // Regression for the same "own content must survive resolving a
    // different node's content" guarantee the pre-graph design's own
    // single-slot generate_abstract_stage_/generate_layout_stage_
    // sharing needed a copy-before-recursion fix for: TOP's own direct
    // content (a real routing Blockage, not just its diearea outline)
    // must survive discovering/resolving SUB (a DIFFERENT LayoutId, its
    // own HierarchyLayoutNodeStage node) and, deeper still, LEAF's own
    // Abstract (its own HierarchyAbstractLeafStage node). In this design
    // each node owns its own permanent, per-node
    // AbstractGeometryStage/LayoutGeometryStage runner (no single-slot
    // aliasing hazard is even structurally possible any more - see
    // HierarchyLayoutNodeStage's own doc comment) - this test still
    // exercises the equivalent end-to-end guarantee (TOP's own content
    // isn't corrupted by resolving its own nested SUB/LEAF subtree)
    // rather than the original single-slot-race mechanism specifically.
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{0, 0}, le::Orientation::N, "U1");

    auto [top_design, top_layout] = create_layout_design("TOP", le::Point{3000, 3000});
    add_placement(top_layout, sub_design, le::Point{1000, 1000}, le::Orientation::N, "U1");

    const le::BlockageId blockage_id = root.create_blockage(le::BlockageData{.layout = top_layout, .kind = le::BlockageKind::ROUTING});
    le::Shape blockage_shape;
    blockage_shape.blockage = blockage_id;
    blockage_shape.layer = m1;
    blockage_shape.rects.push_back(le::Rect{.ll = {2000, 2000}, .ur = {2200, 2200}});
    root.create_shape(std::move(blockage_shape));

    const sk_sp<SkPicture> picture = resolver.build_layout_picture(root, top_layout, /*remaining_depth=*/2, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 4000, 4000);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 2010, 2010, 2190, 2190)); // TOP's own blockage - must survive the recursive SUB/LEAF resolution
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 1010, 1010, 1090, 1090)); // the nested leaf still resolves correctly too
}

TEST_F(HierarchyResolverFixture, ResolveDesignPictureFallsBackToAbstractOnlyWhenRemainingDepthIsExhausted)
{
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400}); // Layout only, no Abstract
    add_placement(sub_layout, leaf, le::Point{0, 0}, le::Orientation::N, "U1");

    EXPECT_TRUE(resolver.resolve_design_picture(root, sub_design, /*remaining_depth=*/1, view_layers, scene, 1.0));
    EXPECT_FALSE(resolver.resolve_design_picture(root, sub_design, /*remaining_depth=*/0, view_layers, scene, 1.0));
}

TEST_F(HierarchyResolverFixture, UnresolvedReferenceDesignIsSkippedWithoutAffectingOtherPlacements)
{
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{100, 100}, le::Orientation::N, "GOOD");
    add_placement(sub_layout, le::DesignId{}, le::Point{0, 0}, le::Orientation::N, "BAD"); // invalid reference_design

    const sk_sp<SkPicture> picture = resolver.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 800, 800);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 110, 110, 190, 190)); // the valid placement still drew, at world (100,100)-(200,200)
}

TEST_F(HierarchyResolverFixture, ReusesCacheUntilAMutationOrScaleChangeInvalidatesTheWholeEpoch)
{
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{0, 0}, le::Orientation::N, "U1");

    resolver.build_layout_picture(root, sub_layout, 1, view_layers, scene, 1.0);
    const uint64_t after_first = resolver.design_picture_recompute_count();
    ASSERT_GT(after_first, 0u);

    resolver.build_layout_picture(root, sub_layout, 1, view_layers, scene, 1.0);
    EXPECT_EQ(resolver.design_picture_recompute_count(), after_first); // same epoch, same key - cache hit

    root.bump_mutation_version();
    resolver.build_layout_picture(root, sub_layout, 1, view_layers, scene, 1.0);
    EXPECT_GT(resolver.design_picture_recompute_count(), after_first); // mutation bumped - whole epoch invalidated
    const uint64_t after_mutation = resolver.design_picture_recompute_count();

    resolver.build_layout_picture(root, sub_layout, 1, view_layers, scene, 2.0); // different scale
    EXPECT_GT(resolver.design_picture_recompute_count(), after_mutation);
}

TEST_F(HierarchyResolverFixture, SubPixelCullingReflectsTheCurrentScaleNotAStaleOne)
{
    resolver.set_min_visible_instance_pixels(50.0);

    le::DesignId leaf = create_leaf_design("LEAF", le::Point{50, 50});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{100, 100}, le::Orientation::N, "U1");

    // scale=2.0: min_visible_dbu=25 - the 50x50 leaf is comfortably visible.
    const sk_sp<SkPicture> visible = resolver.build_layout_picture(root, sub_layout, 0, view_layers, scene, 2.0);
    ASSERT_TRUE(visible);
    EXPECT_TRUE(region_has_opaque_pixel(rasterize(visible, 800, 800), 220, 220, 280, 280));

    // scale=0.4: min_visible_dbu=125 - the same leaf's own PLACEMENT is
    // now sub-pixel, collapsing to its own boundary rect OUTLINE only.
    const sk_sp<SkPicture> outlined = resolver.build_layout_picture(root, sub_layout, 0, view_layers, scene, 0.4);
    ASSERT_TRUE(outlined);
    const SkBitmap bitmap = rasterize(outlined, 160, 160);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 39, 49, 41, 51)); // left edge of the outline, vertically centered
    EXPECT_EQ(SkColorGetA(bitmap.getColor(50, 50)), 0);           // dead center - unfilled
}

TEST_F(HierarchyResolverFixture, LayerVisibilityAppliesInsideACachedInstancePicture)
{
    // Regression for this port specifically (not present verbatim in
    // instancing_test.cpp, since the original always passed the SAME
    // Scene to both the throwaway cull culling and the real layer-
    // visibility check by construction) - build_abstract_picture/
    // build_layout_picture_uncached must check layer visibility against
    // the REAL Scene passed in, not the throwaway cull_scene used for
    // viewport culling (see record_local_picture's own comment). Hiding
    // M1 must make the leaf's own content disappear from a freshly
    // resolved instance picture.
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{500, 500}, le::Orientation::N, "U1");

    scene.set_layer_name_visible("M1", false);

    const sk_sp<SkPicture> picture = resolver.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 800, 800);
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 510, 510, 590, 590)); // M1 hidden - the leaf's own content must not draw
}

// --- render_layout_frame (Phase C) ---

TEST_F(HierarchyResolverFixture, RenderLayoutFrameDrawsAtTheCorrectPostFlipPixelPosition)
{
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{500, 500}, le::Orientation::N, "U1");

    scene.set_pan(le::Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);

    const le::PixelBuffer &buffer = resolver.render_layout_frame(root, sub_layout, /*hierarchy_depth=*/1, view_layers, scene);
    ASSERT_TRUE(buffer.data);
    EXPECT_EQ(buffer.width, 800);
    EXPECT_EQ(buffer.height, 800);

    // Pre-flip pixel rect (pan=0, scale=1) is exactly the leaf's own world
    // dbu rect, (500,500)-(600,600); the Y-flip baked into the shared
    // RasterizePictureStage chain maps dbu y -> height - y, so the
    // post-flip rect is (500,200)-(600,300).
    EXPECT_TRUE(pixel_buffer_region_has_opaque_pixel(buffer, 520, 220, 580, 280));
    EXPECT_FALSE(pixel_buffer_region_has_opaque_pixel(buffer, 10, 10, 50, 50));
}

TEST_F(HierarchyResolverFixture, RenderLayoutFramePanShiftsTheDrawnPosition)
{
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{500, 500}, le::Orientation::N, "U1");

    scene.set_pan(le::Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);
    resolver.render_layout_frame(root, sub_layout, 1, view_layers, scene); // establish the un-panned frame first

    scene.set_pan(le::Point{100, 100});
    const le::PixelBuffer &panned = resolver.render_layout_frame(root, sub_layout, 1, view_layers, scene);

    // world dbu rect (500,500)-(600,600), pan=(100,100) -> pre-flip pixel
    // (400,400)-(500,500) -> post-flip (height - y): (400,300)-(500,400).
    EXPECT_TRUE(pixel_buffer_region_has_opaque_pixel(panned, 420, 320, 480, 380));
    EXPECT_FALSE(pixel_buffer_region_has_opaque_pixel(panned, 520, 220, 580, 280));
}

TEST_F(HierarchyResolverFixture, RenderLayoutFrameReusesCacheUntilSceneChanges)
{
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{500, 500}, le::Orientation::N, "U1");

    scene.set_pan(le::Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);

    const le::PixelBuffer &first = resolver.render_layout_frame(root, sub_layout, 1, view_layers, scene);
    const uint8_t *first_data = first.data;

    const le::PixelBuffer &second = resolver.render_layout_frame(root, sub_layout, 1, view_layers, scene);
    EXPECT_EQ(second.data, first_data); // unchanged scene - cache hit, same underlying surface

    scene.set_pan(le::Point{10, 10});
    const le::PixelBuffer &after_pan = resolver.render_layout_frame(root, sub_layout, 1, view_layers, scene);
    EXPECT_NE(after_pan.data, first_data); // pan changed - must recompute
}

TEST_F(HierarchyResolverFixture, RenderLayoutFrameRecomputesAfterARootMutation)
{
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{500, 500}, le::Orientation::N, "U1");

    scene.set_pan(le::Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);

    const le::PixelBuffer &first = resolver.render_layout_frame(root, sub_layout, 1, view_layers, scene);
    const uint8_t *first_data = first.data;

    root.bump_mutation_version();
    const le::PixelBuffer &after_mutation = resolver.render_layout_frame(root, sub_layout, 1, view_layers, scene);
    EXPECT_NE(after_mutation.data, first_data); // mutation invalidated the whole epoch - must recompute
    EXPECT_TRUE(pixel_buffer_region_has_opaque_pixel(after_mutation, 520, 220, 580, 280));
}

// --- MemoizingStage/tbb::flow::graph rewrite - new coverage ---

TEST_F(HierarchyResolverFixture, DedupByKeyComputesASharedLeafExactlyOnceRegardlessOfPlacementCount)
{
    // N placements of the SAME leaf design in one Layout - direct test of
    // "one graph node per distinct NodeKey, not one per Placement."
    // 100x100, matching the size convention every other fixture in this
    // file uses - anything smaller than min_visible_instance_pixels_
    // (100.0 default) at scale=1.0 gets sub-pixel-culled into a
    // tiny_instance_rects entry instead of a real graph edge, which would
    // make this test exercise culling, not dedup.
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{4000, 4000});
    constexpr int kPlacementCount = 100;
    for (int i = 0; i < kPlacementCount; ++i)
        add_placement(sub_layout, leaf, le::Point{static_cast<int64_t>(i) * 20, 0}, le::Orientation::N, "U" + std::to_string(i));

    const uint64_t before = resolver.design_picture_recompute_count();
    const sk_sp<SkPicture> picture = resolver.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    // Exactly 2 new recomputes total - one for LEAF's own shared
    // HierarchyAbstractLeafStage node, one for SUB's own
    // HierarchyLayoutNodeStage node - regardless of kPlacementCount.
    EXPECT_EQ(resolver.design_picture_recompute_count() - before, 2u);
}

TEST_F(HierarchyResolverFixture, IncrementalMultiRootDiscoverySharesAChildAcrossTwoRootsWithinOneEpoch)
{
    // Two different, partially-overlapping roots resolved within one
    // epoch (shallow-ish then a second root sharing the first's own
    // child) - targets ensure_node_built's own incremental extension of
    // an already-live, already-quiescent flow_graph.
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{0, 0}, le::Orientation::N, "U1");

    auto [top_a_design, top_a_layout] = create_layout_design("TOPA", le::Point{3000, 3000});
    add_placement(top_a_layout, sub_design, le::Point{0, 0}, le::Orientation::N, "U1");

    auto [top_b_design, top_b_layout] = create_layout_design("TOPB", le::Point{3000, 3000});
    add_placement(top_b_layout, sub_design, le::Point{500, 500}, le::Orientation::N, "U1");

    resolver.build_layout_picture(root, top_a_layout, /*remaining_depth=*/1, view_layers, scene, 1.0);
    const uint64_t after_a = resolver.design_picture_recompute_count();
    ASSERT_GT(after_a, 0u);

    // TOPB references the SAME sub_design at the SAME remaining_depth -
    // its own {sub_layout, 0} and LEAF's own Abstract node should already
    // be live from TOPA's own discovery above (same NodeKey), so only
    // TOPB's own new Layout node should recompute.
    resolver.build_layout_picture(root, top_b_layout, /*remaining_depth=*/1, view_layers, scene, 1.0);
    const uint64_t after_b = resolver.design_picture_recompute_count();

    EXPECT_EQ(after_b - after_a, 1u); // SUB/LEAF reused, not doubled
}

TEST_F(HierarchyResolverFixture, PruningRemovesStaleDepthOnlyNodesButKeepsStillSharedNodesLiveAndUncomputed)
{
    le::DesignId leaf = create_leaf_design("LEAF", le::Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", le::Point{400, 400});
    add_placement(sub_layout, leaf, le::Point{0, 0}, le::Orientation::N, "U1");

    auto [top_design, top_layout] = create_layout_design("TOP", le::Point{3000, 3000});
    add_placement(top_layout, sub_design, le::Point{1000, 1000}, le::Orientation::N, "U1");

    // Deep resolve first (remaining_depth=2): reaches SUB's own
    // {sub_layout, remaining_depth=1} node and, deeper still, LEAF's own
    // Abstract node.
    resolver.build_layout_picture(root, top_layout, /*remaining_depth=*/2, view_layers, scene, 1.0);
    ASSERT_TRUE(resolver.has_node_for_test(sub_layout, /*remaining_depth=*/1));

    // Re-request the SAME top_layout at a SHALLOWER depth (1) repeatedly
    // - a genuinely different {top_layout, 1}/{sub_layout, 0} subtree
    // (Kind::Layout keys are depth-sensitive), sharing only LEAF's own
    // Abstract node (Kind::Abstract keys collapse depth away) with the
    // depth-2 subtree above. Enough repeats to cross
    // kStaleGenerationWindow so the depth-2-only nodes go untouched long
    // enough to be pruned.
    for (int i = 0; i < 3; ++i)
        resolver.build_layout_picture(root, top_layout, /*remaining_depth=*/1, view_layers, scene, 1.0);

    // (a) the depth-2-only nodes are gone.
    EXPECT_FALSE(resolver.has_node_for_test(top_layout, /*remaining_depth=*/2));
    EXPECT_FALSE(resolver.has_node_for_test(sub_layout, /*remaining_depth=*/1));

    // (b) the still-in-use shallow nodes remain, and a repeated identical
    // request doesn't recompute anything - including LEAF's own node,
    // which survived being shared across both subtrees rather than being
    // wrongly swept as part of the depth-2 subtree's own cleanup.
    ASSERT_TRUE(resolver.has_node_for_test(top_layout, /*remaining_depth=*/1));
    ASSERT_TRUE(resolver.has_node_for_test(sub_layout, /*remaining_depth=*/0));
    const uint64_t before_repeat = resolver.design_picture_recompute_count();
    resolver.build_layout_picture(root, top_layout, /*remaining_depth=*/1, view_layers, scene, 1.0);
    EXPECT_EQ(resolver.design_picture_recompute_count(), before_repeat);

    // (c) a subsequent depth-2 request after pruning still resolves
    // correctly - full re-discovery/re-build of the pruned subtree, not a
    // dangling reference into anything unwired/destroyed by the sweep.
    const sk_sp<SkPicture> picture = resolver.build_layout_picture(root, top_layout, /*remaining_depth=*/2, view_layers, scene, 1.0);
    ASSERT_TRUE(picture);
    const SkBitmap bitmap = rasterize(picture, 4000, 4000);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 1010, 1010, 1090, 1090)); // the nested leaf still resolves correctly
}

TEST_F(HierarchyResolverFixture, InterleavedRootsAlternatingEveryCallAreNeverPruned)
{
    // Guards kStaleGenerationWindow's own off-by-one: two roots that each
    // get touched only every OTHER top-level call must never see their
    // own nodes pruned out from under them.
    le::DesignId leaf_a = create_leaf_design("LEAFA", le::Point{100, 100});
    auto [sub_a_design, sub_a_layout] = create_layout_design("SUBA", le::Point{400, 400});
    add_placement(sub_a_layout, leaf_a, le::Point{0, 0}, le::Orientation::N, "U1");

    le::DesignId leaf_b = create_leaf_design("LEAFB", le::Point{100, 100});
    auto [sub_b_design, sub_b_layout] = create_layout_design("SUBB", le::Point{400, 400});
    add_placement(sub_b_layout, leaf_b, le::Point{0, 0}, le::Orientation::N, "U1");

    for (int i = 0; i < 6; ++i)
    {
        if (i % 2 == 0)
            resolver.build_layout_picture(root, sub_a_layout, /*remaining_depth=*/0, view_layers, scene, 1.0);
        else
            resolver.build_layout_picture(root, sub_b_layout, /*remaining_depth=*/0, view_layers, scene, 1.0);

        if (i < 1)
            continue; // the other root's own node doesn't exist yet
        EXPECT_TRUE(resolver.has_node_for_test(sub_a_layout, 0)) << "iteration " << i;
        EXPECT_TRUE(resolver.has_node_for_test(sub_b_layout, 0)) << "iteration " << i;
    }
}

TEST_F(HierarchyResolverFixture, ConcurrencySmokeTestManyDistinctReferenceDesignsResolveCorrectly)
{
    // Fan-out over many GENUINELY DISTINCT reference designs (not all the
    // same, unlike the dedup-by-key test above) - each becomes its own
    // independent sibling HierarchyAbstractLeafStage node, free to be
    // scheduled concurrently by TBB once triggered (see
    // hierarchy_resolver.hpp's own class doc comment for why touching
    // only each node's own private nested-graph runners makes that
    // safe). A plain Release/Debug run only confirms the RESULT is
    // correct (right node count, no crash) - real race coverage needs
    // ThreadSanitizer or a TBB_USE_ASSERT build, flagged as a manual/CI
    // action item, not something this single run can prove on its own.
    constexpr int kDesignCount = 200;
    auto [top_design, top_layout] = create_layout_design("TOP", le::Point{300'000, 300'000});
    for (int i = 0; i < kDesignCount; ++i)
    {
        le::DesignId leaf = create_leaf_design("LEAF" + std::to_string(i), le::Point{100, 100});
        add_placement(top_layout, leaf, le::Point{static_cast<int64_t>(i) * 1000, 0}, le::Orientation::N, "U" + std::to_string(i));
    }

    const uint64_t before = resolver.design_picture_recompute_count();
    const sk_sp<SkPicture> picture = resolver.build_layout_picture(root, top_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    // One node per distinct LEAF design plus one for TOP itself - if
    // concurrent sibling execution corrupted shared state (e.g. a data
    // race on the recompute counter), this count would be unstable
    // across runs rather than exactly kDesignCount + 1 every time.
    EXPECT_EQ(resolver.design_picture_recompute_count() - before, static_cast<uint64_t>(kDesignCount) + 1);
}
