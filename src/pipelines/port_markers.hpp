#pragma once
#include "../database/database.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace le
{
    /// @brief The direction marker for one PhysicalPort (DEF PIN) - the
    /// PORT_MARKER purpose: triangles
    /// just beyond the port's outer edge (the one facing away from the
    /// block's center), so they never cover the port itself.
    ///
    /// Orthogonal only: the port faces whichever side of `die` its own
    /// edge is nearest - on a tie (a stripe touching both ends of the
    /// die), the side along which the port is narrower, so a stripe's
    /// marker sits on its end, not along it - and the marker points
    /// straight in (+ toward the block)
    /// or out along that side's normal. An input gets one triangle pointing
    /// in, an output (or tristate output) one pointing out; an inout - and
    /// a feedthru or unspecified direction, which could carry either way -
    /// two opposed triangles that point at each other and overlap. Each
    /// triangle's base spans the port's extent along the edge, and its
    /// depth equals that extent.
    ///
    /// `result[0].points[0]` is always the midpoint of the port's outer
    /// edge - the anchor `port_marker_scale_factor`'s scaling is about, so
    /// a marker clamped to its on-screen size range stays attached to its
    /// port.
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

        // The side of the die nearest the port's edge - its outward normal.
        struct Side
        {
            int64_t distance;
            int64_t extent; // the port's extent along that side
            int nx, ny;
        };
        const Side sides[] = {
            {.distance = port.ll.x - die.ll.x, .extent = port_h, .nx = -1, .ny = 0},
            {.distance = die.ur.x - port.ur.x, .extent = port_h, .nx = 1, .ny = 0},
            {.distance = port.ll.y - die.ll.y, .extent = port_w, .nx = 0, .ny = -1},
            {.distance = die.ur.y - port.ur.y, .extent = port_w, .nx = 0, .ny = 1},
        };
        const Side &side = *std::ranges::min_element(sides, {}, [](const Side &s)
                                                     { return std::pair{s.distance, s.extent}; });
        const int nx = side.nx;
        const int ny = side.ny;

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

    /// @brief The largest a port marker is drawn, in device pixels, on
    /// each side of its bbox - so a wide port zoomed in doesn't get a
    /// marker that swamps the view.
    inline constexpr double kMaxPortMarkerPixelSize = 16.0;

    /// @brief The factor to scale one port's marker (`port_marker_polygons`'
    /// output) by, about its anchor (`polygons[0].points[0]`), so that at
    /// `scale` (pixels per dbu) its bbox's shorter side is at least `min_px`
    /// and its longer side at most `max_px` - the maximum wins if both
    /// can't hold. 1.0 if it already fits, or is empty or degenerate.
    inline double port_marker_scale_factor(std::span<const Polygon> polygons, double scale,
                                           double min_px = kMinPortMarkerPixelSize, double max_px = kMaxPortMarkerPixelSize)
    {
        if (polygons.empty() || polygons.front().points.empty() || scale <= 0.0)
            return 1.0;
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
        const double short_px = static_cast<double>(std::min(max_x - min_x, max_y - min_y)) * scale;
        const double long_px = static_cast<double>(std::max(max_x - min_x, max_y - min_y)) * scale;
        // A degenerate (zero-side) marker has no shape to scale.
        if (short_px <= 0.0)
            return 1.0;

        double factor = short_px < min_px ? min_px / short_px : 1.0;
        if (long_px * factor > max_px)
            factor = max_px / long_px;
        return factor;
    }
}
