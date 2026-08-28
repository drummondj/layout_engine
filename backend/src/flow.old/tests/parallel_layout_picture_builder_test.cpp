#include "../instancing/parallel_layout_picture_builder.hpp"
#include "../../instancing/instance_renderer.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include <gtest/gtest.h>
#include <string>

using namespace le;

namespace
{
    // Mirrors src/instancing/tests/instancing_test.cpp's own
    // InstancingFixture (a different translation unit's own
    // anonymous-namespace member, not reusable directly) - same
    // scaffolding/helpers, plus a flow::ParallelLayoutPictureBuilder
    // member and a real InstanceRenderer member (read-only, never
    // edited) for the A/B comparison test below.
    struct ParallelInstancingFixture : public ::testing::Test
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

        // A Design whose Abstract's declared size is `size`, containing one
        // Obstruction Shape that fills that whole size on M1 - makes "is
        // this leaf's own content visible at pixel P" trivial to check.
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

        // Same technique as InstancingFixture's own rasterize - both
        // InstanceRenderer and ParallelLayoutPictureBuilder record in the
        // same "local pixel space" convention (local_pixel = dbu * scale,
        // no Y-flip, no pan subtraction), so their own output pictures are
        // directly comparable pixel-for-pixel via this same helper.
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
        // - a single fixed-point pixel sample can coincidentally land on a
        // gap even though the shape really is there, so every check below
        // scans a region instead.
        bool region_has_opaque_pixel(const SkBitmap &bitmap, int x0, int y0, int x1, int y1)
        {
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (SkColorGetA(bitmap.getColor(x, y)) > 0)
                        return true;
            return false;
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        ViewLayerSet view_layers;
        LibraryId library_id;
        Scene scene; // default-constructed - every ViewLayer visible
        flow::ParallelLayoutPictureBuilder builder;
        InstanceRenderer real_renderer; // real, unmodified InstanceRenderer - read-only reference for the A/B test below
    };
}

TEST_F(ParallelInstancingFixture, SinglePlacementDrawsLeafAtItsAbsoluteLocation)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{500, 500}, Orientation::N, "U1");

    tf::Executor executor(2);
    const sk_sp<SkPicture> picture = builder.run(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0, executor);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 800, 800);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 510, 510, 590, 590)); // inside the leaf's world rect (500,500)-(600,600)
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 700, 700, 750, 750)); // outside both the leaf and sub's own diearea outline
}

TEST_F(ParallelInstancingFixture, OrientationRotatesTheInstancedLeaf)
{
    // Asymmetric bbox (200 wide, 100 tall) - same case as
    // instancing_test.cpp's own analogous test, cross-checked here
    // through this class's own draw_placement_chunk/process_placement
    // path instead of InstanceRenderer's.
    DesignId leaf = create_leaf_design("LEAF", Point{200, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::W, "U1");

    tf::Executor executor(2);
    const sk_sp<SkPicture> picture = builder.run(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0, executor);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 400, 400);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 10, 110, 90, 190)); // inside the rotated bbox
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 110, 10, 190, 90)); // inside the UNrotated bbox only
}

TEST_F(ParallelInstancingFixture, ManyPlacementsAcrossMultipleChunksAllRenderAtTheirCorrectLocation)
{
    // 4 workers, 40 placements (>= 2*4) forces chunk_count == num_workers
    // (4), not the single-chunk fallback (see run()'s own threshold
    // comment). Placements are bin-packed BY REFERENCED DESIGN, not by
    // flat index (TASKFLOW_EXPERIMENT.md - avoids several worker threads
    // contending on the same design's shared sk_sp<SkPicture> refcount),
    // so 40 placements of the SAME design would all land in exactly one
    // chunk - this test uses 8 DISTINCT designs (5 placements each) so
    // the greedy load-balancer still has real, independent buckets to
    // spread across all 4 chunks; sampling one placement from each
    // distinct design checks every chunk's own draw_placement_chunk call
    // produced correct, independent output - not just chunk 0. Leaf size
    // (150) is deliberately above ParallelLayoutPictureBuilder's own
    // 100-dbu min-visible-instance threshold at scale=1.0 - a smaller
    // leaf would collapse to an unfilled outline rect instead of its
    // real content (see the InstancingFixture's own analogous
    // SubPixelCullingReflectsTheCurrentScaleNotAStaleOne test), which
    // this test isn't exercising.
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{8100, 8100});

    constexpr int kDesignCount = 8;
    constexpr int kPlacementsPerDesign = 5;
    for (int d = 0; d < kDesignCount; ++d)
    {
        DesignId leaf = create_leaf_design("LEAF" + std::to_string(d), Point{150, 150});
        for (int i = 0; i < kPlacementsPerDesign; ++i)
        {
            const int64_t x0 = (static_cast<int64_t>(d) * kPlacementsPerDesign + i) * 200;
            add_placement(sub_layout, leaf, Point{x0, 0}, Orientation::N, "U" + std::to_string(d) + "_" + std::to_string(i));
        }
    }
    constexpr int kCount = kDesignCount * kPlacementsPerDesign;

    tf::Executor executor(4);
    const sk_sp<SkPicture> picture = builder.run(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0, executor);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 8100, 200);
    for (int i : {0, 10, 20, 30, 39})
    {
        const int64_t x0 = static_cast<int64_t>(i) * 200;
        EXPECT_TRUE(region_has_opaque_pixel(bitmap, static_cast<int>(x0 + 30), 30, static_cast<int>(x0 + 120), 120)) << "placement " << i;
    }
}

