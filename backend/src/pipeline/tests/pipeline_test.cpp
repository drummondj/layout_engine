#include "../pipeline.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    // Builds a Root with one Technology, an M1 and M2 layer, a matching
    // ViewLayerSet, and one empty Abstract - the common scaffolding every
    // test below attaches to.
    struct PipelineFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);
            abstract_id = root.create_abstract(AbstractData{});
        }

        // Adds a new TerminalPort (with one Shape) to an *existing*
        // Terminal - for tests that need more than one port on the same
        // Terminal. add_terminal_shape below is the common case (a fresh
        // Terminal with a single port/shape) built on top of this.
        TerminalPortId add_port_shape(TerminalId terminal_id, const Shape &shape)
        {
            TerminalPortId port_id = root.create_terminal_port(TerminalPortData{.terminal = terminal_id});
            Shape owned_shape = shape;
            owned_shape.terminal_port = port_id;
            root.create_shape(std::move(owned_shape));
            return port_id;
        }

        TerminalId add_terminal_shape(const Shape &shape)
        {
            // Terminal.name is unique_per_parent (per-Abstract) - a
            // synthetic per-call name, since none of the tests using this
            // helper care about the terminal's own name, just its
            // existence/geometry, and several call it more than once
            // against the same abstract_id.
            TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "T" + std::to_string(next_terminal_index++)});
            add_port_shape(terminal_id, shape);
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

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        LayerId m2;
        ViewLayerSet view_layers;
        AbstractId abstract_id;
        Pipeline pipeline;
        int next_terminal_index = 0;
    };

    // Whether any polygon in `polygons` has a point at exactly (x, y) -
    // an order-independent membership check, for tests that don't care
    // which index a given expanded polygon landed at.
    bool any_polygon_has_point(const std::vector<Polygon> &polygons, int64_t x, int64_t y)
    {
        for (const Polygon &polygon : polygons)
            for (const Point &point : polygon.points)
                if (point.x == x && point.y == y)
                    return true;
        return false;
    }
}

TEST_F(PipelineFixture, GenerateShapesCollectsPortAndObstructionShapes)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {20, 20}, .ur = {30, 30}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    EXPECT_EQ(shapes.size(), 2u);
}

TEST_F(PipelineFixture, GenerateShapesComputesPathOutlinesForTerminalAndObstructionPaths)
{
    // RenderedShape::path_outlines is computed once here (not at
    // Renderer::transform_to_pixels time, which reruns on every pan/zoom -
    // see generate_shapes's own doc comment) so draw_group can fill/
    // outline a PATH like a real POLYGON instead of stroking its
    // centerline (see BENCHMARKS.md for the solid-fill bug this fixes).
    const Path terminal_path{.polygon = Polygon{.points = {{0, 0}, {10, 0}}}, .width = 4};
    add_terminal_shape(Shape{.layer_name = "M1", .paths = {terminal_path}});

    const Path obstruction_path{.polygon = Polygon{.points = {{20, 20}, {20, 40}}}, .width = 6};
    add_obstruction_shape(Shape{.layer_name = "M1", .paths = {obstruction_path}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);

    for (const auto &rs : shapes)
    {
        ASSERT_EQ(rs.shape.paths.size(), 1u);
        ASSERT_NE(rs.path_outlines, nullptr);
        ASSERT_EQ(rs.path_outlines->size(), 1u);
        const auto expected = Geometry::path_to_polygons(rs.shape.paths.front());
        ASSERT_EQ(rs.path_outlines->front().size(), expected.size());
        ASSERT_FALSE(expected.empty());

        const Polygon &actual_poly = rs.path_outlines->front().front();
        const Polygon &expected_poly = expected.front();
        ASSERT_EQ(actual_poly.points.size(), expected_poly.points.size());
        for (size_t i = 0; i < actual_poly.points.size(); ++i)
        {
            EXPECT_EQ(actual_poly.points[i].x, expected_poly.points[i].x);
            EXPECT_EQ(actual_poly.points[i].y, expected_poly.points[i].y);
        }
    }
}

TEST_F(PipelineFixture, GenerateShapesForUnknownAbstractIsEmpty)
{
    AbstractId unknown{999, 0};
    EXPECT_TRUE(pipeline.generate_shapes(root, unknown, view_layers).empty());
}

TEST_F(PipelineFixture, GenerateShapesResolvesViewLayerByOrigin)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}});
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);
    EXPECT_EQ(shapes[0].view_layer, view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    EXPECT_EQ(shapes[1].view_layer, view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION));
    EXPECT_NE(shapes[0].view_layer, shapes[1].view_layer);
}

TEST_F(PipelineFixture, GenerateShapesIncludesAbstractBoundaryResolvedToBoundaryViewLayer)
{
    root.get_abstract(abstract_id)->boundary = {Polygon{.points = {{0, 0}, {0, 100}, {100, 100}, {100, 0}, {0, 0}}}};

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_EQ(shapes.front().shape.layer_name, "BOUNDARY");
    EXPECT_EQ(shapes.front().view_layer, view_layers.boundary_view_layer());
}

