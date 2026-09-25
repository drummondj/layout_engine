#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "fin_grid.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

namespace le
{
    /// @brief What a resized edge/segment snaps to (NEW_FEATURES_SEPT_2026.md
    /// item 3) - values are the C API's own LE_SHAPE_SNAP_* numbering.
    /// Rects and polygons offer NONE/USER_GRID/MANUFACTURING_GRID/FIN_GRID;
    /// paths offer NONE/USER_GRID/MANUFACTURING_GRID (their *edges* - the
    /// centerline +/- half the width - land on it)/TRACKS (the centerline
    /// lands on a routing track of the path's layer). See shape_snap_mode_applies.
    enum class ShapeSnapMode : int32_t
    {
        NONE = 0,
        USER_GRID = 1,
        MANUFACTURING_GRID = 2,
        FIN_GRID = 3,
        TRACKS = 4,
    };

    inline bool shape_snap_mode_applies(PieceKind kind, ShapeSnapMode mode)
    {
        switch (mode)
        {
        case ShapeSnapMode::NONE:
        case ShapeSnapMode::USER_GRID:
        case ShapeSnapMode::MANUFACTURING_GRID:
            return true;
        case ShapeSnapMode::FIN_GRID:
            return kind != PieceKind::PATH;
        case ShapeSnapMode::TRACKS:
            return kind == PieceKind::PATH;
        }
        return false; // unreachable
    }

    /// @brief One family of routing tracks: lines at `start + k * step`
    /// (0 <= k < count, or any integer k if count <= 0 - an unbounded
    /// pitch grid) - vertical lines (constant x) when `vertical`, else
    /// horizontal ones. DEF TRACKS X is vertical, TRACKS Y horizontal.
    struct TrackGrid
    {
        bool vertical = false;
        int64_t start = 0;
        int64_t step = 0;
        int count = 0;

        std::optional<int64_t> nearest(int64_t v) const
        {
            if (step <= 0)
                return std::nullopt;
            int64_t k = std::llround(static_cast<double>(v - start) / static_cast<double>(step));
            if (count > 0)
                k = std::clamp<int64_t>(k, 0, count - 1);
            return start + k * step;
        }
    };

    /// @brief Everything a resize snaps against, resolved once per gesture
    /// frame by the caller (api.cpp) - `tracks` only for a path piece: its
    /// own layer's tracks.
    struct ShapeSnapContext
    {
        ShapeSnapMode mode = ShapeSnapMode::NONE;
        int64_t user_grid = 0;
        std::optional<int64_t> manufacturing_grid;
        std::optional<FinGrid> fin_grid;
        std::vector<TrackGrid> tracks;

        /// A rect/polygon coordinate on the x (`vertical_edge`) or y axis.
        /// FIN_GRID snaps the axis across the fins to the fin grid and the
        /// other to the manufacturing grid (the Placement Move rule).
        int64_t snap(int64_t v, bool x_axis) const
        {
            switch (mode)
            {
            case ShapeSnapMode::USER_GRID:
                return snap_to_grid_value(v, user_grid, 0);
            case ShapeSnapMode::MANUFACTURING_GRID:
                return manufacturing_grid ? snap_to_grid_value(v, *manufacturing_grid, 0) : v;
            case ShapeSnapMode::FIN_GRID:
                if (fin_grid && fin_grid->horizontal != x_axis)
                    return snap_to_grid_value(v, fin_grid->pitch, fin_grid->offset);
                return manufacturing_grid ? snap_to_grid_value(v, *manufacturing_grid, 0) : v;
            case ShapeSnapMode::NONE:
            case ShapeSnapMode::TRACKS:
                return v;
            }
            return v;
        }