TEST_F(ParallelInstancingFixture, NestedLayoutReferenceFallsBackToSerialRecursionAndStillRendersCorrectly)
{
    // A distinct referenced design that is itself a Layout (not just an
    // Abstract) exercises build_design_picture_serial's own recursive
    // fallback (see ParallelLayoutPictureBuilder's own "recursion scope"
    // comment) - only the top-level run() call gets the two-phase
    // parallel treatment; a design one level down is still built
    // correctly, just serially.
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{0, 0}, Orientation::N, "U1");

    auto [top_design, top_layout] = create_layout_design("TOP", Point{3000, 3000});
    add_placement(top_layout, sub_design, Point{1000, 1000}, Orientation::N, "U1");

    tf::Executor executor(4);
    const sk_sp<SkPicture> picture = builder.run(root, top_layout, /*remaining_depth=*/2, view_layers, scene, /*scale=*/1.0, executor);
    ASSERT_TRUE(picture);

    // sub's own declared bbox (0,0)-(400,400) lands its own ll at
    // (1000,1000); the leaf, placed at sub-local (0,0)-(100,100), should
    // therefore land at world (1000,1000)-(1100,1100).
    const SkBitmap bitmap = rasterize(picture, 4000, 4000);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 1010, 1010, 1090, 1090)); // inside the leaf
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 1250, 1250, 1290, 1290)); // inside sub's own diearea but outside the leaf
}

TEST_F(ParallelInstancingFixture, UnresolvedReferenceDesignIsSkippedWithoutAffectingOtherPlacements)
{
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{400, 400});
    add_placement(sub_layout, leaf, Point{100, 100}, Orientation::N, "GOOD");
    add_placement(sub_layout, DesignId{}, Point{0, 0}, Orientation::N, "BAD"); // invalid reference_design

    tf::Executor executor(2);
    const sk_sp<SkPicture> picture = builder.run(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0, executor);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 800, 800);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 110, 110, 190, 190)); // the valid placement still drew, at world (100,100)-(200,200)
}

TEST_F(ParallelInstancingFixture, ComposedPictureIncludesTheLayoutsOwnDirectContentAlongsidePlacements)
{
    // Regression for the "generate_own_shapes runs concurrently with
    // placement resolution" restructure (TASKFLOW_EXPERIMENT.md) -
    // proves the Layout's own direct content (here a routing Blockage,
    // computed by the concurrent generate_own_shapes task) still reaches
    // the final composed picture correctly alongside a placed instance.
    DesignId leaf = create_leaf_design("LEAF", Point{100, 100});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{3000, 3000});
    add_placement(sub_layout, leaf, Point{100, 100}, Orientation::N, "U1");

    const BlockageId blockage_id = root.create_blockage(BlockageData{.layout = sub_layout, .kind = BlockageKind::ROUTING});
    Shape blockage_shape;
    blockage_shape.blockage = blockage_id;
    blockage_shape.layer = m1;
    blockage_shape.rects.push_back(Rect{.ll = {2000, 2000}, .ur = {2200, 2200}});
    root.create_shape(std::move(blockage_shape));

    tf::Executor executor(4);
    const sk_sp<SkPicture> picture = builder.run(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0, executor);
    ASSERT_TRUE(picture);

    const SkBitmap bitmap = rasterize(picture, 3000, 3000);
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 2010, 2010, 2190, 2190)); // the Layout's own direct blockage content
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 110, 110, 190, 190)); // the placed leaf
}

