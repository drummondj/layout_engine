// backend/ONETBB_INTEGRATION.md migration plan: Phase 0's toolchain-proving
// MemoizingStage test (below), plus one section per real stage ported from
// src/pipeline/src/render/src/instancing as the migration proceeds -
// mirrors pipeline_test.cpp/render_test.cpp/instancing_test.cpp's own
// "one file per module, grouped by class" convention.
#include "../abstract_shape_pipeline.hpp"
#include "../stages/viewport_filter_stage.hpp"
#include "../tbb_core.hpp"
#include <gtest/gtest.h>
#include <memory>

namespace
{
    struct TestOptions
    {
        int trigger = 0;
        bool operator==(const TestOptions &) const = default;
    };

    class DoublingStage : public le::MemoizingStage<int, int, TestOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;
        int compute_count = 0;

    protected:
        int compute(const int &data, const TestOptions &options) override
        {
            ++compute_count;
            return data * 2 + options.trigger;
        }

        bool options_did_change(const TestOptions &last, const TestOptions &current) const override
        {
            return last.trigger != current.trigger;
        }
    };
}

TEST(MemoizingStage, CacheHitSkipsRecomputeForUnchangedDataVersionAndOptions)
{
    using namespace oneapi::tbb::flow;
    graph g;
    auto stage = std::make_unique<DoublingStage>(g, "doubling");

    le::StageData<int, TestOptions> received{};
    function_node<le::StageData<int, TestOptions>> sink(g, serial, [&](le::StageData<int, TestOptions> in)
                                                          { received = in; });
    make_edge(stage->node(), sink);

    TestOptions options{.trigger = 1};

    stage->try_put({.data = 21, .data_version = 1, .options = options});
    g.wait_for_all();
    EXPECT_EQ(received.data, 43);
    EXPECT_EQ(stage->compute_count, 1);

    // Same data_version, same options -> cache hit, no recompute, but the
    // node still forwards the previous result downstream.
    stage->try_put({.data = 21, .data_version = 1, .options = options});
    g.wait_for_all();
    EXPECT_EQ(stage->compute_count, 1);
    EXPECT_EQ(received.data, 43);

    // data_version changed -> recompute, even though the payload's own
    // value happens to be unchanged (MemoizingStage never inspects data
    // itself, only data_version).
    stage->try_put({.data = 21, .data_version = 2, .options = options});
    g.wait_for_all();
    EXPECT_EQ(stage->compute_count, 2);

    // data_version unchanged, but options_did_change() says recompute.
    TestOptions changed_options{.trigger = 5};
    stage->try_put({.data = 21, .data_version = 2, .options = changed_options});
    g.wait_for_all();
    EXPECT_EQ(stage->compute_count, 3);
    EXPECT_EQ(received.data, 47);
}

// --- ViewportFilterStage (Phase 1: oneTBB port of
// src/pipeline/stages/filter_by_viewport_and_size_stage.hpp) - mirrors
// pipeline_test.cpp's own FilterByViewportAndSize* cases, adapted for
// MemoizingStage's try_put/wait_for_all API in place of a plain run() call.

namespace
{
    struct ViewportFilterStageFixture : public ::testing::Test
    {
        oneapi::tbb::flow::graph g;
        std::unique_ptr<le::ViewportFilterStage> stage = std::make_unique<le::ViewportFilterStage>(g, "viewport_filter");
        // Captures the whole StageData, not just .data - MemoizingStage
        // exposes no public recompute-count accessor of its own (unlike
        // core::VersionedStage's call_count()); the output's own
        // data_version, bumped only on a real recompute, is the only
        // observable signal of cache-hit vs. recompute from outside.
        le::StageData<std::vector<le::RenderedShape>, le::PipelineOptions> received;
        oneapi::tbb::flow::function_node<le::StageData<std::vector<le::RenderedShape>, le::PipelineOptions>> sink{
            g, oneapi::tbb::flow::serial, [this](le::StageData<std::vector<le::RenderedShape>, le::PipelineOptions> in)
            { received = std::move(in); }};

        void SetUp() override { make_edge(stage->node(), sink); }

        const std::vector<le::RenderedShape> &run(const std::vector<le::RenderedShape> &shapes, uint64_t data_version, const le::PipelineOptions &options)
        {
            stage->try_put({.data = shapes, .data_version = data_version, .options = options});
            g.wait_for_all();
            return received.data;
        }

