// backend/ONETBB_INTEGRATION.md migration plan: Phase 0's toolchain-proving
// MemoizingStage test (below), plus one section per real stage ported from
// src/pipeline/src/render/src/instancing as the migration proceeds -
// mirrors pipeline_test.cpp/render_test.cpp/instancing_test.cpp's own
// "one file per module, grouped by class" convention.
#include "../abstract_shape_pipeline.hpp"
#include "../layout_shape_pipeline.hpp"
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

// BUGS_AND_ENHANCEMENTS.md B3 - a via with no explicit ViaLayer rects
// (a LEF 5.6 VIARULE-inside-VIA reference) but a real ROWCOL clause is a
// via *array*, synthesized into a real grid of cut rects rather than
// skipped entirely (via_shapes.hpp's own append_via_rule_array).
TEST_F(AbstractShapePipelineFixture, RunSynthesizesAViaRuleReferencesRowColIntoARealCutArray)
{
    const le::LayerId cut_layer = root.create_layer(le::LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});

    const le::ViaId via_id = root.create_via(le::ViaData{.technology = technology_id, .name = "VIAARRAY"});
    root.create_via_rule_reference(le::ViaRuleReferenceData{
        .via = via_id,
        .via_rule_name = "ViaRule1",
        .cut_size = le::Point{.x = 2, .y = 2},
        .bot_layer_name = "M1",
        .cut_layer_name = "V1",
        .top_layer_name = "M2",
        .cut_spacing = le::Point{.x = 1, .y = 1},
        .bot_enclosure = le::Point{.x = 1, .y = 1},
        .top_enclosure = le::Point{.x = 1, .y = 1},
        .num_cut_rows = 2,
        .num_cut_cols = 3,
    });

    le::Shape shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(le::ShapeVia{.via_name = "VIAARRAY", .origin = le::Point{50, 50}});
    add_terminal_shape(shape);

    const auto &grouped = pipeline.run(abstract_id, options());

    const le::ViewLayerId cut_view_layer = view_layers.find(cut_layer, le::ViewLayerPurpose::TERMINAL);
    const auto cut_it = grouped.find(cut_view_layer);
    ASSERT_NE(cut_it, grouped.end());
    ASSERT_EQ(cut_it->second.size(), 1u); // one RenderedShape...
    EXPECT_EQ(cut_it->second.front().shape.rects.size(), 6u); // ...holding all 2 rows x 3 cols = 6 cuts

    // Bottom/top metal each get their own single enclosure rect (not
    // per-cut) - both layers are ordinary routing layers here (m1/m2,
    // the fixture's own), so both show up as real TERMINAL-purpose
    // RenderedShapes too.
    const le::ViewLayerId bot_view_layer = view_layers.find(m1, le::ViewLayerPurpose::TERMINAL);
    const auto bot_it = grouped.find(bot_view_layer);
    ASSERT_NE(bot_it, grouped.end());
    // The Shape's own rect (10x10 at origin) plus the via's own bottom
    // enclosure rect both land on M1/TERMINAL - the enclosure one is
    // whichever entry isn't the original 0,0-10,10 rect.
    bool found_enclosure = false;
    for (const auto &rendered : bot_it->second)
        if (rendered.shape.rects.size() == 1 && rendered.shape.rects[0].ll.x != 0)
            found_enclosure = true;
    EXPECT_TRUE(found_enclosure);

    const le::ViewLayerId top_view_layer = view_layers.find(m2, le::ViewLayerPurpose::TERMINAL);
    const auto top_it = grouped.find(top_view_layer);
    ASSERT_NE(top_it, grouped.end());
    ASSERT_EQ(top_it->second.size(), 1u);
}

