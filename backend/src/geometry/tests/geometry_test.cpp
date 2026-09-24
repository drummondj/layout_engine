#include "../geometry.hpp"
#include <gtest/gtest.h>
#include <tuple>

using namespace le;

namespace
{
    // le::Point/Rect are plain generated aggregates with no operator== - compare fields directly.
    void expect_point_eq(const Point &actual, const Point &expected)
    {
        EXPECT_EQ(actual.x, expected.x);
        EXPECT_EQ(actual.y, expected.y);
    }

    void expect_bounds(const std::vector<Point> &points, Point expected_min, Point expected_max)
    {
        ASSERT_FALSE(points.empty());
        int64_t min_x = points.front().x, max_x = min_x;
        int64_t min_y = points.front().y, max_y = min_y;
        for (const auto &p : points)
        {
            min_x = std::min(min_x, p.x);
            max_x = std::max(max_x, p.x);
            min_y = std::min(min_y, p.y);
            max_y = std::max(max_y, p.y);
        }
        EXPECT_EQ(min_x, expected_min.x);
        EXPECT_EQ(min_y, expected_min.y);
        EXPECT_EQ(max_x, expected_max.x);
        EXPECT_EQ(max_y, expected_max.y);
    }
}

TEST(Geometry, RectToPolygonIsClosedAndAxisAligned)
{
    Rect rect{.ll = {0, 0}, .ur = {10, 20}};
    Polygon polygon = Geometry::rect_to_polygon(rect);

    ASSERT_EQ(polygon.points.size(), 5u);
    expect_point_eq(polygon.points.front(), polygon.points.back());
}

TEST(Geometry, BboxOfShapeCoversAllRects)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.rects.push_back(Rect{.ll = {5, -5}, .ur = {20, 5}});

    std::optional<Rect> box = Geometry::bbox(shape);
    ASSERT_TRUE(box.has_value());
    expect_point_eq(box->ll, Point{0, -5});
    expect_point_eq(box->ur, Point{20, 10});
}

TEST(Geometry, BboxOfShapeCoversPolygonsAndPaths)
{
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {0, 10}, {10, 10}, {10, 0}, {0, 0}}});
    // Centerline (20,0)-(20,10), width 4 -> half-width 2 expands the bbox by 2 on every side.
    shape.paths.push_back(Path{.width = 4, .polygon = Polygon{.points = {{20, 0}, {20, 10}}}});

    std::optional<Rect> box = Geometry::bbox(shape);
    ASSERT_TRUE(box.has_value());
    expect_point_eq(box->ll, Point{0, -2});
    expect_point_eq(box->ur, Point{22, 12});
}

TEST(Geometry, BboxOfEmptyShapeIsNullopt)
{
    EXPECT_FALSE(Geometry::bbox(Shape{}).has_value());
    EXPECT_FALSE(Geometry::bbox(std::vector<Shape>{}).has_value());
    EXPECT_FALSE(Geometry::bbox(std::vector<const Shape *>{}).has_value());
}

TEST(Geometry, BboxOfShapeVectorUnionsAllShapes)
{
    Shape a;
    a.rects.push_back(Rect{.ll = {0, 0}, .ur = {5, 5}});
    Shape b;
    b.rects.push_back(Rect{.ll = {10, 10}, .ur = {15, 15}});

    std::optional<Rect> box = Geometry::bbox(std::vector<Shape>{a, b});
    ASSERT_TRUE(box.has_value());
    expect_point_eq(box->ll, Point{0, 0});
    expect_point_eq(box->ur, Point{15, 15});
}

TEST(Geometry, BboxOfShapePointerVectorUnionsAllShapes)
{
    Shape a;
    a.rects.push_back(Rect{.ll = {0, 0}, .ur = {5, 5}});
    Shape b;
    b.rects.push_back(Rect{.ll = {10, 10}, .ur = {15, 15}});

    std::optional<Rect> box = Geometry::bbox(std::vector<const Shape *>{&a, &b});
    ASSERT_TRUE(box.has_value());
    expect_point_eq(box->ll, Point{0, 0});
    expect_point_eq(box->ur, Point{15, 15});
}

TEST(Geometry, RectsOverlap)
{
    Rect a{.ll = {0, 0}, .ur = {10, 10}};
    Rect b{.ll = {5, 5}, .ur = {15, 15}};
    Rect c{.ll = {20, 20}, .ur = {30, 30}};

    EXPECT_TRUE(Geometry::rects_overlap(a, b));
    EXPECT_FALSE(Geometry::rects_overlap(a, c));
}

TEST(Geometry, TransformTranslatesPoints)
{
    Polygon polygon{.points = {{0, 0}, {10, 0}, {10, 10}}};
    Polygon moved = Geometry::transform(polygon, Point{5, -5});

    ASSERT_EQ(moved.points.size(), polygon.points.size());
    expect_point_eq(moved.points[0], Point{5, -5});
    expect_point_eq(moved.points[1], Point{15, -5});
    expect_point_eq(moved.points[2], Point{15, 5});
}

TEST(Geometry, TransformTranslatesRect)
{
    Rect rect{.ll = {0, 0}, .ur = {10, 20}};
    Rect moved = Geometry::transform(rect, Point{5, -5});

    expect_point_eq(moved.ll, Point{5, -5});
    expect_point_eq(moved.ur, Point{15, 15});
}

TEST(Geometry, TransformTranslatesPath)
{
    Path path{.width = 4, .polygon = Polygon{.points = {{0, 0}, {10, 0}}}};
    Path moved = Geometry::transform(path, Point{5, -5});

    EXPECT_EQ(moved.width, 4);
    ASSERT_EQ(moved.polygon.points.size(), 2u);
    expect_point_eq(moved.polygon.points[0], Point{5, -5});
    expect_point_eq(moved.polygon.points[1], Point{15, -5});
}

TEST(Geometry, TransformTranslatesEveryRectPolygonAndPathInAShapeAndLeavesOtherFieldsUntouched)
{
    const LayerId m1{1, 1};
    ShapeData shape{.layer = m1};
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {10, 0}, {10, 10}}});
    shape.paths.push_back(Path{.width = 4, .polygon = Polygon{.points = {{0, 0}, {10, 0}}}});
    shape.spacing = 7;

    ShapeData moved = Geometry::transform(shape, Point{5, -5});

    EXPECT_EQ(moved.layer, m1);
    EXPECT_EQ(moved.spacing, 7);
    expect_point_eq(moved.rects[0].ll, Point{5, -5});
    expect_point_eq(moved.polygons[0].points[0], Point{5, -5});
    expect_point_eq(moved.paths[0].polygon.points[0], Point{5, -5});
}

TEST(Geometry, EnsureClosedReturnsInputUnchangedWhenFewerThanTwoPoints)
{
    EXPECT_TRUE(Geometry::ensure_closed({}).empty());

    std::vector<Point> single = {{1, 2}};
    auto result = Geometry::ensure_closed(single);
    ASSERT_EQ(result.size(), 1u);
    expect_point_eq(result[0], Point{1, 2});
}

TEST(Geometry, EnsureClosedAppendsFirstPointWhenOpen)
{
    std::vector<Point> open = {{0, 0}, {10, 0}, {10, 10}};
    auto closed = Geometry::ensure_closed(open);

    ASSERT_EQ(closed.size(), 4u);
    expect_point_eq(closed.back(), closed.front());
}

TEST(Geometry, EnsureClosedLeavesAlreadyClosedPolygonUnchanged)
{
    std::vector<Point> already_closed = {{0, 0}, {10, 0}, {10, 10}, {0, 0}};
    auto result = Geometry::ensure_closed(already_closed);

    EXPECT_EQ(result.size(), 4u);
}

