#include "../instance_renderer.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    // Builds a Root with one Technology/M1 layer, a matching ViewLayerSet,
    // and one Library - the common scaffolding every test below attaches
    // to, plus helpers for building small Design hierarchies (Placement ->
    // Design -> Abstract-or-Layout) without going through a DEF/LEF file.
    struct InstancingFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);
            library_id = root.create_library(LibraryData{.name = "LIB"});
        }

        DesignId create_design(const std::string &name)
        {
            return root.create_design(DesignData{.library = library_id, .name = name});
        }

        // A Design whose Abstract's declared size is `size`, containing
        // one Obstruction Shape that fills that whole size (0,0)-(size) on
        // M1 - makes "is this leaf's own content visible at pixel P"
        // trivial to check (any pixel within the leaf's own transformed
        // bbox should be opaque), without needing to hand-track a smaller
        // sub-rect through a rotation.
        DesignId create_leaf_design(const std::string &name, Point size)
        {
            DesignId design_id = create_design(name);
            AbstractId abstract_id = root.create_abstract(AbstractData{.design = design_id, .size = size});
            ObstructionId obstruction_id = root.create_obstruction(ObstructionData{.abstract = abstract_id});
            Shape shape;
            shape.obstruction = obstruction_id;
            shape.layer = m1;
            shape.rects.push_back(Rect{.ll = {0, 0}, .ur = size});
            root.create_shape(std::move(shape));
            return design_id;
        }

        // A Design with a Layout only (no Abstract) whose die area is
        // (0,0)-die_size.
        std::pair<DesignId, LayoutId> create_layout_design(const std::string &name, Point die_size)
        {
            DesignId design_id = create_design(name);
            LayoutId layout_id = root.create_layout(LayoutData{.design = design_id});
            Shape diearea;
            diearea.layout = layout_id;
            diearea.purpose = ShapePurpose::BOUNDARY;
            diearea.rects.push_back(Rect{.ll = {0, 0}, .ur = die_size});
            root.create_shape(std::move(diearea));
            return {design_id, layout_id};
        }

        PlacementId add_placement(LayoutId layout_id, DesignId reference_design, Point location, Orientation orientation, const std::string &name)
        {
            return root.create_placement(PlacementData{
                .layout = layout_id,
                .name = name,
                .reference_design = reference_design,
                .location = location,
                .orientation = orientation,
            });
        }

        // Rasterizes `picture` into a fresh, explicitly-cleared-to-
        // transparent surface - same technique as render_test.cpp's own
        // rasterize/sample_pixel (a different translation unit's
        // anonymous-namespace member, not reusable directly). Reads the
        // picture's own unflipped pixel space directly (no Y-flip - that's
        // RasterizeStage's own whole-canvas concern, nothing
        // InstanceRenderer produces goes through it in this test).
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

        // The leaf's own content is drawn as an OBSTRUCTION, whose
        // FillPattern (BRICK) is a tiled shader with real transparent gaps
        // - not a solid fill (see render_test.cpp's own analogous
        // region_shows_color helper/comment). A single fixed-point pixel
        // sample can coincidentally land on a gap even though the shape
        // really is there, so every check below scans a region instead.
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
        // type) directly, same technique as render_test.cpp's own
        // is_white-style helpers. Checks for a *colored* (non-grayscale)
        // opaque pixel specifically, not just any opacity - render_layout_frame
        // also draws a background dot/axis grid across the WHOLE viewport
        // (draw_grid, always gray/white - kMinorGridColor/kMajorGridColor
        // in draw_helpers.hpp), so a plain alpha-presence check would false-
        // positive on a grid dot even where no real content was drawn. M1
        // (a ROUTING layer) gets kRoutingCutColors[0] = pure red
        // (255,0,0,255) - never grayscale - so r != g reliably distinguishes
        // real M1 content from the grid.
        bool pixel_buffer_region_has_opaque_pixel(const PixelBuffer &buffer, int x0, int y0, int x1, int y1)
        {
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const uint8_t *p = buffer.data + static_cast<size_t>(y) * static_cast<size_t>(buffer.row_bytes) + static_cast<size_t>(x) * 4;
                    if (p[3] > 0 && p[0] != p[1])
                        return true;
                }
            return false;
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        ViewLayerSet view_layers;
        LibraryId library_id;
        InstanceRenderer instance_renderer;
        Scene scene; // default-constructed - every ViewLayer visible
        int next_placement_index = 0;
    };
}

