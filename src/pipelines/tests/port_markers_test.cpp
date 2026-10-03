#include "../port_markers.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    const Rect kDie{.ll = Point{0, 0}, .ur = Point{1000, 1000}};

    // The point of `triangle` farthest along the port's outward normal
    // `sign` on axis x (true) or y (false) - the apex when it points out,
    // the base when it points in.
    int64_t extreme(const Polygon &triangle, bool x, int sign)
    {
        int64_t best = sign > 0 ? INT64_MIN : INT64_MAX;
        for (const Point &p : triangle.points)
            best = sign > 0 ? std::max(best, x ? p.x : p.y) : std::min(best, x ? p.x : p.y);
        return best;
    }
}

TEST(PortMarkers, InputOnTheLeftEdgePointsRightTowardTheBlock)
{
    // Port (0,400)-(20,440): 40 tall along the left edge, outer edge x = 0.
    const auto markers = port_marker_polygons(Rect{.ll = Point{0, 400}, .ur = Point{20, 440}}, kDie, SignalDirection::INPUT);
    ASSERT_EQ(markers.size(), 1u);
    const auto &pts = markers[0].points;
    ASSERT_EQ(pts.size(), 3u);
    // Apex (listed first - the anchor) on the edge; base 40 out, spanning the port.
    EXPECT_EQ(pts[0].x, 0);
    EXPECT_EQ(pts[0].y, 420);
    EXPECT_EQ(pts[1].x, -40);
    EXPECT_EQ(pts[1].y, 400);
    EXPECT_EQ(pts[2].x, -40);
    EXPECT_EQ(pts[2].y, 440);
}

TEST(PortMarkers, OutputOnTheTopEdgePointsUpAwayFromTheBlock)
{
    const auto markers = port_marker_polygons(Rect{.ll = Point{500, 980}, .ur = Point{520, 1000}}, kDie, SignalDirection::OUTPUT);
    ASSERT_EQ(markers.size(), 1u);
    const auto &pts = markers[0].points;
    ASSERT_EQ(pts.size(), 4u); // the base's midpoint (the anchor) first, then the triangle
    EXPECT_EQ(pts[0].x, 510);
    EXPECT_EQ(pts[0].y, 1000);
    EXPECT_EQ(pts[1].y, 1000); // base on the outer edge
    EXPECT_EQ(pts[3].y, 1000);
    EXPECT_EQ(pts[2].x, 510);
    EXPECT_EQ(pts[2].y, 1020); // apex 20 beyond it
}

TEST(PortMarkers, TristateOutputPointsOutLikeAnOutput)
{
    const Rect port{.ll = Point{980, 100}, .ur = Point{1000, 130}};
    const auto markers = port_marker_polygons(port, kDie, SignalDirection::OUTPUT_TRISTATE);
    ASSERT_EQ(markers.size(), 1u);
    EXPECT_EQ(markers[0].points[2].x, 1030);
}

TEST(PortMarkers, InoutIsTwoOpposedOverlappingTrianglesOutsideThePort)
{
    // Bottom edge: outward normal is -y, extent 100 along x.
    const auto markers = port_marker_polygons(Rect{.ll = Point{200, 0}, .ur = Point{300, 50}}, kDie, SignalDirection::INOUT);
    ASSERT_EQ(markers.size(), 2u);
    const Polygon &outward = markers[0];
    const Polygon &inward = markers[1];
    EXPECT_EQ(outward.points[0].x, 250); // anchor: the outer edge's midpoint
    EXPECT_EQ(outward.points[0].y, 0);   // base on the outer edge
    EXPECT_EQ(outward.points[2].y, -65); // apex pointing out, past the other's
    EXPECT_EQ(inward.points[0].y, -35);  // apex pointing back in
    EXPECT_EQ(inward.points[1].y, -100); // base far out
    // Nothing overlaps the port itself.
    EXPECT_LE(extreme(inward, false, 1), 0);
    EXPECT_LE(extreme(outward, false, 1), 0);
}

TEST(PortMarkers, UnsetDirectionDrawsBothWays)
{
    EXPECT_EQ(port_marker_polygons(Rect{.ll = Point{200, 0}, .ur = Point{300, 50}}, kDie, std::nullopt).size(), 2u);
}

TEST(PortMarkers, EmptyForADegeneratePortOrDie)
{
    EXPECT_TRUE(port_marker_polygons(Rect{.ll = Point{0, 0}, .ur = Point{0, 10}}, kDie, SignalDirection::INPUT).empty());
    EXPECT_TRUE(port_marker_polygons(Rect{.ll = Point{0, 0}, .ur = Point{10, 10}}, Rect{}, SignalDirection::INPUT).empty());
}