TEST(Geometry, PathToPolygonsBuffersCenterlineBySymmetricHalfWidth)
{
    // LEF PATH WIDTH is the total trace width, so a width-20 horizontal
    // centerline should buffer to a total height of 20 (10 on each side),
    // not 40 - this is the exact bug path_to_polygons had (see CLAUDE.md).
    // The x extent is also extended by half-width (10) at each free end -
    // the DEF/LEF default end-cap convention, see
    // extend_path_ends_for_buffering's own comment - not left flush to
    // the raw centerline coordinates.
    Path path{.width = 20, .polygon = Polygon{.points = {{0, 0}, {100, 0}}}};
    auto polygons = Geometry::path_to_polygons(path);

    ASSERT_EQ(polygons.size(), 1u);
    expect_bounds(polygons.front().points, Point{-10, -10}, Point{110, 10});
}

TEST(Geometry, PathToPolygonsLeavesInteriorVerticesOfAMultiPointPathUnextended)
{
    // Only the two FREE ends of a multi-point Path get the half-width
    // extension - an interior vertex (a real corner within one continuous
    // Path) already gets a proper miter join from join_miter and must not
    // also be pushed outward as if it were a free end too. An
    // axis-aligned L turning left: (0,0)-(100,0)-(100,100), width 20
    // (half-width 10). The two free ends extend by 10 along their own
    // arm's direction: (0,0)-&gt;(-10,0) and (100,100)-&gt;(100,110). The
    // interior corner at (100,0) is not an end at all - it's covered by
    // join_miter's own outer corner, which for this left turn lands at
    // (110,-10) (the buffered strips' own outer/right-side edges: y=-10
    // from the horizontal arm, x=110 from the vertical arm). Together
    // these three points bound the whole outline exactly.
    Path path{.width = 20, .polygon = Polygon{.points = {{0, 0}, {100, 0}, {100, 100}}}};
    auto polygons = Geometry::path_to_polygons(path);

    ASSERT_EQ(polygons.size(), 1u);
    expect_bounds(polygons.front().points, Point{-10, -10}, Point{110, 110});
}

TEST(Geometry, UnionShapesReturnsNulloptWhenNoGeometry)
{
    EXPECT_FALSE(Geometry::union_shapes({}).has_value());

    Shape empty_shape;
    EXPECT_FALSE(Geometry::union_shapes({&empty_shape}).has_value());
}

TEST(Geometry, UnionShapesSkipsNullShapePointers)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    auto result = Geometry::union_shapes({nullptr, &shape});
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    expect_bounds(result->front().points, Point{0, 0}, Point{10, 10});
}

TEST(Geometry, UnionShapesMergesOverlappingRectsIntoOnePolygon)
{
    Shape a;
    a.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    Shape b;
    b.rects.push_back(Rect{.ll = {5, 5}, .ur = {15, 15}});

    auto result = Geometry::union_shapes({&a, &b});
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u); // overlapping -> merged into a single polygon
    expect_bounds(result->front().points, Point{0, 0}, Point{15, 15});
}

TEST(Geometry, UnionShapesIncludesPolygonsAndPathsNotJustRects)
{
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {10, 0}, {10, 10}}});
    // Centerline (20,0)-(20,10), width 4 -> buffers to x:[18,22] - disjoint from the triangle.
    shape.paths.push_back(Path{.width = 4, .polygon = Polygon{.points = {{20, 0}, {20, 10}}}});

    auto result = Geometry::union_shapes({&shape});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 2u); // disjoint -> stay as two separate polygons
}

TEST(Geometry, LabelLocationOfEmptyShapeIsOrigin)
{
    Shape shape;
    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{0, 0});
}

TEST(Geometry, LabelLocationPicksTheLargestRectWhenShapeHasMultipleRects)
{
    // No fracturing needed for rects (UPDATES.md item 8.2) - the largest
    // one is used directly. Areas are deliberately not tied (100 vs 5000).
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.rects.push_back(Rect{.ll = {20, 20}, .ur = {120, 70}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{70, 45});
}

TEST(Geometry, LabelLocationPicksTheFirstCandidateOnAnAreaTie)
{
    // Two rects of equal area (100x60=6000) - deterministic tie-break
    // keeps the first-encountered candidate (documented behavior, not
    // arbitrary per-run).
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {100, 60}});
    shape.rects.push_back(Rect{.ll = {0, 40}, .ur = {100, 100}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{50, 30});
}

TEST(Geometry, LabelLocationIncludesPolygonsAndPathsNotJustRects)
{
    // A square polygon (fractures into a single slab - its own bbox,
    // area 10000) plus a small path fully inside it (buffers into a
    // much smaller rect) - the polygon's slab wins.
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {100, 0}, {100, 100}, {0, 100}}});
    shape.paths.push_back(Path{.width = 4, .polygon = Polygon{.points = {{40, 40}, {60, 40}}}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{50, 50});
}

TEST(Geometry, LabelLocationOnAWidePolygonFracturesVerticallyAndPicksTheLargestSlab)
{
    // A rectilinear L: a long horizontal leg (0,0)-(100,20) plus a short
    // vertical stub (80,20)-(100,60) at its right end. bbox is 100 wide
    // by 60 tall - wider than tall - so this fractures with vertical
    // cuts at the vertex x-coordinates {0, 80, 100}: the [0,80] slab is
    // the leg alone ((0,0)-(80,20), area 1600), the [80,100] slab spans
    // the leg+stub's full local height ((80,0)-(100,60), area 1200) -
    // the leg's own slab is larger and wins.
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {100, 0}, {100, 60}, {80, 60}, {80, 20}, {0, 20}}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{40, 10});
}

TEST(Geometry, LabelLocationOnATallPolygonFracturesHorizontallyAndPicksTheLargestSlab)
{
    // The same L as above, transposed (x<->y) so its bbox is taller than
    // wide - fractures with horizontal cuts instead, same reasoning
    // rotated 90 degrees.
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {0, 100}, {60, 100}, {60, 80}, {20, 80}, {20, 0}}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{10, 40});
}

TEST(Geometry, LabelLocationOnAStraightPathReturnsItsBufferedCenter)
{
    // A straight, axis-aligned Path buffers (flat ends) into an exact
    // rectangle - fracturing it yields that one rectangle unchanged, so
    // this mainly confirms Paths flow through the same fracture pipeline
    // as Polygons, landing at the path's own centerline midpoint.
    Shape shape;
    shape.paths.push_back(Path{.width = 20, .polygon = Polygon{.points = {{10, 50}, {90, 50}}}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{50, 50});
}

TEST(Geometry, LabelLocationPicksTheLargestCandidateAcrossMixedRectsAndPolygons)
{
    // A small Rect and a clearly-larger Polygon (disjoint, so there's no
    // ambiguity about which one "wins") - confirms both candidate
    // sources are compared on equal footing.
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.polygons.push_back(Polygon{.points = {{200, 200}, {300, 200}, {300, 300}, {200, 300}}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{250, 250});
}

TEST(Geometry, LabelLocationHandlesZeroWidthBoundingBox)
{
    // A zero-width (degenerate vertical-line) rect - used directly as
    // its own (zero-area) candidate, same as any other single rect; its
    // "center" is just the degenerate line's own midpoint.
    Shape shape;
    shape.rects.push_back(Rect{.ll = {50, 0}, .ur = {50, 100}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{50, 50});
}

TEST(Geometry, LabelLocationHandlesZeroHeightBoundingBox)
{
    // Same as above but for a zero-height (degenerate horizontal-line) rect.
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 50}, .ur = {100, 50}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{50, 50});
}

TEST(Geometry, LabelLocationOfDisjointRectsPicksTheLargestNotAnOffShapeCentroid)
{
    // Two disjoint, equal-area bars with a gap between them. The old
    // union+grid-search algorithm could return the raw bbox centroid
    // here (50,50) - a point in the gap, not actually on either bar (see
    // this project's git history). The new algorithm can't: every
    // candidate is a real rect, so the result is always genuinely inside
    // the shape - here, the first-encountered (equal-area tie) bar.
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 100}});
    shape.rects.push_back(Rect{.ll = {90, 0}, .ur = {100, 100}});

    Point label = Geometry::get_label_location(shape);
    expect_point_eq(label, Point{5, 50});
}