TEST_F(PipelineFixture, GenerateShapesLeavesViewLayerInvalidForUnresolvableLayerName)
{
    add_obstruction_shape(Shape{.layer_name = "DOES_NOT_EXIST", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_FALSE(shapes.front().view_layer.valid());
}

TEST_F(PipelineFixture, GenerateShapesKeepsOverlappingRectsWithinATerminalPortShapeAsSeparateRects)
{
    // Rendered geometry always matches the raw database structure exactly
    // (no shape-merging step) - overlapping rects stay as separate rects,
    // not unioned into a polygon, even though they visually overlap.
    add_terminal_shape(Shape{
        .layer_name = "M1",
        .rects = {
            Rect{.ll = {0, 0}, .ur = {10, 10}},
            Rect{.ll = {5, 5}, .ur = {15, 15}},
        },
    });

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    ASSERT_EQ(shapes.front().shape.rects.size(), 2u);
    EXPECT_TRUE(shapes.front().shape.polygons.empty());
}

TEST_F(PipelineFixture, GenerateShapesKeepsOverlappingRectsWithinAnObstructionShapeAsSeparateRects)
{
    add_obstruction_shape(Shape{
        .layer_name = "M1",
        .rects = {
            Rect{.ll = {0, 0}, .ur = {10, 10}},
            Rect{.ll = {5, 5}, .ur = {15, 15}},
        },
    });

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    ASSERT_EQ(shapes.front().shape.rects.size(), 2u);
    EXPECT_TRUE(shapes.front().shape.polygons.empty());
}

TEST_F(PipelineFixture, GenerateShapesExpandsRectIteratesIntoConcreteRects)
{
    // UPDATES.md 12 Phase 1's ITERATE rework - LEFReader stores RECT
    // ITERATE raw; generate_shapes is where it's expanded back into
    // concrete Rects, appended directly to the Shape's own rects (no
    // shape-merging step, so order is deterministic - the two expanded
    // rects land at indices 0 and 1 in iteration order).
    add_obstruction_shape(Shape{
        .layer_name = "M1",
        .rect_iterates = {RectIterate{
            .rect = Rect{.ll = {0, 0}, .ur = {10, 10}},
            .num_x = 2,
            .num_y = 1,
            .space_x = 100,
            .space_y = 0,
        }},
    });

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    const Shape &shape = shapes.front().shape;
    EXPECT_TRUE(shape.rect_iterates.empty()); // consumed by expansion
    ASSERT_EQ(shape.rects.size(), 2u);
    EXPECT_EQ(shape.rects[0].ll.x, 0);
    EXPECT_EQ(shape.rects[1].ll.x, 100);
    EXPECT_TRUE(shape.polygons.empty());
}

TEST_F(PipelineFixture, GenerateShapesExpandsPathIteratesIntoConcretePaths)
{
    add_obstruction_shape(Shape{
        .layer_name = "M1",
        .path_iterates = {PathIterate{
            .path = Path{.polygon = Polygon{.points = {{0, 0}, {10, 0}}}, .width = 2},
            .num_x = 1,
            .num_y = 2,
            .space_x = 0,
            .space_y = 50,
        }},
    });

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    const Shape &shape = shapes.front().shape;
    EXPECT_TRUE(shape.path_iterates.empty()); // consumed by expansion
    ASSERT_EQ(shape.paths.size(), 2u);
    EXPECT_EQ(shape.paths[0].polygon.points[0].y, 0);
    EXPECT_EQ(shape.paths[1].polygon.points[0].y, 50);
    EXPECT_EQ(shape.paths[1].width, 2u); // width carried through from the base path

    // path_outlines is computed from the (post-expansion) shape.paths -
    // one entry per expanded path, not per original ITERATE statement.
    ASSERT_EQ(shapes.front().path_outlines->size(), 2u);
}

TEST_F(PipelineFixture, GenerateShapesExpandsPolygonIteratesIntoConcretePolygons)
{
    add_obstruction_shape(Shape{
        .layer_name = "M1",
        .polygon_iterates = {PolygonIterate{
            .polygon = Polygon{.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}}},
            .num_x = 2,
            .num_y = 1,
            .space_x = 100,
            .space_y = 0,
        }},
    });

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    const Shape &shape = shapes.front().shape;
    EXPECT_TRUE(shape.polygon_iterates.empty()); // consumed by expansion
    ASSERT_EQ(shape.polygons.size(), 2u);
    EXPECT_TRUE(any_polygon_has_point(shape.polygons, 0, 0));
    EXPECT_TRUE(any_polygon_has_point(shape.polygons, 100, 0));
}

