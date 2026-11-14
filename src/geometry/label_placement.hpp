#pragma once
#include "piece_hit_test.hpp"

namespace le
{
    /// @brief Where a Shape's label goes and how much room it has there.
    class LabelPlacement : public PieceHitTest
    {
    public:
        /// @brief Finds the largest candidate rect across `shape`'s own
        /// rects (used directly - no fracturing needed) and its
        /// polygons/paths (each fractured into approximating rects via
        /// fracture_into_rects - see its own comment), and returns that
        /// rect's center. Ties keep the first-encountered candidate -
        /// deterministic, arbitrary but documented, not load-bearing for
        /// correctness. {0,0} for a shape with no geometry at all. The
        /// result can only land outside `shape`'s own geometry when
        /// `shape` is empty - every candidate rect's center is trivially
        /// inside that rect.
        template <typename ShapeLike>
        static Point get_label_location(const ShapeLike &shape)
        {
            const std::optional<LabelCandidate> best = largest_label_candidate(shape);
            if (!best)
                return Point{0, 0};
            return Point{(best->rect.ll.x + best->rect.ur.x) / 2, (best->rect.ll.y + best->rect.ur.y) / 2};
        }

        /// @brief The bbox of the one rect/polygon/path of `shape` that
        /// get_label_location places its label on - std::nullopt for a
        /// shape with no geometry.
        template <typename ShapeLike>
        static std::optional<Rect> label_piece_bbox(const ShapeLike &shape)
        {
            const std::optional<LabelCandidate> best = largest_label_candidate(shape);
            if (!best)
                return std::nullopt;
            return best->piece_bbox;
        }