TEST(Geometry, LocalWidthAtRectReturnsMinDimension)
{
    // Non-square rect: the smaller dimension is the "width" a label
    // should be sized to, not the larger one or some average of the two.
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {100, 20}});

    EXPECT_DOUBLE_EQ(Geometry::local_width_at(shape, Point{50, 10}), 20.0);
}

TEST(Geometry, LocalWidthAtPathReturnsPathWidthDirectly)
{
    // A Path's thickness is already known exactly - local_width_at should
    // read path.width directly rather than derive anything from its
    // (much larger) buffered bbox.
    Shape shape;
    shape.paths.push_back(Path{.width = 6, .polygon = Polygon{.points = {{0, 0}, {100, 0}}}});

    EXPECT_DOUBLE_EQ(Geometry::local_width_at(shape, Point{50, 0}), 6.0);
}

TEST(Geometry, LocalWidthAtSquarePolygonMeasuresDistanceToBoundaryNotFilledArea)
{
    // A point well inside a 100x100 square polygon: distance to the
    // nearest edge is 50 on every side, so width = 2*50 = 100. This is
    // also a regression guard for the "distance to a filled polygon is 0
    // for any interior point" pitfall - a buggy implementation that
    // measured distance to the filled area instead of its boundary would
    // return 0.0 here instead of 100.0.
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {100, 0}, {100, 100}, {0, 100}}});

    EXPECT_NEAR(Geometry::local_width_at(shape, Point{50, 50}), 100.0, 0.01);
}

TEST(Geometry, LocalWidthAtLShapedPolygonUsesArmThicknessNotBbox)
{
    // An L-shaped (non-convex) polygon: a 100x30 bottom arm plus a 30x100
    // vertical arm, so the overall bbox is 100x100 - but a point centered
    // in the vertical arm's own 30-wide interior should be sized to that
    // arm's ~30 thickness, nowhere near the 100-wide bbox. This is the
    // regression test that would fail if sizing were ever "simplified"
    // back to a bbox-based approach - the whole point of this feature.
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {
                                          {0, 0},
                                          {100, 0},
                                          {100, 30},
                                          {30, 30},
                                          {30, 100},
                                          {0, 100},
                                      }});

    const double width = Geometry::local_width_at(shape, Point{15, 70});
    EXPECT_NEAR(width, 30.0, 0.01);
    EXPECT_LT(width, 50.0); // far below the shape's own 100-wide bbox
}

TEST(Geometry, LocalWidthAtFallsBackToNearestPieceWhenPointOutsideEveryPiece)
{
    // Same two disjoint 10-wide bars as
    // LabelLocationReturnsRawCentroidWhenGridSearchFindsNoInteriorPoint,
    // whose raw-centroid fallback (50,50) sits in the gap between them,
    // outside both. local_width_at must fall back to the nearest bar's
    // own width (10), not the shape's overall ~90-wide bbox (which would
    // grossly overstate either bar's actual thickness).
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 100}});
    shape.rects.push_back(Rect{.ll = {90, 0}, .ur = {100, 100}});

    EXPECT_DOUBLE_EQ(Geometry::local_width_at(shape, Point{50, 50}), 10.0);
}

TEST(Geometry, LocalWidthAtOfEmptyShapeIsZero)
{
    Shape shape;
    EXPECT_DOUBLE_EQ(Geometry::local_width_at(shape, Point{0, 0}), 0.0);
}

TEST(Geometry, ContainsIsTrueInsideARectAndFalseOutside)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    EXPECT_TRUE(Geometry::contains(shape, Point{5, 5}));
    EXPECT_TRUE(Geometry::contains(shape, Point{0, 0})); // on the boundary counts
    EXPECT_FALSE(Geometry::contains(shape, Point{11, 5}));
}

TEST(Geometry, ContainsUsesRealPointInPolygonNotBboxForAnLShape)
{
    // Same L-shaped polygon as LocalWidthAtLShapedPolygonUsesArmThicknessNotBbox:
    // a point inside the overall 100x100 bbox but in the notch cut out of
    // the L (not inside either arm) must not count as contained - a bbox
    // check would wrongly say yes.
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {
                                          {0, 0},
                                          {100, 0},
                                          {100, 30},
                                          {30, 30},
                                          {30, 100},
                                          {0, 100},
                                      }});

    EXPECT_TRUE(Geometry::contains(shape, Point{15, 70}));  // inside the vertical arm
    EXPECT_TRUE(Geometry::contains(shape, Point{70, 15}));  // inside the horizontal arm
    EXPECT_FALSE(Geometry::contains(shape, Point{70, 70})); // in the notch - within the bbox, outside the L
}

TEST(Geometry, ContainsUsesThePathsBufferedOutlineNotItsCenterline)
{
    // A horizontal path with width 10 centered on y=0: a point 3 above the
    // centerline is inside the buffered/drawn outline (half-width 5) but
    // would miss a naive "on the centerline" test.
    Shape shape;
    shape.paths.push_back(Path{.width = 10, .polygon = Polygon{.points = {{0, 0}, {100, 0}}}});

    EXPECT_TRUE(Geometry::contains(shape, Point{50, 3}));
    EXPECT_FALSE(Geometry::contains(shape, Point{50, 20})); // well outside the buffered width
}

TEST(Geometry, ContainsOfEmptyShapeIsFalse)
{
    Shape shape;
    EXPECT_FALSE(Geometry::contains(shape, Point{0, 0}));
}

TEST(Geometry, FindHitPieceReturnsOnlyTheOneRectHitNotEveryRectInTheShape)
{
    // Regression: a Shape can bundle several rects together (e.g. several
    // RECT statements in one LEF PORT) - hover highlighting must isolate
    // just the one piece under the cursor, not the whole group (a real
    // reported bug: hovering one rect highlighted every rect on the same
    // Terminal).
    const LayerId m1{1, 1};
    Shape shape;
    shape.layer = m1;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    shape.rects.push_back(Rect{.ll = {100, 100}, .ur = {110, 110}});

    const auto piece = Geometry::find_hit_piece(shape, Point{5, 5});
    ASSERT_TRUE(piece.has_value());
    EXPECT_EQ(piece->kind, PieceKind::RECT);
    EXPECT_EQ(piece->index, 0u);
    EXPECT_EQ(piece->outline.layer, m1);
    ASSERT_EQ(piece->outline.rects.size(), 1u);
    EXPECT_EQ(piece->outline.rects.front().ll.x, 0);
    EXPECT_TRUE(piece->outline.polygons.empty());
    EXPECT_TRUE(piece->outline.paths.empty());
}