// A ViaRuleReference with no ROWCOL clause at all still means something
// real in LEF - a single cut at cut_size - not "nothing to draw" the way
// it was skipped before this fix.
TEST_F(AbstractShapePipelineFixture, RunSynthesizesASingleCutForAViaRuleReferenceWithNoRowCol)
{
    root.create_layer(le::LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});

    const le::ViaId via_id = root.create_via(le::ViaData{.technology = technology_id, .name = "VIASINGLE"});
    root.create_via_rule_reference(le::ViaRuleReferenceData{
        .via = via_id,
        .via_rule_name = "ViaRule1",
        .cut_size = le::Point{.x = 2, .y = 2},
        .bot_layer_name = "M1",
        .cut_layer_name = "V1",
        .top_layer_name = "M2",
    });

    le::Shape shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(le::ShapeVia{.via_name = "VIASINGLE", .origin = le::Point{50, 50}});
    add_terminal_shape(shape);

    const auto &grouped = pipeline.run(abstract_id, options());
    size_t cut_rect_count = 0;
    for (const auto &[view_layer_id, shapes] : grouped)
        for (const auto &rendered : shapes)
            if (rendered.shape.layer.valid() && root.get_layer(rendered.shape.layer) != nullptr &&
                root.get_layer(rendered.shape.layer)->name == "V1")
                cut_rect_count += rendered.shape.rects.size();
    EXPECT_EQ(cut_rect_count, 1u);
}

// BUGS_AND_ENHANCEMENTS.md B3 follow-up - ORIGIN shifts the whole cut
// array's own center away from the via's own placement point; OFFSET
// separately shifts each metal layer's own enclosure-rect center on top
// of that. rows=cols=1 (a single 2x2 cut) keeps the arithmetic small
// enough to hand-verify exactly, matching via_shapes.hpp's own
// synthesize_cut_array formulas: local start = -total/2 + origin, each
// enclosure rect = [start - enc + layer_offset, start + total + enc +
// layer_offset], both transformed by the via's own placement point (50,50).
TEST_F(AbstractShapePipelineFixture, RunAppliesOriginAndOffsetToAViaRuleReferencesCutArray)
{
    root.create_layer(le::LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});

    const le::ViaId via_id = root.create_via(le::ViaData{.technology = technology_id, .name = "VIAORIGIN"});
    root.create_via_rule_reference(le::ViaRuleReferenceData{
        .via = via_id,
        .via_rule_name = "ViaRule1",
        .cut_size = le::Point{.x = 2, .y = 2},
        .bot_layer_name = "M1",
        .cut_layer_name = "V1",
        .top_layer_name = "M2",
        .bot_enclosure = le::Point{.x = 1, .y = 1},
        .top_enclosure = le::Point{.x = 1, .y = 1},
        .origin = le::Point{.x = 5, .y = -5},
        .bot_offset = le::Point{.x = 2, .y = 0},
        .top_offset = le::Point{.x = -2, .y = 0},
    });

    le::Shape shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(le::ShapeVia{.via_name = "VIAORIGIN", .origin = le::Point{50, 50}});
    add_terminal_shape(shape);

    const auto &grouped = pipeline.run(abstract_id, options());

    const le::ViewLayerId cut_view_layer = view_layers.find(root.get_layer_by_name("V1"), le::ViewLayerPurpose::TERMINAL);
    const auto cut_it = grouped.find(cut_view_layer);
    ASSERT_NE(cut_it, grouped.end());
    ASSERT_EQ(cut_it->second.size(), 1u);
    ASSERT_EQ(cut_it->second.front().shape.rects.size(), 1u);
    // ORIGIN (5,-5): local start = (-1+5, -1-5) = (4,-6); cut spans local
    // (4,-6)-(6,-4); + placement (50,50) -> world (54,44)-(56,46).
    const le::Rect &cut_rect = cut_it->second.front().shape.rects[0];
    EXPECT_EQ(cut_rect.ll.x, 54);
    EXPECT_EQ(cut_rect.ll.y, 44);
    EXPECT_EQ(cut_rect.ur.x, 56);
    EXPECT_EQ(cut_rect.ur.y, 46);

    const le::ViewLayerId bot_view_layer = view_layers.find(m1, le::ViewLayerPurpose::TERMINAL);
    const auto bot_it = grouped.find(bot_view_layer);
    ASSERT_NE(bot_it, grouped.end());
    const le::RenderedShape *bot_enclosure = nullptr;
    for (const auto &rendered : bot_it->second)
        if (rendered.shape.rects.size() == 1 && rendered.shape.rects[0].ll.x != 0)
            bot_enclosure = &rendered;
    ASSERT_NE(bot_enclosure, nullptr);
    // bot_offset (2,0): local (5,-7)-(9,-3) -> world (55,43)-(59,47).
    EXPECT_EQ(bot_enclosure->shape.rects[0].ll.x, 55);
    EXPECT_EQ(bot_enclosure->shape.rects[0].ll.y, 43);
    EXPECT_EQ(bot_enclosure->shape.rects[0].ur.x, 59);
    EXPECT_EQ(bot_enclosure->shape.rects[0].ur.y, 47);

    const le::ViewLayerId top_view_layer = view_layers.find(m2, le::ViewLayerPurpose::TERMINAL);
    const auto top_it = grouped.find(top_view_layer);
    ASSERT_NE(top_it, grouped.end());
    ASSERT_EQ(top_it->second.size(), 1u);
    // top_offset (-2,0): local (1,-7)-(5,-3) -> world (51,43)-(55,47).
    EXPECT_EQ(top_it->second.front().shape.rects[0].ll.x, 51);
    EXPECT_EQ(top_it->second.front().shape.rects[0].ll.y, 43);
    EXPECT_EQ(top_it->second.front().shape.rects[0].ur.x, 55);
    EXPECT_EQ(top_it->second.front().shape.rects[0].ur.y, 47);
}