TEST_F(ParallelInstancingFixture, TracksAreDeliberatelyNotGeneratedUnlikeTheRealInstanceRenderer)
{
    // A documented, deliberate divergence from the real InstanceRenderer
    // (TASKFLOW_EXPERIMENT.md) - TRACKS synthesis turned out to be THE
    // dominant cost of generating a Layout's own direct content on a
    // real design, and a user only looks at TRACK geometry zoomed in on
    // a real routing question, not in this pipeline's bird's-eye view -
    // so ParallelGenerateLayoutShapesStage skips it outright. This test
    // exists so a future re-enable of TRACKS shows up as an intentional
    // change to this test, not a silent behavior drift.
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{500, 500});
    root.create_track(TrackData{.layout = sub_layout, .is_x = true, .start = 100, .count = 1, .step = 0, .layer_names = {"M1"}});

    const sk_sp<SkPicture> real_picture = real_renderer.build_layout_picture(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(real_picture);
    const SkBitmap real_bitmap = rasterize(real_picture, 500, 500);
    EXPECT_TRUE(region_has_opaque_pixel(real_bitmap, 95, 10, 105, 490)); // the real InstanceRenderer still draws the track line at x=100

    tf::Executor executor(2);
    const sk_sp<SkPicture> parallel_picture = builder.run(root, sub_layout, /*remaining_depth=*/0, view_layers, scene, /*scale=*/1.0, executor);
    ASSERT_TRUE(parallel_picture);
    const SkBitmap parallel_bitmap = rasterize(parallel_picture, 500, 500);
    EXPECT_FALSE(region_has_opaque_pixel(parallel_bitmap, 95, 10, 105, 490)); // this pipeline deliberately does not
}

TEST_F(ParallelInstancingFixture, ProducesPixelIdenticalOutputToTheRealInstanceRendererAcrossWorkerCounts)
{
    // The strongest correctness proof available: one moderately complex
    // Root (a nested Layout reference, enough placements to force
    // multiple chunks, and the top Layout's own direct content) compared
    // pixel-for-pixel against the real, unmodified InstanceRenderer -
    // read-only, never edited - at several worker counts including 1 (no
    // real concurrency at all, so a divergence can't hide behind "only
    // wrong when actually parallel").
    DesignId leaf_a = create_leaf_design("LEAF_A", Point{40, 40});
    DesignId leaf_b = create_leaf_design("LEAF_B", Point{30, 60});
    auto [sub_design, sub_layout] = create_layout_design("SUB", Point{150, 150});
    add_placement(sub_layout, leaf_b, Point{25, 25}, Orientation::W, "U1");

    auto [top_design, top_layout] = create_layout_design("TOP", Point{800, 800});
    add_placement(top_layout, sub_design, Point{100, 100}, Orientation::N, "SUB1");

    // 21 total top-level placements (1 sub + 20 leaves) exercises every
    // chunk_count branch in run()'s own threshold: 1 at workers=1
    // (num_workers<=1 fallback), 2 at workers=2 (21 >= 2*2), 4 at
    // workers=4 (21 >= 2*4).
    constexpr int kCount = 20;
    for (int i = 0; i < kCount; ++i)
        add_placement(top_layout, leaf_a, Point{300 + static_cast<int64_t>(i) * 15, 300}, Orientation::N, "L" + std::to_string(i));

    const BlockageId blockage_id = root.create_blockage(BlockageData{.layout = top_layout, .kind = BlockageKind::ROUTING});
    Shape blockage_shape;
    blockage_shape.blockage = blockage_id;
    blockage_shape.layer = m1;
    blockage_shape.rects.push_back(Rect{.ll = {700, 700}, .ur = {720, 720}});
    root.create_shape(std::move(blockage_shape));

    const sk_sp<SkPicture> real_picture = real_renderer.build_layout_picture(root, top_layout, /*remaining_depth=*/2, view_layers, scene, /*scale=*/1.0);
    ASSERT_TRUE(real_picture);
    const SkBitmap real_bitmap = rasterize(real_picture, 800, 800);

    for (unsigned workers : {1u, 2u, 4u})
    {
        SCOPED_TRACE(::testing::Message() << "workers=" << workers);
        tf::Executor executor(workers);
        const sk_sp<SkPicture> parallel_picture = builder.run(root, top_layout, /*remaining_depth=*/2, view_layers, scene, /*scale=*/1.0, executor);
        ASSERT_TRUE(parallel_picture);
        const SkBitmap parallel_bitmap = rasterize(parallel_picture, 800, 800);

        ASSERT_EQ(real_bitmap.width(), parallel_bitmap.width());
        ASSERT_EQ(real_bitmap.height(), parallel_bitmap.height());
        for (int y = 0; y < real_bitmap.height(); ++y)
            for (int x = 0; x < real_bitmap.width(); ++x)
                ASSERT_EQ(real_bitmap.getColor(x, y), parallel_bitmap.getColor(x, y)) << "pixel (" << x << "," << y << ")";
    }
}