TEST(Geometry, FindHitPieceReturnsOnlyTheOnePolygonHitNotEveryPolygonInTheShape)
{
    Shape shape;
    shape.polygons.push_back(Polygon{.points = {{0, 0}, {10, 0}, {10, 10}, {0, 10}}});
    shape.polygons.push_back(Polygon{.points = {{100, 100}, {110, 100}, {110, 110}, {100, 110}}});

    const auto piece = Geometry::find_hit_piece(shape, Point{5, 5});
    ASSERT_TRUE(piece.has_value());
    EXPECT_EQ(piece->kind, PieceKind::POLYGON);
    EXPECT_EQ(piece->index, 0u);
    ASSERT_EQ(piece->outline.polygons.size(), 1u);
    EXPECT_TRUE(piece->outline.rects.empty());
}

TEST(Geometry, FindHitPieceReturnsNulloptOnAMiss)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    EXPECT_FALSE(Geometry::find_hit_piece(shape, Point{500, 500}).has_value());
}

TEST(Geometry, FullyEnclosedIsTrueWhenShapeFitsEntirelyInsideTheContainer)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {10, 10}, .ur = {20, 20}});

    EXPECT_TRUE(Geometry::fully_enclosed(Rect{.ll = {0, 0}, .ur = {30, 30}}, shape));
}

TEST(Geometry, FullyEnclosedIsFalseWhenShapeOnlyPartiallyOverlaps)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {10, 10}, .ur = {20, 20}});

    // Container only covers the left half of the shape's bbox.
    EXPECT_FALSE(Geometry::fully_enclosed(Rect{.ll = {0, 0}, .ur = {15, 30}}, shape));
}

TEST(Geometry, FullyEnclosedIsFalseWhenShapeIsCompletelyOutsideTheContainer)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {100, 100}, .ur = {110, 110}});

    EXPECT_FALSE(Geometry::fully_enclosed(Rect{.ll = {0, 0}, .ur = {30, 30}}, shape));
}

TEST(Geometry, FullyEnclosedOfEmptyShapeIsFalse)
{
    Shape shape;
    EXPECT_FALSE(Geometry::fully_enclosed(Rect{.ll = {0, 0}, .ur = {100, 100}}, shape));
}

TEST(Geometry, FullyEnclosedPiecesReturnsOnlyTheIndividuallyEnclosedPieces)
{
    // A bundled multi-piece Shape where some pieces fit inside the
    // container and others don't - the per-piece analog of
    // fully_enclosed, which only ever answers for the whole bundle.
    const LayerId m1{1, 1};
    Shape shape{
        .layer = m1,
        .polygons = {
            Polygon{.points = {{15, 15}, {18, 15}, {18, 18}, {15, 18}}}, // inside
        },
        .rects = {
            Rect{.ll = {10, 10}, .ur = {20, 20}},   // inside
            Rect{.ll = {100, 100}, .ur = {110, 110}}, // outside
        },
    };

    const auto pieces = Geometry::fully_enclosed_pieces(Rect{.ll = {0, 0}, .ur = {30, 30}}, shape);

    ASSERT_EQ(pieces.size(), 2u);
    EXPECT_EQ(pieces[0].kind, PieceKind::RECT);
    EXPECT_EQ(pieces[0].index, 0u);
    EXPECT_EQ(pieces[0].outline.rects.size(), 1u);
    EXPECT_EQ(pieces[0].outline.rects.front().ll.x, 10);
    EXPECT_EQ(pieces[1].kind, PieceKind::POLYGON);
    EXPECT_EQ(pieces[1].index, 0u);
    EXPECT_EQ(pieces[1].outline.polygons.size(), 1u);
    EXPECT_EQ(pieces[0].outline.layer, m1); // layer_name carried onto each single-piece Shape
}

TEST(Geometry, FullyEnclosedPiecesIsEmptyWhenNoPieceFits)
{
    Shape shape;
    shape.rects.push_back(Rect{.ll = {100, 100}, .ur = {110, 110}});

    EXPECT_TRUE(Geometry::fully_enclosed_pieces(Rect{.ll = {0, 0}, .ur = {30, 30}}, shape).empty());
}

TEST(Geometry, ExtractPieceReturnsOnlyTheOneIndexedRectNotItsSiblings)
{
    const LayerId m1{1, 1};
    Shape shape{
        .layer = m1,
        .rects = {
            Rect{.ll = {0, 0}, .ur = {10, 10}},
            Rect{.ll = {100, 100}, .ur = {110, 110}},
        },
    };

    const Shape piece = Geometry::extract_piece(shape, PieceKind::RECT, 1);
    EXPECT_EQ(piece.layer, m1);
    ASSERT_EQ(piece.rects.size(), 1u);
    EXPECT_EQ(piece.rects.front().ll.x, 100);
    EXPECT_TRUE(piece.polygons.empty());
    EXPECT_TRUE(piece.paths.empty());
}

TEST(Geometry, ExtractPieceOfAnOutOfRangeIndexReturnsAnEmptyPiece)
{
    const LayerId m1{1, 1};
    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};

    const Shape piece = Geometry::extract_piece(shape, PieceKind::RECT, 5);
    EXPECT_EQ(piece.layer, m1);
    EXPECT_TRUE(piece.rects.empty());
}

TEST(Geometry, PieceInRangeMatchesWhatExtractPieceWouldReturn)
{
    Shape shape{.polygons = {Polygon{.points = {{0, 0}, {10, 0}, {10, 10}}}}};

    EXPECT_TRUE(Geometry::piece_in_range(shape, PieceKind::POLYGON, 0));
    EXPECT_FALSE(Geometry::piece_in_range(shape, PieceKind::POLYGON, 1));
    EXPECT_FALSE(Geometry::piece_in_range(shape, PieceKind::RECT, 0));
}

TEST(Geometry, TransformPieceInPlaceMovesOnlyTheAddressedPieceLeavingSiblingsUntouched)
{
    const LayerId m1{1, 1};
    Shape shape{
        .layer = m1,
        .polygons = {Polygon{.points = {{0, 0}, {10, 0}, {10, 10}}}},
        .rects = {
            Rect{.ll = {0, 0}, .ur = {10, 10}},
            Rect{.ll = {100, 100}, .ur = {110, 110}},
        },
    };

    Geometry::transform_piece_in_place(shape, PieceKind::RECT, 1, Point{5, -5});

    expect_point_eq(shape.rects[0].ll, Point{0, 0}); // untouched sibling rect
    expect_point_eq(shape.rects[1].ll, Point{105, 95}); // the addressed piece moved
    expect_point_eq(shape.polygons[0].points[0], Point{0, 0}); // untouched, different kind entirely
}

TEST(Geometry, TransformPieceInPlaceOfAnOutOfRangeIndexIsANoOp)
{
    Shape shape{.rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};

    Geometry::transform_piece_in_place(shape, PieceKind::RECT, 5, Point{5, -5});

    expect_point_eq(shape.rects[0].ll, Point{0, 0});
}

TEST(Geometry, OrientationLinearNIsIdentity)
{
    const auto n = Geometry::orientation_linear(Orientation::N);
    expect_point_eq(Geometry::apply_linear(n, Point{7, -3}), Point{7, -3});
}

TEST(Geometry, OrientationLinearEveryFlipIsItsOwnInverse)
{
    const Point sample{7, -3};
    for (Orientation o : {Orientation::FN, Orientation::FS, Orientation::FE, Orientation::FW})
    {
        const auto m = Geometry::orientation_linear(o);
        const Point twice = Geometry::apply_linear(m, Geometry::apply_linear(m, sample));
        expect_point_eq(twice, sample);
    }
}

TEST(Geometry, OrientationLinearWHasOrderFour)
{
    const auto w = Geometry::orientation_linear(Orientation::W);
    Point p{7, -3};
    for (int i = 0; i < 4; i++)
        p = Geometry::apply_linear(w, p);
    expect_point_eq(p, Point{7, -3});
}