        /// A path centerline coordinate on the x (`x_axis`) or y axis, for
        /// a path of `width`.
        int64_t snap_path_center(int64_t v, bool x_axis, int64_t width) const
        {
            switch (mode)
            {
            case ShapeSnapMode::USER_GRID:
                return snap_to_grid_value(v, user_grid, 0);
            case ShapeSnapMode::MANUFACTURING_GRID:
            {
                if (!manufacturing_grid)
                    return v;
                const int64_t half = width / 2;
                return snap_to_grid_value(v - half, *manufacturing_grid, 0) + half;
            }
            case ShapeSnapMode::TRACKS:
            {
                // A centerline at constant x sits on a vertical track.
                std::optional<int64_t> best;
                for (const TrackGrid &grid : tracks)
                    if (grid.vertical == x_axis)
                        if (const auto t = grid.nearest(v); t && (!best || std::llabs(*t - v) < std::llabs(*best - v)))
                            best = t;
                return best.value_or(v);
            }
            case ShapeSnapMode::NONE:
            case ShapeSnapMode::FIN_GRID:
                return v;
            }
            return v;
        }

    private:
        static int64_t snap_to_grid_value(int64_t v, int64_t pitch, int64_t offset)
        {
            if (pitch <= 0)
                return v;
            return offset + static_cast<int64_t>(std::llround(static_cast<double>(v - offset) / static_cast<double>(pitch))) * pitch;
        }
    };

    /// @brief The part of a one-piece Shape a resize drags: a rect's edge
    /// (0 left, 1 right, 2 bottom, 3 top), or a polygon edge / path segment
    /// `edge` (points[edge] -> points[edge + 1], wrapping for a polygon).
    struct ResizeHandle
    {
        PieceKind kind = PieceKind::RECT;
        size_t edge = 0;
        double distance = 0.0; // from the query point, in dbu - find_resize_handle only
    };

    namespace shape_resize_detail
    {
        inline double segment_distance(Point p, Point a, Point b)
        {
            const double dx = static_cast<double>(b.x - a.x);
            const double dy = static_cast<double>(b.y - a.y);
            const double len2 = dx * dx + dy * dy;
            double t = 0.0;
            if (len2 > 0.0)
                t = std::clamp((static_cast<double>(p.x - a.x) * dx + static_cast<double>(p.y - a.y) * dy) / len2, 0.0, 1.0);
            const double cx = static_cast<double>(a.x) + t * dx - static_cast<double>(p.x);
            const double cy = static_cast<double>(a.y) + t * dy - static_cast<double>(p.y);
            return std::sqrt(cx * cx + cy * cy);
        }

        // A polygon's distinct vertex count - LEF/DEF polygons may or may
        // not repeat the first point at the end.
        inline size_t unique_point_count(const Polygon &polygon)
        {
            const auto &pts = polygon.points;
            if (pts.size() > 1 && pts.front().x == pts.back().x && pts.front().y == pts.back().y)
                return pts.size() - 1;
            return pts.size();
        }

        // Moves segment a->b (an axis-aligned one only across its own axis,
        // so it stays axis-aligned) by `delta`, snapping the moved
        // coordinate through `snap_axis(value, x_axis)`.
        template <typename SnapAxis>
        void move_segment(Point &a, Point &b, Point delta, SnapAxis snap_axis)
        {
            if (a.y == b.y && a.x != b.x) // horizontal: moves in y only
            {
                const int64_t y = snap_axis(a.y + delta.y, false);
                a.y = y;
                b.y = y;
            }
            else if (a.x == b.x && a.y != b.y) // vertical: moves in x only
            {
                const int64_t x = snap_axis(a.x + delta.x, true);
                a.x = x;
                b.x = x;
            }
            else // diagonal (or degenerate): both ends by the snapped delta
            {
                const Point snapped{.x = snap_axis(a.x + delta.x, true) - a.x, .y = snap_axis(a.y + delta.y, false) - a.y};
                a = Point{.x = a.x + snapped.x, .y = a.y + snapped.y};
                b = Point{.x = b.x + snapped.x, .y = b.y + snapped.y};
            }
        }
    }

