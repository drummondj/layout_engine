#include "../instance_renderer.hpp"
#include "../../pipeline/pipeline.hpp"
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
        // render_layout_frame's own real Renderer collaborator - shares
        // its rasterize/compose stages directly (Renderer::
        // design_rasterize_stage()'s own comment) rather than
        // instance_renderer owning a second, duplicate copy of them.
        Renderer renderer;
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

TEST_F(InstancingFixture, NestedLayoutRecursionDoesNotCorruptTheOuterLayoutsOwnDirectContent)
{
    // Regression for generate_abstract_stage_/generate_layout_stage_
    // becoming persistent, shared members (previously constructed fresh
    // per call specifically to avoid this): TOP's own direct content (a
    // real routing Blockage, not just its diearea outline) must survive
    // the recursive placement loop below, which - through the SAME
    // shared generate_layout_stage_ - also resolves SUB (a DIFFERENT
    // LayoutId) and, deeper still, LEAF's own Abstract via the SAME
    // shared generate_abstract_stage_. If either fix regressed back to
    // holding a reference across that recursion instead of copying,
    // TOP's own content would be silently replaced by whatever SUB/LEAF
    // last computed by the time record_local_picture consumes it.
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::N, "U1");

    auto [top_design, top_layout] = create_layout_design("TOP", Point{3000, 3000});
    add_placement(top_layout, sub_design, Point{1000, 1000}, Orientation::N, "U1");

    // TOP's own direct content - a routing Blockage far from where SUB's
    // own placement lands (SUB occupies world (1000,1000)-(1400,1400)).
    const BlockageId blockage_id = root.create_blockage(BlockageData{.layout = top_layout, .kind = BlockageKind::ROUTING});
    Shape blockage_shape;
    blockage_shape.blockage = blockage_id;
    blockage_shape.layer = m1;
    blockage_shape.rects.push_back(Rect{.ll = {2000, 2000}, .ur = {2200, 2200}});
    root.create_shape(std::move(blockage_shape));

    const sk_sp<SkPicture> picture = instance_renderer.build_layout_picture(root, top_layout, /*remaining_depth=*/2, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 4000, 4000);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 2010, 2010, 2190, 2190)); // TOP's own blockage - must survive the recursive SUB/LEAF resolution
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 1010, 1010, 1090, 1090)); // the nested leaf still resolves correctly too
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

TEST_F(InstancingFixture, PlacementReferencingAContentlessDesignDrawsNoPhantomDot)
{
    // Regression: a VALID DesignId that simply has neither an Abstract
    // nor a reachable Layout (unlike the invalid-DesignId{} case above,
    // which never reaches the sub-pixel logic at all) must still be
    // skipped entirely - not collapsed into a MIGRATION_REVIEW.md item 2
    // dot. resolved_local_bbox's own Rect{} fallback for "unresolved" is
    // indistinguishable from a legitimately zero-sized declared bbox, so
    // without design_is_resolvable's own guard, this degenerate case
    // would trivially satisfy the sub-pixel threshold at ANY scale and
    // draw a phantom BOUNDARY dot where there's really nothing at all.
    DesignId empty_design = create_design("EMPTY"); // no Abstract, no Layout
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, empty_design, Point{200, 200}, Orientation::N, "EMPTY1");

    const sk_sp<SkPicture> picture = instance_renderer.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 400, 400);
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 199, 199, 201, 201)); // nothing at all at its own location - not even a dot
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
    // Regression: the sub-pixel threshold (min_visible_dbu =
    // min_visible_instance_pixels() / scale) depends on scale, but
    // resolving the SAME AbstractId again at a different scale (a real
    // scenario - every InstanceRenderer cache is keyed on the design/
    // layout id, not on scale) must still re-derive that threshold
    // correctly rather than reusing internal filter-stage state left over
    // from an earlier scale. Sets an explicit threshold rather than
    // relying on InstanceRenderer's own current default - that default is
    // a testing/tuning value (see min_visible_instance_pixels()'s own doc
    // comment) that may change independently of this test's own intent.
    instance_renderer.set_min_visible_instance_pixels(50.0);

    DesignId leaf = create_leaf_design("LEAF", Point{50, 50});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{100, 100}, Orientation::N, "U1");

    // scale=2.0: min_visible_dbu=25 - the 50x50 leaf is comfortably visible
    // (50 dbu not < 25 dbu threshold).
    const sk_sp<SkPicture> visible = instance_renderer.build_layout_picture(root, sub_layout, 0, view_layers, scene, 2.0);
    ASSERT_TRUE(visible);
    EXPECT_TRUE(region_has_opaque_pixel(rasterize(visible, 800, 800), 220, 220, 280, 280)); // world (100,100)-(150,150)dbu * 2.0

    // scale=0.4: min_visible_dbu=125 - the same 50x50 leaf's own PLACEMENT
    // is now sub-pixel (50 dbu < 125 dbu threshold), so
    // build_layout_picture_uncached collapses it to its own boundary rect
    // OUTLINE - no fill, no internal content (MIGRATION_REVIEW.md item 2's
    // own follow-up) - instead of resolving/drawing its own Abstract
    // content at all. World bbox (100,100)-(150,150)dbu * 0.4 = a clean
    // (40,40)-(60,60)px rect: opaque along its border, but NOT filled at
    // its own center.
    const sk_sp<SkPicture> outlined = instance_renderer.build_layout_picture(root, sub_layout, 0, view_layers, scene, 0.4);
    ASSERT_TRUE(outlined);
    const SkBitmap bitmap = rasterize(outlined, 160, 160);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 39, 49, 41, 51)); // left edge of the outline, vertically centered
    EXPECT_EQ(SkColorGetA(bitmap.getColor(50, 50)), 0); // dead center - unfilled
}