TEST(Geometry, OrientationLinearEAndWAreMutualInverses)
{
    const auto w = Geometry::orientation_linear(Orientation::W);
    const auto e = Geometry::orientation_linear(Orientation::E);
    const Point sample{7, -3};
    expect_point_eq(Geometry::apply_linear(e, Geometry::apply_linear(w, sample)), sample);
}

TEST(Geometry, OrientationLinearSAppliedTwiceIsIdentity)
{
    const auto s = Geometry::orientation_linear(Orientation::S);
    const Point sample{7, -3};
    expect_point_eq(Geometry::apply_linear(s, Geometry::apply_linear(s, sample)), sample);
}

TEST(Geometry, InstanceTransformSquareBboxLandsLlAtLocationForEveryOrientation)
{
    // A square bbox's own transformed size never changes (rotating/
    // mirroring a square yields a square of the same size) - every
    // orientation should land ll at the same placement_location and ur at
    // the same offset from it, regardless of orientation.
    const Rect local_bbox{.ll = {0, 0}, .ur = {100, 100}};
    const Point location{500, 1000};

    for (Orientation o : {Orientation::N, Orientation::S, Orientation::E, Orientation::W, Orientation::FN, Orientation::FS, Orientation::FE, Orientation::FW})
    {
        const auto t = Geometry::instance_transform(o, local_bbox, location);
        const Rect world = Geometry::transform_bbox(t, local_bbox);
        expect_point_eq(world.ll, location);
        expect_point_eq(world.ur, Point{location.x + 100, location.y + 100});
    }
}

TEST(Geometry, InstanceTransformAsymmetricBboxSwapsAxesUnderEAndW)
{
    // A non-square bbox is the case a memorized per-orientation W/H-swap
    // table gets subtly wrong if transcribed carelessly - this confirms
    // the general corner-based derivation actually swaps which local axis
    // becomes "width" under a 90-degree orientation.
    const Rect local_bbox{.ll = {0, 0}, .ur = {200, 100}}; // 200 wide, 100 tall
    const Point location{0, 0};

    const auto w = Geometry::instance_transform(Orientation::W, local_bbox, location);
    const Rect world_w = Geometry::transform_bbox(w, local_bbox);
    expect_point_eq(world_w.ll, location);
    expect_point_eq(world_w.ur, Point{100, 200}); // swapped: 100 wide, 200 tall

    const auto e = Geometry::instance_transform(Orientation::E, local_bbox, location);
    const Rect world_e = Geometry::transform_bbox(e, local_bbox);
    expect_point_eq(world_e.ll, location);
    expect_point_eq(world_e.ur, Point{100, 200}); // also swapped

    const auto n = Geometry::instance_transform(Orientation::N, local_bbox, location);
    const Rect world_n = Geometry::transform_bbox(n, local_bbox);
    expect_point_eq(world_n.ur, Point{200, 100}); // unrotated - not swapped
}

TEST(Geometry, InstanceTransformHandlesANonZeroLocalBboxLowerLeft)
{
    // A Layout's own die area (unlike an Abstract's assumed (0,0)-origin
    // size) can start at an arbitrary point - confirms the derivation
    // isn't secretly a (0,0)-assuming shortcut.
    const Rect local_bbox{.ll = {100, 100}, .ur = {600, 700}}; // 500 x 600
    const Point location{1000, 2000};

    const auto n = Geometry::instance_transform(Orientation::N, local_bbox, location);
    const Rect world_n = Geometry::transform_bbox(n, local_bbox);
    expect_point_eq(world_n.ll, location);
    expect_point_eq(world_n.ur, Point{1500, 2600});

    const auto s = Geometry::instance_transform(Orientation::S, local_bbox, location);
    const Rect world_s = Geometry::transform_bbox(s, local_bbox);
    expect_point_eq(world_s.ll, location); // same size as N (180-degree rotation)
    expect_point_eq(world_s.ur, Point{1500, 2600});
}

TEST(Geometry, ComposeAppliesInnerThenOuter)
{
    // outer translates by (1000, 2000); inner places a square's own ll at
    // (10, 10) under orientation W (which swaps axes). Composing them and
    // applying to the square's own local bbox directly should land at the
    // same place as applying inner then outer by hand, one step at a time.
    const Rect local_bbox{.ll = {0, 0}, .ur = {100, 100}};
    const Geometry::InstanceTransform inner = Geometry::instance_transform(Orientation::W, local_bbox, Point{10, 10});
    const Geometry::InstanceTransform outer{.linear = Geometry::orientation_linear(Orientation::N), .translation = Point{1000, 2000}};

    const Rect inner_world = Geometry::transform_bbox(inner, local_bbox);
    const Rect expected = Rect{.ll = {inner_world.ll.x + 1000, inner_world.ll.y + 2000}, .ur = {inner_world.ur.x + 1000, inner_world.ur.y + 2000}};

    const Geometry::InstanceTransform composed = Geometry::compose(outer, inner);
    const Rect actual = Geometry::transform_bbox(composed, local_bbox);
    expect_point_eq(actual.ll, expected.ll);
    expect_point_eq(actual.ur, expected.ur);
}

TEST(Geometry, ComposeWithIdentityIsANoOp)
{
    const Rect local_bbox{.ll = {0, 0}, .ur = {200, 100}};
    const Geometry::InstanceTransform t = Geometry::instance_transform(Orientation::E, local_bbox, Point{50, 60});
    const Geometry::InstanceTransform identity = Geometry::identity_transform();

    const Rect direct = Geometry::transform_bbox(t, local_bbox);
    const Rect via_outer_identity = Geometry::transform_bbox(Geometry::compose(identity, t), local_bbox);
    const Rect via_inner_identity = Geometry::transform_bbox(Geometry::compose(t, identity), local_bbox);

    expect_point_eq(via_outer_identity.ll, direct.ll);
    expect_point_eq(via_outer_identity.ur, direct.ur);
    expect_point_eq(via_inner_identity.ll, direct.ll);
    expect_point_eq(via_inner_identity.ur, direct.ur);
}

TEST(Geometry, InvertUndoesEveryOrientationAndTranslation)
{
    // Composing a transform with its own invert() (in either order) must
    // collapse back to identity - confirmed here by checking a bbox
    // survives transform-then-untransform unchanged, for every one of the
    // 8 orientations and a non-trivial translation.
    const Rect local_bbox{.ll = {0, 0}, .ur = {200, 100}};
    for (Orientation o : {Orientation::N, Orientation::S, Orientation::E, Orientation::W, Orientation::FN, Orientation::FS, Orientation::FE, Orientation::FW})
    {
        const Geometry::InstanceTransform t = Geometry::instance_transform(o, local_bbox, Point{700, -300});
        const Geometry::InstanceTransform inverse = Geometry::invert(t);

        const Rect round_trip_outer = Geometry::transform_bbox(Geometry::compose(inverse, t), local_bbox);
        expect_point_eq(round_trip_outer.ll, local_bbox.ll);
        expect_point_eq(round_trip_outer.ur, local_bbox.ur);

        const Rect round_trip_inner = Geometry::transform_bbox(Geometry::compose(t, inverse), local_bbox);
        expect_point_eq(round_trip_inner.ll, local_bbox.ll);
        expect_point_eq(round_trip_inner.ur, local_bbox.ur);
    }
}

TEST(Geometry, InvertBringsAWorldRectBackToLocalSpace)
{
    // The actual use case (ViewportCullStage): a world-space rect run
    // through invert(t) should land exactly where the un-transformed
    // local rect would have to be for transform_bbox(t, .) to reproduce
    // the original world-space rect.
    const Rect local_bbox{.ll = {0, 0}, .ur = {200, 100}};
    const Geometry::InstanceTransform t = Geometry::instance_transform(Orientation::FE, local_bbox, Point{300, 400});
    const Rect world_bbox = Geometry::transform_bbox(t, local_bbox);

    const Rect recovered_local = Geometry::transform_bbox(Geometry::invert(t), world_bbox);
    expect_point_eq(recovered_local.ll, local_bbox.ll);
    expect_point_eq(recovered_local.ur, local_bbox.ur);
}

