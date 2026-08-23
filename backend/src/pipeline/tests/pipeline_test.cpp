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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_obstruction_shape(Shape{.layer = m1, .rects = {Rect{.ll = {20, 20}, .ur = {30, 30}}}});

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
    add_terminal_shape(Shape{.layer = m1, .paths = {terminal_path}});

    const Path obstruction_path{.polygon = Polygon{.points = {{20, 20}, {20, 40}}}, .width = 6};
    add_obstruction_shape(Shape{.layer = m1, .paths = {obstruction_path}});

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

TEST_F(PipelineFixture, GenerateShapesResolvesAViaReferencedByATerminalOntoItsOwnLayerWithTerminalPurpose)
{
    // Mirrors LayoutPipelineFixture's own identical Route-side test - the
    // via's own ViaLayer geometry lives on M2, distinct from the
    // Terminal's own real M1 geometry.
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{50, 50}});
    add_terminal_shape(shape);

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);

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

TEST_F(PipelineFixture, GenerateShapesResolvesAViaReferencedByAnObstructionOntoItsOwnLayerWithObstructionPurpose)
{
    // A genuinely separate call site from the Terminal-loop test above
    // (GenerateAbstractShapesStage's Terminal and Obstruction loops are
    // two independent append_via_shapes calls, not one shared lambda the
    // way GenerateLayoutShapesStage's push_shape_id is) - must resolve
    // with OBSTRUCTION purpose, not TERMINAL.
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{50, 50}});
    add_obstruction_shape(shape);

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);

    const ViewLayerId expected = view_layers.find(m2, ViewLayerPurpose::OBSTRUCTION);
    const RenderedShape *via_shape = nullptr;
    for (const RenderedShape &rs : shapes)
        if (rs.view_layer == expected)
            via_shape = &rs;
    ASSERT_NE(via_shape, nullptr);
    ASSERT_EQ(via_shape->shape.rects.size(), 1u);
    EXPECT_EQ(via_shape->shape.rects[0].ll.x, 45);
    EXPECT_EQ(via_shape->shape.rects[0].ll.y, 45);
    EXPECT_FALSE(via_shape->shape_id.has_value());

    // Not also resolved under TERMINAL - purpose actually came from this
    // call site's own OBSTRUCTION argument, not a stray default.
    const ViewLayerId terminal_m2 = view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    for (const RenderedShape &rs : shapes)
        EXPECT_NE(rs.view_layer, terminal_m2);
}

TEST_F(PipelineFixture, GenerateShapesForUnknownAbstractIsEmpty)
{
    AbstractId unknown{999, 0};
    EXPECT_TRUE(pipeline.generate_shapes(root, unknown, view_layers).empty());
}

TEST_F(PipelineFixture, GenerateShapesResolvesViewLayerByOrigin)
{
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}});
    add_obstruction_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);
    EXPECT_EQ(shapes[0].view_layer, view_layers.find(m1, ViewLayerPurpose::TERMINAL));
    EXPECT_EQ(shapes[1].view_layer, view_layers.find(m1, ViewLayerPurpose::OBSTRUCTION));
    EXPECT_NE(shapes[0].view_layer, shapes[1].view_layer);
}

TEST_F(PipelineFixture, GenerateShapesIncludesAbstractBoundaryResolvedToBoundaryViewLayer)
{
    root.create_shape(ShapeData{.abstract = abstract_id, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {{0, 0}, {0, 100}, {100, 100}, {100, 0}, {0, 0}}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    ASSERT_TRUE(shapes.front().shape.purpose.has_value());
    EXPECT_EQ(*shapes.front().shape.purpose, ShapePurpose::BOUNDARY);
    EXPECT_EQ(shapes.front().view_layer, view_layers.boundary_view_layer());
}

TEST_F(PipelineFixture, GenerateShapesLeavesViewLayerInvalidForUnresolvableLayer)
{
    // A Shape with an invalid/unresolved layer can't be constructed via a
    // real reader (LEFReader/DEFReader both error and skip rather than
    // create one - see Shape.layer's own schema.py comment), but this
    // pipeline behavior (kept, not dropped - no visibility toggle exists
    // for an invalid ViewLayerId) is still worth covering directly against
    // a hand-built Shape.
    add_obstruction_shape(Shape{.layer = LayerId{}, .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}});

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
        .layer = m1,
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
        .layer = m1,
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
        .layer = m1,
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
        .layer = m1,
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
        .layer = m1,
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
        .layer = m1,
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
    add_port_shape(terminal_id, Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_port_shape(terminal_id, Shape{.layer = m1, .rects = {Rect{.ll = {5, 5}, .ur = {15, 15}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);
    EXPECT_EQ(shapes[0].shape.rects.size(), 1u);
    EXPECT_EQ(shapes[1].shape.rects.size(), 1u);
}

TEST_F(PipelineFixture, GenerateShapesAddsTerminalLabelAtComputedLocation)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "A1"});
    add_port_shape(terminal_id, Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

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
    add_port_shape(terminal_id, Shape{.layer = m1, .paths = {Path{.polygon = Polygon{.points = {{0, 0}, {100, 0}}}, .width = 6}}});

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
    add_port_shape(terminal_id, Shape{.layer = m1, .polygons = {Polygon{.points = {
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
    add_port_shape(terminal_id, Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_port_shape(terminal_id, Shape{.layer = m1, .rects = {Rect{.ll = {20, 0}, .ur = {30, 10}}}});

    const auto &shapes = pipeline.generate_shapes(root, abstract_id, view_layers);
    ASSERT_EQ(shapes.size(), 2u);
    ASSERT_EQ(shapes[0].shape.texts.size(), 1u);
    EXPECT_EQ(shapes[0].shape.texts.front().label, "B2");
    EXPECT_TRUE(shapes[1].shape.texts.empty());
}

TEST_F(PipelineFixture, GenerateShapesAddsOneLabelPerDistinctLayer)
{
    TerminalId terminal_id = root.create_terminal(TerminalData{.abstract = abstract_id, .name = "C3"});
    add_port_shape(terminal_id, Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_port_shape(terminal_id, Shape{.layer = m2, .rects = {Rect{.ll = {20, 0}, .ur = {30, 10}}}});

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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
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
        RenderedShape{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}},
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
        RenderedShape{.shape = Shape{.layer = m1, .texts = {Text{.label = "A1", .location = {5, 5}}}}, .view_layer = {}},
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
        RenderedShape{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {1000, 1000}, .ur = {1010, 1010}}}}, .view_layer = {}},
    };
    EXPECT_TRUE(pipeline.filter_by_viewport_and_size(root, shapes, scene, view_layers).empty());
}

TEST_F(PipelineFixture, FilterByViewportAndSizeDropsSubPixelDotsButKeepsThinLongShapes)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0); // 1 dbu == 1 px, so the sub-pixel threshold is 1 dbu
    scene.set_viewport_size(200, 200);

    RenderedShape dot{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}}, .view_layer = {}};             // 0x0
    RenderedShape thin_long_line{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {5, 5}, .ur = {5, 105}}}}, .view_layer = {}}; // 0 wide, 100 tall

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
        RenderedShape{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}}, .view_layer = {}},
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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

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
    add_obstruction_shape(Shape{.layer = m1, .rects = {Rect{.ll = {50, 50}, .ur = {60, 60}}}});
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
        RenderedShape{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = m1_obstruction},
        RenderedShape{.shape = Shape{.layer = m2, .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = m2_obstruction},
    };

    const auto &result = pipeline.filter_by_layer_visibility(root, shapes, scene, view_layers);
    ASSERT_EQ(result.size(), 1u); // only the M1 group survives - the whole M2 group is dropped
    ASSERT_TRUE(result.contains(m1_obstruction));
    EXPECT_EQ(result.at(m1_obstruction).front().shape.layer, m1);
}