// BUGS_AND_ENHANCEMENTS.md B3 follow-up - a via_name resolving only to a
// top-level VIARULE ... GENERATE rule (no Via/LayoutVia, no
// ViaRuleReference anywhere) synthesizes a cut array fit to the
// available routing width (ShapeVia.width) - via_shapes.hpp's own
// append_generate_via_array. cut_w=cut_h=2 (RECT -1 -1 1 1), spacing=1,
// margin=1 on both metal layers (bot/top both ENCLOSURE 1 1, so
// margin_x=margin_y=max(1,1)=1) and width=13 is chosen so the fit is
// exact: floor((13 - 2*1 + 1) / (2+1)) = floor(12/3) = 4 cuts per axis,
// consuming exactly 4*2 + 3*1 + 2*1 = 13 of the available width - no
// slack, so this also confirms the fit isn't off-by-one in either
// direction.
TEST_F(AbstractShapePipelineFixture, RunFitsAGenerateViaRulesCutArrayToTheAvailableRoutingWidth)
{
    const le::LayerId cut_layer = root.create_layer(le::LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});

    const le::ViaRuleId via_rule_id = root.create_via_rule(le::ViaRuleData{.technology = technology_id, .name = "GENRULE", .is_generate = true});
    root.create_via_rule_layer(le::ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "M1", .enclosure_overhang1 = 1, .enclosure_overhang2 = 1});
    root.create_via_rule_layer(le::ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "M2", .enclosure_overhang1 = 1, .enclosure_overhang2 = 1});
    root.create_via_rule_layer(le::ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "V1", .rect = le::Rect{.ll = {-1, -1}, .ur = {1, 1}}, .spacing_step_x = 1, .spacing_step_y = 1});

    le::Shape shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(le::ShapeVia{.via_name = "GENRULE", .origin = le::Point{50, 50}, .width = 13});
    add_terminal_shape(shape);

    const auto &grouped = pipeline.run(abstract_id, options());

    const le::ViewLayerId cut_view_layer = view_layers.find(cut_layer, le::ViewLayerPurpose::TERMINAL);
    const auto cut_it = grouped.find(cut_view_layer);
    ASSERT_NE(cut_it, grouped.end());
    ASSERT_EQ(cut_it->second.size(), 1u);
    EXPECT_EQ(cut_it->second.front().shape.rects.size(), 16u); // 4 rows x 4 cols

    const le::ViewLayerId bot_view_layer = view_layers.find(m1, le::ViewLayerPurpose::TERMINAL);
    ASSERT_NE(grouped.find(bot_view_layer), grouped.end());
    const le::ViewLayerId top_view_layer = view_layers.find(m2, le::ViewLayerPurpose::TERMINAL);
    ASSERT_NE(grouped.find(top_view_layer), grouped.end());
}