// --- Shape boolean operations and conversions (shape_* TCL commands) ---

namespace
{
    void expect_rect(const Rect &actual, int64_t llx, int64_t lly, int64_t urx, int64_t ury)
    {
        EXPECT_EQ(actual.ll.x, llx);
        EXPECT_EQ(actual.ll.y, lly);
        EXPECT_EQ(actual.ur.x, urx);
        EXPECT_EQ(actual.ur.y, ury);
    }

    int64_t rect_area(const Rect &r)
    {
        return (r.ur.x - r.ll.x) * (r.ur.y - r.ll.y);
    }

    int64_t polygon_area(const Polygon &polygon)
    {
        __int128 twice = 0;
        for (size_t i = 0; i + 1 < polygon.points.size(); ++i)
            twice += static_cast<__int128>(polygon.points[i].x) * polygon.points[i + 1].y -
                     static_cast<__int128>(polygon.points[i + 1].x) * polygon.points[i].y;
        return static_cast<int64_t>(twice < 0 ? -twice / 2 : twice / 2);
    }

    int64_t total_area(const AreaGeometry &geometry)
    {
        int64_t area = 0;
        for (const Rect &r : geometry.rects)
            area += rect_area(r);
        for (const Polygon &p : geometry.polygons)
            area += polygon_area(p);
        return area;
    }

    int64_t total_area(const std::vector<Rect> &rects)
    {
        int64_t area = 0;
        for (const Rect &r : rects)
            area += rect_area(r);
        return area;
    }

    Shape rect_shape(std::vector<Rect> rects)
    {
        return Shape{.rects = std::move(rects)};
    }

    // (0,0)-(20,0)-(20,10)-(10,10)-(10,20)-(0,20): area 300.
    Shape l_shape()
    {
        return Shape{.polygons = {Polygon{.points = {{0, 0}, {20, 0}, {20, 10}, {10, 10}, {10, 20}, {0, 20}, {0, 0}}}}};
    }
}

TEST(Geometry, BooleanOrOfOverlappingRectsIsOneRect)
{
    const Shape a = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}});
    const Shape b = rect_shape({Rect{.ll = {5, 0}, .ur = {15, 10}}});
    const AreaGeometry result = Geometry::boolean_shapes({&a}, {&b}, BooleanOp::Or);

    // The merged outline keeps a collinear vertex at each old seam -
    // simplify_ring drops them, so this comes back as a real Rect.
    ASSERT_EQ(result.rects.size(), 1u);
    EXPECT_TRUE(result.polygons.empty());
    expect_rect(result.rects[0], 0, 0, 15, 10);
}

TEST(Geometry, BooleanOrOfLShapedUnionIsOnePolygon)
{
    const Shape a = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}});
    const Shape b = rect_shape({Rect{.ll = {0, 0}, .ur = {5, 20}}});
    const AreaGeometry result = Geometry::boolean_shapes({&a}, {&b}, BooleanOp::Or);

    ASSERT_EQ(result.polygons.size(), 1u);
    EXPECT_TRUE(result.rects.empty());
    EXPECT_EQ(result.polygons[0].points.size(), 7u); // 6 corners, closed
    EXPECT_EQ(total_area(result), 150);
}

TEST(Geometry, BooleanAndIsTheOverlap)
{
    const Shape a = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}});
    const Shape b = rect_shape({Rect{.ll = {5, 5}, .ur = {15, 15}}});
    const AreaGeometry result = Geometry::boolean_shapes({&a}, {&b}, BooleanOp::And);

    ASSERT_EQ(result.rects.size(), 1u);
    expect_rect(result.rects[0], 5, 5, 10, 10);
}

TEST(Geometry, BooleanAndOfDisjointShapesIsEmpty)
{
    const Shape a = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}});
    const Shape b = rect_shape({Rect{.ll = {20, 20}, .ur = {30, 30}}});
    EXPECT_TRUE(Geometry::boolean_shapes({&a}, {&b}, BooleanOp::And).empty());
}

TEST(Geometry, BooleanNotSubtractsAnEdge)
{
    const Shape a = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}});
    const Shape b = rect_shape({Rect{.ll = {5, 0}, .ur = {15, 10}}});
    const AreaGeometry result = Geometry::boolean_shapes({&a}, {&b}, BooleanOp::Not);

    ASSERT_EQ(result.rects.size(), 1u);
    expect_rect(result.rects[0], 0, 0, 5, 10);
}

TEST(Geometry, BooleanNotLeavingAHoleIsEmittedAsExactRects)
{
    // A Polygon can't hold a hole - the donut must come back as rects
    // that cover exactly its area (no silently filled-in hole).
    const Shape big = rect_shape({Rect{.ll = {0, 0}, .ur = {30, 30}}});
    const Shape small = rect_shape({Rect{.ll = {10, 10}, .ur = {20, 20}}});
    const AreaGeometry result = Geometry::boolean_shapes({&big}, {&small}, BooleanOp::Not);

    EXPECT_TRUE(result.polygons.empty());
    EXPECT_EQ(result.rects.size(), 4u); // bottom band, two side pieces, top band
    EXPECT_EQ(total_area(result), 900 - 100);
}

TEST(Geometry, BooleanMergesEveryShapeWithinAGroup)
{
    const Shape a1 = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}});
    const Shape a2 = rect_shape({Rect{.ll = {20, 0}, .ur = {30, 10}}});
    const Shape b = rect_shape({Rect{.ll = {5, 0}, .ur = {25, 10}}});
    const AreaGeometry result = Geometry::boolean_shapes({&a1, &a2}, {&b}, BooleanOp::And);

    ASSERT_EQ(result.rects.size(), 2u);
    EXPECT_EQ(total_area(result), 50 + 50);
}

TEST(Geometry, ShapeToRectsFracturesHorizontally)
{
    const std::vector<Rect> rects = Geometry::shape_to_rects(l_shape(), FractureDirection::Horizontal);

    ASSERT_EQ(rects.size(), 2u);
    expect_rect(rects[0], 0, 0, 20, 10);
    expect_rect(rects[1], 0, 10, 10, 20);
}

TEST(Geometry, ShapeToRectsFracturesVertically)
{
    const std::vector<Rect> rects = Geometry::shape_to_rects(l_shape(), FractureDirection::Vertical);

    ASSERT_EQ(rects.size(), 2u);
    expect_rect(rects[0], 0, 0, 10, 20);
    expect_rect(rects[1], 10, 0, 20, 10);
}

TEST(Geometry, ShapeToRectsNeverOverlapsEvenWhenTheInputDoes)
{
    const Shape shape = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}, Rect{.ll = {5, 5}, .ur = {15, 15}}});
    const std::vector<Rect> rects = Geometry::shape_to_rects(shape, FractureDirection::Horizontal);

    EXPECT_EQ(total_area(rects), 100 + 100 - 25); // the union's area - any overlap would exceed it
}

TEST(Geometry, ShapeToRectsMergesSlabsBackIntoOneRect)
{
    // The nested rect adds cut lines at y=2/4 - the three slabs between
    // them must merge back into the one real rectangle.
    const Shape shape = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}, Rect{.ll = {2, 2}, .ur = {4, 4}}});
    const std::vector<Rect> rects = Geometry::shape_to_rects(shape, FractureDirection::Horizontal);

    ASSERT_EQ(rects.size(), 1u);
    expect_rect(rects[0], 0, 0, 10, 10);
}