    /// @brief The resize handle of one-piece `piece` nearest `p`, within
    /// `tolerance` dbu - a rect edge (within its own span), a polygon edge,
    /// or a path segment (anywhere on the segment: within half its width,
    /// or `tolerance` if larger). nullopt if nothing is that close.
    inline std::optional<ResizeHandle> find_resize_handle(const Shape &piece, Point p, int64_t tolerance)
    {
        using shape_resize_detail::segment_distance;
        std::optional<ResizeHandle> best;
        double best_distance = std::numeric_limits<double>::infinity();
        const auto consider = [&](ResizeHandle h, double d, double limit)
        {
            if (d <= limit && d < best_distance)
            {
                best = h;
                best->distance = d;
                best_distance = d;
            }
        };

        const double tol = static_cast<double>(tolerance);
        if (!piece.rects.empty())
        {
            const Rect &r = piece.rects.front();
            const Point ll = r.ll, lr{r.ur.x, r.ll.y}, ul{r.ll.x, r.ur.y}, ur = r.ur;
            consider({PieceKind::RECT, 0}, segment_distance(p, ll, ul), tol);
            consider({PieceKind::RECT, 1}, segment_distance(p, lr, ur), tol);
            consider({PieceKind::RECT, 2}, segment_distance(p, ll, lr), tol);
            consider({PieceKind::RECT, 3}, segment_distance(p, ul, ur), tol);
        }
        else if (!piece.polygons.empty())
        {
            const Polygon &polygon = piece.polygons.front();
            const size_t n = shape_resize_detail::unique_point_count(polygon);
            for (size_t i = 0; n >= 2 && i < n; ++i)
                consider({PieceKind::POLYGON, i}, segment_distance(p, polygon.points[i], polygon.points[(i + 1) % n]), tol);
        }
        else if (!piece.paths.empty())
        {
            const Path &path = piece.paths.front();
            const double limit = std::max(tol, static_cast<double>(path.width) / 2.0);
            for (size_t i = 0; i + 1 < path.polygon.points.size(); ++i)
                consider({PieceKind::PATH, i}, segment_distance(p, path.polygon.points[i], path.polygon.points[i + 1]), limit);
        }
        return best;
    }

    /// @brief Which way a handle moves when dragged - for the hover cursor
    /// (NEW_FEATURES_SEPT_2026.md item 3): across x (a vertical edge), across
    /// y (a horizontal one), or both (a diagonal polygon edge/path segment).
    enum class ResizeAxis
    {
        X,
        Y,
        BOTH,
    };

    /// @brief `handle`'s own segment in `piece` (one-piece) - what the hover
    /// indicator highlights - plus the axis it moves along. nullopt if the
    /// handle doesn't address a real edge/segment of `piece`.
    struct ResizeHandleSegment
    {
        Point a;
        Point b;
        ResizeAxis axis = ResizeAxis::BOTH;
    };

