#pragma once
#include "transforms.hpp"
#include <vector>

namespace le
{
    /// @brief The result of a piece-level hit-test (find_hit_piece) - the
    /// one rect/polygon/path (as its own one-piece Shape, matching
    /// find_hit_piece's own return convention) under the query point,
    /// plus `kind`/`index` identifying exactly where it lives in the
    /// owning Shape's own rects/polygons/paths vector - needed so a
    /// caller (LeHandle::select, Move) can address that same piece again
    /// later, not just look at a disconnected copy of its geometry.
    struct HitPiece
    {
        PieceKind kind;
        size_t index;
        ShapeData outline;
    };

    /// @brief Point and rect hit-tests against a Shape's individual rects/polygons/paths, and addressing one such piece.
    class PieceHitTest : public GeometryTransforms
    {
    public:
        /// @brief Like `contains`, but returns the single rect/polygon/path
        /// piece that contains `point` - both a copy of its geometry (as
        /// its own one-piece Shape, same `layer`) and `kind`/`index`
        /// identifying exactly where it lives in `shape`'s own rects/
        /// polygons/paths vector (see HitPiece) - `shape` can bundle
        /// several rects/polygons/paths together (e.g. several RECT
        /// statements in one LEF PORT, or an OBS's array of rects), and
        /// click/hover hit-testing must identify only the one piece
        /// actually under `point`, not the whole group. nullopt if no
        /// piece contains `point`.
        ///
        /// Polygons/paths are pre-checked against a cheap bbox (see
        /// point_in_rect above, and bbox_of for paths - already accounts
        /// for width) before the real `bg::within`/`path_to_polygons`
        /// test. Without it a point hit-test on a 1M-shape design costs
        /// ~17ms (dominated by path_to_polygons, a Boost.Geometry buffer
        /// operation, running on every visible path), too slow for every
        /// pointer-move event (see le_set_mouse_position). With it, most
        /// candidates are rejected by four integer comparisons before any
        /// Boost.Geometry call.
        static std::optional<HitPiece> find_hit_piece(const ShapeData &shape, const Point &point)
        {
            std::vector<HitPiece> hits = find_hit_pieces(shape, point, /*first_only=*/true);
            if (hits.empty())
                return std::nullopt;
            return std::move(hits.front());
        }

        /// @brief Whether `point` is strictly inside `path`'s area, exactly
        /// as path_to_polygons would say (the flat-ended, mitred buffer of
        /// its end-extended centerline), without buffering - for the common
        /// case of an open path of orthogonal, non-zero, non-reversing
        /// segments, whose area is then the union of each segment's
        /// rectangle (half the width either side, extended by half the
        /// width at both ends - llround'ed at the path's own two ends, as
        /// extend_path_ends_for_buffering does). A zero-width path buffers to
        /// nothing, so is never hit. nullopt for anything else (the caller
        /// buffers). Edges are truncated to integers, as
        /// path_to_polygons' corners are. Strictness: `point` must be in
        /// some rectangle and so must all 8 neighbours 0.25 away - exact,
        /// since every edge is then an integer.
        static std::optional<bool> point_in_orthogonal_path(const Path &path, const Point &point)
        {
            const std::vector<Point> &pts = path.polygon.points;
            if (path.width <= 0)
                return false; // buffers to nothing - never hit (real designs have 3000-point ones)
            if (pts.size() < 2 || (pts.front().x == pts.back().x && pts.front().y == pts.back().y))
                return std::nullopt;
            for (std::size_t i = 0; i + 1 < pts.size(); ++i)
            {
                const Point &a = pts[i];
                const Point &b = pts[i + 1];
                if ((a.x == b.x) == (a.y == b.y))
                    return std::nullopt; // diagonal or zero-length
                if (i + 2 < pts.size() && (b.x - a.x) * (pts[i + 2].x - b.x) + (b.y - a.y) * (pts[i + 2].y - b.y) < 0)
                    return std::nullopt; // reverses - the miter differs
            }

            // Segment i's rectangle, edges truncated to integers as
            // path_to_polygons' corners are (from_boost_polygon).
            const double half = static_cast<double>(path.width) / 2.0;
            const double end_extension = static_cast<double>(std::llround(half));
            auto box = [&](std::size_t i, double &x0, double &y0, double &x1, double &y1)
            {
                const Point &a = pts[i];
                const Point &b = pts[i + 1];
                const double ext_a = i == 0 ? end_extension : half;
                const double ext_b = i + 2 == pts.size() ? end_extension : half;
                if (a.y == b.y)
                {
                    const bool forward = a.x < b.x;
                    x0 = static_cast<double>(std::min(a.x, b.x)) - (forward ? ext_a : ext_b);
                    x1 = static_cast<double>(std::max(a.x, b.x)) + (forward ? ext_b : ext_a);
                    y0 = static_cast<double>(a.y) - half;
                    y1 = static_cast<double>(a.y) + half;
                }
                else
                {
                    const bool forward = a.y < b.y;
                    y0 = static_cast<double>(std::min(a.y, b.y)) - (forward ? ext_a : ext_b);
                    y1 = static_cast<double>(std::max(a.y, b.y)) + (forward ? ext_b : ext_a);
                    x0 = static_cast<double>(a.x) - half;
                    x1 = static_cast<double>(a.x) + half;
                }
                x0 = std::trunc(x0);
                y0 = std::trunc(y0);
                x1 = std::trunc(x1);
                y1 = std::trunc(y1);
            };
            auto covered = [&](double x, double y)
            {
                double x0, y0, x1, y1;
                for (std::size_t i = 0; i + 1 < pts.size(); ++i)
                {
                    box(i, x0, y0, x1, y1);
                    if (x >= x0 && x <= x1 && y >= y0 && y <= y1)
                        return true;
                }
                return false;
            };
            const double px = static_cast<double>(point.x);
            const double py = static_cast<double>(point.y);
            if (!covered(px, py))
                return false; // the common case - no neighbour checks
            constexpr double e = 0.25;
            for (const double dx : {-e, 0.0, e})
                for (const double dy : {-e, 0.0, e})
                    if ((dx != 0.0 || dy != 0.0) && !covered(px + dx, py + dy))
                        return false;
            return true;
        }