        // Builds a PipelineOptions whose `viewport` sub-struct mirrors a
        // Scene's own current pan/scale/viewport-size state -
        // ViewportFilterStage::compute reads the Scene directly via
        // options.ctx.scene, while options_did_change compares
        // viewport.viewport_version - both must reflect the same Scene.
        static le::PipelineOptions options_for(const le::Scene &scene)
        {
            le::PipelineOptions options;
            options.ctx.scene = &scene;
            options.viewport.viewport_version = scene.viewport_version();
            options.viewport.scale = scene.scale();
            return options;
        }
    };
}

TEST_F(ViewportFilterStageFixture, KeepsShapesInsideViewport)
{
    le::Scene scene;
    scene.set_pan(le::Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    std::vector<le::RenderedShape> shapes = {
        le::RenderedShape{.shape = le::Shape{.rects = {le::Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}},
    };
    EXPECT_EQ(run(shapes, 1, options_for(scene)).size(), 1u);
}

TEST_F(ViewportFilterStageFixture, DropsShapesWithNoGeometry)
{
    // Geometry::bbox() doesn't account for Shape::texts, so a text-only
    // shape (no rects/polygons/paths) has no bbox at all.
    le::Scene scene;
    scene.set_pan(le::Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    std::vector<le::RenderedShape> shapes = {
        le::RenderedShape{.shape = le::Shape{.texts = {le::Text{.label = "A1", .location = {5, 5}}}}, .view_layer = {}},
    };
    EXPECT_TRUE(run(shapes, 1, options_for(scene)).empty());
}

TEST_F(ViewportFilterStageFixture, DropsShapesOutsideViewport)
{
    le::Scene scene;
    scene.set_pan(le::Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    std::vector<le::RenderedShape> shapes = {
        le::RenderedShape{.shape = le::Shape{.rects = {le::Rect{.ll = {1000, 1000}, .ur = {1010, 1010}}}}, .view_layer = {}},
    };
    EXPECT_TRUE(run(shapes, 1, options_for(scene)).empty());
}

TEST_F(ViewportFilterStageFixture, DropsSubPixelDotsButKeepsThinLongShapes)
{
    le::Scene scene;
    scene.set_pan(le::Point{0, 0});
    scene.set_scale(1.0); // 1 dbu == 1 px, so the sub-pixel threshold is 1 dbu
    scene.set_viewport_size(200, 200);

    le::RenderedShape dot{.shape = le::Shape{.rects = {le::Rect{.ll = {5, 5}, .ur = {5, 5}}}}, .view_layer = {}};             // 0x0
    le::RenderedShape thin_long_line{.shape = le::Shape{.rects = {le::Rect{.ll = {5, 5}, .ur = {5, 105}}}}, .view_layer = {}}; // 0 wide, 100 tall

    const auto &result = run({dot, thin_long_line}, 1, options_for(scene));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.front().shape.rects.front().ur.y, 105);
}

TEST_F(ViewportFilterStageFixture, ReusesCacheUntilViewportVersionChanges)
{
    le::Scene scene;
    scene.set_pan(le::Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    std::vector<le::RenderedShape> shapes = {
        le::RenderedShape{.shape = le::Shape{.rects = {le::Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}},
    };

    run(shapes, 1, options_for(scene));
    run(shapes, 1, options_for(scene));
    EXPECT_EQ(received.data_version, 1u);

    scene.set_pan(le::Point{1, 1}); // bumps viewport_version()
    run(shapes, 1, options_for(scene));
    EXPECT_EQ(received.data_version, 2u);
}

TEST_F(ViewportFilterStageFixture, RecomputesWhenDataVersionChangesEvenWithSameViewportVersion)
{
    // Mirrors pipeline_test.cpp's own
    // FilterByViewportAndSizeRecomputesWhenUpstreamVersionChangesEvenWithSameViewportVersion -
    // the caller sets data_version to the upstream stage's own version(),
    // so a change there (independent of options.viewport.viewport_version,
    // left unchanged throughout) must still trigger a recompute.
    le::Scene scene;
    scene.set_pan(le::Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    std::vector<le::RenderedShape> shapes = {
        le::RenderedShape{.shape = le::Shape{.rects = {le::Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}},
    };

    run(shapes, 1, options_for(scene));
    EXPECT_EQ(received.data_version, 1u);

    run(shapes, 2, options_for(scene));
    EXPECT_EQ(received.data_version, 2u);
}

// --- AbstractShapePipeline (Phase 2: oneTBB wiring of
// AbstractGeometryStage -> ViewportFilterStage -> LayerVisibilityFilterStage
// / -> TinyViewportFilterStage -> TinyLayerVisibilityFilterStage) - a
// representative subset of pipeline_test.cpp's own GenerateShapes*/
// FilterByLayerVisibility* cases against Pipeline, since AbstractGeometryStage's
// compute() body is an unchanged copy of GenerateAbstractShapesStage::run's
// own lambda (already exhaustively covered there) - these tests exist to
// prove the new wrapper/wiring/recompute-trigger machinery, not to
// re-derive geometry correctness a second time.

namespace
{
    struct AbstractShapePipelineFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(le::TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(le::LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(le::LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
            view_layers = le::ViewLayerSet::build_for_technology(root, technology_id);
            abstract_id = root.create_abstract(le::AbstractData{});

            scene.set_pan(le::Point{0, 0});
            scene.set_scale(1.0);
            scene.set_viewport_size(2000, 2000); // large enough to enclose every test shape below
        }

        le::TerminalId add_terminal_shape(const le::Shape &shape)
        {
            le::TerminalId terminal_id = root.create_terminal(le::TerminalData{.abstract = abstract_id, .name = "T" + std::to_string(next_terminal_index++)});
            le::TerminalPortId port_id = root.create_terminal_port(le::TerminalPortData{.terminal = terminal_id});
            le::Shape owned_shape = shape;
            owned_shape.terminal_port = port_id;
            root.create_shape(std::move(owned_shape));
            return terminal_id;
        }

        le::ObstructionId add_obstruction_shape(const le::Shape &shape)
        {
            le::ObstructionId obstruction_id = root.create_obstruction(le::ObstructionData{.abstract = abstract_id});
            le::Shape owned_shape = shape;
            owned_shape.obstruction = obstruction_id;
            root.create_shape(std::move(owned_shape));
            return obstruction_id;
        }

        le::PipelineOptions options() const
        {
            le::PipelineOptions o;
            o.ctx.root = &root;
            o.ctx.view_layers = &view_layers;
            o.ctx.scene = &scene;
            o.epoch.root_mutation_version = root.mutation_version();
            o.epoch.view_layers_generation = view_layers.generation();
            o.viewport.viewport_version = scene.viewport_version();
            o.viewport.visibility_version = scene.visibility_version();
            o.viewport.scale = scene.scale();
            return o;
        }

        // Sums every group's own size - the pipeline's own result is
        // grouped by ViewLayerId, unlike Pipeline::generate_shapes's own
        // flat vector, so tests that only care about total shape count
        // (not which layer) go through this helper.
        static size_t total_shapes(const std::map<le::ViewLayerId, std::vector<le::RenderedShape>> &grouped)
        {
            size_t total = 0;
            for (const auto &[_, shapes] : grouped)
                total += shapes.size();
            return total;
        }

        le::Root root;
        le::TechnologyId technology_id;
        le::LayerId m1;
        le::LayerId m2;
        le::ViewLayerSet view_layers;
        le::AbstractId abstract_id;
        le::Scene scene;
        le::AbstractShapePipeline pipeline;
        int next_terminal_index = 0;
    };
}

TEST_F(AbstractShapePipelineFixture, RunCollectsPortAndObstructionShapes)
{
    add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_obstruction_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {20, 20}, .ur = {30, 30}}}});

    EXPECT_EQ(total_shapes(pipeline.run(abstract_id, options())), 2u);
}

TEST_F(AbstractShapePipelineFixture, RunIncludesAbstractBoundaryResolvedToBoundaryViewLayer)
{
    root.create_shape(le::ShapeData{.abstract = abstract_id, .purpose = le::ShapePurpose::BOUNDARY, .polygons = {le::Polygon{.points = {{0, 0}, {0, 100}, {100, 100}, {100, 0}, {0, 0}}}}});

    const auto &grouped = pipeline.run(abstract_id, options());
    const auto it = grouped.find(view_layers.boundary_view_layer());
    ASSERT_NE(it, grouped.end());
    ASSERT_EQ(it->second.size(), 1u);
    ASSERT_TRUE(it->second.front().shape.purpose.has_value());
    EXPECT_EQ(*it->second.front().shape.purpose, le::ShapePurpose::BOUNDARY);
}

TEST_F(AbstractShapePipelineFixture, RunForUnknownAbstractIsEmpty)
{
    le::AbstractId unknown{999, 0};
    EXPECT_TRUE(pipeline.run(unknown, options()).empty());
}

TEST_F(AbstractShapePipelineFixture, RunResolvesAViaReferencedByATerminalOntoItsOwnLayerWithTerminalPurpose)
{
    const le::ViaId via_id = root.create_via(le::ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(le::ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {le::Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    le::Shape shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(le::ShapeVia{.via_name = "VIA12", .origin = le::Point{50, 50}});
    add_terminal_shape(shape);

    const auto &grouped = pipeline.run(abstract_id, options());
    const le::ViewLayerId expected = view_layers.find(m2, le::ViewLayerPurpose::TERMINAL);
    const auto it = grouped.find(expected);
    ASSERT_NE(it, grouped.end());
    ASSERT_EQ(it->second.size(), 1u);
    EXPECT_EQ(it->second.front().shape.rects[0].ll.x, 45);
    EXPECT_EQ(it->second.front().shape.rects[0].ll.y, 45);
}

TEST_F(AbstractShapePipelineFixture, RunDropsShapesOutsideViewportAndOnHiddenLayers)
{
    add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {10, 10}, .ur = {20, 20}}}});
    add_terminal_shape(le::Shape{.layer = m2, .rects = {le::Rect{.ll = {30, 30}, .ur = {40, 40}}}});

    // Baseline: both visible, both in viewport.
    EXPECT_EQ(total_shapes(pipeline.run(abstract_id, options())), 2u);

    // Hide M2's TERMINAL ViewLayer - only the M1 shape should survive.
    const le::ViewLayerData *m2_terminal = view_layers.get(view_layers.find(m2, le::ViewLayerPurpose::TERMINAL));
    ASSERT_NE(m2_terminal, nullptr);
    scene.set_layer_name_visible(m2_terminal->layer_name, false);
    EXPECT_EQ(total_shapes(pipeline.run(abstract_id, options())), 1u);
}

TEST_F(AbstractShapePipelineFixture, RunReusesCacheForSameAbstractIdAndEpoch)
{
    add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    pipeline.run(abstract_id, options());
    const uint64_t first_version = pipeline.shapes_version();
    pipeline.run(abstract_id, options());
    EXPECT_EQ(pipeline.shapes_version(), first_version); // cache hit - nothing changed
}

TEST_F(AbstractShapePipelineFixture, RunRecomputesAfterACrudMutationEvenForTheSameAbstractIdAndViewLayerSet)
{
    // Mirrors pipeline_test.cpp's own
    // GenerateShapesRecomputesAfterACrudMutationEvenForTheSameAbstractIdAndViewLayerSet -
    // the real regression this stage's original key was built to fix (see
    // AbstractGeometryStage's own doc comment).
    pipeline.run(abstract_id, options());
    const uint64_t first_version = pipeline.shapes_version();
    EXPECT_TRUE(pipeline.run(abstract_id, options()).empty());
    EXPECT_EQ(pipeline.shapes_version(), first_version); // nothing changed - cache hit

    add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    root.bump_mutation_version();

    EXPECT_EQ(total_shapes(pipeline.run(abstract_id, options())), 1u);
    EXPECT_NE(pipeline.shapes_version(), first_version); // same AbstractId, same ViewLayerSet - must still recompute
}

TEST_F(AbstractShapePipelineFixture, RunTinyShapesProducesADotForASubPixelShapeButNotAnOrdinaryOne)
{
    scene.set_scale(1.0); // 1 dbu == 1 px, so the sub-pixel threshold is 1 dbu
    add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {5, 5}, .ur = {5, 5}}}});   // 0x0 - sub-pixel
    add_terminal_shape(le::Shape{.layer = m1, .rects = {le::Rect{.ll = {50, 50}, .ur = {60, 60}}}}); // 10x10 - ordinary

    const auto &tiny = pipeline.run_tiny_shapes(abstract_id, options());
    EXPECT_EQ(total_shapes(pipeline.run(abstract_id, options())), 1u); // only the ordinary shape survives the normal chain

    size_t tiny_count = 0;
    for (const auto &[_, dots] : tiny)
        tiny_count += dots.size();
    EXPECT_EQ(tiny_count, 1u);
}