TEST(Geometry, ShapeToPolygonsConvertsARect)
{
    const std::vector<Polygon> polygons = Geometry::shape_to_polygons(rect_shape({Rect{.ll = {0, 0}, .ur = {10, 20}}}));

    ASSERT_EQ(polygons.size(), 1u);
    EXPECT_EQ(polygons[0].points.size(), 5u);
    expect_bounds(polygons[0].points, {0, 0}, {10, 20});
}

TEST(Geometry, ShapeToPolygonsFracturesARegionWithAHole)
{
    const Shape ring = rect_shape({
        Rect{.ll = {0, 0}, .ur = {30, 10}},
        Rect{.ll = {0, 20}, .ur = {30, 30}},
        Rect{.ll = {0, 10}, .ur = {10, 20}},
        Rect{.ll = {20, 10}, .ur = {30, 20}},
    });
    const std::vector<Polygon> polygons = Geometry::shape_to_polygons(ring);

    int64_t area = 0;
    for (const Polygon &p : polygons)
        area += polygon_area(p);
    EXPECT_EQ(area, 900 - 100);
}

TEST(Geometry, SizeGrowsByIndependentXAndY)
{
    const auto result = Geometry::size_shape(rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}}), 2, 3);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->rects.size(), 1u);
    expect_rect(result->rects[0], -2, -3, 12, 13);
}

TEST(Geometry, SizeShrinks)
{
    const auto result = Geometry::size_shape(rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}}), -2, -1);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->rects.size(), 1u);
    expect_rect(result->rects[0], 2, 1, 8, 9);
}

TEST(Geometry, SizeWithMixedSignsGrowsXAndShrinksY)
{
    const auto result = Geometry::size_shape(rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}}), 2, -3);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->rects.size(), 1u);
    expect_rect(result->rects[0], -2, 3, 12, 7);
}

TEST(Geometry, SizeShrinkingAwayEverythingIsEmptyNotUnsupported)
{
    const auto result = Geometry::size_shape(rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}}), -6, -6);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(Geometry, SizeGrowthClosesAGapBetweenEntries)
{
    const Shape shape = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}, Rect{.ll = {12, 0}, .ur = {22, 10}}});
    const auto result = Geometry::size_shape(shape, 1, 1);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->rects.size(), 1u);
    expect_rect(result->rects[0], -1, -1, 23, 11);
}

TEST(Geometry, SizeGrowsAnLShapeExactly)
{
    // [-1,21]x[-1,11] union [-1,11]x[-1,21]: an exact Minkowski sum, not a
    // bbox approximation.
    const auto result = Geometry::size_shape(l_shape(), 1, 1);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->polygons.size(), 1u);
    EXPECT_EQ(total_area(*result), 22 * 12 + 12 * 22 - 12 * 12);
}

TEST(Geometry, SizeNonRectilinearOnlySupportsIsotropic)
{
    const Shape triangle{.polygons = {Polygon{.points = {{0, 0}, {100, 0}, {0, 100}, {0, 0}}}}};

    EXPECT_FALSE(Geometry::size_shape(triangle, 5, 10).has_value());

    const auto grown = Geometry::size_shape(triangle, 5, 5);
    ASSERT_TRUE(grown.has_value());
    EXPECT_GT(total_area(*grown), 100 * 100 / 2);
}

TEST(Geometry, OutlinePathOfARectIsOneClosedRing)
{
    const std::vector<Path> paths = Geometry::shape_outline_paths(rect_shape({Rect{.ll = {0, 0}, .ur = {10, 20}}}), 2);

    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0].width, 2);
    ASSERT_EQ(paths[0].polygon.points.size(), 5u);
    expect_point_eq(paths[0].polygon.points.front(), paths[0].polygon.points.back());
    expect_bounds(paths[0].polygon.points, {0, 0}, {10, 20});
}

TEST(Geometry, OutlinePathsFollowAHoleToo)
{
    const Shape ring = rect_shape({
        Rect{.ll = {0, 0}, .ur = {30, 10}},
        Rect{.ll = {0, 20}, .ur = {30, 30}},
        Rect{.ll = {0, 10}, .ur = {10, 20}},
        Rect{.ll = {20, 10}, .ur = {30, 20}},
    });
    const std::vector<Path> paths = Geometry::shape_outline_paths(ring, 1);

    ASSERT_EQ(paths.size(), 2u); // outer boundary + the hole
}

TEST(Geometry, OutlinePathsRestrokeInputPaths)
{
    const Shape shape{.paths = {Path{.width = 4, .polygon = Polygon{.points = {{0, 0}, {50, 0}}}}}};
    const std::vector<Path> paths = Geometry::shape_outline_paths(shape, 10);

    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0].width, 10);
    ASSERT_EQ(paths[0].polygon.points.size(), 2u);
    expect_point_eq(paths[0].polygon.points[1], {50, 0});
}

// --- Corner cases: paths as inputs, corner-touching shapes, shrinks that
// split a shape, multiple holes and an island inside a hole ---

namespace
{
    std::vector<Rect> sorted_rects(std::vector<Rect> rects)
    {
        std::sort(rects.begin(), rects.end(), [](const Rect &a, const Rect &b)
                  { return std::tie(a.ll.x, a.ll.y, a.ur.x, a.ur.y) < std::tie(b.ll.x, b.ll.y, b.ur.x, b.ur.y); });
        return rects;
    }

    // A width-2 closed path round the square (0,0)-(20,20): stroked, a
    // band from -1 to 21 with a hole from 1 to 19.
    Shape closed_square_path()
    {
        return Shape{.paths = {Path{.width = 2, .polygon = Polygon{.points = {{0, 0}, {20, 0}, {20, 20}, {0, 20}, {0, 0}}}}}};
    }

    // Four frame rects round (0,0)-(30,30) leaving a hole (5,5)-(25,25),
    // plus an island (12,12)-(18,18) inside that hole.
    Shape frame_with_island()
    {
        return rect_shape({
            Rect{.ll = {0, 0}, .ur = {30, 5}},
            Rect{.ll = {0, 25}, .ur = {30, 30}},
            Rect{.ll = {0, 5}, .ur = {5, 25}},
            Rect{.ll = {25, 5}, .ur = {30, 25}},
            Rect{.ll = {12, 12}, .ur = {18, 18}},
        });
    }
}

TEST(Geometry, BooleanTreatsAPathAsItsStrokedArea)
{
    // Width 4, so each free end is extended by 2 (square caps): the stroke
    // covers (-2,-2)-(22,2).
    const Shape path{.paths = {Path{.width = 4, .polygon = Polygon{.points = {{0, 0}, {20, 0}}}}}};
    const Shape window = rect_shape({Rect{.ll = {0, -10}, .ur = {10, 10}}});
    const AreaGeometry result = Geometry::boolean_shapes({&path}, {&window}, BooleanOp::And);

    ASSERT_EQ(result.rects.size(), 1u);
    expect_rect(result.rects[0], 0, -2, 10, 2);
}

TEST(Geometry, AClosedPathKeepsItsHole)
{
    // path_to_polygons (outer rings only) would fill the hole in; the
    // shape_* operations must not.
    const Shape ring = closed_square_path();
    EXPECT_EQ(total_area(Geometry::shape_to_rects(ring, FractureDirection::Horizontal)), 22 * 22 - 18 * 18);

    const Shape inside_hole = rect_shape({Rect{.ll = {5, 5}, .ur = {15, 15}}});
    EXPECT_TRUE(Geometry::boolean_shapes({&ring}, {&inside_hole}, BooleanOp::And).empty());
}