TEST_F(PipelineFixture, FilterByLayerVisibilityKeepsShapesWithInvalidViewLayer)
{
    Scene scene; // no layers explicitly hidden
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer = LayerId{}, .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = {}},
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
    ViewLayerId boundary_view_layer = view_layers.boundary_view_layer();

    Scene scene;
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.purpose = ShapePurpose::BOUNDARY}, .view_layer = boundary_view_layer},
        RenderedShape{.shape = Shape{.layer = m2}, .view_layer = m2_terminal},
        RenderedShape{.shape = Shape{.layer = m1}, .view_layer = m1_terminal},
    };

    const auto &result = pipeline.filter_by_layer_visibility(root, shapes, scene, view_layers);
    ASSERT_EQ(result.size(), 3u);

    std::vector<ViewLayerId> order;
    for (const auto &[view_layer, group] : result)
        order.push_back(view_layer);

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], m1_terminal);
    EXPECT_EQ(order[1], m2_terminal);
    EXPECT_EQ(order[2], boundary_view_layer);
}

TEST_F(PipelineFixture, FilterByLayerVisibilityReusesCacheUntilVisibilityVersionChanges)
{
    Scene scene;
    std::vector<RenderedShape> shapes = {
        RenderedShape{.shape = Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}}}, .view_layer = {}},
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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}});
    // Normal-sized shape - filter_by_viewport_and_size keeps this, so tiny_shapes_by_viewport must not.
    add_obstruction_shape(Shape{.layer = m1, .rects = {Rect{.ll = {50, 50}, .ur = {60, 60}}}});

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

    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {5, 5}, .ur = {5, 105}}}}); // 0 wide, 100 tall

    EXPECT_TRUE(pipeline.tiny_shapes_by_viewport(root, abstract_id, scene, view_layers).empty());
}

TEST_F(PipelineFixture, TinyShapesByViewportExcludesATinyShapeOutsideTheViewport)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {5000, 5000}, .ur = {5000, 5000}}}});

    EXPECT_TRUE(pipeline.tiny_shapes_by_viewport(root, abstract_id, scene, view_layers).empty());
}

TEST_F(PipelineFixture, TinyShapesByLayerVisibilityDropsHiddenViewLayerKeepsVisible)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(200, 200);
    scene.set_layer_name_visible("M2", false);

    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}});
    add_terminal_shape(Shape{.layer = m2, .rects = {Rect{.ll = {50, 50}, .ur = {50, 50}}}});

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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {5, 5}, .ur = {5, 5}}}});

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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    // Dropped by the viewport filter: on M1, but far outside it.
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {5000, 5000}, .ur = {5010, 5010}}}});
    // Dropped by the layer filter: M2 obstructions are hidden below.
    add_obstruction_shape(Shape{.layer = m2, .rects = {Rect{.ll = {1, 1}, .ur = {5, 5}}}});

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
    EXPECT_EQ(group.front().shape.layer, m1);
    EXPECT_EQ(group.front().shape.rects.front().ur.x, 10);
}

TEST_F(PipelineFixture, RunOnUnchangedSceneHitsCacheForEveryStage)
{
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    add_obstruction_shape(Shape{.layer = m1, .rects = {Rect{.ll = {20, 20}, .ur = {30, 30}}}});

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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});
    const TerminalId m2_terminal = add_terminal_shape(Shape{.layer = m2, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

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
    const TerminalId terminal_id = add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

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
        .layer = m1,
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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);
    scene.set_layer_name_selectable("M1", false);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    EXPECT_FALSE(Pipeline::hit_test_point(shapes, view_layers, scene, Point{5, 5}).has_value());
}

