// backend/ONETBB_INTEGRATION.md migration plan: Phase 0's toolchain-proving
// MemoizingStage test (below), plus one section per real stage ported from
// src/pipeline/src/render/src/instancing as the migration proceeds -
// mirrors pipeline_test.cpp/render_test.cpp/instancing_test.cpp's own
// "one file per module, grouped by class" convention.
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
