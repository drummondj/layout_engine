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

TEST_F(ViewRenderPipelineFixture, RunColdMatchesIndependentlyComputedResults)
{
    const ViewRenderPipeline::ColdOutput output = pipeline.run_cold(&root, options_for(1));

    ASSERT_NE(output.view_layers, nullptr);
    ASSERT_NE(output.hierarchy, nullptr);

    const ViewLayerSet expected_view_layers = ViewLayerSet::build_for_technology(root, technology_id);
    EXPECT_EQ(output.view_layers->all().size(), expected_view_layers.all().size());
    EXPECT_TRUE(output.view_layers->find(m1, ViewLayerPurpose::TERMINAL).valid());

    // depth 1: TOP resolves leaf0 -> LEAF's Abstract (LEAF has no Layout
    // of its own, so it's always a leaf regardless of depth) - see
    // hierarchy_resolver_stage_test.cpp's own depth-semantics comment.
    EXPECT_EQ(output.hierarchy->view_data.size(), 2u);
    EXPECT_TRUE(output.hierarchy->view_data.contains(HierarchyId{top_layout}));
    EXPECT_TRUE(output.hierarchy->view_data.contains(HierarchyId{leaf_abstract}));
}

TEST_F(ViewRenderPipelineFixture, CacheHitReturnsIdenticalHandlesOnUnchangedInputs)
{
    const ViewRenderOptions options = options_for(1);
    const ViewRenderPipeline::ColdOutput first = pipeline.run_cold(&root, options);
    const ViewRenderPipeline::ColdOutput second = pipeline.run_cold(&root, options);

    // Same shared_ptr identity, not just equal content - proves neither
    // stage actually recomputed (would_recompute's own short-circuit, or
    // execute()'s own should_recompute==false path, skipped real work).
    EXPECT_EQ(first.view_layers, second.view_layers);
    EXPECT_EQ(first.hierarchy, second.hierarchy);
}

TEST_F(ViewRenderPipelineFixture, HierarchyDepthChangeAloneLeavesViewLayersUntouched)
{
    const ViewRenderPipeline::ColdOutput depth_one = pipeline.run_cold(&root, options_for(1));
    const ViewRenderPipeline::ColdOutput depth_two = pipeline.run_cold(&root, options_for(2));

    // LayerGenerationStage doesn't care about hierarchy_depth at all - its
    // own handle should be untouched even though HierarchyResolverStage's
    // own output changed.
    EXPECT_EQ(depth_one.view_layers, depth_two.view_layers);
    EXPECT_NE(depth_one.hierarchy, depth_two.hierarchy);
}

TEST_F(ViewRenderPipelineFixture, RootMutationCascadesIntoBothStagesRecomputing)
{
    const ViewRenderPipeline::ColdOutput before = pipeline.run_cold(&root, options_for(1));

    root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
    root.bump_mutation_version();

    const ViewRenderPipeline::ColdOutput after = pipeline.run_cold(&root, options_for(1));

    // LayerGenerationStage recomputes (root_mutation_version changed) -
    // its own bumped version() becomes HierarchyResolverStage's own
    // incoming data_version, forcing it to recompute too even though
    // top_level/hierarchy_depth didn't change.
    EXPECT_NE(before.view_layers, after.view_layers);
    EXPECT_NE(before.hierarchy, after.hierarchy);
    EXPECT_TRUE(after.view_layers->find(root.get_layer_by_name("M2"), ViewLayerPurpose::TERMINAL).valid());
}

TEST_F(ViewRenderPipelineFixture, NullRootProducesEmptyOutputs)
{
    const ViewRenderPipeline::ColdOutput output = pipeline.run_cold(nullptr, options_for(1));

    ASSERT_NE(output.view_layers, nullptr);
    ASSERT_NE(output.hierarchy, nullptr);
    EXPECT_TRUE(output.view_layers->all().empty());
    EXPECT_TRUE(output.hierarchy->view_data.empty());
}

TEST_F(ViewRenderPipelineFixture, RunWarmProducesAFrameSizedToTheViewport)
{
    const ViewRenderPipeline::WarmOutput output = pipeline.run_warm(&root, warm_options_for(1));

    ASSERT_NE(output.view_layers, nullptr);
    ASSERT_NE(output.hierarchy, nullptr);
    ASSERT_NE(output.culled, nullptr);
    ASSERT_NE(output.rasterized, nullptr);
    ASSERT_NE(output.frame, nullptr);

    EXPECT_FALSE(output.frame->empty);
    EXPECT_EQ(output.frame->buffer.width, 250);  // 5000 dbu * scale 0.05
    EXPECT_EQ(output.frame->buffer.height, 250);
    EXPECT_NE(output.frame->buffer.data, nullptr);

    // Warm's own output should agree with what run_cold() independently
    // computes for the same options - run_warm() reuses run_cold()
    // internally rather than duplicating its logic (ViewRenderPipeline's
    // own doc comment), so this also guards against that reuse silently
    // drifting apart.
    EXPECT_EQ(output.view_layers, pipeline.run_cold(&root, options_for(1)).view_layers);
}

TEST_F(ViewRenderPipelineFixture, RunWarmCacheHitReturnsIdenticalFrameHandleOnUnchangedInputs)
{
    const ViewRenderOptions options = warm_options_for(1);
    const ViewRenderPipeline::WarmOutput first = pipeline.run_warm(&root, options);
    const ViewRenderPipeline::WarmOutput second = pipeline.run_warm(&root, options);

    EXPECT_EQ(first.culled, second.culled);
    EXPECT_EQ(first.rasterized, second.rasterized);
    EXPECT_EQ(first.frame, second.frame);
}

TEST_F(ViewRenderPipelineFixture, RunWarmNullRootProducesEmptyFrame)
{
    const ViewRenderPipeline::WarmOutput output = pipeline.run_warm(nullptr, warm_options_for(1));

    ASSERT_NE(output.frame, nullptr);
    EXPECT_TRUE(output.frame->empty);
}
