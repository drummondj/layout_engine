#include "../pipeline/filter_by_viewport_and_size_stage.hpp"
#include "../pipeline/generate_and_filter_pipeline.hpp"
#include "../pipeline/generate_obstructions_on_layer_stage.hpp"
#include "../pipeline/generate_terminals_on_layer_stage.hpp"
#include "../pipeline/generate_vias_on_layer_stage.hpp"
#include <gtest/gtest.h>
#include <set>
#include <string>

using namespace le;

namespace
{
    // Mirrors src/pipeline/tests/pipeline_test.cpp's own PipelineFixture -
    // built by reading that file as reference, not by including it (this
    // experiment stays self-contained under src/flow, see
    // backend/TASKFLOW_EXPERIMENT.md).
    struct FlowPipelineStagesFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);
            abstract_id = root.create_abstract(AbstractData{});
        }

        TerminalId add_terminal_shape(const Shape &shape)
        {
            TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "T" + std::to_string(next_terminal_index++)});
            TerminalPortId port_id = root.create_terminal_port(TerminalPortData{.terminal = terminal_id});
            Shape owned_shape = shape;
            owned_shape.terminal_port = port_id;
            root.create_shape(std::move(owned_shape));
            return terminal_id;
        }

        ObstructionId add_obstruction_shape(const Shape &shape)
        {
            ObstructionId obstruction_id = root.create_obstruction(ObstructionData{.abstract = abstract_id});
            Shape owned_shape = shape;
            owned_shape.obstruction = obstruction_id;
            root.create_shape(std::move(owned_shape));
            return obstruction_id;
        }

        // Adds a CUT-type Layer (e.g. "VIA12") and rebuilds view_layers so
        // it gets its own row - GenerateAndFilterPipeline's row-driven
        // dispatch only ever spawns a via task for a layer that actually
        // exists in view_layers.rows().
        LayerId add_cut_layer(const std::string &name)
        {
            const LayerId id = root.create_layer(LayerData{.technology = technology_id, .name = name, .type = "CUT"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);
            return id;
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        LayerId m2;
        ViewLayerSet view_layers;
        AbstractId abstract_id;
        int next_terminal_index = 0;
    };
}

TEST_F(FlowPipelineStagesFixture, GenerateTerminalsOnLayerStageCollectsOnlyMatchingLayerShapes)
{
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_terminal_shape(Shape{.layer = m2, .rects = {Rect{.ll = {20, 20}, .ur = {30, 30}}}});

    flow::GenerateTerminalsOnLayerStage stage;
    const auto &shapes = stage.run(root, abstract_id, m1, view_layers);

    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_EQ(shapes[0].view_layer, view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    EXPECT_EQ(shapes[0].shape.rects[0].ll.x, 0);
}

TEST_F(FlowPipelineStagesFixture, GenerateTerminalsOnLayerStageDoesNotResolveVias)
{
    const LayerId via12 = add_cut_layer("VIA12");
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{50, 50}});
    add_terminal_shape(shape);

    flow::GenerateTerminalsOnLayerStage stage;
    const auto &shapes = stage.run(root, abstract_id, m1, view_layers);

    // Only the direct M1 shape - via resolution is GenerateViasOnLayerStage's
    // own job now, not this stage's.
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_EQ(shapes[0].view_layer, view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    (void)via12;
}

TEST_F(FlowPipelineStagesFixture, GenerateObstructionsOnLayerStageCollectsOnlyMatchingLayerShapes)
{
    add_obstruction_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_obstruction_shape(Shape{.layer = m2, .rects = {Rect{.ll = {20, 20}, .ur = {30, 30}}}});

    flow::GenerateObstructionsOnLayerStage stage;
    const auto &shapes = stage.run(root, abstract_id, m2, view_layers);

    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_EQ(shapes[0].view_layer, view_layers.find(m2, ViewLayerPurpose::OBSTRUCTION));
    EXPECT_EQ(shapes[0].shape.rects[0].ll.x, 20);
}

TEST_F(FlowPipelineStagesFixture, GenerateViasOnLayerStageResolvesATerminalViaOntoAdjacentRoutingLayer)
{
    const LayerId via12 = add_cut_layer("VIA12");
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{50, 50}});
    add_terminal_shape(shape);

    flow::GenerateViasOnLayerStage stage;
    const auto &shapes = stage.run(root, abstract_id, via12, view_layers);

    const ViewLayerId expected = view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    const RenderedShape *via_shape = nullptr;
    for (const RenderedShape &rs : shapes)
        if (rs.view_layer == expected)
            via_shape = &rs;
    ASSERT_NE(via_shape, nullptr);
    ASSERT_EQ(via_shape->shape.rects.size(), 1u);
    EXPECT_EQ(via_shape->shape.rects[0].ll.x, 45);
    EXPECT_EQ(via_shape->shape.rects[0].ll.y, 45);
    EXPECT_FALSE(via_shape->shape_id.has_value());
}