TEST_F(PipelineFixture, GenerateShapesSkipsAnIteratesEntryWithNonPositiveCounts)
{
    // Defense in depth (see generate_shapes's own comment) - a degenerate
    // num_x/num_y (shouldn't occur via LEFReader, which already validates
    // this at parse time, but the database itself doesn't enforce it) is
    // silently skipped rather than looping zero-or-negative times.
    add_obstruction_shape(Shape{
        .layer_name = "M1",
        .rect_iterates = {RectIterate{
            .rect = Rect{.ll = {0, 0}, .ur = {10, 10}},
            .num_x = 0,
            .num_y = 1,
            .space_x = 100,
            .space_y = 0,
        }},
    });

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_TRUE(shapes.front().shape.rects.empty());
}

TEST_F(PipelineFixture, GenerateShapesDoesNotMergeRectsAcrossDifferentPortsOrObstructions)
{
    // Two disjoint rects, but each its own separate Shape (own port) -
    // merging is scoped per-Shape, not across a Terminal's whole geometry.
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "D4"});
    add_port_shape(terminal_id, Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_port_shape(terminal_id, Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {15, 15}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);
    EXPECT_EQ(shapes[0].shape.rects.size(), 1u);
    EXPECT_EQ(shapes[1].shape.rects.size(), 1u);
}

TEST_F(PipelineFixture, GenerateShapesAddsTerminalLabelAtComputedLocation)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "A1"});
    add_port_shape(terminal_id, Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    ASSERT_EQ(shapes.front().shape.texts.size(), 1u);
    EXPECT_EQ(shapes.front().shape.texts.front().label, "A1");
    // get_label_location on a single {0,0}-{10,10} rect returns its centroid.
    EXPECT_EQ(shapes.front().shape.texts.front().location.x, 5);
    EXPECT_EQ(shapes.front().shape.texts.front().location.y, 5);
    // local_width_at on the same rect: min(10, 10) = 10.
    EXPECT_DOUBLE_EQ(shapes.front().shape.texts.front().size, 10.0);
}

