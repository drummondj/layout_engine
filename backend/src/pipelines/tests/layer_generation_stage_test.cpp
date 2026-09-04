#include "../stages/layer_generation_stage.hpp"
#include "../synchronous_stage_runner.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    using LayerGenerationRunner = SynchronousStageRunner<LayerGenerationStage, const Root *, ViewLayerSet, ColdPipelineOptions>;

    struct LayerGenerationStageFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        LayerId m2;
        LayerGenerationRunner runner{"LayerGeneration"};
    };
}

TEST_F(LayerGenerationStageFixture, MatchesDirectViewLayerSetBuild)
{
    ColdPipelineOptions options{.root_mutation_version = root.mutation_version()};
    const ViewLayerSet &generated = runner.run(&root, 0, options);

    const ViewLayerSet expected = ViewLayerSet::build_for_technology(root, technology_id);

    EXPECT_EQ(generated.all().size(), expected.all().size());
    EXPECT_TRUE(generated.find(m1, ViewLayerPurpose::TERMINAL).valid());
    EXPECT_TRUE(generated.find(m2, ViewLayerPurpose::OBSTRUCTION).valid());
}

TEST_F(LayerGenerationStageFixture, NullRootProducesEmptyViewLayerSet)
{
    ColdPipelineOptions options{.root_mutation_version = 0};
    const ViewLayerSet &generated = runner.run(nullptr, 0, options);
    EXPECT_TRUE(generated.all().empty());
}

TEST_F(LayerGenerationStageFixture, RootWithNoTechnologyProducesEmptyViewLayerSet)
{
    Root empty_root;
    ColdPipelineOptions options{.root_mutation_version = empty_root.mutation_version()};
    const ViewLayerSet &generated = runner.run(&empty_root, 0, options);
    EXPECT_TRUE(generated.all().empty());
}

TEST_F(LayerGenerationStageFixture, CacheHitOnUnchangedMutationVersion)
{
    ColdPipelineOptions options{.root_mutation_version = root.mutation_version()};
    runner.run(&root, 0, options);
    ASSERT_FALSE(runner.would_recompute(0, options));

    const ViewLayerSet &second = runner.run(&root, 0, options);
    EXPECT_EQ(second.all().size(), 18u);
}

TEST_F(LayerGenerationStageFixture, RecomputesWhenRootMutationVersionChanges)
{
    ColdPipelineOptions first_options{.root_mutation_version = root.mutation_version()};
    const ViewLayerSet &first = runner.run(&root, 0, first_options);
    EXPECT_EQ(first.all().size(), 18u);
    const std::uint64_t generation_before = first.generation();

    root.create_layer(LayerData{.technology = technology_id, .name = "M3", .type = "ROUTING"});
    root.bump_mutation_version();

    ColdPipelineOptions second_options{.root_mutation_version = root.mutation_version()};
    ASSERT_TRUE(runner.would_recompute(0, second_options));

    const ViewLayerSet &second = runner.run(&root, 0, second_options);
    EXPECT_EQ(second.all().size(), 24u); // +1 layer x 6 purposes
    EXPECT_NE(second.generation(), generation_before);
}