// No routing-width context at all (ShapeVia.width unset - e.g. a LEF
// PORT/OBS VIA, which has no enclosing routed path) falls back to a
// single cut, the same "nothing to size an array from" meaning a
// ViaRuleReference with no ROWCOL clause already uses.
TEST_F(AbstractShapePipelineFixture, RunFallsBackToASingleCutForAGenerateViaRuleWithNoWidthContext)
{
    root.create_layer(le::LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});

    const le::ViaRuleId via_rule_id = root.create_via_rule(le::ViaRuleData{.technology = technology_id, .name = "GENRULE", .is_generate = true});
    root.create_via_rule_layer(le::ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "M1", .enclosure_overhang1 = 1, .enclosure_overhang2 = 1});
    root.create_via_rule_layer(le::ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "M2", .enclosure_overhang1 = 1, .enclosure_overhang2 = 1});
    root.create_via_rule_layer(le::ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "V1", .rect = le::Rect{.ll = {-1, -1}, .ur = {1, 1}}, .spacing_step_x = 1, .spacing_step_y = 1});

    le::Shape shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(le::ShapeVia{.via_name = "GENRULE", .origin = le::Point{50, 50}}); // width left unset
    add_terminal_shape(shape);

    const auto &grouped = pipeline.run(abstract_id, options());
    size_t cut_rect_count = 0;
    for (const auto &[view_layer_id, shapes] : grouped)
        for (const auto &rendered : shapes)
            if (rendered.shape.layer.valid() && root.get_layer(rendered.shape.layer) != nullptr &&
                root.get_layer(rendered.shape.layer)->name == "V1")
                cut_rect_count += rendered.shape.rects.size();
    EXPECT_EQ(cut_rect_count, 1u);
}