TEST_F(PipelineFixture, GenerateShapesSizesLabelToPathWidth)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "P1"});
    add_port_shape(terminal_id, Shape{.layer_name = "M1", .paths = {Path{.polygon = Polygon{.points = {{0, 0}, {100, 0}}}, .width = 6}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    ASSERT_EQ(shapes.front().shape.texts.size(), 1u);
    EXPECT_DOUBLE_EQ(shapes.front().shape.texts.front().size, 6.0);
}

TEST_F(PipelineFixture, GenerateShapesSizesLabelToLocalPolygonWidthNotBbox)
{
    // Same L-shaped polygon as Geometry.LocalWidthAtLShapedPolygonUsesArmThicknessNotBbox
    // (100x30 bottom arm + 30x100 vertical arm, ~100x100 overall bbox) -
    // proves the schema field -> Geometry::local_width_at wiring is
    // actually connected end to end, not just correct in isolation.
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "L1"});
    add_port_shape(terminal_id, Shape{.layer_name = "M1", .polygons = {Polygon{.points = {
                                                                                    {0, 0},
                                                                                    {100, 0},
                                                                                    {100, 30},
                                                                                    {30, 30},
                                                                                    {30, 100},
                                                                                    {0, 100},
                                                                                }}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    ASSERT_EQ(shapes.front().shape.texts.size(), 1u);
    EXPECT_LT(shapes.front().shape.texts.front().size, 50.0); // far below the ~100-wide bbox
}

TEST_F(PipelineFixture, GenerateShapesLabelsOnlyTheFirstPortsFirstShape)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "B2"});
    add_port_shape(terminal_id, Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_port_shape(terminal_id, Shape{.layer_name = "M1", .rects = {Rect{.ll = {20, 0}, .ur = {30, 10}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);
    ASSERT_EQ(shapes[0].shape.texts.size(), 1u);
    EXPECT_EQ(shapes[0].shape.texts.front().label, "B2");
    EXPECT_TRUE(shapes[1].shape.texts.empty());
}

TEST_F(PipelineFixture, GenerateShapesAddsOneLabelPerDistinctLayer)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "C3"});
    add_port_shape(terminal_id, Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_port_shape(terminal_id, Shape{.layer_name = "M2", .rects = {Rect{.ll = {20, 0}, .ur = {30, 10}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);

    // M1 shape (index 0) gets its own label at its own centroid.
    ASSERT_EQ(shapes[0].shape.texts.size(), 1u);
    EXPECT_EQ(shapes[0].shape.texts.front().label, "C3");
    EXPECT_EQ(shapes[0].shape.texts.front().location.x, 5);
    EXPECT_EQ(shapes[0].shape.texts.front().location.y, 5);

    // M2 shape (index 1) gets its own separate label at its own centroid -
    // not the M1 one, and not left without a label.
    ASSERT_EQ(shapes[1].shape.texts.size(), 1u);
    EXPECT_EQ(shapes[1].shape.texts.front().label, "C3");
    EXPECT_EQ(shapes[1].shape.texts.front().location.x, 25);
    EXPECT_EQ(shapes[1].shape.texts.front().location.y, 5);
}

TEST_F(PipelineFixture, GenerateShapesReusesCacheForSameAbstractId)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    pipeline.generate_shapes(root, abstract_id, view_layers);
    pipeline.generate_shapes(root, abstract_id, view_layers);
    EXPECT_EQ(pipeline.generate_calls(), 1u);

    AbstractId other = root.create_abstract(AbstractData{});
    pipeline.generate_shapes(root, other, view_layers);
    EXPECT_EQ(pipeline.generate_calls(), 2u);
}

TEST_F(PipelineFixture, GenerateShapesRecomputesWhenViewLayersIsRebuiltEvenForTheSameAbstractId)
{
    // Regression: generate_shapes's cache key used to be AbstractId alone,
    // even though its compute lambda resolves every shape's ViewLayerId
    // against the given ViewLayerSet. le_read_lef (api.cpp) rebuilds its
    // handle's ViewLayerSet from scratch - a brand-new Pool, not an
    // in-place update - on every call, so re-reading a LEF file while
    // viewing an already-cached Abstract must invalidate this cache even
    // though the AbstractId itself hasn't changed, or it would keep
    // returning RenderedShapes resolved against the discarded
    // ViewLayerSet. See ViewLayerSet::generation() for the fix.
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    pipeline.generate_shapes(root, abstract_id, view_layers);
    pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(pipeline.generate_calls(), 1u); // same ViewLayerSet instance - cache hit

    ViewLayerSet rebuilt_view_layers = ViewLayerSet::build_for_technology(root, technology_id);
    pipeline.generate_shapes(root, abstract_id, rebuilt_view_layers);
    EXPECT_EQ(pipeline.generate_calls(), 2u); // same AbstractId, but a freshly rebuilt ViewLayerSet - must recompute
}

TEST_F(PipelineFixture, GenerateShapesRecomputesAfterACrudMutationEvenForTheSameAbstractIdAndViewLayerSet)
{
    // Regression: a real crash-adjacent bug (see TCL_EXPLORATION.md and
    // pipeline.hpp's own class comment) - a Tcl/API CRUD mutation
    // (UPDATES.md item 15's Terminal/TerminalPort/Obstruction/Shape
    // surface - api.cpp's le_create_terminal, le_update_shape, etc.)
    // changes neither AbstractId nor ViewLayerSet (no LEF was re-read),
    // so before Root::mutation_version() existed, this cache had no way
    // to know the database changed at all - a Tcl-created Shape never
    // appeared on screen no matter how many times the caller re-rendered,
    // since the frame *was* regenerating, just from a stale cache. Found
    // this way (a real user hitting it through the show_gui Tcl console),
    // not anticipated up front.
    pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(pipeline.generate_calls(), 1u);
    ASSERT_TRUE(pipeline.generate_shapes(root, abstract_id, view_layers).empty());
    ASSERT_EQ(pipeline.generate_calls(), 1u); // nothing changed - cache hit

    // Mirrors api.cpp's own CRUD functions: mutate, then bump the
    // counter - see e.g. le_create_shape/le_update_shape.
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    root.bump_mutation_version();

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    EXPECT_EQ(pipeline.generate_calls(), 2u); // same AbstractId, same ViewLayerSet - must still recompute
    EXPECT_EQ(shapes.size(), 1u);
}

TEST_F(PipelineFixture, FilterByViewportAndSizeKeepsShapesInsideViewport)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}},
    };
    const auto &result = pipeline.filter_by_viewport_and_size(root, shapes, scene, view_layers);
    EXPECT_EQ(result.size(), 1u);
}

TEST_F(PipelineFixture, FilterByViewportAndSizeDropsShapesWithNoGeometry)
{
    // Geometry::bbox() doesn't account for Shape::texts, so a text-only
    // shape (no rects/polygons/paths) has no bbox at all.
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .texts = {Text{.label = "A1", .location = {5, 5}}}}, .view_layer = {}},
    };
    EXPECT_TRUE(pipeline.filter_by_viewport_and_size(root, shapes, scene, view_layers).empty());
}

TEST_F(PipelineFixture, FilterByViewportAndSizeDropsShapesOutsideViewport)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {1000, 1000}, .ur = {1010, 1010}}}}, .view_layer = {}},
    };
    EXPECT_TRUE(pipeline.filter_by_viewport_and_size(root, shapes, scene, view_layers).empty());
}

TEST_F(PipelineFixture, FilterByViewportAndSizeDropsSubPixelDotsButKeepsThinLongShapes)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0); // 1 dbu == 1 px, so the sub-pixel threshold is 1 dbu
    scene.set_viewport_size(200, 200);

    RenderedShape dot{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}}, .view_layer = {}};             // 0x0
    RenderedShape thin_long_line{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {5, 105}}}}, .view_layer = {}}; // 0 wide, 100 tall

    const auto &result = pipeline.filter_by_viewport_and_size(root, std::vector<RenderedShape>{dot, thin_long_line}, scene, view_layers);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.front().shape.rects.front().ur.y, 105);
}

