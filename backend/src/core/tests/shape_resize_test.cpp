#include "core/shape_resize.hpp"

#include <gtest/gtest.h>

#include <ostream>

namespace le
{
    // Defined in placement_move_test.cpp (same test binary).
    bool operator==(const Point &a, const Point &b);
    bool operator==(const Rect &a, const Rect &b);
    void PrintTo(const Point &p, std::ostream *os);
}

using namespace le;

namespace
{
    Shape rect_piece(Rect r) { return Shape{.rects = {r}}; }

    ShapeSnapContext snap(ShapeSnapMode mode)
    {
        ShapeSnapContext context;
        context.mode = mode;
        context.user_grid = 100;
        context.manufacturing_grid = 5;
        context.fin_grid = FinGrid{.pitch = 48, .offset = 0, .horizontal = true};
        return context;
    }

    // An L: (0,0) (400,0) (400,200) (200,200) (200,400) (0,400).
    Polygon l_shape()
    {
        return Polygon{.points = {{0, 0}, {400, 0}, {400, 200}, {200, 200}, {200, 400}, {0, 400}}};
    }

    // A U-turn path: up, right, down - width 20.
    Path u_path()
    {
        return Path{.width = 20, .polygon = Polygon{.points = {{0, 0}, {0, 1000}, {1000, 1000}, {1000, 0}}}};
    }
}

TEST(ShapeResize, FindsTheNearestRectEdgeWithinTolerance)
{
    const Shape piece = rect_piece(Rect{.ll = {0, 0}, .ur = {1000, 500}});
    auto h = find_resize_handle(piece, Point{1003, 250}, 10);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->kind, PieceKind::RECT);
    EXPECT_EQ(h->edge, 1u); // right
    h = find_resize_handle(piece, Point{500, 498}, 10);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->edge, 3u); // top
    EXPECT_FALSE(find_resize_handle(piece, Point{500, 250}, 10)); // interior, far from every edge
    EXPECT_FALSE(find_resize_handle(piece, Point{1100, 250}, 10));
}

TEST(ShapeResize, APathSegmentIsGrabbedAnywhereWithinItsWidth)
{
    const Shape piece{.paths = {u_path()}};
    auto h = find_resize_handle(piece, Point{500, 1009}, 2); // inside the 20-wide top segment, beyond the 2-dbu tolerance
    ASSERT_TRUE(h);
    EXPECT_EQ(h->kind, PieceKind::PATH);
    EXPECT_EQ(h->edge, 1u);
    EXPECT_FALSE(find_resize_handle(piece, Point{500, 500}, 2));
}

TEST(ShapeResize, RectEdgeMovesAcrossItsAxisAndSnaps)
{
    const Shape piece = rect_piece(Rect{.ll = {0, 0}, .ur = {1000, 500}});
    const ResizeHandle right{PieceKind::RECT, 1};
    EXPECT_EQ(resize_piece(piece, right, Point{237, 999}, snap(ShapeSnapMode::NONE)).rects[0], (Rect{.ll = {0, 0}, .ur = {1237, 500}}));
    EXPECT_EQ(resize_piece(piece, right, Point{237, 0}, snap(ShapeSnapMode::USER_GRID)).rects[0], (Rect{.ll = {0, 0}, .ur = {1200, 500}}));
    EXPECT_EQ(resize_piece(piece, right, Point{237, 0}, snap(ShapeSnapMode::MANUFACTURING_GRID)).rects[0], (Rect{.ll = {0, 0}, .ur = {1235, 500}}));

    // FIN_GRID: horizontal fins - a y edge lands on the fin grid, an x edge
    // on the manufacturing grid.
    const ResizeHandle top{PieceKind::RECT, 3};
    EXPECT_EQ(resize_piece(piece, top, Point{0, 30}, snap(ShapeSnapMode::FIN_GRID)).rects[0], (Rect{.ll = {0, 0}, .ur = {1000, 528}}));
    EXPECT_EQ(resize_piece(piece, right, Point{237, 0}, snap(ShapeSnapMode::FIN_GRID)).rects[0], (Rect{.ll = {0, 0}, .ur = {1235, 500}}));
}