TEST_F(InstancingFixture, SinglePlacementDrawsLeafAtItsAbsoluteLocation)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{500, 500}, Orientation::N, "U1");

    const sk_sp<SkPicture> picture = instance_renderer.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 800, 800);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 510, 510, 590, 590)); // inside the leaf's world rect (500,500)-(600,600)
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 700, 700, 750, 750)); // outside both the leaf and sub's own diearea outline
}

TEST_F(InstancingFixture, OrientationRotatesTheInstancedLeaf)
{
    // Asymmetric bbox (200 wide, 100 tall) - the case a memorized
    // per-orientation W/H-swap table gets subtly wrong (see
    // geometry_test.cpp's own analogous case) - here cross-checked
    // end-to-end through real Skia picture playback, not just the
    // Geometry unit math.
    DesignId leaf = create_leaf_design("LEAF", Point{200, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::W, "U1");

    const sk_sp<SkPicture> picture = instance_renderer.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    // W-rotated: world bbox becomes (0,0)-(100,200) (swapped) - a region
    // that would have been inside the UNROTATED (0,0)-(200,100) footprint
    // but is outside the rotated one is the strongest possible check that
    // rotation, not just translation, actually happened.
    const SkBitmap bitmap = rasterize(picture, 400, 400);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 10, 110, 90, 190)); // inside the rotated bbox
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 110, 10, 190, 90)); // inside the UNrotated bbox only
}

TEST_F(InstancingFixture, TwoLevelHierarchyRecursesAndPlacesTheLeafRelativeToItsSubBlock)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::N, "U1");

    auto [top_design, top_layout] = create_layout_design("TOP", Point{3000, 3000});
    add_placement(top_layout, sub_design, Point{1000, 1000}, Orientation::N, "U1");

    const sk_sp<SkPicture> picture = instance_renderer.build_layout_picture(root, top_layout, /*remaining_depth=*/2, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    // sub's own declared bbox (its diearea, (0,0)-(400,400)) lands its own
    // ll at (1000,1000) under identity orientation - the leaf, placed at
    // sub-local (0,0)-(100,100), should therefore land at world
    // (1000,1000)-(1100,1100).
    const SkBitmap bitmap = rasterize(picture, 4000, 4000);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 1010, 1010, 1090, 1090)); // inside the leaf
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 1250, 1250, 1290, 1290)); // inside sub's own diearea but outside the leaf
}

TEST_F(InstancingFixture, ResolveDesignPictureFallsBackToAbstractOnlyWhenRemainingDepthIsExhausted)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400}); // Layout only, no Abstract
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::N, "U1");

    // remaining_depth > 0 and sub has a Layout - resolves via recursion.
    EXPECT_TRUE(instance_renderer.resolve_design_picture(root, sub_design, /*remaining_depth=*/1, view_layers, scene, 1.0));

    // remaining_depth == 0 - falls back to sub's own Abstract, which
    // doesn't exist - nothing to draw.
    EXPECT_FALSE(instance_renderer.resolve_design_picture(root, sub_design, /*remaining_depth=*/0, view_layers, scene, 1.0));
}

TEST_F(InstancingFixture, UnresolvedReferenceDesignIsSkippedWithoutAffectingOtherPlacements)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{100, 100}, Orientation::N, "GOOD");
    add_placement(sub_layout, DesignId{}, Point{0, 0}, Orientation::N, "BAD"); // invalid reference_design

    const sk_sp<SkPicture> picture = instance_renderer.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 800, 800);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 110, 110, 190, 190)); // the valid placement still drew, at world (100,100)-(200,200)
}

TEST_F(InstancingFixture, PlacementWithNoLocationIsSkippedWithoutCrashing)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    root.create_placement(PlacementData{.layout = sub_layout, .name = "UNPLACED", .reference_design = leaf}); // no location/orientation

    const sk_sp<SkPicture> picture = instance_renderer.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture); // doesn't crash - the Layout's own diearea still records
}