TEST_F(FlowPipelineStagesFixture, GenerateViasOnLayerStageTagsObstructionOriginatedViaWithObstructionPurpose)
{
    const LayerId via12 = add_cut_layer("VIA12");
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{50, 50}});
    add_obstruction_shape(shape);

    flow::GenerateViasOnLayerStage stage;
    const auto &shapes = stage.run(root, abstract_id, via12, view_layers);

    const ViewLayerId expected_obstruction = view_layers.find(m2, ViewLayerPurpose::OBSTRUCTION);
    const ViewLayerId terminal_m2 = view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    bool found_as_obstruction = false;
    for (const RenderedShape &rs : shapes)
    {
        EXPECT_NE(rs.view_layer, terminal_m2) << "obstruction-originated via resolved with the wrong purpose";
        if (rs.view_layer == expected_obstruction)
            found_as_obstruction = true;
    }
    EXPECT_TRUE(found_as_obstruction);
}

TEST_F(FlowPipelineStagesFixture, FilterByViewportAndSizeDropsOutOfViewportAndSubPixelShapes)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0); // 1 dbu == 1 px, so min_visible_dbu == 1
    scene.set_viewport_size(100, 100);

    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}}, // inside, big enough - kept
        RenderedShape{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {1000, 1000}, .ur = {1010, 1010}}}}, .view_layer = {}}, // outside viewport - dropped
        RenderedShape{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}}, .view_layer = {}}, // zero-size both dims - dropped
    };

    flow::GenerateTerminalsOnLayerStage upstream;
    flow::FilterByViewportAndSizeStage filter;
    const auto &result = filter.run(upstream, shapes, scene);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].shape.rects[0].ll.x, 10);
}

TEST_F(FlowPipelineStagesFixture, FilterByViewportAndSizeKeepsLongThinShapeEvenIfOneDimensionIsSubPixel)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {10, 10}, .ur = {10, 60}}}}, .view_layer = {}},
    };

    flow::GenerateTerminalsOnLayerStage upstream;
    flow::FilterByViewportAndSizeStage filter;
    const auto &result = filter.run(upstream, shapes, scene);
    ASSERT_EQ(result.size(), 1u);
}

TEST_F(FlowPipelineStagesFixture, FilterByViewportAndSizeReusesCacheUntilViewportVersionChanges)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}},
    };

    flow::GenerateTerminalsOnLayerStage upstream;
    flow::FilterByViewportAndSizeStage filter;
    filter.run(upstream, shapes, scene);
    filter.run(upstream, shapes, scene);
    EXPECT_EQ(filter.call_count(), 1u);

    scene.set_pan(Point{1, 1});
    filter.run(upstream, shapes, scene);
    EXPECT_EQ(filter.call_count(), 2u);
}

namespace
{
    Scene wide_open_scene(AbstractId abstract_id)
    {
        Scene scene;
        scene.set_current_abstract(abstract_id);
        scene.set_pan(Point{-100, -100});
        scene.set_scale(1.0); // 1 dbu == 1 px, so min_visible_dbu == 1 - trivial for these fixtures' real shapes
        scene.set_viewport_size(10000, 10000); // covers dbu [-100, 9900) - comfortably wide for these fixtures
        return scene;
    }
}

TEST_F(FlowPipelineStagesFixture, GenerateAndFilterPipelineCollectsTerminalsObstructionsAndBoundary)
{
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_obstruction_shape(Shape{.layer = m2, .rects = {Rect{.ll = {20, 20}, .ur = {30, 30}}}});

    root.create_shape(Shape{.abstract = abstract_id, .rects = {Rect{.ll = {0, 0}, .ur = {1000, 1000}}}});

    Scene scene = wide_open_scene(abstract_id);
    tf::Executor executor;
    flow::GenerateAndFilterPipeline pipeline;
    const auto &result = pipeline.run(root, abstract_id, view_layers, scene, executor);

    EXPECT_EQ(result.size(), 3u); // terminal + obstruction + boundary
}

TEST_F(FlowPipelineStagesFixture, GenerateAndFilterPipelineResolvesCrossLayerViaEndToEnd)
{
    add_cut_layer("VIA12");
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{50, 50}});
    add_terminal_shape(shape);

    Scene scene = wide_open_scene(abstract_id);
    tf::Executor executor;
    flow::GenerateAndFilterPipeline pipeline;
    const auto &result = pipeline.run(root, abstract_id, view_layers, scene, executor);

    ASSERT_EQ(result.size(), 2u); // the direct M1 shape + the M2 via-shape
    const ViewLayerId m1_terminal = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const ViewLayerId m2_terminal = view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    std::set<ViewLayerId> seen;
    for (const auto &rs : result)
        seen.insert(rs.view_layer);
    EXPECT_TRUE(seen.count(m1_terminal));
    EXPECT_TRUE(seen.count(m2_terminal));
}