// A via_name resolving to neither an explicit Via/LayoutVia, nor a
// ViaRuleReference, nor a GENERATE ViaRule is still skipped, not an
// error - the pre-existing "no explicit geometry to draw" behavior for
// every other case must survive tier 3's own addition.
TEST_F(AbstractShapePipelineFixture, RunSkipsAViaNameResolvingToNothingAtAll)
{
    le::Shape shape{.layer = m1, .rects = {le::Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(le::ShapeVia{.via_name = "NO_SUCH_VIA_OR_RULE", .origin = le::Point{50, 50}});
    add_terminal_shape(shape);

    // Only the Shape's own 10x10 rect should show up - nothing extra
    // from the unresolved via.
    EXPECT_EQ(total_shapes(pipeline.run(abstract_id, options())), 1u);
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

// --- LayoutShapePipeline (Phase 3: oneTBB wiring of LayoutGeometryStage ->
// ViewportFilterStage -> LayerVisibilityFilterStage / -> TinyViewportFilterStage
// -> TinyLayerVisibilityFilterStage) - a representative subset of
// pipeline_test.cpp's own LayoutPipelineFixture cases against Pipeline, for
// the same reason AbstractShapePipelineFixture's own subset is representative
// rather than exhaustive (see that section's own comment) - LayoutGeometryStage's
// compute() body is an unchanged copy of GenerateLayoutShapesStage::run's
// own lambda plus its private helper statics.

namespace
{
    struct LayoutShapePipelineFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(le::TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(le::LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(le::LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
            view_layers = le::ViewLayerSet::build_for_technology(root, technology_id);

            le::LibraryId library_id = root.create_library(le::LibraryData{.name = "LIB"});
            le::DesignId design_id = root.create_design(le::DesignData{.library = library_id, .name = "TOP"});
            layout_id = root.create_layout(le::LayoutData{.design = design_id});

            scene.set_pan(le::Point{0, 0});
            scene.set_scale(1.0);
            scene.set_viewport_size(4000, 4000); // large enough to enclose every test shape below
        }

        void add_diearea(const le::Rect &rect)
        {
            le::Shape shape;
            shape.layout = layout_id;
            shape.purpose = le::ShapePurpose::BOUNDARY;
            shape.rects.push_back(rect);
            root.create_shape(std::move(shape));
        }

        le::BlockageId add_routing_blockage(le::LayerId layer, const le::Rect &rect)
        {
            le::BlockageId blockage_id = root.create_blockage(le::BlockageData{.layout = layout_id, .kind = le::BlockageKind::ROUTING});
            le::Shape shape;
            shape.blockage = blockage_id;
            shape.layer = layer;
            shape.rects.push_back(rect);
            root.create_shape(std::move(shape));
            return blockage_id;
        }

        le::RouteId add_route(le::LayerId layer, const le::Rect &rect)
        {
            le::RouteId route_id = root.create_route(le::RouteData{.layout = layout_id, .name = "NET1"});
            le::Shape shape;
            shape.route = route_id;
            shape.layer = layer;
            shape.rects.push_back(rect);
            root.create_shape(std::move(shape));
            return route_id;
        }

        le::SiteId add_site(const std::string &name, le::Point size)
        {
            return root.create_site(le::SiteData{.technology = technology_id, .name = name, .size = size});
        }

        le::RowId add_row(const std::string &site_name, le::Point origin, std::optional<int> num_x = std::nullopt, std::optional<int> num_y = std::nullopt, std::optional<int64_t> step_x = std::nullopt, std::optional<int64_t> step_y = std::nullopt)
        {
            return root.create_row(le::RowData{
                .layout = layout_id,
                .name = "ROW" + std::to_string(next_row_index++),
                .site_name = site_name,
                .origin = origin,
                .orientation = le::Orientation::N,
                .num_x = num_x,
                .num_y = num_y,
                .step_x = step_x,
                .step_y = step_y,
            });
        }

        le::TrackId add_track(bool is_x, int64_t start, int count, int64_t step, std::vector<std::string> layer_names)
        {
            return root.create_track(le::TrackData{.layout = layout_id, .is_x = is_x, .start = start, .count = count, .step = step, .layer_names = std::move(layer_names)});
        }

        le::GCellGridId add_gcell_grid(bool is_x, int64_t start, int count, int64_t step)
        {
            return root.create_g_cell_grid(le::GCellGridData{.layout = layout_id, .is_x = is_x, .start = start, .count = count, .step = step});
        }

        le::RegionId add_region(const le::Rect &rect)
        {
            return root.create_region(le::RegionData{.layout = layout_id, .name = "REGION" + std::to_string(next_region_index++), .rects = {rect}});
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

        static const le::RenderedShape *find_by_purpose(const std::map<le::ViewLayerId, std::vector<le::RenderedShape>> &grouped, const le::ViewLayerSet &view_layers, le::ViewLayerPurpose purpose)
        {
            for (const auto &[view_layer_id, shapes] : grouped)
            {
                const le::ViewLayerData *data = view_layers.get(view_layer_id);
                if (data && data->purpose == purpose && !shapes.empty())
                    return &shapes.front();
            }
            return nullptr;
        }

        le::Root root;
        le::TechnologyId technology_id;
        le::LayerId m1;
        le::LayerId m2;
        le::ViewLayerSet view_layers;
        le::LayoutId layout_id;
        le::Scene scene;
        le::LayoutShapePipeline pipeline;
        int next_row_index = 0;
        int next_region_index = 0;
    };
}

TEST_F(LayoutShapePipelineFixture, RunIncludesDieAreaResolvedToBoundaryViewLayer)
{
    add_diearea(le::Rect{.ll = {0, 0}, .ur = {1000, 2000}});

    const auto &grouped = pipeline.run(layout_id, options());
    const auto it = grouped.find(view_layers.boundary_view_layer());
    ASSERT_NE(it, grouped.end());
    ASSERT_EQ(it->second.size(), 1u);
    EXPECT_EQ(it->second.front().shape.rects[0].ur.x, 1000);
    EXPECT_EQ(it->second.front().shape.rects[0].ur.y, 2000);
}

TEST_F(LayoutShapePipelineFixture, RunForUnknownLayoutIsEmpty)
{
    EXPECT_TRUE(pipeline.run(le::LayoutId{}, options()).empty());
}

TEST_F(LayoutShapePipelineFixture, RunResolvesRoutingBlockageAndRouteToTheirOwnLayerColumns)
{
    add_routing_blockage(m1, le::Rect{.ll = {0, 0}, .ur = {10, 10}});
    add_route(m2, le::Rect{.ll = {5, 5}, .ur = {15, 15}});

    const auto &grouped = pipeline.run(layout_id, options());
    const le::RenderedShape *blockage = find_by_purpose(grouped, view_layers, le::ViewLayerPurpose::ROUTING_BLOCKAGE);
    ASSERT_NE(blockage, nullptr);
    EXPECT_EQ(blockage->view_layer, view_layers.find(m1, le::ViewLayerPurpose::ROUTING_BLOCKAGE));

    const le::RenderedShape *route = find_by_purpose(grouped, view_layers, le::ViewLayerPurpose::ROUTE);
    ASSERT_NE(route, nullptr);
    EXPECT_EQ(route->view_layer, view_layers.find(m2, le::ViewLayerPurpose::ROUTE));
}

TEST_F(LayoutShapePipelineFixture, RunSynthesizesRowRectangleFromSiteSizeAndTiling)
{
    // ROW defaults to invisible (BUGS_AND_ENHANCEMENTS.md E2) - this test
    // is about the synthesized geometry itself, not visibility filtering.
    scene.set_purpose_visible(le::ViewLayerPurpose::ROW, true);
    add_site("SITE1", le::Point{500, 1000});
    add_row("SITE1", le::Point{100, 200}, /*num_x=*/3, /*num_y=*/2, /*step_x=*/600, /*step_y=*/std::nullopt);

    const auto &grouped = pipeline.run(layout_id, options());
    const le::RenderedShape *found = find_by_purpose(grouped, view_layers, le::ViewLayerPurpose::ROW);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.rects.size(), 1u);
    // width = site.x + (num_x - 1) * step_x = 500 + 2 * 600 = 1700
    // height = site.y + (num_y - 1) * step_y(fallback site.y=1000) = 1000 + 1000 = 2000
    EXPECT_EQ(found->shape.rects[0].ll.x, 100);
    EXPECT_EQ(found->shape.rects[0].ll.y, 200);
    EXPECT_EQ(found->shape.rects[0].ur.x, 1800);
    EXPECT_EQ(found->shape.rects[0].ur.y, 2200);
}

TEST_F(LayoutShapePipelineFixture, RunSynthesizesTrackLinesSpanningDieAreaInThePerpendicularDirection)
{
    // M1's own LayerData.direction defaults to RoutingDirection::H (no
    // explicit direction set in this fixture's SetUp) - an is_x=true
    // track (DEF TRACKS X, vertical lines) doesn't match H, so this
    // resolves to TRACK_NON_PREFERRED, not TRACK_PREFERRED (see
    // append_track_shapes's own comment on the is_x/direction mapping).
    // TRACK_NON_PREFERRED defaults to invisible (E2) - this test is about
    // the synthesized geometry itself, not visibility filtering.
    scene.set_purpose_visible(le::ViewLayerPurpose::TRACK_NON_PREFERRED, true);
    add_diearea(le::Rect{.ll = {0, 0}, .ur = {1000, 2000}});
    add_track(/*is_x=*/true, /*start=*/0, /*count=*/3, /*step=*/100, {"M1"});

    const auto &grouped = pipeline.run(layout_id, options());
    const le::RenderedShape *found = find_by_purpose(grouped, view_layers, le::ViewLayerPurpose::TRACK_NON_PREFERRED);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.paths.size(), 3u);
    for (int i = 0; i < 3; i++)
    {
        const int64_t x = i * 100;
        EXPECT_EQ(found->shape.paths[i].polygon.points[0].x, x);
        EXPECT_EQ(found->shape.paths[i].polygon.points[1].x, x);
    }
}

TEST_F(LayoutShapePipelineFixture, RunSynthesizesPreferredDirectionTrackLinesWhenIsXMatchesLayersOwnDirection)
{
    // Same M1 (direction defaults to H) but is_x=false (DEF TRACKS Y,
    // horizontal lines) matches H - resolves to TRACK_PREFERRED instead.
    // TRACK_PREFERRED defaults to invisible (E2) - this test is about the
    // synthesized geometry itself, not visibility filtering.
    scene.set_purpose_visible(le::ViewLayerPurpose::TRACK_PREFERRED, true);
    add_diearea(le::Rect{.ll = {0, 0}, .ur = {1000, 2000}});
    add_track(/*is_x=*/false, /*start=*/0, /*count=*/2, /*step=*/500, {"M1"});

    const auto &grouped = pipeline.run(layout_id, options());
    const le::RenderedShape *preferred = find_by_purpose(grouped, view_layers, le::ViewLayerPurpose::TRACK_PREFERRED);
    const le::RenderedShape *non_preferred = find_by_purpose(grouped, view_layers, le::ViewLayerPurpose::TRACK_NON_PREFERRED);
    ASSERT_NE(preferred, nullptr);
    EXPECT_EQ(non_preferred, nullptr);
    ASSERT_EQ(preferred->shape.paths.size(), 2u);
    for (int i = 0; i < 2; i++)
    {
        const int64_t y = i * 500;
        EXPECT_EQ(preferred->shape.paths[i].polygon.points[0].y, y);
        EXPECT_EQ(preferred->shape.paths[i].polygon.points[1].y, y);
    }
}

TEST_F(LayoutShapePipelineFixture, RunSynthesizesGCellGridLinesSpanningDieArea)
{
    // GCELLGRID defaults to invisible (BUGS_AND_ENHANCEMENTS.md E2) -
    // this test is about the synthesized geometry, not visibility filtering.
    scene.set_purpose_visible(le::ViewLayerPurpose::GCELLGRID, true);
    add_diearea(le::Rect{.ll = {0, 0}, .ur = {1000, 2000}});
    add_gcell_grid(/*is_x=*/false, /*start=*/500, /*count=*/2, /*step=*/300);

    const auto &grouped = pipeline.run(layout_id, options());
    const le::RenderedShape *found = find_by_purpose(grouped, view_layers, le::ViewLayerPurpose::GCELLGRID);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.paths.size(), 2u);
    EXPECT_EQ(found->shape.paths[0].polygon.points[0].y, 500);
    EXPECT_EQ(found->shape.paths[1].polygon.points[0].y, 800);
}

TEST_F(LayoutShapePipelineFixture, RunSkipsTrackAndGCellGridEntirelyWhenLayoutHasNoDieArea)
{
    add_track(/*is_x=*/true, /*start=*/0, /*count=*/3, /*step=*/100, {"M1"});
    add_gcell_grid(/*is_x=*/false, /*start=*/500, /*count=*/2, /*step=*/300);

    EXPECT_TRUE(pipeline.run(layout_id, options()).empty());
}

TEST_F(LayoutShapePipelineFixture, RunSynthesizesRegionFromStoredRects)
{
    add_region(le::Rect{.ll = {0, 0}, .ur = {50, 50}});

    const auto &grouped = pipeline.run(layout_id, options());
    const le::RenderedShape *found = find_by_purpose(grouped, view_layers, le::ViewLayerPurpose::REGION);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.rects.size(), 1u);
    EXPECT_EQ(found->shape.rects[0].ur.x, 50);
}

TEST_F(LayoutShapePipelineFixture, RunReusesCacheForSameLayoutIdAndEpoch)
{
    add_diearea(le::Rect{.ll = {0, 0}, .ur = {1000, 2000}});

    pipeline.run(layout_id, options());
    const uint64_t first_version = pipeline.shapes_version();
    pipeline.run(layout_id, options());
    EXPECT_EQ(pipeline.shapes_version(), first_version);
}

TEST_F(LayoutShapePipelineFixture, RunRecomputesAfterACrudMutationEvenForTheSameLayoutIdAndViewLayerSet)
{
    pipeline.run(layout_id, options());
    const uint64_t first_version = pipeline.shapes_version();

    add_diearea(le::Rect{.ll = {0, 0}, .ur = {1000, 2000}});
    root.bump_mutation_version();

    EXPECT_FALSE(pipeline.run(layout_id, options()).empty());
    EXPECT_NE(pipeline.shapes_version(), first_version);
}