TEST(PortMarkers, EveryDirectionAnchorsAtTheOuterEdgeMidpoint)
{
    const Rect port{.ll = Point{0, 400}, .ur = Point{20, 440}};
    for (auto direction : {std::optional{SignalDirection::INPUT}, std::optional{SignalDirection::OUTPUT}, std::optional{SignalDirection::INOUT}, std::optional<SignalDirection>{}})
    {
        const auto markers = port_marker_polygons(port, kDie, direction);
        ASSERT_FALSE(markers.empty());
        EXPECT_EQ(markers[0].points[0].x, 0);
        EXPECT_EQ(markers[0].points[0].y, 420);
    }
}

TEST(PortMarkers, ADieSpanningStripeGetsItsMarkerOnItsEnd)
{
    // A 20-wide stripe running the die's full height, nearer the left side
    // than the bottom by its center but touching both bottom and top: the
    // marker goes under its bottom end, 20 wide, not along its 1000 length.
    const auto markers = port_marker_polygons(Rect{.ll = Point{100, 0}, .ur = Point{120, 1000}}, kDie, SignalDirection::INPUT);
    ASSERT_EQ(markers.size(), 1u);
    const auto &pts = markers[0].points;
    EXPECT_EQ(pts[0].x, 110); // apex at the bottom end's midpoint
    EXPECT_EQ(pts[0].y, 0);
    EXPECT_EQ(pts[1].x, 100);
    EXPECT_EQ(pts[1].y, -20);
    EXPECT_EQ(pts[2].x, 120);
    EXPECT_EQ(pts[2].y, -20);
}

TEST(PortMarkers, TheSideNearestThePortsEdgeWinsOverItsCenter)
{
    // 10 from the bottom, 30 from the left by edge - but the left side is
    // nearer the center (40 vs 210).
    const auto markers = port_marker_polygons(Rect{.ll = Point{30, 10}, .ur = Point{50, 410}}, kDie, SignalDirection::OUTPUT);
    ASSERT_EQ(markers.size(), 1u);
    EXPECT_EQ(markers[0].points[0].x, 40);
    EXPECT_EQ(markers[0].points[0].y, 10);
    EXPECT_EQ(markers[0].points[2].y, -10); // apex 20 below
}

TEST(PortMarkers, ScaleFactorIsOneWhenAlreadyWithinRange)
{
    // 40 dbu at 0.1 px/dbu is 4 px - between the 3 px minimum and 16 px maximum.
    const auto markers = port_marker_polygons(Rect{.ll = Point{0, 400}, .ur = Point{20, 440}}, kDie, SignalDirection::INPUT);
    EXPECT_DOUBLE_EQ(port_marker_scale_factor(markers, 0.1), 1.0);
}

TEST(PortMarkers, ScaleFactorGrowsATinyMarkerToThreePixels)
{
    // 40 dbu at 0.01 px/dbu is 0.4 px - grown 7.5x to 3 px.
    const auto markers = port_marker_polygons(Rect{.ll = Point{0, 400}, .ur = Point{20, 440}}, kDie, SignalDirection::INPUT);
    EXPECT_DOUBLE_EQ(port_marker_scale_factor(markers, 0.01), 7.5);
}

TEST(PortMarkers, ScaleFactorShrinksALargeMarkerToSixteenPixels)
{
    // 40 dbu at 1 px/dbu is 40 px - shrunk to 16 px.
    const auto markers = port_marker_polygons(Rect{.ll = Point{0, 400}, .ur = Point{20, 440}}, kDie, SignalDirection::INPUT);
    EXPECT_DOUBLE_EQ(port_marker_scale_factor(markers, 1.0), 0.4);
    // Zoomed in far enough that 16 px is under one dbu.
    EXPECT_DOUBLE_EQ(port_marker_scale_factor(markers, 100.0), 16.0 / 4000.0);
}

TEST(PortMarkers, ScaleFactorCoversBothInoutTrianglesTogether)
{
    // The inout pair spans 100 dbu along the edge and 100 out from it.
    const auto markers = port_marker_polygons(Rect{.ll = Point{200, 0}, .ur = Point{300, 50}}, kDie, SignalDirection::INOUT);
    EXPECT_DOUBLE_EQ(port_marker_scale_factor(markers, 0.01), 3.0);
    EXPECT_DOUBLE_EQ(port_marker_scale_factor(markers, 1.0), 0.16);
}

TEST(PortMarkers, ScaleFactorPrefersTheMaximumWhenBothCantHold)
{
    // A 1:10 bbox can't be >= 3 px short and <= 16 px long at once.
    const std::vector<Polygon> sliver{Polygon{.points = {Point{0, 0}, Point{100, 0}, Point{100, 10}}}};
    EXPECT_DOUBLE_EQ(port_marker_scale_factor(sliver, 0.1), 1.6);
}

TEST(PortMarkers, ScaleFactorIsOneForEmptyOrDegenerateMarkers)
{
    EXPECT_DOUBLE_EQ(port_marker_scale_factor({}, 1.0), 1.0);
    const std::vector<Polygon> line{Polygon{.points = {Point{0, 0}, Point{100, 0}}}};
    EXPECT_DOUBLE_EQ(port_marker_scale_factor(line, 1.0), 1.0);
}