TEST_F(PipelineFixture, HitTestPointReturnsNulloptOnAMiss)
{
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    EXPECT_FALSE(Pipeline::hit_test_point(shapes, view_layers, scene, Point{500, 500}).has_value());
}

TEST_F(PipelineFixture, HitTestPointNeverHitsTheBoundaryShape)
{
    root.create_shape(ShapeData{.abstract = abstract_id, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {{0, 0}, {0, 1000}, {1000, 1000}, {1000, 0}, {0, 0}}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    EXPECT_FALSE(Pipeline::hit_test_point(shapes, view_layers, scene, Point{500, 500}).has_value());
}

TEST_F(PipelineFixture, HitTestRectFindsShapesFullyEnclosedAcrossAllLayers)
{
    const TerminalId inside_m1 = add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}});
    const ObstructionId inside_m2 = add_obstruction_shape(Shape{.layer = m2, .rects = {Rect{.ll = {30, 30}, .ur = {40, 40}}}});
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {45, 45}, .ur = {60, 60}}}}); // straddles the rect's edge

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
    root.create_shape(ShapeData{.obstruction = obstruction_id, .layer = m1, .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}});
    root.create_shape(ShapeData{.obstruction = obstruction_id, .layer = m1, .rects = {Rect{.ll = {30, 30}, .ur = {40, 40}}}});
    root.create_shape(ShapeData{.obstruction = obstruction_id, .layer = m1, .rects = {Rect{.ll = {100, 100}, .ur = {110, 110}}}}); // outside the drag rect

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
    add_terminal_shape(Shape{.layer = m1, .rects = {Rect{.ll = {10, 10}, .ur = {20, 20}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);
    scene.set_layer_name_selectable("M1", false);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    EXPECT_TRUE(Pipeline::hit_test_rect(shapes, view_layers, scene, Rect{.ll = {0, 0}, .ur = {50, 50}}).empty());
}

TEST_F(PipelineFixture, HitTestRectNeverReturnsTheBoundaryShape)
{
    root.create_shape(ShapeData{.abstract = abstract_id, .purpose = ShapePurpose::BOUNDARY, .polygons = {Polygon{.points = {{0, 0}, {0, 1000}, {1000, 1000}, {1000, 0}, {0, 0}}}}});

    Scene scene;
    scene.set_current_abstract(abstract_id);
    scene.set_viewport_size(1000, 1000);

    const auto &shapes = pipeline.run(root, scene, view_layers);
    EXPECT_TRUE(Pipeline::hit_test_rect(shapes, view_layers, scene, Rect{.ll = {0, 0}, .ur = {1000, 1000}}).empty());
}

namespace
{
    // Extends PipelineFixture with a Layout (owned by a minimal
    // Library/Design) - the common scaffolding every
    // GenerateLayoutShapesStage test attaches to, mirroring
    // PipelineFixture's own role for the Abstract path.
    struct LayoutPipelineFixture : public PipelineFixture
    {
        void SetUp() override
        {
            PipelineFixture::SetUp();
            LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
            DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "TOP"});
            layout_id = root.create_layout(LayoutData{.design = design_id});
        }

        void add_diearea(const Rect &rect)
        {
            Shape shape;
            shape.layout = layout_id;
            shape.purpose = ShapePurpose::BOUNDARY;
            shape.rects.push_back(rect);
            root.create_shape(std::move(shape));
        }

        BlockageId add_routing_blockage(LayerId layer, const Rect &rect)
        {
            BlockageId blockage_id = root.create_blockage(BlockageData{.layout = layout_id, .kind = BlockageKind::ROUTING});
            Shape shape;
            shape.blockage = blockage_id;
            shape.layer = layer;
            shape.rects.push_back(rect);
            root.create_shape(std::move(shape));
            return blockage_id;
        }

        BlockageId add_placement_blockage(const Rect &rect)
        {
            BlockageId blockage_id = root.create_blockage(BlockageData{.layout = layout_id, .kind = BlockageKind::PLACEMENT});
            Shape shape;
            shape.blockage = blockage_id;
            shape.purpose = ShapePurpose::PLACEMENT_BLOCKAGE;
            shape.rects.push_back(rect);
            root.create_shape(std::move(shape));
            return blockage_id;
        }

        RouteId add_route(LayerId layer, const Rect &rect)
        {
            RouteId route_id = root.create_route(RouteData{.layout = layout_id, .name = "NET1"});
            Shape shape;
            shape.route = route_id;
            shape.layer = layer;
            shape.rects.push_back(rect);
            root.create_shape(std::move(shape));
            return route_id;
        }

        PhysicalPortId add_physical_port(LayerId layer, const Rect &rect)
        {
            PhysicalPortId port_id = root.create_physical_port(PhysicalPortData{.layout = layout_id, .name = "P1"});
            PhysicalPortSegmentId segment_id = root.create_physical_port_segment(PhysicalPortSegmentData{.physical_port = port_id});
            Shape shape;
            shape.physical_port_segment = segment_id;
            shape.layer = layer;
            shape.rects.push_back(rect);
            root.create_shape(std::move(shape));
            return port_id;
        }

        SiteId add_site(const std::string &name, Point size)
        {
            return root.create_site(SiteData{.technology = technology_id, .name = name, .size = size});
        }

        RowId add_row(const std::string &site_name, Point origin, std::optional<int> num_x = std::nullopt, std::optional<int> num_y = std::nullopt, std::optional<int64_t> step_x = std::nullopt, std::optional<int64_t> step_y = std::nullopt)
        {
            return root.create_row(RowData{
                .layout = layout_id,
                .name = "ROW" + std::to_string(next_row_index++),
                .site_name = site_name,
                .origin = origin,
                .orientation = Orientation::N,
                .num_x = num_x,
                .num_y = num_y,
                .step_x = step_x,
                .step_y = step_y,
            });
        }

        TrackId add_track(bool is_x, int64_t start, int count, int64_t step, std::vector<std::string> layer_names)
        {
            return root.create_track(TrackData{.layout = layout_id, .is_x = is_x, .start = start, .count = count, .step = step, .layer_names = std::move(layer_names)});
        }

        GCellGridId add_gcell_grid(bool is_x, int64_t start, int count, int64_t step)
        {
            return root.create_g_cell_grid(GCellGridData{.layout = layout_id, .is_x = is_x, .start = start, .count = count, .step = step});
        }

        RegionId add_region(const Rect &rect)
        {
            return root.create_region(RegionData{.layout = layout_id, .name = "REGION" + std::to_string(next_region_index++), .rects = {rect}});
        }

        LayoutId layout_id;
        int next_row_index = 0;
        int next_region_index = 0;
    };

    // The one RenderedShape (if any) among `shapes` whose ViewLayerId
    // resolves to `purpose` under `view_layers` - the tests below only
    // ever expect at most one match per purpose, so an assert-friendly
    // single-shape lookup is more useful than filtering a subrange.
    const RenderedShape *find_by_purpose(const std::vector<RenderedShape> &shapes, const ViewLayerSet &view_layers, ViewLayerPurpose purpose)
    {
        for (const RenderedShape &shape : shapes)
        {
            const ViewLayerData *view_layer = view_layers.get(shape.view_layer);
            if (view_layer && view_layer->purpose == purpose)
                return &shape;
        }
        return nullptr;
    }
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesIncludesDieAreaResolvedToBoundaryViewLayer)
{
    add_diearea(Rect{.ll = {0, 0}, .ur = {1000, 2000}});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u);
    EXPECT_EQ(shapes[0].view_layer, view_layers.boundary_view_layer());
    ASSERT_EQ(shapes[0].shape.rects.size(), 1u);
    EXPECT_EQ(shapes[0].shape.rects[0].ll.x, 0);
    EXPECT_EQ(shapes[0].shape.rects[0].ll.y, 0);
    EXPECT_EQ(shapes[0].shape.rects[0].ur.x, 1000);
    EXPECT_EQ(shapes[0].shape.rects[0].ur.y, 2000);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesForUnknownLayoutIsEmpty)
{
    const auto &shapes = pipeline.generate_layout_shapes(root, LayoutId{}, view_layers);
    EXPECT_TRUE(shapes.empty());
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesResolvesRoutingBlockageToRoutingBlockageColumnOfItsLayer)
{
    add_routing_blockage(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = find_by_purpose(shapes, view_layers, ViewLayerPurpose::ROUTING_BLOCKAGE);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->view_layer, view_layers.find(m1, ViewLayerPurpose::ROUTING_BLOCKAGE));
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesResolvesPlacementBlockageToPlacementBlockagePseudoRow)
{
    add_placement_blockage(Rect{.ll = {0, 0}, .ur = {10, 10}});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = find_by_purpose(shapes, view_layers, ViewLayerPurpose::PLACEMENT_BLOCKAGE);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->view_layer, view_layers.find(LayerId{}, ViewLayerPurpose::PLACEMENT_BLOCKAGE));
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesResolvesRouteToRouteColumnOfItsLayer)
{
    add_route(m2, Rect{.ll = {5, 5}, .ur = {15, 15}});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = find_by_purpose(shapes, view_layers, ViewLayerPurpose::ROUTE);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->view_layer, view_layers.find(m2, ViewLayerPurpose::ROUTE));
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesResolvesAViaReferencedByARouteOntoItsOwnLayerWithRoutePurpose)
{
    // The via's own ViaLayer geometry lives on M2 - a different layer
    // from the route's own real M1 geometry - so the resolved via shape
    // is unambiguously identifiable by its own ViewLayerId (M2/ROUTE),
    // distinct from the route's own M1/ROUTE shape.
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    const RouteId route_id = root.create_route(RouteData{.layout = layout_id, .name = "NET1"});
    Shape shape;
    shape.route = route_id;
    shape.layer = m1;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{50, 50}});
    root.create_shape(std::move(shape));

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);

    const ViewLayerId expected = view_layers.find(m2, ViewLayerPurpose::ROUTE);
    const RenderedShape *via_shape = nullptr;
    for (const RenderedShape &rs : shapes)
        if (rs.view_layer == expected)
            via_shape = &rs;
    ASSERT_NE(via_shape, nullptr);
    ASSERT_EQ(via_shape->shape.rects.size(), 1u);
    EXPECT_EQ(via_shape->shape.rects[0].ll.x, 45);
    EXPECT_EQ(via_shape->shape.rects[0].ll.y, 45);
    EXPECT_EQ(via_shape->shape.rects[0].ur.x, 55);
    EXPECT_EQ(via_shape->shape.rects[0].ur.y, 55);
    EXPECT_FALSE(via_shape->shape_id.has_value()); // synthesized, not independently selectable

    // The route's own real M1 geometry is unaffected/still present.
    const RenderedShape *route_shape = find_by_purpose(shapes, view_layers, ViewLayerPurpose::ROUTE);
    ASSERT_NE(route_shape, nullptr);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesPrefersALayoutViaOverASameNamedLibraryVia)
{
    // A DEF VIAS (LayoutVia) definition local to this Layout takes
    // precedence over a same-named LEF VIA (Via) from the library - the
    // resolved geometry lands on M3 (the LayoutVia's own layer), not M2
    // (the Via's).
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    const LayoutViaId layout_via_id = root.create_layout_via(LayoutViaData{.layout = layout_id, .name = "VIA12"});
    const LayerId m3 = root.create_layer(LayerData{.technology = technology_id, .name = "M3", .type = "ROUTING"});
    root.create_via_layer(ViaLayerData{.layout_via = layout_via_id, .layer_name = "M3", .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}});

    const RouteId route_id = root.create_route(RouteData{.layout = layout_id, .name = "NET1"});
    Shape shape;
    shape.route = route_id;
    shape.layer = m1;
    shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{0, 0}});
    root.create_shape(std::move(shape));

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);

    bool found_on_m3 = false, found_on_m2 = false;
    for (const RenderedShape &rs : shapes)
    {
        if (rs.view_layer == view_layers.find(m3, ViewLayerPurpose::ROUTE))
            found_on_m3 = true;
        if (rs.view_layer == view_layers.find(m2, ViewLayerPurpose::ROUTE))
            found_on_m2 = true;
    }
    EXPECT_TRUE(found_on_m3);
    EXPECT_FALSE(found_on_m2);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesTransformsAnOrientedViaByGeometrysOwnOrientationMath)
{
    // An asymmetric via rect under Orientation::E swaps axes (matches
    // Geometry::orientation_linear's own E case: {0,1,-1,0}) - confirms
    // real orientation math is applied, not always the identity.
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {0, 0}, .ur = {20, 10}}}});

    const RouteId route_id = root.create_route(RouteData{.layout = layout_id, .name = "NET1"});
    Shape shape;
    shape.route = route_id;
    shape.layer = m1;
    shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{100, 100}, .orientation = Orientation::E});
    root.create_shape(std::move(shape));

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const ViewLayerId expected = view_layers.find(m2, ViewLayerPurpose::ROUTE);
    const RenderedShape *via_shape = nullptr;
    for (const RenderedShape &rs : shapes)
        if (rs.view_layer == expected)
            via_shape = &rs;
    ASSERT_NE(via_shape, nullptr);
    ASSERT_EQ(via_shape->shape.rects.size(), 1u);

    // Geometry::orientation_linear(E) = {0,1,-1,0}: (0,0)->(0,0),
    // (20,0)->(0,-20), (0,10)->(10,0), (20,10)->(10,-20) - transformed
    // bbox is (0,-20)-(10,0), then translated by origin (100,100).
    EXPECT_EQ(via_shape->shape.rects[0].ll.x, 100);
    EXPECT_EQ(via_shape->shape.rects[0].ll.y, 80);
    EXPECT_EQ(via_shape->shape.rects[0].ur.x, 110);
    EXPECT_EQ(via_shape->shape.rects[0].ur.y, 100);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesExpandsAViaIterateIntoOneResolvedShapePerTile)
{
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    const RouteId route_id = root.create_route(RouteData{.layout = layout_id, .name = "NET1"});
    Shape shape;
    shape.route = route_id;
    shape.layer = m1;
    shape.via_iterates.push_back(ShapeViaIterate{.via_name = "VIA12", .origin = Point{0, 0}, .num_x = 2, .num_y = 1, .space_x = 100, .space_y = 0});
    root.create_shape(std::move(shape));

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const ViewLayerId expected = view_layers.find(m2, ViewLayerPurpose::ROUTE);
    std::vector<int64_t> centers;
    for (const RenderedShape &rs : shapes)
        if (rs.view_layer == expected)
        {
            ASSERT_EQ(rs.shape.rects.size(), 1u);
            centers.push_back((rs.shape.rects[0].ll.x + rs.shape.rects[0].ur.x) / 2);
        }
    ASSERT_EQ(centers.size(), 2u);
    std::sort(centers.begin(), centers.end());
    EXPECT_EQ(centers[0], 0);
    EXPECT_EQ(centers[1], 100);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesSkipsAnUnresolvedViaNameWithoutAffectingTheReferencingShape)
{
    const RouteId route_id = root.create_route(RouteData{.layout = layout_id, .name = "NET1"});
    Shape shape;
    shape.route = route_id;
    shape.layer = m1;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.vias.push_back(ShapeVia{.via_name = "NO_SUCH_VIA", .origin = Point{0, 0}});
    root.create_shape(std::move(shape));

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *route_shape = find_by_purpose(shapes, view_layers, ViewLayerPurpose::ROUTE);
    ASSERT_NE(route_shape, nullptr);
    ASSERT_EQ(route_shape->shape.rects.size(), 1u); // the route's own real geometry, unaffected
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesSkipsAViaWithNoExplicitLayerGeometry)
{
    // A Via with no ViaLayer children at all (e.g. a LEF 5.6
    // VIARULE-inside-VIA reference, or a pure VIARULE-GENERATE via) has
    // no explicit geometry to draw - skipped, not an error.
    root.create_via(ViaData{.technology = technology_id, .name = "VIA_NO_GEOMETRY"});

    const RouteId route_id = root.create_route(RouteData{.layout = layout_id, .name = "NET1"});
    Shape shape;
    shape.route = route_id;
    shape.layer = m1;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.vias.push_back(ShapeVia{.via_name = "VIA_NO_GEOMETRY", .origin = Point{0, 0}});
    root.create_shape(std::move(shape));

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    ASSERT_EQ(shapes.size(), 1u); // just the route's own real shape, no extra via shapes
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesComputesPathOutlinesForARoutedNetPath)
{
    // Regression test for the push_shape_id code path specifically (a
    // separate RenderedShape-constructing call site from the Track one
    // above) - a routed net drawn as a PATH is the most common real-world
    // trigger of the path_outlines bug (e.g. NETS/SPECIALNETS in a real
    // routing DEF file). add_route only builds a rects-based Shape, so
    // this constructs a path-based Route Shape directly.
    const RouteId route_id = root.create_route(RouteData{.layout = layout_id, .name = "NET1"});
    const Path route_path{.polygon = Polygon{.points = {{0, 0}, {0, 100}}}, .width = 10};
    Shape shape;
    shape.route = route_id;
    shape.layer = m2;
    shape.paths = {route_path};
    root.create_shape(std::move(shape));

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = find_by_purpose(shapes, view_layers, ViewLayerPurpose::ROUTE);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.paths.size(), 1u);
    ASSERT_NE(found->path_outlines, nullptr);
    ASSERT_EQ(found->path_outlines->size(), 1u);
    const auto expected = Geometry::path_to_polygons(found->shape.paths.front());
    ASSERT_FALSE(expected.empty());
    ASSERT_EQ(found->path_outlines->front().size(), expected.size());
    const Polygon &actual_poly = found->path_outlines->front().front();
    const Polygon &expected_poly = expected.front();
    ASSERT_EQ(actual_poly.points.size(), expected_poly.points.size());
    for (size_t i = 0; i < actual_poly.points.size(); ++i)
    {
        EXPECT_EQ(actual_poly.points[i].x, expected_poly.points[i].x);
        EXPECT_EQ(actual_poly.points[i].y, expected_poly.points[i].y);
    }
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesResolvesPhysicalPortToTerminalColumnOfItsLayer)
{
    add_physical_port(m1, Rect{.ll = {0, 0}, .ur = {5, 5}});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = find_by_purpose(shapes, view_layers, ViewLayerPurpose::TERMINAL);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->view_layer, view_layers.find(m1, ViewLayerPurpose::TERMINAL));
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesSynthesizesRowRectangleFromSiteSizeAndTiling)
{
    add_site("SITE1", Point{500, 1000});
    add_row("SITE1", Point{100, 200}, /*num_x=*/3, /*num_y=*/2, /*step_x=*/600, /*step_y=*/std::nullopt);

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = find_by_purpose(shapes, view_layers, ViewLayerPurpose::ROW);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.rects.size(), 1u);
    // width = site.x + (num_x - 1) * step_x = 500 + 2 * 600 = 1700
    // height = site.y + (num_y - 1) * step_y(fallback site.y=1000) = 1000 + 1000 = 2000
    EXPECT_EQ(found->shape.rects[0].ll.x, 100);
    EXPECT_EQ(found->shape.rects[0].ll.y, 200);
    EXPECT_EQ(found->shape.rects[0].ur.x, 1800);
    EXPECT_EQ(found->shape.rects[0].ur.y, 2200);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesSkipsRowWithUnresolvableSite)
{
    add_row("NO_SUCH_SITE", Point{0, 0});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    EXPECT_EQ(find_by_purpose(shapes, view_layers, ViewLayerPurpose::ROW), nullptr);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesSkipsRowWithNoOrigin)
{
    add_site("SITE1", Point{500, 1000});
    // create_row directly (not the add_row helper) - leaves origin unset.
    root.create_row(RowData{.layout = layout_id, .name = "ROW_NO_ORIGIN", .site_name = "SITE1", .orientation = Orientation::N});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    EXPECT_EQ(find_by_purpose(shapes, view_layers, ViewLayerPurpose::ROW), nullptr);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesSkipsTrackAndGCellGridEntirelyWhenLayoutHasNoDieArea)
{
    // No add_diearea() call - layout_die_area_bbox has nothing to bound
    // the synthesized lines against, so both stay unsynthesized rather
    // than spanning some made-up default range.
    add_track(/*is_x=*/true, /*start=*/0, /*count=*/3, /*step=*/100, {"M1"});
    add_gcell_grid(/*is_x=*/false, /*start=*/500, /*count=*/2, /*step=*/300);

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    EXPECT_EQ(find_by_purpose(shapes, view_layers, ViewLayerPurpose::TRACK), nullptr);
    EXPECT_EQ(find_by_purpose(shapes, view_layers, ViewLayerPurpose::GCELLGRID), nullptr);
    EXPECT_TRUE(shapes.empty());
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesSynthesizesTrackLinesSpanningDieAreaInThePerpendicularDirection)
{
    add_diearea(Rect{.ll = {0, 0}, .ur = {1000, 2000}});
    add_track(/*is_x=*/true, /*start=*/0, /*count=*/3, /*step=*/100, {"M1"});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = nullptr;
    for (const RenderedShape &shape : shapes)
        if (shape.view_layer == view_layers.find(m1, ViewLayerPurpose::TRACK))
            found = &shape;
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.paths.size(), 3u);
    for (int i = 0; i < 3; i++)
    {
        const int64_t x = i * 100;
        EXPECT_EQ(found->shape.paths[i].polygon.points[0].x, x);
        EXPECT_EQ(found->shape.paths[i].polygon.points[0].y, 0);
        EXPECT_EQ(found->shape.paths[i].polygon.points[1].x, x);
        EXPECT_EQ(found->shape.paths[i].polygon.points[1].y, 2000);
    }
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesComputesPathOutlinesForSynthesizedTrackPaths)
{
    // Regression test: GenerateLayoutShapesStage used to leave
    // RenderedShape::path_outlines at its default-constructed empty
    // vector for every shape it built, while transform_to_pixels_stage's
    // transform_shapes_to_pixel_space indexes it by shape.paths' own
    // index with no bounds check - a real SIGSEGV on any real DEF file
    // with TRACKS (which always synthesize paths, unlike a Route/
    // Blockage/PhysicalPort Shape, which only sometimes does). Mirrors
    // GenerateShapesComputesPathOutlinesForTerminalAndObstructionPaths
    // above, for the Layout path instead of the Abstract path.
    add_diearea(Rect{.ll = {0, 0}, .ur = {1000, 2000}});
    add_track(/*is_x=*/true, /*start=*/0, /*count=*/3, /*step=*/100, {"M1"});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = find_by_purpose(shapes, view_layers, ViewLayerPurpose::TRACK);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.paths.size(), 3u);
    ASSERT_NE(found->path_outlines, nullptr);
    ASSERT_EQ(found->path_outlines->size(), 3u);
    // Tracks are zero-width lines by design (Path::width == 0), so
    // Geometry::path_to_polygons legitimately returns no polygon for
    // each one - the invariant this test exists to protect is the
    // CONTAINER-level size match above (what transform_shapes_to_pixel_space
    // actually indexes by), not that each entry's own polygon list is
    // non-empty.
    for (size_t i = 0; i < found->shape.paths.size(); i++)
    {
        const auto expected = Geometry::path_to_polygons(found->shape.paths[i]);
        EXPECT_EQ((*found->path_outlines)[i].size(), expected.size());
        EXPECT_TRUE(expected.empty());
    }
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesSkipsATrackLayerNameThatDoesNotResolveButKeepsResolvableOnes)
{
    add_diearea(Rect{.ll = {0, 0}, .ur = {1000, 2000}});
    add_track(/*is_x=*/true, /*start=*/0, /*count=*/1, /*step=*/100, {"M1", "NO_SUCH_LAYER"});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    int track_shape_count = 0;
    for (const RenderedShape &shape : shapes)
    {
        const ViewLayerData *view_layer = view_layers.get(shape.view_layer);
        if (view_layer && view_layer->purpose == ViewLayerPurpose::TRACK)
            track_shape_count++;
    }
    EXPECT_EQ(track_shape_count, 1); // only M1 resolved
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesSynthesizesGCellGridLinesSpanningDieArea)
{
    add_diearea(Rect{.ll = {0, 0}, .ur = {1000, 2000}});
    add_gcell_grid(/*is_x=*/false, /*start=*/500, /*count=*/2, /*step=*/300);

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = find_by_purpose(shapes, view_layers, ViewLayerPurpose::GCELLGRID);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.paths.size(), 2u);
    EXPECT_EQ(found->shape.paths[0].polygon.points[0].x, 0);
    EXPECT_EQ(found->shape.paths[0].polygon.points[0].y, 500);
    EXPECT_EQ(found->shape.paths[0].polygon.points[1].x, 1000);
    EXPECT_EQ(found->shape.paths[0].polygon.points[1].y, 500);
    EXPECT_EQ(found->shape.paths[1].polygon.points[0].x, 0);
    EXPECT_EQ(found->shape.paths[1].polygon.points[0].y, 800);
    EXPECT_EQ(found->shape.paths[1].polygon.points[1].x, 1000);
    EXPECT_EQ(found->shape.paths[1].polygon.points[1].y, 800);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesComputesPathOutlinesForSynthesizedGCellGridPaths)
{
    // Regression test for append_gcell_grid_shapes specifically - a
    // separate RenderedShape-constructing call site from the Track one
    // above, using its own local `path_outlines` variable computed
    // before the `std::move(lines)` that builds the RenderedShape.
    // GCellGrid lines are, like Track lines, an unconditional trigger of
    // the original SIGSEGV (always non-empty shape.paths).
    add_diearea(Rect{.ll = {0, 0}, .ur = {1000, 2000}});
    add_gcell_grid(/*is_x=*/false, /*start=*/500, /*count=*/2, /*step=*/300);

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = find_by_purpose(shapes, view_layers, ViewLayerPurpose::GCELLGRID);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.paths.size(), 2u);
    ASSERT_NE(found->path_outlines, nullptr);
    ASSERT_EQ(found->path_outlines->size(), 2u);
    // Zero-width, same reasoning as the Track regression test above.
    for (size_t i = 0; i < found->shape.paths.size(); i++)
    {
        const auto expected = Geometry::path_to_polygons(found->shape.paths[i]);
        EXPECT_EQ((*found->path_outlines)[i].size(), expected.size());
        EXPECT_TRUE(expected.empty());
    }
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesSynthesizesRegionFromStoredRects)
{
    add_region(Rect{.ll = {10, 10}, .ur = {50, 50}});

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    const RenderedShape *found = find_by_purpose(shapes, view_layers, ViewLayerPurpose::REGION);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->shape.rects.size(), 1u);
    EXPECT_EQ(found->shape.rects[0].ll.x, 10);
    EXPECT_EQ(found->shape.rects[0].ll.y, 10);
    EXPECT_EQ(found->shape.rects[0].ur.x, 50);
    EXPECT_EQ(found->shape.rects[0].ur.y, 50);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesReusesCacheForSameLayoutId)
{
    add_diearea(Rect{.ll = {0, 0}, .ur = {1000, 1000}});

    pipeline.generate_layout_shapes(root, layout_id, view_layers);
    pipeline.generate_layout_shapes(root, layout_id, view_layers);
    EXPECT_EQ(pipeline.generate_layout_calls(), 1u);

    LayoutId other = root.create_layout(LayoutData{.design = root.get_layout(layout_id)->design});
    pipeline.generate_layout_shapes(root, other, view_layers);
    EXPECT_EQ(pipeline.generate_layout_calls(), 2u);
}