TEST_F(InstancingFixture, SelfReferencingLayoutTerminatesAndFallsBackToAbstract)
{
    DesignId self_design = create_design("SELF");
    AbstractId self_abstract = root.create_abstract(AbstractData{.design = self_design, .size = Point{50, 50}});
    ObstructionId obstruction_id = root.create_obstruction(ObstructionData{.abstract = self_abstract});
    Shape shape;
    shape.obstruction = obstruction_id;
    shape.layer = m1;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {50, 50}});
    root.create_shape(std::move(shape));

    LayoutId self_layout = root.create_layout(LayoutData{.design = self_design});
    add_placement(self_layout, self_design, Point{10, 10}, Orientation::N, "SELF_INSTANCE"); // places itself

    // Bounded by remaining_depth strictly decreasing on every recursion
    // step - must return (not hang/crash/stack-overflow) regardless of
    // this self-referential Root content.
    const sk_sp<SkPicture> picture = instance_renderer.build_layout_picture(root, self_layout, /*remaining_depth=*/3, view_layers, scene, /*scale=*/1.0);
    EXPECT_TRUE(picture);
}

TEST_F(InstancingFixture, ReusesCacheUntilAMutationOrScaleChangeInvalidatesTheWholeEpoch)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::N, "U1");

    instance_renderer.build_layout_picture(root, sub_layout, 1, view_layers, scene, 1.0);
    const uint64_t after_first = instance_renderer.design_picture_recompute_count();
    ASSERT_GT(after_first, 0u);

    instance_renderer.build_layout_picture(root, sub_layout, 1, view_layers, scene, 1.0);
    EXPECT_EQ(instance_renderer.design_picture_recompute_count(), after_first); // same epoch, same key - cache hit

    root.bump_mutation_version();
    instance_renderer.build_layout_picture(root, sub_layout, 1, view_layers, scene, 1.0);
    EXPECT_GT(instance_renderer.design_picture_recompute_count(), after_first); // mutation bumped - whole epoch invalidated
    const uint64_t after_mutation = instance_renderer.design_picture_recompute_count();

    instance_renderer.build_layout_picture(root, sub_layout, 1, view_layers, scene, 2.0); // different scale
    EXPECT_GT(instance_renderer.design_picture_recompute_count(), after_mutation);
}

TEST_F(InstancingFixture, SubPixelCullingReflectsTheCurrentScaleNotAStaleOne)
{
    // Regression: FilterByViewportAndSizeStage's own sub-pixel-culling
    // threshold (min_visible_dbu = 1.0/scale) depends on scale, but
    // resolving the SAME AbstractId again at a different scale (a real
    // scenario - every InstanceRenderer cache is keyed on the design/
    // layout id, not on scale) must still re-derive that threshold
    // correctly rather than reusing internal filter-stage state left over
    // from an earlier scale.
    DesignId leaf = create_leaf_design("LEAF", Point{50, 50});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::N, "U1");

    // scale=1.0: min_visible_dbu=1 - the 50x50 leaf is comfortably visible.
    const sk_sp<SkPicture> visible = instance_renderer.build_layout_picture(root, sub_layout, 0, view_layers, scene, 1.0);
    ASSERT_TRUE(visible);
    EXPECT_TRUE(region_has_opaque_pixel(rasterize(visible, 400, 400), 5, 5, 45, 45));

    // scale=0.001: min_visible_dbu=1000 - the same 50x50 leaf is now
    // sub-pixel and should be culled entirely.
    const sk_sp<SkPicture> culled = instance_renderer.build_layout_picture(root, sub_layout, 0, view_layers, scene, 0.001);
    ASSERT_TRUE(culled);
    EXPECT_FALSE(region_has_opaque_pixel(rasterize(culled, 1, 1), 0, 0, 0, 0));
}

TEST_F(InstancingFixture, PlacementMathFallsBackToTheBoundaryShapeWhenAbstractSizeIsUnset)
{
    // AbstractData.size is optional - when unset, the declared local bbox
    // used for orientation/placement math falls back to the boundary
    // Shape's own bbox instead of a degenerate Rect{} (which would place
    // the leaf with no orientation-driven offset at all, still visible
    // but at the wrong spot for any non-identity orientation).
    DesignId leaf = create_design("LEAF");
    AbstractId leaf_abstract = root.create_abstract(AbstractData{.design = leaf}); // no .size
    Shape boundary;
    boundary.abstract = leaf_abstract;
    boundary.purpose = ShapePurpose::BOUNDARY;
    boundary.polygons.push_back(Polygon{.points = {{0, 0}, {0, 200}, {100, 200}, {100, 0}, {0, 0}}});
    root.create_shape(std::move(boundary));
    ObstructionId obstruction_id = root.create_obstruction(ObstructionData{.abstract = leaf_abstract});
    Shape obstruction;
    obstruction.obstruction = obstruction_id;
    obstruction.layer = m1;
    obstruction.rects.push_back(Rect{.ll = {0, 0}, .ur = {100, 200}});
    root.create_shape(std::move(obstruction));

    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::W, "U1");

    const sk_sp<SkPicture> picture = instance_renderer.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    // Boundary-derived local bbox (0,0)-(100,200), W-rotated -> world
    // bbox (0,0)-(200,100) (swapped, same as the AbstractData.size-driven
    // OrientationRotatesTheInstancedLeaf case above).
    const SkBitmap bitmap = rasterize(picture, 400, 400);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 10, 10, 190, 90));
}