TEST_F(PipelineFixture, FilterByViewportAndSizeReusesCacheUntilViewportVersionChanges)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}},
    };

    pipeline.filter_by_viewport_and_size(root, shapes, scene, view_layers);
    pipeline.filter_by_viewport_and_size(root, shapes, scene, view_layers);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 1u);

    scene.set_pan(Point{1, 1});
    pipeline.filter_by_viewport_and_size(root, shapes, scene, view_layers);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 2u);
}

TEST_F(PipelineFixture, FilterByViewportAndSizeRecomputesWhenUpstreamVersionChangesEvenWithSameViewportVersion)
{
    // The structural property UPDATES.md item 16's refactor exists for:
    // FilterByViewportAndSizeStage's key composes via GenerateShapesStage's
    // own version() (see pipeline.hpp's class comments), not by
    // re-deriving every trigger GenerateShapesStage itself depends on - so
    // *any* future trigger added to GenerateShapesStage (not just
    // Root::mutation_version(), the one that actually caused a real bug -
    // see GenerateShapesRecomputesAfterACrudMutation... above) invalidates
    // this stage automatically, without this stage's own key - or this
    // test - needing to know what that trigger is. Unlike that earlier
    // regression test (which only proves GenerateShapesStage itself
    // recomputes), this one proves the *downstream* stage does too, with
    // its own direct input (viewport_version) deliberately left unchanged
    // throughout - the exact property that would have caught the
    // mutation_version() gap automatically before it ever shipped.
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    const auto &generated = pipeline.generate_shapes(root, abstract_id, view_layers);
    const size_t generated_size_before = generated.size();
    pipeline.filter_by_viewport_and_size(root, generated, scene, view_layers);
    pipeline.filter_by_viewport_and_size(root, generated, scene, view_layers);
    ASSERT_EQ(pipeline.generate_calls(), 1u);
    ASSERT_EQ(pipeline.viewport_filter_calls(), 1u); // nothing changed - cache hit

    // Mutate + bump, exactly like api.cpp's own CRUD functions - scene's
    // viewport_version() is never touched.
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {50, 50}, .ur = {60, 60}}}});
    root.bump_mutation_version();

    const auto &regenerated = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(pipeline.generate_calls(), 2u); // GenerateShapesStage recomputed
    ASSERT_NE(regenerated.size(), generated_size_before); // real content change, not a coincidence

    pipeline.filter_by_viewport_and_size(root, regenerated, scene, view_layers);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 2u); // must recompute - upstream's version changed, even though viewport_version() alone didn't
}

TEST_F(PipelineFixture, FilterByLayerVisibilityDropsHiddenViewLayerKeepsVisible)
{
    ViewLayerId m1_obstruction = view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION);
    ViewLayerId m2_obstruction = view_layers.find(m2, ViewLayerPurpose::OBSTRUCTION);

    Scene scene;
    scene.set_layer_name_visible("M2", false);

    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = m1_obstruction},
        RenderedShape{.shape = Shape{.layer_name = "M2", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = m2_obstruction},
    };

    const auto &result = pipeline.filter_by_layer_visibility(root, shapes, scene, view_layers);
    ASSERT_EQ(result.size(), 1u); // only the M1 group survives - the whole M2 group is dropped
    ASSERT_TRUE(result.contains(m1_obstruction));
    EXPECT_EQ(result.at(m1_obstruction).front().shape.layer_name, "M1");
}

TEST_F(PipelineFixture, FilterByLayerVisibilityKeepsShapesWithInvalidViewLayer)
{
    Scene scene; // no layers explicitly hidden
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "DOES_NOT_EXIST", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = {}},
    };

    const auto &result = pipeline.filter_by_layer_visibility(root, shapes, scene, view_layers);
    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result.contains(ViewLayerId{}));
    EXPECT_EQ(result.at(ViewLayerId{}).size(), 1u);
}

TEST_F(PipelineFixture, FilterByLayerVisibilityGroupsInBottomUpLayerOrder)
{
    // M1 was declared before M2 in SetUp(), so its ViewLayers got lower
    // pool indices - map iteration order should put M1 first, M2 second,
    // BOUNDARY last (see the class comment on why that's bottom-up order).
    ViewLayerId m1_terminal = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    ViewLayerId m2_terminal = view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    ViewLayerId boundary = view_layers.boundary_view_layer();

    Scene scene;
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "BOUNDARY"}, .view_layer = boundary},
        RenderedShape{.shape = Shape{.layer_name = "M2"}, .view_layer = m2_terminal},
        RenderedShape{.shape = Shape{.layer_name = "M1"}, .view_layer = m1_terminal},
    };

    const auto &result = pipeline.filter_by_layer_visibility(root, shapes, scene, view_layers);
    ASSERT_EQ(result.size(), 3u);

    std::vector<ViewLayerId> order;
    for (const auto &[view_layer, group] : result)
        order.push_back(view_layer);

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], m1_terminal);
    EXPECT_EQ(order[1], m2_terminal);
    EXPECT_EQ(order[2], boundary);
}