        /// @brief False when `point` certainly isn't in `path`'s area: it's
        /// outside every segment's bbox grown by the farthest the buffer
        /// reaches from a segment (a miter tip, path_to_area's limit 5 x
        /// half the width, plus a unit for rounding). True means "maybe" -
        /// buffer to decide. Lets a click skip buffering long non-orthogonal
        /// paths it's nowhere near.
        static bool path_may_contain(const Path &path, const Point &point)
        {
            const std::vector<Point> &pts = path.polygon.points;
            if (pts.size() < 2)
                return true;
            const double reach = 5.0 * static_cast<double>(path.width) / 2.0 + 1.0;
            const double px = static_cast<double>(point.x);
            const double py = static_cast<double>(point.y);
            for (std::size_t i = 0; i + 1 < pts.size(); ++i)
            {
                const double x0 = static_cast<double>(std::min(pts[i].x, pts[i + 1].x)) - reach;
                const double x1 = static_cast<double>(std::max(pts[i].x, pts[i + 1].x)) + reach;
                const double y0 = static_cast<double>(std::min(pts[i].y, pts[i + 1].y)) - reach;
                const double y1 = static_cast<double>(std::max(pts[i].y, pts[i + 1].y)) + reach;
                if (px >= x0 && px <= x1 && py >= y0 && py <= y1)
                    return true;
            }
            return false;
        }