TEST_F(LayoutPipelineFixture, GenerateLayoutShapesRecomputesAfterACrudMutationEvenForTheSameLayoutIdAndViewLayerSet)
{
    pipeline.generate_layout_shapes(root, layout_id, view_layers);
    ASSERT_EQ(pipeline.generate_layout_calls(), 1u);
    ASSERT_TRUE(pipeline.generate_layout_shapes(root, layout_id, view_layers).empty());
    ASSERT_EQ(pipeline.generate_layout_calls(), 1u); // nothing changed - cache hit

    add_diearea(Rect{.ll = {0, 0}, .ur = {100, 100}});
    root.bump_mutation_version();

    const auto &shapes = pipeline.generate_layout_shapes(root, layout_id, view_layers);
    EXPECT_EQ(pipeline.generate_layout_calls(), 2u); // same LayoutId, same ViewLayerSet - must still recompute
    EXPECT_EQ(shapes.size(), 1u);
}

// E1 (BUGS_AND_ENHANCEMENTS.md) - hit_test_point/hit_test_rect are
// already fully generic over either run()'s or run_layout()'s own
// output (see their own doc comment) - these confirm the *new* origins
// GenerateLayoutShapesStage now populates (Blockage/Route/PhysicalPort/
// Row/Region) actually round-trip through that existing, unmodified
// machinery, not just that generate_layout_shapes itself sets `.origin`.
TEST_F(LayoutPipelineFixture, HitTestPointFindsARoutingBlockageViaItsOrigin)
{
    const BlockageId blockage_id = add_routing_blockage(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});

    Scene scene;
    scene.set_viewport_size(1000, 1000);
    const auto &shapes = pipeline.run_layout(root, layout_id, scene, view_layers);
    const auto hit = Pipeline::hit_test_point(shapes, view_layers, scene, Point{5, 5});

    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(hit->shape_id.has_value()); // a Blockage has a real backing Shape
    ASSERT_TRUE(std::holds_alternative<BlockageId>(hit->origin));
    EXPECT_EQ(std::get<BlockageId>(hit->origin), blockage_id);
}