TEST(ShapeResize, ARectDraggedPastItsOppositeEdgeIsRenormalized)
{
    const Shape piece = rect_piece(Rect{.ll = {0, 0}, .ur = {1000, 500}});
    EXPECT_EQ(resize_piece(piece, ResizeHandle{PieceKind::RECT, 0}, Point{1500, 0}, snap(ShapeSnapMode::NONE)).rects[0],
              (Rect{.ll = {1000, 0}, .ur = {1500, 500}}));
}

TEST(ShapeResize, APolygonEdgeMovesWholeAndItsNeighboursStretch)
{
    // Edge 1 is the vertical (400,0)-(400,200): it moves in x only.
    const Shape piece{.polygons = {l_shape()}};
    const Shape out = resize_piece(piece, ResizeHandle{PieceKind::POLYGON, 1}, Point{130, 77}, snap(ShapeSnapMode::USER_GRID));
    EXPECT_EQ(out.polygons[0].points, (std::vector<Point>{{0, 0}, {500, 0}, {500, 200}, {200, 200}, {200, 400}, {0, 400}}));
}

TEST(ShapeResize, AClosedPolygonsRepeatedFirstPointFollowsItsEdge)
{
    Polygon closed = l_shape();
    closed.points.push_back(closed.points.front());
    // Edge 5 is (0,400)-(0,0) - the wrap-around edge, touching the repeat.
    const Shape out = resize_piece(Shape{.polygons = {closed}}, ResizeHandle{PieceKind::POLYGON, 5}, Point{-100, 0}, snap(ShapeSnapMode::NONE));
    EXPECT_EQ(out.polygons[0].points.front(), (Point{-100, 0}));
    EXPECT_EQ(out.polygons[0].points.back(), (Point{-100, 0}));
    EXPECT_EQ(out.polygons[0].points[5], (Point{-100, 400}));
}

TEST(ShapeResize, APathSegmentMovesWithItsAdjacentPoints)
{
    // The top segment (0,1000)-(1000,1000) moves in y; its neighbours stretch.
    const Shape piece{.paths = {u_path()}};
    const Shape out = resize_piece(piece, ResizeHandle{PieceKind::PATH, 1}, Point{55, 233}, snap(ShapeSnapMode::NONE));
    EXPECT_EQ(out.paths[0].polygon.points, (std::vector<Point>{{0, 0}, {0, 1233}, {1000, 1233}, {1000, 0}}));
    EXPECT_EQ(out.paths[0].width, 20);
}

TEST(ShapeResize, PathSnapsItsCenterlineToTracksOrItsEdgesToTheManufacturingGrid)
{
    const Shape piece{.paths = {u_path()}};
    const ResizeHandle top{PieceKind::PATH, 1};

    ShapeSnapContext tracks = snap(ShapeSnapMode::TRACKS);
    tracks.tracks = {TrackGrid{.vertical = false, .start = 50, .step = 140, .count = 20}, TrackGrid{.vertical = true, .start = 0, .step = 7, .count = 0}};
    // 1000 + 233 = 1233 -> nearest horizontal track 50 + 8*140 = 1170 (vs 1310).
    EXPECT_EQ(resize_piece(piece, top, Point{0, 233}, tracks).paths[0].polygon.points[1], (Point{0, 1170}));

    // Width 20: edges at centerline +/- 10 land on the 5-dbu grid -
    // centerline 1233 -> edge 1223 -> 1225 -> centerline 1235.
    EXPECT_EQ(resize_piece(piece, top, Point{0, 233}, snap(ShapeSnapMode::MANUFACTURING_GRID)).paths[0].polygon.points[1], (Point{0, 1235}));
    EXPECT_EQ(resize_piece(piece, top, Point{0, 233}, snap(ShapeSnapMode::USER_GRID)).paths[0].polygon.points[1], (Point{0, 1200}));
}