TEST_F(InstancingFixture, SubPixelInstanceSkipsResolveEntirelyWhileNormalSizedOneStillFullyRenders)
{
    // MIGRATION_REVIEW.md item 2 - the actual perf fix, not just the
    // visual outline: a sub-pixel Placement's own reference_design must
    // never reach resolve_design_picture at all (no recursion, no picture
    // build/cache entry for it), while a normal-sized Placement alongside
    // it in the SAME Layout still resolves and renders its own full
    // content exactly as before.
    instance_renderer.set_min_visible_instance_pixels(50.0);

    DesignId leaf_big = create_leaf_design("BIG", Point{100'000, 100'000});
    DesignId leaf_small = create_leaf_design("SMALL", Point{20'000, 20'000});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{200'000, 200'000});
    add_placement(sub_layout, leaf_big, Point{0, 0}, Orientation::N, "BIG1");
    add_placement(sub_layout, leaf_small, Point{150'000, 150'000}, Orientation::N, "SMALL1");

    // scale=0.001: min_visible_dbu=50,000 - BIG (100,000 dbu) stays fully
    // visible, SMALL (20,000 dbu, i.e. a 20x20px world bbox at this scale)
    // is sub-pixel by threshold even though it's still a real, visible
    // handful of pixels on screen.
    const sk_sp<SkPicture> picture = instance_renderer.build_layout_picture(root, sub_layout, 1, view_layers, scene, 0.001);
    ASSERT_TRUE(picture);

    // recompute_count: exactly one for SUB's own build_layout_picture_uncached
    // plus one for BIG's own build_abstract_picture (via resolve_design_picture) -
    // SMALL's own resolve_design_picture/build_abstract_picture is never
    // reached, so it contributes zero, not one.
    EXPECT_EQ(instance_renderer.design_picture_recompute_count(), 2u);

    const SkBitmap bitmap = rasterize(picture, 200, 200);
    // BIG's own M1 obstruction fill, comfortably inside its (0,0)-(100,100)px bbox.
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 10, 10, 90, 90));
    // SMALL's own outline: world (150,150)-(170,170)dbu * 0.001... wait,
    // world bbox is (150000,150000)-(170000,170000)dbu * 0.001 scale =
    // (150,150)-(170,170)px - opaque along its left edge, unfilled center.
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 149, 159, 151, 161));
    EXPECT_EQ(SkColorGetA(bitmap.getColor(160, 160)), 0); // dead center - unfilled, not BIG's own M1 red either
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

