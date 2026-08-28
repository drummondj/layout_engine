// backend/ONETBB_INTEGRATION.md migration plan, Phase 4: HierarchyResolver -
// a representative subset of instancing_test.cpp's own InstancingFixture
// cases, covering Phase B scope only (resolve_design_picture and its own
// recursive helpers) - render_layout_frame (Phase C) is a deliberate
// follow-up, see hierarchy_resolver.hpp's own class comment.
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
    // Regression for generate_abstract_stage_/generate_layout_stage_
    // being persistent, shared members: TOP's own direct content (a real
    // routing Blockage, not just its diearea outline) must survive the
    // recursive placement loop below, which - through the SAME shared
    // generate_layout_stage_ - also resolves SUB (a DIFFERENT LayoutId)
    // and, deeper still, LEAF's own Abstract via the SAME shared
    // generate_abstract_stage_. If the copy-before-recursion fix
    // regressed back to holding a reference across that recursion,
    // TOP's own content would be silently replaced by whatever SUB/LEAF
    // last computed by the time record_local_picture consumes it.
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