    inline std::optional<ResizeHandleSegment> resize_handle_segment(const Shape &piece, ResizeHandle handle)
    {
        const auto classify = [](Point a, Point b)
        {
            if (a.y == b.y && a.x != b.x)
                return ResizeAxis::Y;
            if (a.x == b.x && a.y != b.y)
                return ResizeAxis::X;
            return ResizeAxis::BOTH;
        };
        switch (handle.kind)
        {
        case PieceKind::RECT:
        {
            if (piece.rects.empty() || handle.edge > 3)
                return std::nullopt;
            const Rect &r = piece.rects.front();
            switch (handle.edge)
            {
            case 0:
                return ResizeHandleSegment{r.ll, Point{r.ll.x, r.ur.y}, ResizeAxis::X};
            case 1:
                return ResizeHandleSegment{Point{r.ur.x, r.ll.y}, r.ur, ResizeAxis::X};
            case 2:
                return ResizeHandleSegment{r.ll, Point{r.ur.x, r.ll.y}, ResizeAxis::Y};
            default:
                return ResizeHandleSegment{Point{r.ll.x, r.ur.y}, r.ur, ResizeAxis::Y};
            }
        }
        case PieceKind::POLYGON:
        {
            if (piece.polygons.empty())
                return std::nullopt;
            const Polygon &polygon = piece.polygons.front();
            const size_t n = shape_resize_detail::unique_point_count(polygon);
            if (n < 2 || handle.edge >= n)
                return std::nullopt;
            const Point a = polygon.points[handle.edge];
            const Point b = polygon.points[(handle.edge + 1) % n];
            return ResizeHandleSegment{a, b, classify(a, b)};
        }
        case PieceKind::PATH:
        {
            if (piece.paths.empty() || handle.edge + 1 >= piece.paths.front().polygon.points.size())
                return std::nullopt;
            const Point a = piece.paths.front().polygon.points[handle.edge];
            const Point b = piece.paths.front().polygon.points[handle.edge + 1];
            return ResizeHandleSegment{a, b, classify(a, b)};
        }
        case PieceKind::VIA: // a via or via array has no edges to resize
        case PieceKind::VIA_ITERATE:
            return std::nullopt;
        }
        return std::nullopt;
    }

    /// @brief `piece` (one-piece) with `handle` dragged by `delta` (dbu),
    /// the moved coordinate snapped per `snap`:
    ///  - a rect edge moves across its own axis (the rect is renormalized
    ///    if dragged past its opposite edge);
    ///  - a polygon edge moves as a whole - an axis-aligned one only across
    ///    its own axis, so a rectilinear polygon stays rectilinear - and its
    ///    two neighbours stretch to follow;
    ///  - a path segment likewise, both of its points moving, so the
    ///    adjacent segments stretch; its snap applies to the centerline
    ///    (ShapeSnapContext::snap_path_center).
    inline Shape resize_piece(const Shape &piece, ResizeHandle handle, Point delta, const ShapeSnapContext &snap)
    {
        Shape out = piece;
        switch (handle.kind)
        {
        case PieceKind::RECT:
        {
            if (out.rects.empty())
                break;
            Rect &r = out.rects.front();
            switch (handle.edge)
            {
            case 0:
                r.ll.x = snap.snap(r.ll.x + delta.x, true);
                break;
            case 1:
                r.ur.x = snap.snap(r.ur.x + delta.x, true);
                break;
            case 2:
                r.ll.y = snap.snap(r.ll.y + delta.y, false);
                break;
            default:
                r.ur.y = snap.snap(r.ur.y + delta.y, false);
                break;
            }
            r = Rect{.ll = {std::min(r.ll.x, r.ur.x), std::min(r.ll.y, r.ur.y)}, .ur = {std::max(r.ll.x, r.ur.x), std::max(r.ll.y, r.ur.y)}};
            break;
        }
        case PieceKind::POLYGON:
        {
            if (out.polygons.empty())
                break;
            Polygon &polygon = out.polygons.front();
            const size_t n = shape_resize_detail::unique_point_count(polygon);
            if (n < 2 || handle.edge >= n)
                break;
            const bool closed_duplicate = n < polygon.points.size();
            Point &a = polygon.points[handle.edge];
            Point &b = polygon.points[(handle.edge + 1) % n];
            shape_resize_detail::move_segment(a, b, delta, [&](int64_t v, bool x_axis)
                                              { return snap.snap(v, x_axis); });
            if (closed_duplicate)
                polygon.points.back() = polygon.points.front();
            break;
        }
        case PieceKind::PATH:
        {
            if (out.paths.empty())
                break;
            Path &path = out.paths.front();
            if (handle.edge + 1 >= path.polygon.points.size())
                break;
            shape_resize_detail::move_segment(path.polygon.points[handle.edge], path.polygon.points[handle.edge + 1], delta,
                                              [&](int64_t v, bool x_axis)
                                              { return snap.snap_path_center(v, x_axis, path.width); });
            break;
        }
        case PieceKind::VIA: // a via or via array has no edges to resize
        case PieceKind::VIA_ITERATE:
            break;
        }
        return out;
    }