// E1 (BUGS_AND_ENHANCEMENTS.md) - hit_test_placements_point/_rect
// (src/core/placement_geometry.hpp) - the "separate, bbox-only
// mechanism" Pipeline::hit_test_point's own doc comment already
// anticipated, since a Placement never enters the RenderedShape map at
// all (see GenerateLayoutShapesStage's own comment).
TEST_F(InstancingFixture, HitTestPlacementsPointRespectsOrientation)
{
    // Same asymmetric-bbox case as OrientationRotatesTheInstancedLeaf
    // above (world bbox becomes (0,0)-(100,200), swapped), cross-checked
    // against the hit-test helper instead of real Skia picture playback -
    // a point inside the UNrotated (200,100) footprint but outside the
    // W-rotated (100,200) one must miss, the strongest possible check
    // that orientation, not just translation, is actually applied.
    DesignId leaf = create_leaf_design("LEAF", Point{200, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::W, "U1");

    EXPECT_EQ(hit_test_placements_point(root, sub_layout, /*remaining_depth=*/0, Point{150, 50}), std::nullopt); // inside the UNrotated bbox only
    const auto hit = hit_test_placements_point(root, sub_layout, /*remaining_depth=*/0, Point{50, 150}); // inside the rotated bbox
    ASSERT_TRUE(hit.has_value());
}

TEST_F(InstancingFixture, HitTestPlacementsPointReturnsTheTopmostOfTwoOverlappingPlacements)
{
    // Placements are drawn in Root::get_layout_placements' own order
    // (BuildLayoutPictureStage::run's instances loop, topmost last) -
    // the hit-test iterates in reverse to match, so the LAST-added of
    // two overlapping placements should win, mirroring
    // Pipeline::hit_test_point's own "topmost first" contract.
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::N, "U1");
    const PlacementId top = add_placement(sub_layout, leaf, Point{0, 0}, Orientation::N, "U2"); // same location, added after - topmost

    const auto hit = hit_test_placements_point(root, sub_layout, /*remaining_depth=*/0, Point{50, 50});
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, top);
}

TEST_F(InstancingFixture, HitTestPlacementsPointMissesOutsideEveryPlacement)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::N, "U1");

    EXPECT_EQ(hit_test_placements_point(root, sub_layout, /*remaining_depth=*/0, Point{300, 300}), std::nullopt);
}

TEST_F(InstancingFixture, HitTestPlacementsRectFindsEveryFullyEnclosedPlacement)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    const PlacementId enclosed = add_placement(sub_layout, leaf, Point{0, 0}, Orientation::N, "U1");
    add_placement(sub_layout, leaf, Point{350, 350}, Orientation::N, "U2"); // straddles the query rect's edge - not fully enclosed

    const std::vector<PlacementId> hits = hit_test_placements_rect(root, sub_layout, /*remaining_depth=*/0, Rect{.ll = {0, 0}, .ur = {200, 200}});
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits.front(), enclosed);
}

TEST_F(InstancingFixture, RenderLayoutFrameDrawsAtTheCorrectPostFlipPixelPosition)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{500, 500}, Orientation::N, "U1");

    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);

    const PixelBuffer &buffer = instance_renderer.render_layout_frame(root, sub_layout, /*hierarchy_depth=*/1, view_layers, scene, renderer);
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
    instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer); // establish the un-panned frame first

    scene.set_pan(Point{100, 100});
    const PixelBuffer &panned = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer);

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

    const PixelBuffer &first = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer);
    const uint8_t *first_data = first.data;

    const PixelBuffer &second = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer);
    EXPECT_EQ(second.data, first_data); // unchanged scene - cache hit, same underlying surface

    scene.set_pan(Point{10, 10});
    const PixelBuffer &after_pan = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer);
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

    const PixelBuffer &first = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer);
    const uint8_t *first_data = first.data;

    root.bump_mutation_version();
    const PixelBuffer &after_mutation = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer);
    EXPECT_NE(after_mutation.data, first_data); // mutation invalidated the whole epoch - must recompute
    EXPECT_TRUE(pixel_buffer_region_has_opaque_pixel(after_mutation, 520, 220, 580, 280)); // content still correct after recompute
}