        /// @brief Every rect/polygon/path piece of `shape` containing
        /// `point`, in rects/polygons/paths order (find_hit_piece's own
        /// priority) - for click-cycling through overlapping pieces
        /// (le_mouse_up). `first_only` stops at the first, which is what
        /// find_hit_piece returns.
        static std::vector<HitPiece> find_hit_pieces(const ShapeData &shape, const Point &point, bool first_only = false)
        {
            std::vector<HitPiece> hits;
            for (size_t i = 0; i < shape.rects.size(); ++i)
            {
                if (point_in_rect(point, shape.rects[i]))
                {
                    hits.push_back(HitPiece{.kind = PieceKind::RECT, .index = i, .outline = ShapeData{.layer = shape.layer, .rects = {shape.rects[i]}}});
                    if (first_only)
                        return hits;
                }
            }

            for (size_t i = 0; i < shape.polygons.size(); ++i)
            {
                if (!point_in_rect(point, bbox_of(shape.polygons[i])))
                    continue;

                if (bg::within(point, to_boost_polygon(shape.polygons[i])))
                {
                    hits.push_back(HitPiece{.kind = PieceKind::POLYGON, .index = i, .outline = ShapeData{.layer = shape.layer, .polygons = {shape.polygons[i]}}});
                    if (first_only)
                        return hits;
                }
            }

            for (size_t i = 0; i < shape.paths.size(); ++i)
            {
                if (!point_in_rect(point, bbox_of(shape.paths[i])))
                    continue;

                // Buffering a path is costly (the bulk of a click's hit test
                // near busy routing) - most are orthogonal, and tested directly.
                bool hit = false;
                if (const std::optional<bool> fast = point_in_orthogonal_path(shape.paths[i], point))
                    hit = *fast;
                else if (path_may_contain(shape.paths[i], point))
                    for (const auto &part : path_to_polygons(shape.paths[i]))
                        if (bg::within(point, to_boost_polygon(part)))
                        {
                            hit = true;
                            break; // one hit per path, whichever of its parts contains the point
                        }
                if (hit)
                {
                    hits.push_back(HitPiece{.kind = PieceKind::PATH, .index = i, .outline = ShapeData{.layer = shape.layer, .paths = {shape.paths[i]}}});
                    if (first_only)
                        return hits;
                }
            }

            return hits;
        }

        /// @brief True if `point` falls within `shape`'s actual drawn
        /// geometry (any rect/polygon/path piece) - used where only a
        /// yes/no answer is needed. See find_hit_piece for the same test
        /// when the specific piece itself is also needed (e.g. hover
        /// highlighting).
        static bool contains(const ShapeData &shape, const Point &point)
        {
            return find_hit_piece(shape, point).has_value();
        }

        /// @brief Extracts just the one rect/polygon/path piece at
        /// `index` within `kind`'s own vector of `shape`, as its own
        /// one-piece Shape (same `layer`, same convention
        /// find_hit_piece's own `outline` uses) - for rendering/moving a
        /// single selected piece of a Shape that may bundle several
        /// together, without pulling in its siblings. Returns an empty
        /// (no rects/polygons/paths) one-piece Shape if `index` is out of
        /// range for `kind` - e.g. a stale selection after the shape's
        /// own geometry shrank since it was selected - drawing/moving
        /// nothing rather than crashing or silently substituting a
        /// different piece.
        static ShapeData extract_piece(const ShapeData &shape, PieceKind kind, size_t index)
        {
            ShapeData piece{.layer = shape.layer};
            switch (kind)
            {
            case PieceKind::RECT:
                if (index < shape.rects.size())
                    piece.rects = {shape.rects[index]};
                break;
            case PieceKind::POLYGON:
                if (index < shape.polygons.size())
                    piece.polygons = {shape.polygons[index]};
                break;
            case PieceKind::PATH:
                if (index < shape.paths.size())
                    piece.paths = {shape.paths[index]};
                break;
            case PieceKind::VIA:
                if (index < shape.vias.size())
                    piece.vias = {shape.vias[index]};
                break;
            case PieceKind::VIA_ITERATE:
                if (index < shape.via_iterates.size())
                    piece.via_iterates = {shape.via_iterates[index]};
                break;
            }
            return piece;
        }

        /// @brief True if `extract_piece(shape, kind, index)` would
        /// return a real (non-empty) piece - i.e. `index` is in range for
        /// `kind`'s own vector. Lets a caller distinguish "this piece
        /// really has no geometry" from "the index is stale" without
        /// extracting a whole Shape copy just to check.
        static bool piece_in_range(const ShapeData &shape, PieceKind kind, size_t index)
        {
            switch (kind)
            {
            case PieceKind::RECT:
                return index < shape.rects.size();
            case PieceKind::POLYGON:
                return index < shape.polygons.size();
            case PieceKind::PATH:
                return index < shape.paths.size();
            case PieceKind::VIA:
                return index < shape.vias.size();
            case PieceKind::VIA_ITERATE:
                return index < shape.via_iterates.size();
            }
            return false;
        }

