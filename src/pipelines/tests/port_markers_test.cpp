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

TEST(PortMarkers, EnlargedIsNulloptWhenAlreadyBigEnough)
{
    // 40 dbu at 0.1 px/dbu is 4 px - over the 3 px minimum.
    const auto markers = port_marker_polygons(Rect{.ll = Point{0, 400}, .ur = Point{20, 440}}, kDie, SignalDirection::INPUT);
    EXPECT_FALSE(enlarged_port_marker(markers, 0.1).has_value());
}

TEST(PortMarkers, EnlargedGrowsToThreePixelsAboutTheAnchor)
{
    // 40 dbu at 0.01 px/dbu is 0.4 px - grown 7.5x to 3 px (300 dbu).
    const auto markers = port_marker_polygons(Rect{.ll = Point{0, 400}, .ur = Point{20, 440}}, kDie, SignalDirection::INPUT);
    const auto enlarged = enlarged_port_marker(markers, 0.01);
    ASSERT_TRUE(enlarged.has_value());
    ASSERT_EQ(enlarged->size(), 1u);
    const auto &pts = (*enlarged)[0].points;
    EXPECT_EQ(pts[0].x, 0); // the anchor stays put on the port's edge
    EXPECT_EQ(pts[0].y, 420);
    EXPECT_EQ(pts[1].x, -300); // still entirely outside the block, pointing in
    EXPECT_EQ(pts[1].y, 270);
    EXPECT_EQ(pts[2].x, -300);
    EXPECT_EQ(pts[2].y, 570);
}

TEST(PortMarkers, EnlargedInoutKeepsBothTrianglesTogether)
{
    const auto markers = port_marker_polygons(Rect{.ll = Point{200, 0}, .ur = Point{300, 50}}, kDie, SignalDirection::INOUT);
    // 100 dbu at 0.01 px/dbu is 1 px - grown 3x about (250, 0).
    const auto enlarged = enlarged_port_marker(markers, 0.01);
    ASSERT_TRUE(enlarged.has_value());
    EXPECT_EQ((*enlarged)[0].points[2].y, -195); // outward apex: -65 x 3
    EXPECT_EQ((*enlarged)[1].points[0].y, -105); // inward apex: -35 x 3
    EXPECT_EQ((*enlarged)[1].points[1].y, -300); // inward base: -100 x 3
    EXPECT_EQ((*enlarged)[1].points[1].x, 100);  // 200 -> 250 - 50 x 3
}