TEST(ShapeResize, SnapModesOfferedPerKind)
{
    EXPECT_TRUE(shape_snap_mode_applies(PieceKind::RECT, ShapeSnapMode::FIN_GRID));
    EXPECT_FALSE(shape_snap_mode_applies(PieceKind::RECT, ShapeSnapMode::TRACKS));
    EXPECT_TRUE(shape_snap_mode_applies(PieceKind::PATH, ShapeSnapMode::TRACKS));
    EXPECT_FALSE(shape_snap_mode_applies(PieceKind::PATH, ShapeSnapMode::FIN_GRID));
}

TEST(ShapeResize, LayerTrackGridsUseLayoutTracksElseTheLayersPitch)
{
    Root root;
    const TechnologyId tech = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
    LayerData m2{};
    m2.technology = tech;
    m2.name = "M2";
    m2.pitch = 200;
    const LayerId layer = root.create_layer(m2);

    // No Layout: the LEF PITCH grid, offset defaulting to half the pitch.
    auto grids = layer_track_grids(root, LayoutId{}, layer);
    ASSERT_EQ(grids.size(), 2u);
    EXPECT_EQ(grids[0].nearest(1234), 1300);

    const LibraryId library = root.create_library(LibraryData{.name = "LIB"});
    const LayoutId layout = root.create_layout(LayoutData{.design = root.create_design(DesignData{.library = library, .name = "TOP"})});
    TrackData track{};
    track.layout = layout;
    track.is_x = false;
    track.start = 70;
    track.count = 10;
    track.step = 140;
    track.layer_names = {"M1", "M2"};
    root.create_track(track);
    grids = layer_track_grids(root, layout, layer);
    ASSERT_EQ(grids.size(), 1u);
    EXPECT_FALSE(grids[0].vertical);
    EXPECT_EQ(grids[0].nearest(1234), 1190);  // 70 + 8*140
    EXPECT_EQ(grids[0].nearest(99999), 1330); // clamped to the last track, 70 + 9*140
}

TEST(ShapeResize, ReplacePieceSwapsOnlyThatPiece)
{
    Shape data{.rects = {Rect{.ll = {0, 0}, .ur = {1, 1}}, Rect{.ll = {5, 5}, .ur = {6, 6}}}};
    replace_piece(data, PieceKind::RECT, 1, rect_piece(Rect{.ll = {5, 5}, .ur = {9, 9}}));
    EXPECT_EQ(data.rects[0], (Rect{.ll = {0, 0}, .ur = {1, 1}}));
    EXPECT_EQ(data.rects[1], (Rect{.ll = {5, 5}, .ur = {9, 9}}));
}

TEST(ShapeResize, OtherPathRunsOnAMovedSegmentsEndpointsFollowIt)
{
    // A DEF route's three wire runs as separate paths: up, across, down.
    Shape shape{.paths = {
                    Path{.width = 20, .polygon = Polygon{.points = {{0, 0}, {0, 1000}}}},
                    Path{.width = 20, .polygon = Polygon{.points = {{0, 1000}, {1000, 1000}}}},
                    Path{.width = 20, .polygon = Polygon{.points = {{1000, 1000}, {1000, 0}}}},
                    Path{.width = 20, .polygon = Polygon{.points = {{5000, 5000}, {6000, 5000}}}},
                }};
    // Move the middle run up by 300 (already applied to piece 1).
    shape.paths[1].polygon.points = {{0, 1300}, {1000, 1300}};
    const std::vector<size_t> changed = follow_moved_path_segment(shape, 1, Point{0, 1000}, Point{0, 1300}, Point{1000, 1000}, Point{1000, 1300});
    EXPECT_EQ(changed, (std::vector<size_t>{0, 2}));
    EXPECT_EQ(shape.paths[0].polygon.points, (std::vector<Point>{{0, 0}, {0, 1300}}));
    EXPECT_EQ(shape.paths[2].polygon.points, (std::vector<Point>{{1000, 1300}, {1000, 0}}));
    EXPECT_EQ(shape.paths[3].polygon.points, (std::vector<Point>{{5000, 5000}, {6000, 5000}})); // unconnected - untouched
}