        /// @brief Translates just the one rect/polygon/path piece at
        /// `index` within `kind`'s own vector of `data`, in place,
        /// leaving every other piece (including other entries of the
        /// same kind) untouched. A no-op if `index` is out of range for
        /// `kind` (see piece_in_range - check first if the caller needs
        /// to know whether anything actually happened). Per-piece Move
        /// uses this to move exactly the selected piece, not the whole
        /// Shape.
        static void transform_piece_in_place(ShapeData &data, PieceKind kind, size_t index, const Point &offset)
        {
            switch (kind)
            {
            case PieceKind::RECT:
                if (index < data.rects.size())
                    data.rects[index] = transform(data.rects[index], offset);
                break;
            case PieceKind::POLYGON:
                if (index < data.polygons.size())
                    data.polygons[index] = transform(data.polygons[index], offset);
                break;
            case PieceKind::PATH:
                if (index < data.paths.size())
                    data.paths[index] = transform(data.paths[index], offset);
                break;
            case PieceKind::VIA:
                if (index < data.vias.size())
                {
                    data.vias[index].origin.x += offset.x;
                    data.vias[index].origin.y += offset.y;
                }
                break;
            case PieceKind::VIA_ITERATE: // the whole array moves with its origin
                if (index < data.via_iterates.size())
                {
                    data.via_iterates[index].origin.x += offset.x;
                    data.via_iterates[index].origin.y += offset.y;
                }
                break;
            }
        }

        /// @brief True if `shape`'s bbox is entirely inside `container` -
        /// used for rubber-band drag-select ("completely enclosed by the
        /// selection rectangle"). Exact, not
        /// an approximation: for an axis-aligned rectangle, "every point
        /// of shape is inside container" and "shape's tightest bounding
        /// box is inside container" are equivalent - if the bbox fits, so
        /// does everything inside it; if any point of the shape were
        /// outside, the bbox (being the tightest fit around the shape)
        /// would be too. So no per-rect/polygon/path testing is needed
        /// here, unlike `contains`. False for a shape with no geometry
        /// (no bbox).
        static bool fully_enclosed(const Rect &container, const ShapeData &shape)
        {
            const auto bbox = GeometryCore::bbox(shape);
            if (!bbox)
                return false;

            return bbox->ll.x >= container.ll.x && bbox->ll.y >= container.ll.y &&
                   bbox->ur.x <= container.ur.x && bbox->ur.y <= container.ur.y;
        }

        /// @brief The per-piece analog of fully_enclosed - every individual
        /// rect/polygon/path of `shape` whose *own* bbox is entirely inside
        /// `container` (same exact-for-axis-aligned-containment reasoning
        /// as fully_enclosed's own doc comment - no Boost calls needed
        /// here either), each returned as its own HitPiece (matching
        /// find_hit_piece's return convention, including `kind`/`index`).
        /// Needed because one Shape can bundle several rects/polygons/
        /// paths together (e.g. several RECT statements in one PORT) - a
        /// drag-select needs to know *which* of them individually
        /// qualify, not just whether the bundle's own combined bbox does.
        static std::vector<HitPiece> fully_enclosed_pieces(const Rect &container, const ShapeData &shape)
        {
            auto bbox_enclosed = [&](const Rect &bbox)
            {
                return bbox.ll.x >= container.ll.x && bbox.ll.y >= container.ll.y &&
                       bbox.ur.x <= container.ur.x && bbox.ur.y <= container.ur.y;
            };

            std::vector<HitPiece> result;

            for (size_t i = 0; i < shape.rects.size(); ++i)
                if (bbox_enclosed(shape.rects[i]))
                    result.push_back(HitPiece{.kind = PieceKind::RECT, .index = i, .outline = ShapeData{.layer = shape.layer, .rects = {shape.rects[i]}}});

            for (size_t i = 0; i < shape.polygons.size(); ++i)
                if (bbox_enclosed(bbox_of(shape.polygons[i])))
                    result.push_back(HitPiece{.kind = PieceKind::POLYGON, .index = i, .outline = ShapeData{.layer = shape.layer, .polygons = {shape.polygons[i]}}});

            for (size_t i = 0; i < shape.paths.size(); ++i)
                if (bbox_enclosed(bbox_of(shape.paths[i])))
                    result.push_back(HitPiece{.kind = PieceKind::PATH, .index = i, .outline = ShapeData{.layer = shape.layer, .paths = {shape.paths[i]}}});

            return result;
        }
    };
}