TEST_F(FlowPipelineStagesFixture, GenerateAndFilterPipelineCachesPerTierIndependently)
{
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene = wide_open_scene(abstract_id);
    tf::Executor executor;
    flow::GenerateAndFilterPipeline pipeline;

    pipeline.run(root, abstract_id, view_layers, scene, executor);
    const uint64_t generate_calls_after_first_run = pipeline.total_generate_calls();
    const uint64_t filter_calls_after_first_run = pipeline.total_filter_calls();

    // Identical call - both tiers should be pure cache hits.
    pipeline.run(root, abstract_id, view_layers, scene, executor);
    EXPECT_EQ(pipeline.total_generate_calls(), generate_calls_after_first_run);
    EXPECT_EQ(pipeline.total_filter_calls(), filter_calls_after_first_run);

    // Viewport-only change - generate tier must stay a cache hit (pan/zoom
    // never invalidates it), only the filter tier should recompute.
    scene.set_pan(Point{1, 1});
    pipeline.run(root, abstract_id, view_layers, scene, executor);
    EXPECT_EQ(pipeline.total_generate_calls(), generate_calls_after_first_run);
    EXPECT_GT(pipeline.total_filter_calls(), filter_calls_after_first_run);
}

// Several layers, several Terminals/Obstructions per layer, plus a via
// crossing layers - a real, if small, multi-task-DAG exercise of
// collect_shapes gathering every chain's own output correctly.
TEST_F(FlowPipelineStagesFixture, GenerateAndFilterPipelineLargeMultiLayerFixtureIsCorrect)
{
    add_cut_layer("VIA12");
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-2, -2}, .ur = {2, 2}}}});

    std::set<ShapeId> expected_terminal_and_obstruction_ids;

    constexpr int kTerminalCount = 40;
    for (int i = 0; i < kTerminalCount; ++i)
    {
        const LayerId layer = (i % 2 == 0) ? m1 : m2;
        Shape shape{.layer = layer, .rects = {Rect{.ll = {i * 100, 0}, .ur = {i * 100 + 10, 10}}}};
        if (i % 5 == 0) // every 5th Terminal also has a via crossing to the other layer
            shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{i * 100 + 50, 50}});

        const TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "T" + std::to_string(i)});
        const TerminalPortId port_id = root.create_terminal_port(TerminalPortData{.terminal = terminal_id});
        const ShapeId shape_id = root.create_shape(Shape{.layer = shape.layer, .terminal_port = port_id, .rects = shape.rects, .vias = shape.vias});
        expected_terminal_and_obstruction_ids.insert(shape_id);
    }

    constexpr int kObstructionCount = 20;
    for (int i = 0; i < kObstructionCount; ++i)
    {
        const LayerId layer = (i % 2 == 0) ? m1 : m2;
        const ObstructionId obstruction_id = root.create_obstruction(ObstructionData{.abstract = abstract_id});
        const ShapeId shape_id = root.create_shape(Shape{.layer = layer, .obstruction = obstruction_id, .rects = {Rect{.ll = {i * 100, 500}, .ur = {i * 100 + 10, 510}}}});
        expected_terminal_and_obstruction_ids.insert(shape_id);
    }

    Scene scene = wide_open_scene(abstract_id);
    tf::Executor executor;
    flow::GenerateAndFilterPipeline pipeline;
    const auto &result = pipeline.run(root, abstract_id, view_layers, scene, executor);

    // Every direct Terminal/Obstruction shape must appear exactly once,
    // identified by shape_id; via-shapes (no shape_id) are extra, on top.
    std::set<ShapeId> seen_direct_ids;
    int via_shape_count = 0;
    for (const auto &rs : result)
    {
        if (rs.shape_id.has_value())
        {
            EXPECT_TRUE(expected_terminal_and_obstruction_ids.count(*rs.shape_id));
            EXPECT_TRUE(seen_direct_ids.insert(*rs.shape_id).second) << "duplicate shape_id in collected output";
        }
        else
        {
            ++via_shape_count;
        }
    }
    EXPECT_EQ(seen_direct_ids.size(), expected_terminal_and_obstruction_ids.size());
    EXPECT_EQ(via_shape_count, kTerminalCount / 5); // one via-shape per every-5th Terminal
}