TEST_F(PipelineFixture, FilterByLayerVisibilityReusesCacheUntilVisibilityVersionChanges)
{
    Scene scene;
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = {}},
    };

    pipeline.filter_by_layer_visibility(root, shapes, scene, view_layers);
    pipeline.filter_by_layer_visibility(root, shapes, scene, view_layers);
    EXPECT_EQ(pipeline.layer_filter_calls(), 1u);

    scene.set_layer_name_visible("M1", false);
    pipeline.filter_by_layer_visibility(root, shapes, scene, view_layers);
    EXPECT_EQ(pipeline.layer_filter_calls(), 2u);
}

TEST_F(PipelineFixture, TinyShapesByViewportKeepsOnlyShapesUnderOnePixelInBothDimensions)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0); // 1 dbu == 1 px, so the sub-pixel threshold is 1 dbu
    scene.set_viewport_size(200, 200);

    // Sub-pixel dot: 0x0 bbox - the exact case filter_by_viewport_and_size drops.
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}});
    // Normal-sized shape - filter_by_viewport_and_size keeps this, so tiny_shapes_by_viewport must not.
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {50, 50}, .ur = {60, 60}}}});

    const auto &result = pipeline.tiny_shapes_by_viewport(root, abstract_id, scene, view_layers);
    ASSERT_EQ(result.size(), 1u);
    // bbox center of a 0x0 box is itself
    EXPECT_EQ(result.front().location.x, 5);
    EXPECT_EQ(result.front().location.y, 5);
}

TEST_F(PipelineFixture, TinyShapesByViewportExcludesAThinLongShapeThatSurvivesTheNormalFilter)
{
    // Mirrors FilterByViewportAndSizeDropsSubPixelDotsButKeepsThinLongShapes -
    // the two stages must always agree on which shapes are "tiny" vs "normal".
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(200, 200);

    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {5, 105}}}}); // 0 wide, 100 tall

    EXPECT_TRUE(pipeline.tiny_shapes_by_viewport(root, abstract_id, scene, view_layers).empty());
}

TEST_F(PipelineFixture, TinyShapesByViewportExcludesATinyShapeOutsideTheViewport)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {5000, 5000}, .ur = {5000, 5000}}}});

    EXPECT_TRUE(pipeline.tiny_shapes_by_viewport(root, abstract_id, scene, view_layers).empty());
}

TEST_F(PipelineFixture, TinyShapesByLayerVisibilityDropsHiddenViewLayerKeepsVisible)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(200, 200);
    scene.set_layer_name_visible("M2", false);

    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}});
    add_terminal_shape(Shape{.layer_name = "M2", .rects = {Rect{.ll = {50, 50}, .ur = {50, 50}}}});

    const auto &tiny_shapes = pipeline.tiny_shapes_by_viewport(root, abstract_id, scene, view_layers);
    ASSERT_EQ(tiny_shapes.size(), 2u);

    const auto &result = pipeline.tiny_shapes_by_layer_visibility(root, tiny_shapes, scene, view_layers);
    ASSERT_EQ(result.size(), 1u); // only the M1/TERMINAL group survives
    const auto m1_terminal = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    ASSERT_TRUE(result.contains(m1_terminal));
    ASSERT_EQ(result.at(m1_terminal).size(), 1u);
    EXPECT_EQ(result.at(m1_terminal).front().x, 5);
    EXPECT_EQ(result.at(m1_terminal).front().y, 5);
}

TEST_F(PipelineFixture, TinyShapesByViewportReusesCacheUntilViewportVersionChanges)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(200, 200);
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}});

    pipeline.tiny_shapes_by_viewport(root, abstract_id, scene, view_layers);
    pipeline.tiny_shapes_by_viewport(root, abstract_id, scene, view_layers);
    EXPECT_EQ(pipeline.tiny_shapes_viewport_filter_calls(), 1u);

    scene.set_pan(Point{1, 1});
    pipeline.tiny_shapes_by_viewport(root, abstract_id, scene, view_layers);
    EXPECT_EQ(pipeline.tiny_shapes_viewport_filter_calls(), 2u);
}