TEST(Geometry, SizeGrowsAPathsStrokedArea)
{
    const Shape path{.paths = {Path{.width = 4, .polygon = Polygon{.points = {{0, 0}, {20, 0}}}}}};
    const auto result = Geometry::size_shape(path, 1, 1);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->rects.size(), 1u);
    expect_rect(result->rects[0], -3, -3, 23, 3);
}

TEST(Geometry, ShapesTouchingOnlyAtACornerStayTwoRegions)
{
    const Shape a = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}});
    const Shape b = rect_shape({Rect{.ll = {10, 10}, .ur = {20, 20}}});

    const AreaGeometry merged = Geometry::boolean_shapes({&a}, {&b}, BooleanOp::Or);
    EXPECT_EQ(total_area(merged), 200);
    for (const Polygon &polygon : merged.polygons)
        EXPECT_GT(polygon_area(polygon), 0);

    const Shape both = rect_shape({a.rects[0], b.rects[0]});
    const std::vector<Rect> rects = sorted_rects(Geometry::shape_to_rects(both, FractureDirection::Horizontal));
    ASSERT_EQ(rects.size(), 2u);
    expect_rect(rects[0], 0, 0, 10, 10);
    expect_rect(rects[1], 10, 10, 20, 20);
}

TEST(Geometry, CornerTouchingShapesHaveNoOverlapAndNothingToSubtract)
{
    const Shape a = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}});
    const Shape b = rect_shape({Rect{.ll = {10, 10}, .ur = {20, 20}}});

    EXPECT_TRUE(Geometry::boolean_shapes({&a}, {&b}, BooleanOp::And).empty());

    const AreaGeometry difference = Geometry::boolean_shapes({&a}, {&b}, BooleanOp::Not);
    ASSERT_EQ(difference.rects.size(), 1u);
    expect_rect(difference.rects[0], 0, 0, 10, 10);
}

TEST(Geometry, ShrinkingAwayANarrowNeckSplitsTheShape)
{
    // Two 10x10 blocks joined by a 2-high bar - shrinking by 2 removes the
    // bar entirely, leaving two separate, independently shrunk blocks.
    const Shape dumbbell = rect_shape({
        Rect{.ll = {0, 0}, .ur = {10, 10}},
        Rect{.ll = {10, 4}, .ur = {30, 6}},
        Rect{.ll = {30, 0}, .ur = {40, 10}},
    });
    const auto result = Geometry::size_shape(dumbbell, -2, -2);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->polygons.empty());
    const std::vector<Rect> rects = sorted_rects(result->rects);
    ASSERT_EQ(rects.size(), 2u);
    expect_rect(rects[0], 2, 2, 8, 8);
    expect_rect(rects[1], 32, 2, 38, 8);
}

TEST(Geometry, ShrinkingOnlyInYAlsoSplitsTheShape)
{
    const Shape dumbbell = rect_shape({
        Rect{.ll = {0, 0}, .ur = {10, 10}},
        Rect{.ll = {10, 4}, .ur = {30, 6}},
        Rect{.ll = {30, 0}, .ur = {40, 10}},
    });
    const auto result = Geometry::size_shape(dumbbell, 0, -2);

    ASSERT_TRUE(result.has_value());
    const std::vector<Rect> rects = sorted_rects(result->rects);
    ASSERT_EQ(rects.size(), 2u);
    expect_rect(rects[0], 0, 2, 10, 8);
    expect_rect(rects[1], 30, 2, 40, 8);
}

TEST(Geometry, BooleanNotLeavingTwoHolesIsExactRects)
{
    const Shape big = rect_shape({Rect{.ll = {0, 0}, .ur = {50, 20}}});
    const Shape holes = rect_shape({Rect{.ll = {10, 5}, .ur = {20, 15}}, Rect{.ll = {30, 5}, .ur = {40, 15}}});
    const AreaGeometry result = Geometry::boolean_shapes({&big}, {&holes}, BooleanOp::Not);

    EXPECT_TRUE(result.polygons.empty());
    EXPECT_EQ(result.rects.size(), 5u); // bottom band, three pieces between/around the holes, top band
    EXPECT_EQ(total_area(result), 1000 - 2 * 100);
}

TEST(Geometry, ShapeToRectsHandlesAnIslandInsideAHole)
{
    const std::vector<Rect> rects = sorted_rects(Geometry::shape_to_rects(frame_with_island(), FractureDirection::Horizontal));

    // Bottom band, left and right columns (each merged back across the
    // island's own cut lines), the island, top band.
    ASSERT_EQ(rects.size(), 5u);
    expect_rect(rects[0], 0, 0, 30, 5);
    expect_rect(rects[1], 0, 5, 5, 25);
    expect_rect(rects[2], 0, 25, 30, 30);
    expect_rect(rects[3], 12, 12, 18, 18);
    expect_rect(rects[4], 25, 5, 30, 25);
    EXPECT_EQ(total_area(rects), 900 - 400 + 36);
}

TEST(Geometry, ShapeToPolygonsKeepsAnIslandInsideAHole)
{
    int64_t area = 0;
    for (const Polygon &polygon : Geometry::shape_to_polygons(frame_with_island()))
        area += polygon_area(polygon);
    EXPECT_EQ(area, 900 - 400 + 36);
}

TEST(Geometry, OutlinePathsFollowOuterEdgeHoleAndIsland)
{
    EXPECT_EQ(Geometry::shape_outline_paths(frame_with_island(), 1).size(), 3u);
}

TEST(Geometry, BooleanHandlesComponentsWithOnlyOneSidePresent)
{
    // a1 overlaps b1; a2 and b2 each sit alone in their own component, so
    // the per-component shortcut decides them without any overlay: OR
    // keeps both, AND drops both, NOT keeps a2 but not b2.
    const Shape a = rect_shape({Rect{.ll = {0, 0}, .ur = {10, 10}}, Rect{.ll = {100, 0}, .ur = {110, 10}}});
    const Shape b = rect_shape({Rect{.ll = {5, 0}, .ur = {15, 10}}, Rect{.ll = {200, 0}, .ur = {210, 10}}});

    EXPECT_EQ(total_area(Geometry::boolean_shapes({&a}, {&b}, BooleanOp::Or)), 150 + 100 + 100);
    EXPECT_EQ(total_area(Geometry::boolean_shapes({&a}, {&b}, BooleanOp::And)), 50);

    const std::vector<Rect> difference = sorted_rects(Geometry::boolean_shapes({&a}, {&b}, BooleanOp::Not).rects);
    ASSERT_EQ(difference.size(), 2u);
    expect_rect(difference[0], 0, 0, 5, 10);
    expect_rect(difference[1], 100, 0, 110, 10);
}

TEST(Geometry, ShapeToRectsOfDiagonalGeometryOverCoversIt)
{
    // Not rectilinear, so the exact sweep doesn't apply - the intersection
    // fallback approximates each strip by its bbox: never less than the
    // triangle, never beyond its bbox.
    const Shape triangle{.polygons = {Polygon{.points = {{0, 0}, {100, 0}, {0, 100}, {0, 0}}}}};
    const std::vector<Rect> rects = Geometry::shape_to_rects(triangle, FractureDirection::Horizontal);

    ASSERT_FALSE(rects.empty());
    EXPECT_GE(total_area(rects), 100 * 100 / 2);
    for (const Rect &r : rects)
    {
        EXPECT_GE(r.ll.x, 0);
        EXPECT_GE(r.ll.y, 0);
        EXPECT_LE(r.ur.x, 100);
        EXPECT_LE(r.ur.y, 100);
    }
}