TEST_F(InstancingFixture, RenderLayoutFrameReusesRendererOwnRasterizeAndComposeStagesNotDuplicates)
{
    // Regression for Fix 2 (the "instancing seam" follow-up): render_layout_frame
    // now takes a real Renderer& and calls straight through its
    // design_rasterize_stage()/tiny_shapes_rasterize_stage()/
    // selection_rasterize_stage()/ruler_rasterize_stage()/compose_stage()
    // accessors instead of owning a second, duplicate copy of those five
    // classes. Proven directly via Renderer's own call_count() counters -
    // an unchanged Scene must NOT bump them a second time (real reuse of
    // the shared instance, not "still correct because nothing actually
    // shares"), and a real content/scene change must.
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{500, 500}, Orientation::N, "U1");

    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);

    instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer);
    ASSERT_EQ(renderer.design_rasterize_stage().call_count(), 1u);
    ASSERT_EQ(renderer.compose_stage().call_count(), 1u);

    instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer); // unchanged scene - must reuse
    EXPECT_EQ(renderer.design_rasterize_stage().call_count(), 1u);
    EXPECT_EQ(renderer.compose_stage().call_count(), 1u);

    scene.set_pan(Point{25, 25});
    instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer); // pan changed - must recompute
    EXPECT_EQ(renderer.design_rasterize_stage().call_count(), 2u);
    EXPECT_EQ(renderer.compose_stage().call_count(), 2u);
}

TEST_F(InstancingFixture, SwitchingFromAbstractViewToLayoutViewThroughTheSameRendererDoesNotShowStaleContent)
{
    // The direct regression guard for Fix 2's own safety claim: sharing
    // Renderer's rasterize/compose instances between Renderer::render()
    // (Abstract view) and InstanceRenderer::render_layout_frame() (Layout
    // view) is only safe because a real caller (api.cpp's
    // le_render_pixel_buffer) never calls both for the same frame - a
    // view switch is a genuine content change, not extra thrashing. This
    // proves the Layout-view frame's own pixels are correct - not the
    // previous Abstract-view frame's stale, cached content - immediately
    // after switching, through the SAME shared Renderer instance.
    const AbstractId abstract_id = root.create_abstract(AbstractData{});
    const TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id});
    const TerminalPortId port_id = root.create_terminal_port(TerminalPortData{.terminal = terminal_id});
    Shape abstract_shape;
    abstract_shape.terminal_port = port_id;
    abstract_shape.layer = m1;
    abstract_shape.rects.push_back(Rect{.ll = {10, 10}, .ur = {30, 30}});
    root.create_shape(std::move(abstract_shape));

    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{500, 500}, Orientation::N, "U1");

    Pipeline pipeline;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(800, 800);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto &tiny_shapes = pipeline.run_tiny_shapes(root, scene, view_layers);
    renderer.render(root, shapes, tiny_shapes, scene, view_layers); // an Abstract-view frame, cached in renderer's own instances

    // Switch views, exactly as le_set_current_design_layout_by_id does.
    scene.set_current_layout(sub_layout);
    scene.set_current_abstract(AbstractId{});

    const PixelBuffer &layout_buffer = instance_renderer.render_layout_frame(root, sub_layout, 1, view_layers, scene, renderer);
    ASSERT_TRUE(layout_buffer.data);
    // The leaf's own world rect, (500,500)-(600,600) pre-flip -> post-flip
    // (500,200)-(600,300) - present means real Layout-view content, not a
    // stale Abstract-view frame reused because the compose key happened
    // to collide.
    EXPECT_TRUE(pixel_buffer_region_has_opaque_pixel(layout_buffer, 520, 220, 580, 280));
}