    /// @brief Keeps a moved path segment connected to the rest of its
    /// route: a DEF route stores each wire run as its own Path, so the
    /// runs meeting the moved segment are separate pieces that merely
    /// share its endpoint coordinates. Every point of every path in
    /// `shape` - except piece `skip` (the segment's own path, already
    /// moved) - sitting exactly on the segment's original endpoint `a_from`
    /// / `b_from` moves to `a_to` / `b_to`, stretching that run to follow.
    /// Returns the indices of the paths that changed.
    inline std::vector<size_t> follow_moved_path_segment(Shape &shape, std::optional<size_t> skip, Point a_from, Point a_to, Point b_from, Point b_to)
    {
        const auto same = [](Point p, Point q)
        { return p.x == q.x && p.y == q.y; };
        std::vector<size_t> changed;
        for (size_t i = 0; i < shape.paths.size(); ++i)
        {
            if (skip && *skip == i)
                continue;
            bool touched = false;
            for (Point &point : shape.paths[i].polygon.points)
            {
                if (same(point, a_from))
                {
                    point = a_to;
                    touched = true;
                }
                else if (same(point, b_from))
                {
                    point = b_to;
                    touched = true;
                }
            }
            if (touched)
                changed.push_back(i);
        }
        return changed;
    }

    /// @brief Replaces piece `index` of `kind` in `data` with one-piece
    /// `piece`'s own geometry. A no-op if the index is out of range or
    /// `piece` has no geometry of that kind.
    inline void replace_piece(Shape &data, PieceKind kind, size_t index, const Shape &piece)
    {
        switch (kind)
        {
        case PieceKind::RECT:
            if (index < data.rects.size() && !piece.rects.empty())
                data.rects[index] = piece.rects.front();
            break;
        case PieceKind::POLYGON:
            if (index < data.polygons.size() && !piece.polygons.empty())
                data.polygons[index] = piece.polygons.front();
            break;
        case PieceKind::PATH:
            if (index < data.paths.size() && !piece.paths.empty())
                data.paths[index] = piece.paths.front();
            break;
        case PieceKind::VIA:
            if (index < data.vias.size() && !piece.vias.empty())
                data.vias[index] = piece.vias.front();
            break;
        case PieceKind::VIA_ITERATE:
            if (index < data.via_iterates.size() && !piece.via_iterates.empty())
                data.via_iterates[index] = piece.via_iterates.front();
            break;
        }
    }

    /// @brief The routing tracks of `layer_name` in `layout_id` (DEF TRACKS
    /// naming that layer), else - an Abstract view, or a Layout with no
    /// tracks for it - a synthetic grid from the Layer's own LEF PITCH/
    /// OFFSET (offset defaulting to half the pitch, LEF's own default) in
    /// both directions.
    inline std::vector<TrackGrid> layer_track_grids(const Root &root, LayoutId layout_id, LayerId layer_id)
    {
        std::vector<TrackGrid> grids;
        const LayerData *layer = root.get_layer(layer_id);
        if (!layer)
            return grids;
        if (layout_id.valid())
            for (const TrackId track_id : root.get_layout_tracks(layout_id))
                if (const TrackData *track = root.get_track(track_id))
                    if (std::ranges::find(track->layer_names, layer->name) != track->layer_names.end())
                        grids.push_back(TrackGrid{.vertical = track->is_x, .start = track->start, .step = track->step, .count = track->count});
        if (grids.empty() && layer->pitch && *layer->pitch > 0)
        {
            const int64_t offset = layer->offset.value_or(*layer->pitch / 2);
            grids.push_back(TrackGrid{.vertical = true, .start = offset, .step = *layer->pitch, .count = 0});
            grids.push_back(TrackGrid{.vertical = false, .start = offset, .step = *layer->pitch, .count = 0});
        }
        return grids;
    }
}