TEST_F(InstancingFixture, RenderLayoutFrameDrawsAtTheCorrectPostFlipPixelPosition)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{500, 500}, Orientation::N, "U1");

    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);

    const PixelBuffer &buffer = instance_renderer.render_layout_frame(root, sub_layout, /*hierarchy_depth=*/1, view_layers, scene);
    ASSERT_TRUE(buffer.data);
    EXPECT_EQ(buffer.width, 800);
    EXPECT_EQ(buffer.height, 800);

    // Pre-flip pixel rect (pan=0, scale=1) is exactly the leaf's own world
    // dbu rect, (500,500)-(600,600); RasterizeStage's own Y-flip (baked
    // into render_layout_frame's own chain, same as the Abstract path)
    // maps dbu y -> height - y, so the post-flip rect is (500,200)-(600,300).
    EXPECT_TRUE(pixel_buffer_region_has_opaque_pixel(buffer, 520, 220, 580, 280));
    EXPECT_FALSE(pixel_buffer_region_has_opaque_pixel(buffer, 10, 10, 50, 50));
}

TEST_F(InstancingFixture, RenderLayoutFramePanShiftsTheDrawnPosition)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{500, 500}, Orientation::N, "U1");

    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);
    instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene); // establish the un-panned frame first

    scene.set_pan(Point{100, 100});
    const PixelBuffer &panned = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene);

    // world dbu rect (500,500)-(600,600), pan=(100,100) -> pre-flip pixel
    // (400,400)-(500,500) -> post-flip (height - y): (400,300)-(500,400).
    EXPECT_TRUE(pixel_buffer_region_has_opaque_pixel(panned, 420, 320, 480, 380));
    // The un-panned frame's own region should no longer be lit - proves
    // the picture actually moved, not that it's drawn twice.
    EXPECT_FALSE(pixel_buffer_region_has_opaque_pixel(panned, 520, 220, 580, 280));
}

TEST_F(InstancingFixture, RenderLayoutFrameReusesCacheUntilSceneChanges)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{500, 500}, Orientation::N, "U1");

    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);

    const PixelBuffer &first = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene);
    const uint8_t *first_data = first.data;

    const PixelBuffer &second = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene);
    EXPECT_EQ(second.data, first_data); // unchanged scene - cache hit, same underlying surface

    scene.set_pan(Point{10, 10});
    const PixelBuffer &after_pan = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene);
    EXPECT_NE(after_pan.data, first_data); // pan changed - must recompute
}

TEST_F(InstancingFixture, RenderLayoutFrameRecomputesAfterARootMutation)
{
    // Regression: render_layout_frame's own compose-step cache key relies
    // entirely on recompute_count_ as a proxy for "did anything upstream
    // (including root.mutation_version()) change" - this only holds
    // because build_layout_picture (which checks root.mutation_version()
    // via ensure_epoch) is always called first, on every single call,
    // before recompute_count_ is read. Exercises that path directly,
    // not just the pan-changed case RenderLayoutFrameReusesCacheUntilSceneChanges
    // already covers.
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{500, 500}, Orientation::N, "U1");

    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);

    const PixelBuffer &first = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene);
    const uint8_t *first_data = first.data;

    root.bump_mutation_version();
    const PixelBuffer &after_mutation = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene);
    EXPECT_NE(after_mutation.data, first_data); // mutation invalidated the whole epoch - must recompute
    EXPECT_TRUE(pixel_buffer_region_has_opaque_pixel(after_mutation, 520, 220, 580, 280)); // content still correct after recompute
}