TEST_F(LayoutPipelineFixture, HitTestPointFindsARouteViaItsOrigin)
{
    const RouteId route_id = add_route(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});

    Scene scene;
    scene.set_viewport_size(1000, 1000);
    const auto &shapes = pipeline.run_layout(root, layout_id, scene, view_layers);
    const auto hit = Pipeline::hit_test_point(shapes, view_layers, scene, Point{5, 5});

    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(std::holds_alternative<RouteId>(hit->origin));
    EXPECT_EQ(std::get<RouteId>(hit->origin), route_id);
}

TEST_F(LayoutPipelineFixture, HitTestPointFindsAPhysicalPortViaItsOrigin)
{
    const PhysicalPortId port_id = add_physical_port(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});

    Scene scene;
    scene.set_viewport_size(1000, 1000);
    const auto &shapes = pipeline.run_layout(root, layout_id, scene, view_layers);
    const auto hit = Pipeline::hit_test_point(shapes, view_layers, scene, Point{5, 5});

    ASSERT_TRUE(hit.has_value());
    ASSERT_TRUE(std::holds_alternative<PhysicalPortId>(hit->origin));
    EXPECT_EQ(std::get<PhysicalPortId>(hit->origin), port_id);
}

// Row/Region have no backing Shape at all (synthesized geometry - see
// append_row_shapes'/append_region_shapes' own comments), so a hit here
// must carry an origin but no shape_id - api.cpp's le_mouse_up relies on
// exactly this distinction to fork into scene.select(RowId)/
// scene.select(RegionId) instead of the ShapeId+piece re-resolution path.
TEST_F(LayoutPipelineFixture, HitTestPointFindsARowWithOriginButNoShapeId)
{
    add_site("SITE1", Point{10, 20});
    const RowId row_id = add_row("SITE1", Point{0, 0});

    Scene scene;
    scene.set_viewport_size(1000, 1000);
    const auto &shapes = pipeline.run_layout(root, layout_id, scene, view_layers);
    const auto hit = Pipeline::hit_test_point(shapes, view_layers, scene, Point{5, 5});

    ASSERT_TRUE(hit.has_value());
    EXPECT_FALSE(hit->shape_id.has_value());
    ASSERT_TRUE(std::holds_alternative<RowId>(hit->origin));
    EXPECT_EQ(std::get<RowId>(hit->origin), row_id);
}

TEST_F(LayoutPipelineFixture, HitTestRectFindsARegionWithOriginButNoShapeId)
{
    const RegionId region_id = add_region(Rect{.ll = {10, 10}, .ur = {50, 50}});

    Scene scene;
    scene.set_viewport_size(1000, 1000);
    const auto &shapes = pipeline.run_layout(root, layout_id, scene, view_layers);
    const auto hits = Pipeline::hit_test_rect(shapes, view_layers, scene, Rect{.ll = {0, 0}, .ur = {100, 100}});

    const auto it = std::ranges::find_if(hits, [](const HoverTarget &h)
                                          { return std::holds_alternative<RegionId>(h.origin); });
    ASSERT_NE(it, hits.end());
    EXPECT_FALSE(it->shape_id.has_value());
    EXPECT_EQ(std::get<RegionId>(it->origin), region_id);
}