TEST_F(PipelineFixture, RunChainsAllThreeStagesForCurrentAbstract)
{
    // Kept: on M1 (visible), inside the viewport, well above the sub-pixel threshold.
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    // Dropped by the viewport filter: on M1, but far outside it.
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {5000, 5000}, .ur = {5010, 5010}}}});
    // Dropped by the layer filter: M2 obstructions are hidden below.
    add_obstruction_shape(Shape{.layer_name = "M2", .rects = {Rect{.ll = {1, 1}, .ur = {5, 5}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_layer_name_visible("M2", false);

    const auto &result = pipeline.run(root, scene, view_layers);
    ASSERT_EQ(result.size(), 1u); // only the M1/TERMINAL group survives
    const auto m1_terminal = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    ASSERT_TRUE(result.contains(m1_terminal));
    const auto &group = result.at(m1_terminal);
    ASSERT_EQ(group.size(), 1u);
    EXPECT_EQ(group.front().shape.layer_name, "M1");
    EXPECT_EQ(group.front().shape.rects.front().ur.x, 10);
}

TEST_F(PipelineFixture, RunOnUnchangedSceneHitsCacheForEveryStage)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_obstruction_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {20, 20}, .ur = {30, 30}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    pipeline.run(root, scene, view_layers);
    pipeline.run(root, scene, view_layers);

    EXPECT_EQ(pipeline.generate_calls(), 1u);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 1u);
    EXPECT_EQ(pipeline.layer_filter_calls(), 1u);
}

TEST_F(PipelineFixture, RunOnViewportOnlyChangeRecomputesViewportAndLayerFilterButNotGenerate)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    pipeline.run(root, scene, view_layers);
    scene.set_pan(Point{1, 1});
    pipeline.run(root, scene, view_layers);

    EXPECT_EQ(pipeline.generate_calls(), 1u);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 2u);
    EXPECT_EQ(pipeline.layer_filter_calls(), 2u);
}

TEST_F(PipelineFixture, RunOnVisibilityOnlyChangeRecomputesOnlyTheLayerFilterStage)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    pipeline.run(root, scene, view_layers);
    scene.set_layer_name_visible("M2", false);
    pipeline.run(root, scene, view_layers);

    EXPECT_EQ(pipeline.generate_calls(), 1u);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 1u);
    EXPECT_EQ(pipeline.layer_filter_calls(), 2u);
}

TEST_F(PipelineFixture, RunOnAbstractChangeRecomputesAllThreeStages)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    AbstractId other_abstract_id = root.create_abstract(AbstractData{});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    pipeline.run(root, scene, view_layers);
    scene.set_current_abstract(other_abstract_id);
    pipeline.run(root, scene, view_layers);

    EXPECT_EQ(pipeline.generate_calls(), 2u);
    EXPECT_EQ(pipeline.viewport_filter_calls(), 2u);
    EXPECT_EQ(pipeline.layer_filter_calls(), 2u);
}

TEST_F(PipelineFixture, HitTestPointReturnsTopmostLayerFirst)
{
    // Overlapping shapes at the same point on different layers - M1 was
    // declared before M2 in SetUp(), so M2's ViewLayer sorts higher (see
    // FilterByLayerVisibilityGroupsInBottomUpLayerOrder) - topmost.
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    const TerminalId m2_terminal = add_terminal_shape(Shape{.layer_name = "M2", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto hit = Pipeline::hit_test_point(shapes, view_layers, scene, Point{5, 5});

    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(std::holds_alternative<TerminalId>(hit->origin));
    EXPECT_EQ(std::get<TerminalId>(hit->origin), m2_terminal);
}

TEST_F(PipelineFixture, HitTestPointReturnsACopyOfTheHitShapesOwnGeometry)
{
    const TerminalId terminal_id = add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto hit = Pipeline::hit_test_point(shapes, view_layers, scene, Point{5, 5});

    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(std::holds_alternative<TerminalId>(hit->origin));
    EXPECT_EQ(std::get<TerminalId>(hit->origin), terminal_id);
    ASSERT_EQ(hit->outline.rects.size(), 1u);
    EXPECT_EQ(hit->outline.rects.front().ur.x, 10);
}

TEST_F(PipelineFixture, HitTestPointHighlightsOnlyTheHitPieceNotEveryRectOnTheSameTerminal)
{
    // Regression: a single TerminalPort Shape can bundle several rects
    // together (e.g. several RECT statements in one LEF PORT) - a real
    // reported bug had hovering one rect highlight every rect on the same
    // Terminal, because hit_test_point copied the whole RenderedShape
    // instead of just the piece under the cursor.
    const TerminalId terminal_id = add_terminal_shape(Shape{
        .layer_name = "M1",
        .rects = {
            Rect{.ll = {0, 0}, .ur = {10, 10}},
            Rect{.ll = {100, 100}, .ur = {110, 110}},
        },
    });

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto hit = Pipeline::hit_test_point(shapes, view_layers, scene, Point{5, 5});

    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(std::holds_alternative<TerminalId>(hit->origin));
    EXPECT_EQ(std::get<TerminalId>(hit->origin), terminal_id);
    ASSERT_EQ(hit->outline.rects.size(), 1u); // only the hit piece, not both
    EXPECT_TRUE(hit->outline.polygons.empty());

    // Confirm it's the piece near (0,0), not the other one near (100,100).
    const auto bbox = Geometry::bbox(hit->outline);
    ASSERT_TRUE(bbox.has_value());
    EXPECT_LT(bbox->ur.x, 50);
}

TEST_F(PipelineFixture, HitTestPointSkipsAnUnselectableLayer)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);
    scene.set_layer_name_selectable("M1", false);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    EXPECT_FALSE(Pipeline::hit_test_point(shapes, view_layers, scene, Point{5, 5}).has_value());
}

