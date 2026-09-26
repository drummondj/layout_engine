#pragma once
#include "../database/database.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace le
{
    /// @brief The direction marker for one PhysicalPort (DEF PIN) -
    /// NEW_FEATURES_SEPT_2026.md item 28's "portMarker" purpose: triangles
    /// just beyond the port's outer edge (the one facing away from the
    /// block's center), so they never cover the port itself.
    ///
    /// Orthogonal only: the port faces whichever side of `die` its center
    /// is nearest, and the marker points straight in (+ toward the block)
    /// or out along that side's normal. An input gets one triangle pointing
    /// in, an output (or tristate output) one pointing out; an inout - and
    /// a feedthru or unspecified direction, which could carry either way -
    /// two opposed triangles that point at each other and overlap. Each
    /// triangle's base spans the port's extent along the edge, and its
    /// depth equals that extent.
    ///
    /// `result[0].points[0]` is always the midpoint of the port's outer
    /// edge - the anchor `enlarged_port_marker` scales about, so a marker
    /// grown to its minimum on-screen size stays attached to its port.
    /// (An outward triangle carries that midpoint as a collinear extra
    /// vertex on its base.)
    ///
    /// Empty if the port or die has no area.
    inline std::vector<Polygon> port_marker_polygons(const Rect &port, const Rect &die, std::optional<SignalDirection> direction)
    {
        const int64_t port_w = port.ur.x - port.ll.x;
        const int64_t port_h = port.ur.y - port.ll.y;
        if (port_w <= 0 || port_h <= 0 || die.ur.x <= die.ll.x || die.ur.y <= die.ll.y)
            return {};

        // The side of the die nearest the port's center - its outward normal.
        const double cx = 0.5 * static_cast<double>(port.ll.x + port.ur.x);
        const double cy = 0.5 * static_cast<double>(port.ll.y + port.ur.y);
        const double to_left = cx - static_cast<double>(die.ll.x);
        const double to_right = static_cast<double>(die.ur.x) - cx;
        const double to_bottom = cy - static_cast<double>(die.ll.y);
        const double to_top = static_cast<double>(die.ur.y) - cy;
        const double nearest = std::min({to_left, to_right, to_bottom, to_top});
        int nx = 0;
        int ny = 0;
        if (nearest == to_left)
            nx = -1;
        else if (nearest == to_right)
            nx = 1;
        else if (nearest == to_bottom)
            ny = -1;
        else
            ny = 1;

        // Marker frame: `outer` is the coordinate of the port's outer edge
        // along the normal, [lo, hi] its extent along the edge.
        const bool horizontal_normal = nx != 0;
        const int64_t outer = horizontal_normal ? (nx < 0 ? port.ll.x : port.ur.x) : (ny < 0 ? port.ll.y : port.ur.y);
        const int64_t lo = horizontal_normal ? port.ll.y : port.ll.x;
        const int64_t hi = horizontal_normal ? port.ur.y : port.ur.x;
        const int64_t length = hi - lo;
        const int64_t mid = lo + length / 2;
        const int sign = horizontal_normal ? nx : ny;

        // A point `depth` outward from the outer edge, at `along` on the edge.
        auto at = [&](double depth, int64_t along) -> Point
        {
            const int64_t n = outer + static_cast<int64_t>(std::llround(sign * depth));
            return horizontal_normal ? Point{.x = n, .y = along} : Point{.x = along, .y = n};
        };
        // Triangles with their base `base_depth` out and apex `apex_depth`
        // out: pointing in lists the apex first, pointing out the base's
        // midpoint first.
        auto pointing_in = [&](double base_depth, double apex_depth) -> Polygon
        {
            return Polygon{.points = {at(apex_depth, mid), at(base_depth, lo), at(base_depth, hi)}};
        };
        auto pointing_out = [&](double base_depth, double apex_depth) -> Polygon
        {
            return Polygon{.points = {at(base_depth, mid), at(base_depth, hi), at(apex_depth, mid), at(base_depth, lo)}};
        };

        const double depth = static_cast<double>(length);
        const SignalDirection d = direction.value_or(SignalDirection::NONE);
        if (d == SignalDirection::INPUT)
            return {pointing_in(depth, 0.0)}; // apex at the port
        if (d == SignalDirection::OUTPUT || d == SignalDirection::OUTPUT_TRISTATE)
            return {pointing_out(0.0, depth)}; // base on the port
        // Both ways: an outward-pointing triangle against the port and an
        // inward-pointing one beyond it, apexes past each other.
        return {pointing_out(0.0, 0.65 * depth), pointing_in(depth, 0.35 * depth)};
    }

    /// @brief The smallest a port marker is drawn, in device pixels, on
    /// each side of its bbox - so markers stay visible zoomed out, after
    /// the ports themselves are sub-pixel.
    inline constexpr double kMinPortMarkerPixelSize = 3.0;

    /// @brief One port's marker (`port_marker_polygons`' output) scaled
    /// up about its anchor (`polygons[0].points[0]`) until its bbox is at
    /// least `min_px` device pixels on each side at `scale` (pixels per
    /// dbu) - std::nullopt when it already is (or is empty).
    inline std::optional<std::vector<Polygon>> enlarged_port_marker(const std::vector<Polygon> &polygons, double scale, double min_px = kMinPortMarkerPixelSize)
    {
        if (polygons.empty() || polygons.front().points.empty() || scale <= 0.0)
            return std::nullopt;
        int64_t min_x = polygons.front().points.front().x, max_x = min_x;
        int64_t min_y = polygons.front().points.front().y, max_y = min_y;
        for (const Polygon &polygon : polygons)
            for (const Point &p : polygon.points)
            {
                min_x = std::min(min_x, p.x);
                max_x = std::max(max_x, p.x);
                min_y = std::min(min_y, p.y);
                max_y = std::max(max_y, p.y);
            }
        const double min_side_px = static_cast<double>(std::min(max_x - min_x, max_y - min_y)) * scale;
        if (min_side_px >= min_px)
            return std::nullopt;
        // A degenerate (zero-side) marker has no shape to scale up.
        if (min_side_px <= 0.0)
            return std::nullopt;

        const double factor = min_px / min_side_px;
        const Point anchor = polygons.front().points.front();
        std::vector<Polygon> enlarged = polygons;
        for (Polygon &polygon : enlarged)
            for (Point &p : polygon.points)
                p = Point{.x = anchor.x + static_cast<int64_t>(std::llround(static_cast<double>(p.x - anchor.x) * factor)),
                          .y = anchor.y + static_cast<int64_t>(std::llround(static_cast<double>(p.y - anchor.y) * factor))};
        return enlarged;
    }
}
