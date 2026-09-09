#include "../view_render_pipeline.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    // Same LEAF/BLOCK/TOP fixture as hierarchy_resolver_stage_test.cpp -
    // see that file's own comment for the full depth-semantics rationale.
    // Kept as its own private copy rather than shared, matching this
    // module's own "each test file gets its own fixture" convention.
    struct ViewRenderPipelineFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});

            const LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});

            const DesignId leaf_design = root.create_design(DesignData{.library = library_id, .name = "LEAF"});
            leaf_abstract = root.create_abstract(AbstractData{.design = leaf_design});
            root.create_shape(ShapeData{.abstract = leaf_abstract, .purpose = ShapePurpose::BOUNDARY, .rects = {Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}}});

            const DesignId top_design = root.create_design(DesignData{.library = library_id, .name = "TOP"});
            top_layout = root.create_layout(LayoutData{.design = top_design});
            root.create_shape(ShapeData{.layout = top_layout, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {Point{0, 0}, Point{5000, 5000}}}}});
            root.create_placement(PlacementData{.layout = top_layout, .name = "leaf0", .reference_design = leaf_design, .placement_status = PlacementStatus::PLACED, .location = Point{100, 100}, .orientation = Orientation::N});
        }

        ViewRenderOptions options_for(int hierarchy_depth) const
        {
            return ViewRenderOptions{.root_mutation_version = root.mutation_version(), .top_level = HierarchyId{top_layout}, .hierarchy_depth = hierarchy_depth};
        }

        ViewRenderOptions warm_options_for(int hierarchy_depth) const
        {
            ViewRenderOptions options = options_for(hierarchy_depth);
            options.viewport = Rect{.ll = Point{0, 0}, .ur = Point{5000, 5000}}; // TOP's own full diearea
            options.scale = 0.05; // 5000 dbu * 0.05 = 250px - a manageable test image size
            return options;
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        AbstractId leaf_abstract;
        LayoutId top_layout;
        ViewRenderPipeline pipeline;
    };
}

// Everything besides `frame` (view_layers/hierarchy/culled/rasterized) is
// gone from WarmOutput - nothing outside this class ever read them
// (view_render_pipeline.hpp's own WarmOutput doc comment), and each one's
// own claim is already covered directly at the stage that actually owns
// it: LayerGenerationStageFixture.{CacheHitOnUnchangedMutationVersion,
// RecomputesWhenRootMutationVersionChanges} for view_layers'
// caching behavior, HierarchyResolverStageFixture.{ShapesResolveExpectedViewLayers,
// RecomputesWhenHierarchyDepthChangesEvenIfMutationVersionDoesNot,
// NullRootProducesEmptyOutput} for hierarchy's. What's left here tests
// only what a real caller (api.cpp) can actually observe through the
// wired graph: does a render produce the right frame, and does an
// unchanged/changed input correctly reuse/replace it.

TEST_F(ViewRenderPipelineFixture, RunProducesAFrameSizedToTheViewport)
{
    const ViewRenderPipeline::WarmOutput output = pipeline.run(&root, warm_options_for(1));

    ASSERT_NE(output.frame, nullptr);
    EXPECT_FALSE(output.frame->empty);
    EXPECT_EQ(output.frame->buffer.width, 250);  // 5000 dbu * scale 0.05
    EXPECT_EQ(output.frame->buffer.height, 250);
    EXPECT_NE(output.frame->buffer.data, nullptr);
}

TEST_F(ViewRenderPipelineFixture, RunCacheHitReturnsIdenticalFrameHandleOnUnchangedInputs)
{
    const ViewRenderOptions options = warm_options_for(1);
    const ViewRenderPipeline::WarmOutput first = pipeline.run(&root, options);
    const ViewRenderPipeline::WarmOutput second = pipeline.run(&root, options);

    // Same shared_ptr identity, not just equal content - proves nothing
    // in the whole chain actually recomputed (would_recompute's own
    // short-circuit in run(), or each stage's own execute()'s
    // should_recompute==false path, skipped real work all the way
    // through).
    EXPECT_EQ(first.frame, second.frame);
}

TEST_F(ViewRenderPipelineFixture, RootMutationForcesANewFrame)
{
    const ViewRenderPipeline::WarmOutput before = pipeline.run(&root, warm_options_for(1));

    root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
    root.bump_mutation_version();

    // A fresh warm_options_for(1) call, not the same `options` reused
    // from above - it re-reads root.mutation_version() at call time, the
    // only way this test can see the mutation at all (ViewRenderOptions
    // is a plain snapshot, not a live reference back into Root).
    const ViewRenderPipeline::WarmOutput after = pipeline.run(&root, warm_options_for(1));

    // A real, wiring-level guard (as opposed to LayerGenerationStageFixture's
    // own isolated RecomputesWhenRootMutationVersionChanges): proves a
    // root mutation actually cascades all the way through the real
    // make_edge chain - LayerGeneration -> HierarchyResolver ->
    // ViewportCull -> Rasterize -> Compose - to produce a genuinely new
    // frame, not just that LayerGenerationStage's own version() bumps in
    // isolation.
    EXPECT_NE(before.frame, after.frame);
}

TEST_F(ViewRenderPipelineFixture, RunNullRootProducesEmptyFrame)
{
    const ViewRenderPipeline::WarmOutput output = pipeline.run(nullptr, warm_options_for(1));

    ASSERT_NE(output.frame, nullptr);
    EXPECT_TRUE(output.frame->empty);
}

// Confirms the Blend2D-backed sibling (ViewRenderPipelineImpl<RasterizeBlend2DStage>,
// view_render_pipeline.hpp's own doc comment) is wired correctly end-to-end
// through the exact same graph shape/ComposeStage as the default Skia
// pipeline - not just unit-testable via RasterizeBlend2DStage in
// isolation (rasterize_blend2d_stage_test.cpp).
TEST_F(ViewRenderPipelineFixture, Blend2DBackedPipelineProducesAFrameSizedToTheViewport)
{
    ViewRenderPipelineBlend2D blend2d_pipeline;
    const ViewRenderPipelineBlend2D::WarmOutput output = blend2d_pipeline.run(&root, warm_options_for(1));

    ASSERT_NE(output.frame, nullptr);
    EXPECT_FALSE(output.frame->empty);
    EXPECT_EQ(output.frame->buffer.width, 250); // 5000 dbu * scale 0.05
    EXPECT_EQ(output.frame->buffer.height, 250);
    EXPECT_NE(output.frame->buffer.data, nullptr);
}