namespace
{
    // Builds a small two-segment routed net (mirrors a real DEF net's own
    // "each NEW-delimited segment is its own Path" shape - see
    // extend_path_ends_for_buffering's own comment in geometry.hpp) whose
    // every dbu coordinate is `offset` away from (0,0), then renders a
    // fixed relative view (pan chosen so the joint always lands at the
    // same point in the viewport, regardless of `offset`) and returns the
    // resulting PixelBuffer's own raw bytes as a self-contained copy - a
    // fresh Root/InstanceRenderer per call, so the returned buffer can't
    // be invalidated by a second call reusing the same cache slot.
    std::vector<uint8_t> render_joint_at_offset(Point offset, int &out_width, int &out_height, size_t &out_row_bytes)
    {
        Root root;
        const TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
        const LayerId m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
        const ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_id);
        const LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
        const DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "D"});
        const LayoutId layout_id = root.create_layout(LayoutData{.design = design_id});

        Shape diearea;
        diearea.layout = layout_id;
        diearea.purpose = ShapePurpose::BOUNDARY;
        diearea.rects.push_back(Rect{.ll = Point{offset.x, offset.y}, .ur = Point{offset.x + 2000, offset.y + 2000}});
        root.create_shape(std::move(diearea));

        const RouteId route_id = root.create_route(RouteData{.layout = layout_id, .name = "net", .is_special = false});
        Shape vertical;
        vertical.route = route_id;
        vertical.layer = m1;
        vertical.paths = {Path{.polygon = Polygon{.points = {{offset.x + 500, offset.y + 1260}, {offset.x + 500, offset.y + 500}}}, .width = 100}};
        root.create_shape(std::move(vertical));

        Shape horizontal;
        horizontal.route = route_id;
        horizontal.layer = m1;
        horizontal.paths = {Path{.polygon = Polygon{.points = {{offset.x + 500, offset.y + 500}, {offset.x + 1176, offset.y + 500}}}, .width = 100}};
        root.create_shape(std::move(horizontal));

        InstanceRenderer renderer;
        Renderer render_engine;
        Scene scene;
        constexpr int kViewport = 900;
        constexpr double kScale = 40.0;
        scene.set_viewport_size(kViewport, kViewport);
        scene.set_scale(kScale);
        // Centers the joint (dbu offset+500,+500) in the viewport - same
        // relative framing regardless of offset.
        scene.set_pan(Point{offset.x + 500 - static_cast<int64_t>(kViewport / (2 * kScale)), offset.y + 500 - static_cast<int64_t>(kViewport / (2 * kScale))});
        scene.set_purpose_visible(ViewLayerPurpose::TRACK, false);
        scene.set_purpose_visible(ViewLayerPurpose::ROW, false);
        scene.set_purpose_visible(ViewLayerPurpose::GCELLGRID, false);
        // Large enough that no grid dot/axis line falls inside this tiny
        // 200x200 viewport for either offset - isolates the comparison
        // below to the routed net's own geometry.
        scene.set_minor_grid_spacing(1000000000);
        scene.set_major_grid_spacing(1000000000);

        const PixelBuffer &buffer = renderer.render_layout_frame(root, layout_id, /*hierarchy_depth=*/0, view_layers, scene, render_engine);
        out_width = buffer.width;
        out_height = buffer.height;
        out_row_bytes = buffer.row_bytes;
        return std::vector<uint8_t>(buffer.data, buffer.data + buffer.row_bytes * static_cast<size_t>(buffer.height));
    }
}

TEST(InstanceRendererPrecisionTest, RenderLayoutFrameStaysPixelIdenticalFarFromOrigin)
{
    // Regression, BUGS_AND_ENHANCEMENTS.md B1 follow-up: build_layout_picture_uncached
    // used to bake a Layout's own top-level content as `local_pixel =
    // dbu * scale` with no origin subtraction - fine near dbu (0,0), but
    // real chip coordinates (commonly hundreds of thousands to millions
    // of dbu) times a real zoom scale routinely exceeded float32's
    // exact-integer range (~16.7M), collapsing adjacent screen pixels
    // onto the same coordinate - a dense checkerboard/moire, reproduced
    // and confirmed via this exact joint shape at dbu ~940000, scale 40.
    // The SAME shape, at the SAME relative pan/scale, must render
    // byte-identically regardless of how far its own absolute DBU
    // position is from the origin - a real difference here means
    // precision was lost for the far-from-origin copy.
    int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    size_t rb1 = 0, rb2 = 0;
    const std::vector<uint8_t> near_origin = render_joint_at_offset(Point{10000, 10000}, w1, h1, rb1);
    const std::vector<uint8_t> far_from_origin = render_joint_at_offset(Point{2000000, 2000000}, w2, h2, rb2);

    ASSERT_EQ(w1, w2);
    ASSERT_EQ(h1, h2);
    ASSERT_EQ(rb1, rb2);
    EXPECT_EQ(near_origin, far_from_origin);
}