        /// @brief The local "width" (thickness) of `shape` at `point`: the
        /// diameter of the largest circle centered at `point` that still
        /// fits inside whichever rect/polygon/path of `shape` contains it.
        /// Used to size a label to the actual geometry it sits on rather
        /// than the shape's overall bounding box, which is wrong for a
        /// long, thin, non-convex (e.g. L-shaped) polygon.
        ///
        /// - Rects are checked first (cheapest, exact): if `point` falls
        ///   inside one, its width is simply `min(dx, dy)` - no boost call.
        /// - Paths are checked next: if `point` falls inside one's
        ///   buffered polygon (via `path_to_polygons`), its width is
        ///   `path.width` directly - a Path's thickness is already known
        ///   exactly and uniform along its whole centerline, so no
        ///   distance computation applies.
        /// - Polygons are checked last: if `point` falls inside one,
        ///   `2 * distance_to_boundary(...)` approximates local thickness
        ///   there - distance to the *boundary*, not the filled area
        ///   (`bg::distance` against either the filled polygon or its own
        ///   closed exterior ring directly both return 0 for any interior
        ///   point, treating them as areal geometry - see
        ///   distance_to_boundary's own comment for why the ring's points
        ///   are copied into an explicit linestring first). This stays
        ///   correct for non-convex/L-shaped polygons because it's local
        ///   to `point`, unlike an area/perimeter ratio over the whole
        ///   polygon (perimeter sums the concave boundary too, inflating
        ///   the ratio; area/max(dim) conflates both arms of an L into one
        ///   wrong average for either arm).
        ///
        /// Like find_hit_piece, polygons/paths are pre-checked against a
        /// cheap bbox (point_in_rect) before the real bg::within/
        /// path_to_polygons call, for the same measured reason - see that
        /// function's own doc comment.
        ///
        /// If `point` falls inside none of the shape's pieces (a label
        /// location can, for disjoint geometry), falls back to whichever
        /// individual piece's own bbox center is nearest to `point`, and
        /// returns that piece's width by the same rules above - a
        /// meaningful per-piece answer, not the whole shape's bbox (which
        /// could span disjoint pieces and grossly overstate their actual
        /// width). Returns 0.0 for a shape with no geometry at all.
        template <typename ShapeLike>
        static double local_width_at(const ShapeLike &shape, const Point &point)
        {
            for (const auto &rect : shape.rects)
            {
                if (point_in_rect(point, rect))
                    return static_cast<double>(std::min(rect.ur.x - rect.ll.x, rect.ur.y - rect.ll.y));
            }

            for (const auto &path : shape.paths)
            {
                if (!point_in_rect(point, bbox_of(path)))
                    continue;

                for (const auto &part : path_to_polygons(path))
                {
                    if (bg::within(point, to_boost_polygon(part)))
                        return static_cast<double>(path.width);
                }
            }

            for (const auto &polygon : shape.polygons)
            {
                if (!point_in_rect(point, bbox_of(polygon)))
                    continue;

                const auto bg_polygon = to_boost_polygon(polygon);
                if (bg::within(point, bg_polygon))
                    return 2.0 * distance_to_boundary(bg_polygon, point);
            }

            // Fallback: point isn't inside any single piece - use whichever
            // piece's own bbox center is nearest to point, and that
            // piece's own width (by the same rules as above).
            std::optional<double> best_width;
            int64_t best_dist_sq = std::numeric_limits<int64_t>::max();

            auto consider = [&](const Point &center, double width)
            {
                const int64_t dx = center.x - point.x;
                const int64_t dy = center.y - point.y;
                const int64_t dist_sq = dx * dx + dy * dy;
                if (dist_sq < best_dist_sq)
                {
                    best_dist_sq = dist_sq;
                    best_width = width;
                }
            };

            for (const auto &rect : shape.rects)
            {
                const Point center{(rect.ll.x + rect.ur.x) / 2, (rect.ll.y + rect.ur.y) / 2};
                consider(center, static_cast<double>(std::min(rect.ur.x - rect.ll.x, rect.ur.y - rect.ll.y)));
            }

            for (const auto &path : shape.paths)
            {
                const Rect bbox = bbox_of(path);
                const Point center{(bbox.ll.x + bbox.ur.x) / 2, (bbox.ll.y + bbox.ur.y) / 2};
                consider(center, static_cast<double>(path.width));
            }

            for (const auto &polygon : shape.polygons)
            {
                const Rect bbox = bbox_of(polygon);
                const Point center{(bbox.ll.x + bbox.ur.x) / 2, (bbox.ll.y + bbox.ur.y) / 2};
                const auto bg_polygon = to_boost_polygon(polygon);
                consider(center, 2.0 * distance_to_boundary(bg_polygon, center));
            }

            return best_width.value_or(0.0);
        }

    protected:
        /// A label candidate rect and the bbox of the piece it came from.
        struct LabelCandidate
        {
            Rect rect;
            Rect piece_bbox;
        };

        // get_label_location's choice: the largest candidate rect, first
        // wins on a tie.
        template <typename ShapeLike>
        static std::optional<LabelCandidate> largest_label_candidate(const ShapeLike &shape)
        {
            std::optional<LabelCandidate> best;
            int64_t best_area = -1;

            auto consider = [&](const Rect &r, const Rect &piece_bbox)
            {
                const int64_t area = (r.ur.x - r.ll.x) * (r.ur.y - r.ll.y);
                if (area > best_area)
                {
                    best_area = area;
                    best = LabelCandidate{.rect = r, .piece_bbox = piece_bbox};
                }
            };

            for (const auto &rect : shape.rects)
                consider(rect, rect);

            for (const auto &polygon : shape.polygons)
            {
                const Rect piece_bbox = bbox_of(polygon);
                for (const auto &r : fracture_into_rects(to_boost_polygon(polygon)))
                    consider(r, piece_bbox);
            }

            for (const auto &path : shape.paths)
            {
                const Rect piece_bbox = bbox_of(path);
                for (const auto &poly : path_to_polygons(path))
                    for (const auto &r : fracture_into_rects(to_boost_polygon(poly)))
                        consider(r, piece_bbox);
            }

            return best;
        }
    };
}