TEST_F(PipelineFixture, HitTestPointReturnsNulloptOnAMiss)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    EXPECT_FALSE(Pipeline::hit_test_point(shapes, view_layers, scene, Point{500, 500}).has_value());
}

TEST_F(PipelineFixture, HitTestPointNeverHitsTheBoundaryShape)
{
    root.get_abstract(abstract_id)->boundary = {Polygon{.points = {{0, 0}, {0, 1000}, {1000, 1000}, {1000, 0}, {0, 0}}}};

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    EXPECT_FALSE(Pipeline::hit_test_point(shapes, view_layers, scene, Point{500, 500}).has_value());
}

TEST_F(PipelineFixture, HitTestRectFindsShapesFullyEnclosedAcrossAllLayers)
{
    const TerminalId inside_m1 = add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}});
    const ObstructionId inside_m2 = add_obstruction_shape(Shape{.layer_name = "M2", .rects = {Rect{.ll = {30, 30}, .ur = {40, 40}}}});
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {45, 45}, .ur = {60, 60}}}}); // straddles the rect's edge

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto hits = Pipeline::hit_test_rect(shapes, view_layers, scene, Rect{.ll = {0, 0}, .ur = {50, 50}});

    auto has = [&](SelectionRef ref)
    {
        for (const auto &hit : hits)
            if (hit.origin == ref)
                return true;
        return false;
    };

    ASSERT_EQ(hits.size(), 2u);
    EXPECT_TRUE(has(SelectionRef{inside_m1}));
    EXPECT_TRUE(has(SelectionRef{inside_m2}));
}

TEST_F(PipelineFixture, HitTestRectReturnsOneHoverTargetPerEnclosedPieceOfTheSameOrigin)
{
    // Regression: a single Obstruction can bundle several disjoint Shape
    // entries together in its own .shapes list (e.g. every RECT
    // statement in one OBS block gets its own Shape - see
    // stress_data.hpp's "fresh LAYER before every geometry item" trick,
    // which is exactly how a real stress-test LEF produces one
    // Obstruction with ~900,000 single-piece Shapes) - dragging a
    // rectangle around two of them must report two independently-
    // selectable pieces sharing the same origin, not collapse to "the
    // object is selected" (which would then highlight every piece
    // belonging to it, however many there are - the actual reported
    // bug). Constructed directly (not via add_obstruction_shape, which
    // only ever creates one Shape per Obstruction) so each rect lands in
    // its own Shape/RenderedShape, matching that real structure.
    const ObstructionId obstruction_id = root.create_obstruction(ObstructionData{.abstract = abstract_id});
    root.create_shape(ShapeData{.obstruction = obstruction_id, .layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}});
    root.create_shape(ShapeData{.obstruction = obstruction_id, .layer_name = "M1", .rects = {Rect{.ll = {30, 30}, .ur = {40, 40}}}});
    root.create_shape(ShapeData{.obstruction = obstruction_id, .layer_name = "M1", .rects = {Rect{.ll = {100, 100}, .ur = {110, 110}}}}); // outside the drag rect

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    const auto hits = Pipeline::hit_test_rect(shapes, view_layers, scene, Rect{.ll = {0, 0}, .ur = {50, 50}});

    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].origin, SelectionRef{obstruction_id});
    EXPECT_EQ(hits[1].origin, SelectionRef{obstruction_id});
    ASSERT_EQ(hits[0].outline.rects.size(), 1u);
    ASSERT_EQ(hits[1].outline.rects.size(), 1u);
    EXPECT_NE(hits[0].outline.rects.front().ll.x, hits[1].outline.rects.front().ll.x); // genuinely different pieces
}

TEST_F(PipelineFixture, HitTestRectSkipsAnUnselectableLayer)
{
    add_terminal_shape(Shape{.layer_name = "M1", .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);
    scene.set_layer_name_selectable("M1", false);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    EXPECT_TRUE(Pipeline::hit_test_rect(shapes, view_layers, scene, Rect{.ll = {0, 0}, .ur = {50, 50}}).empty());
}

TEST_F(PipelineFixture, HitTestRectNeverReturnsTheBoundaryShape)
{
    root.get_abstract(abstract_id)->boundary = {Polygon{.points = {{0, 0}, {0, 1000}, {1000, 1000}, {1000, 0}, {0, 0}}}};

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    EXPECT_TRUE(Pipeline::hit_test_rect(shapes, view_layers, scene, Rect{.ll = {0, 0}, .ur = {1000, 1000}}).empty());
}
