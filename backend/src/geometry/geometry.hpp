#pragma once
#include "../database/database.hpp"
#include <algorithm>
#include <boost/geometry.hpp>
#include <boost/geometry/algorithms/buffer.hpp>
#include <boost/geometry/algorithms/intersection.hpp>
#include <boost/geometry/algorithms/union.hpp>
#include <boost/geometry/geometries/register/point.hpp>
#include <boost/geometry/geometries/register/box.hpp>
#include <boost/geometry/geometries/register/linestring.hpp>
#include <boost/geometry/geometries/linestring.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/strategies/transform/matrix_transformers.hpp>
#include <boost/geometry/strategies/buffer.hpp>
#include <limits>
#include <optional>

namespace bg = boost::geometry;

BOOST_GEOMETRY_REGISTER_POINT_2D(le::Point, int64_t, bg::cs::cartesian, x, y)
BOOST_GEOMETRY_REGISTER_BOX(le::Rect, le::Point, ll, ur)
BOOST_GEOMETRY_REGISTER_LINESTRING(std::vector<le::Point>)

namespace le
{
    /// @brief The result of a piece-level hit-test (find_hit_piece) - the
    /// one rect/polygon/path (as its own one-piece Shape, matching
    /// find_hit_piece's own return convention) under the query point,
    /// plus `kind`/`index` identifying exactly where it lives in the
    /// owning Shape's own rects/polygons/paths vector - needed so a
    /// caller (Scene::select, Move) can address that same piece again
    /// later, not just look at a disconnected copy of its geometry.
    struct HitPiece
    {
        PieceKind kind;
        size_t index;
        Shape outline;
    };

    /// @brief Boost.Geometry-backed operations over the database's Point/Rect/Polygon/Path/Shape types.
    class Geometry
    {
    private:
        static void expand_bbox(std::optional<Rect> &out, const Rect &r)
        {
            if (!out)
                out = r;
            else
                bg::expand(*out, r);
        }

        // Cheap containment pre-check shared by find_hit_piece and
        // local_width_at, ahead of an expensive bg::within/path_to_polygons
        // call - see find_hit_piece's own doc comment for the measured
        // cost this guards against (~17ms/call on the 1M-shape stress
        // design without it, dominated by path_to_polygons).
        static bool point_in_rect(const Point &point, const Rect &rect)
        {
            return point.x >= rect.ll.x && point.x <= rect.ur.x && point.y >= rect.ll.y && point.y <= rect.ur.y;
        }

        static Rect bbox_of(const Polygon &poly)
        {
            Rect r;
            bg::envelope(poly.points, r);
            return r;
        }

        static Rect bbox_of(const Path &path)
        {
            Rect r;
            bg::envelope(path.polygon.points, r);

            const int64_t half = path.width / 2;
            bg::set<0>(r.ll, bg::get<0>(r.ll) - half);
            bg::set<1>(r.ll, bg::get<1>(r.ll) - half);
            bg::set<0>(r.ur, bg::get<0>(r.ur) + half);
            bg::set<1>(r.ur, bg::get<1>(r.ur) + half);

            return r;
        }

        static void accumulate_bbox(std::optional<Rect> &out, const Shape &shape)
        {
            for (const auto &r : shape.rects)
                expand_bbox(out, r);

            for (const auto &poly : shape.polygons)
                expand_bbox(out, bbox_of(poly));

            for (const auto &path : shape.paths)
                expand_bbox(out, bbox_of(path));
        }

        // Assigns straight from `polygon.points` into `bg_polygon`'s own
        // ring storage - one copy, not two. ensure_closed() (used as-is by
        // the other call site below, which needs the closed vector back)
        // would work here too but builds and returns its own intermediate
        // std::vector<Point> first, which this then has to copy *again*
        // via .assign() - avoided by closing the ring in place instead.
        static bg::model::polygon<Point> to_boost_polygon(const Polygon &polygon)
        {
            bg::model::polygon<Point> bg_polygon;
            bg_polygon.outer().assign(polygon.points.begin(), polygon.points.end());

            if (!polygon.points.empty())
            {
                const auto &first = polygon.points.front();
                const auto &last = polygon.points.back();
                if (last.x != first.x || last.y != first.y)
                    bg_polygon.outer().push_back(first);
            }

            bg::correct(bg_polygon);
            return bg_polygon;
        }

        // Slices bg_polygon into approximating rects via a slab
        // decomposition along its dominant axis (UPDATES.md item 8):
        // vertical cut lines (slabs along x, at each distinct vertex
        // x-coordinate) if its bbox is wider than tall, horizontal cut
        // lines (slabs along y) otherwise. Each slab is intersected
        // against the polygon (bg::intersection) and approximated by ITS
        // OWN bbox - exact for a rectilinear polygon (LEF RECT/POLYGON
        // geometry, and any axis-aligned buffered Path - the common
        // case), a reasonable approximation otherwise (e.g. a diagonal
        // Path's mitered/flat-end buffered outline). Never empty for a
        // non-degenerate polygon - a polygon whose vertices share one
        // coordinate (fewer than 2 distinct cuts) returns its own bbox
        // as the single slab. Used by get_label_location - see its own
        // comment for why this only needs to be an approximation, not
        // an exact decomposition.
        //
        // With exactly 2 distinct cuts (one slab), that slab's own strip
        // - [cuts[0], cuts[1]] on the cut axis, the *full* bbox range on
        // the other - is by construction identical to `bbox` itself, so
        // intersecting the polygon against it is a guaranteed no-op
        // (bg::intersection(polygon, its own bbox) == polygon, whatever
        // the polygon's actual shape) and enveloping that gives back
        // `bbox` again - an exact simplification, not an approximation,
        // so this is folded into the same early return as the <2 case
        // rather than paying for a real bg::intersection call to
        // rediscover it. Confirmed as the dominant cost of this function
        // via `sample` profiling of BM_GenerateShapes on the 1M-shape
        // stress design before this was added (see BENCHMARKS.md) - most
        // real LEF POLYGON geometry and any straight buffered Path is
        // already exactly its own bbox, hitting this path.
        static std::vector<Rect> fracture_into_rects(const bg::model::polygon<Point> &bg_polygon)
        {
            Rect bbox;
            bg::envelope(bg_polygon, bbox);

            const bool wide = (bbox.ur.x - bbox.ll.x) >= (bbox.ur.y - bbox.ll.y);

            std::vector<int64_t> cuts;
            for (const auto &pt : bg_polygon.outer())
                cuts.push_back(wide ? bg::get<0>(pt) : bg::get<1>(pt));
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

            std::vector<Rect> result;
            if (cuts.size() < 3)
            {
                result.push_back(bbox);
                return result;
            }

            using BgPolygon = bg::model::polygon<Point>;
            using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;

            for (size_t i = 0; i + 1 < cuts.size(); ++i)
            {
                const Rect strip = wide
                    ? Rect{.ll = {cuts[i], bbox.ll.y}, .ur = {cuts[i + 1], bbox.ur.y}}
                    : Rect{.ll = {bbox.ll.x, cuts[i]}, .ur = {bbox.ur.x, cuts[i + 1]}};

                BgMultiPolygon pieces;
                bg::intersection(bg_polygon, to_boost_polygon(rect_to_polygon(strip)), pieces);

                for (const auto &piece : pieces)
                {
                    Rect piece_bbox;
                    bg::envelope(piece, piece_bbox);
                    result.push_back(piece_bbox);
                }
            }

            return result;
        }

        // bg::distance(point, ring) treats a *closed ring* as an areal
        // geometry, same trap as bg::distance(point, polygon) - it's 0 for
        // any interior point, not the boundary distance you'd expect.
        // Copying the ring's points into an explicit linestring forces
        // Boost.Geometry to treat it as a 1-D path instead, giving the
        // correct nearest-edge distance regardless of whether `point` is
        // inside or outside.
        static double distance_to_boundary(const bg::model::polygon<Point> &bg_polygon, const Point &point)
        {
            const auto &ring = bg::exterior_ring(bg_polygon);
            bg::model::linestring<Point> boundary(ring.begin(), ring.end());
            return bg::distance(point, boundary);
        }

        static Polygon from_boost_polygon(const bg::model::polygon<Point> &bg_polygon)
        {
            Polygon polygon;
            polygon.points.reserve(bg_polygon.outer().size());

            for (const auto &pt : bg_polygon.outer())
                polygon.points.push_back(Point{
                    static_cast<int64_t>(bg::get<0>(pt)),
                    static_cast<int64_t>(bg::get<1>(pt)),
                });

            polygon.points = ensure_closed(polygon.points);
            return polygon;
        }

    public:
        static std::optional<Rect> bbox(const Shape &shape)
        {
            std::optional<Rect> out;
            accumulate_bbox(out, shape);
            return out;
        }

        static std::optional<Rect> bbox(const std::vector<Shape> &shapes)
        {
            std::optional<Rect> out;

            for (const auto &shape : shapes)
                accumulate_bbox(out, shape);

            return out;
        }

        static std::optional<Rect> bbox(const std::vector<const Shape *> &shapes)
        {
            std::optional<Rect> out;

            for (const auto &shape : shapes)
                accumulate_bbox(out, *shape);

            return out;
        }

        static bool rects_overlap(const Rect &a, const Rect &b)
        {
            return bg::intersects(a, b);
        }

        static Polygon transform(const Polygon &polygon, const Point &offset)
        {
            std::vector<Point> transformed_points;
            bg::strategy::transform::translate_transformer<int64_t, 2, 2> translate(offset.x, offset.y);
            bg::transform(polygon.points, transformed_points, translate);
            return Polygon{.points = transformed_points};
        }

        // Plain hand-translation, deliberately not routed through Boost's
        // translate_transformer like the Polygon overload above - a pure
        // integer offset of two corners/a wrapped polygon needs none of
        // that machinery. Used by Move's commit path and its ghost
        // preview (UPDATES.md item 21, see draw_move_ghost).
        static Rect transform(const Rect &rect, const Point &offset)
        {
            return Rect{
                .ll = Point{.x = rect.ll.x + offset.x, .y = rect.ll.y + offset.y},
                .ur = Point{.x = rect.ur.x + offset.x, .y = rect.ur.y + offset.y},
            };
        }

        static Path transform(const Path &path, const Point &offset)
        {
            return Path{.polygon = transform(path.polygon, offset), .width = path.width};
        }

        // Translates every rect/polygon/path in `data` by `offset` -
        // every other field (layer_name, spacing, ...) is copied
        // unchanged. The whole-Shape convenience Move's commit path uses
        // to build its "after" geometry in one call.
        static ShapeData transform(const ShapeData &data, const Point &offset)
        {
            ShapeData out = data;
            for (auto &r : out.rects)
                r = transform(r, offset);
            for (auto &p : out.polygons)
                p = transform(p, offset);
            for (auto &p : out.paths)
                p = transform(p, offset);
            return out;
        }

        static Polygon rect_to_polygon(const Rect &rect)
        {
            std::vector<Point> points;
            points.reserve(5);
            points.push_back(Point{.x = rect.ll.x, .y = rect.ll.y});
            points.push_back(Point{.x = rect.ll.x, .y = rect.ur.y});
            points.push_back(Point{.x = rect.ur.x, .y = rect.ur.y});
            points.push_back(Point{.x = rect.ur.x, .y = rect.ll.y});
            points.push_back(Point{.x = rect.ll.x, .y = rect.ll.y});
            return Polygon{.points = points};
        }

        static std::vector<Polygon> path_to_polygons(const Path &path)
        {
            using BgPolygon = bg::model::polygon<Point>;
            using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;

            BgMultiPolygon out;
            // distance_symmetric buffers by this distance on EACH side of the
            // centerline (total width = 2x), but LEF's PATH WIDTH is the total
            // trace width, so halve it here rather than doubling every path.
            bg::strategy::buffer::distance_symmetric<double> distance_strategy(path.width / 2.0);
            bg::strategy::buffer::join_miter join_strategy(5);
            bg::strategy::buffer::end_flat end_strategy;
            bg::strategy::buffer::point_square circle_strategy;
            bg::strategy::buffer::side_straight side_strategy;

            bg::buffer(path.polygon.points, out, distance_strategy, side_strategy, join_strategy, end_strategy, circle_strategy);

            std::vector<Polygon> result;
            result.reserve(out.size());

            for (const auto &poly : out)
                result.push_back(from_boost_polygon(poly));

            return result;
        }

        static std::vector<Point> ensure_closed(const std::vector<Point> &points)
        {
            if (points.size() < 2)
                return points;

            std::vector<Point> closed = points;
            const auto &first = closed.front();
            const auto &last = closed.back();

            if (last.x != first.x || last.y != first.y)
                closed.push_back(first);

            return closed;
        }

        static std::optional<std::vector<Polygon>> union_shapes(const std::vector<const Shape *> &shapes)
        {
            std::vector<Polygon> parts;

            // Lower-bound estimate (paths can expand into more than one
            // polygon each via path_to_polygons, so this isn't exact), but
            // still avoids most reallocations compared to reserving nothing.
            size_t estimated_parts = 0;
            for (const auto *shape : shapes)
                if (shape)
                    estimated_parts += shape->rects.size() + shape->polygons.size() + shape->paths.size();
            parts.reserve(estimated_parts);

            for (const auto *shape : shapes)
            {
                if (!shape)
                    continue;

                for (const auto &rect : shape->rects)
                    parts.push_back(rect_to_polygon(rect));

                for (const auto &polygon : shape->polygons)
                    parts.push_back(from_boost_polygon(to_boost_polygon(polygon)));

                for (const auto &path : shape->paths)
                {
                    for (const auto &path_part : path_to_polygons(path))
                        parts.push_back(path_part);
                }
            }

            if (parts.empty())
                return std::nullopt;

            using BgPolygon = bg::model::polygon<Point>;
            using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;

            std::vector<BgPolygon> bg_parts;
            bg_parts.reserve(parts.size());

            for (const auto &part : parts)
                bg_parts.push_back(to_boost_polygon(part));

            // bg_parts is non-empty here: parts (checked above) and bg_parts
            // always have the same size.
            BgMultiPolygon result;
            result.push_back(bg_parts.front());

            for (size_t i = 1; i < bg_parts.size(); ++i)
            {
                BgMultiPolygon next;
                bg::union_(result, bg_parts[i], next);
                result = std::move(next);
            }

            std::vector<Polygon> merged;
            merged.reserve(result.size());

            for (const auto &poly : result)
                merged.push_back(from_boost_polygon(poly));

            return merged;
        }

        /// @brief Finds the largest candidate rect across `shape`'s own
        /// rects (used directly - no fracturing needed) and its
        /// polygons/paths (each fractured into approximating rects via
        /// fracture_into_rects - see its own comment), and returns that
        /// rect's center (UPDATES.md item 8). Ties keep the
        /// first-encountered candidate - deterministic, arbitrary but
        /// documented, not load-bearing for correctness. {0,0} for a
        /// shape with no geometry at all (matches the old algorithm's
        /// own empty-shape fallback). Unlike the old union+grid-search
        /// algorithm this replaces, the result can only land outside
        /// `shape`'s own geometry when `shape` is empty - every
        /// candidate rect's center is trivially inside that rect.
        static Point get_label_location(const Shape &shape)
        {
            std::optional<Rect> best;
            int64_t best_area = -1;

            auto consider = [&](const Rect &r)
            {
                const int64_t area = (r.ur.x - r.ll.x) * (r.ur.y - r.ll.y);
                if (area > best_area)
                {
                    best_area = area;
                    best = r;
                }
            };

            for (const auto &rect : shape.rects)
                consider(rect);

            for (const auto &polygon : shape.polygons)
                for (const auto &r : fracture_into_rects(to_boost_polygon(polygon)))
                    consider(r);

            for (const auto &path : shape.paths)
                for (const auto &poly : path_to_polygons(path))
                    for (const auto &r : fracture_into_rects(to_boost_polygon(poly)))
                        consider(r);

            if (!best)
                return Point{0, 0};

            return Point{(best->ll.x + best->ur.x) / 2, (best->ll.y + best->ur.y) / 2};
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
        /// If `point` falls inside none of the shape's pieces (e.g. it
        /// came from get_label_location's own last-resort raw-centroid
        /// fallback, which can land outside every piece for disjoint
        /// geometry - see its own tests), falls back to whichever
        /// individual piece's own bbox center is nearest to `point`, and
        /// returns that piece's width by the same rules above - a
        /// meaningful per-piece answer, not the whole shape's bbox (which
        /// could span disjoint pieces and grossly overstate their actual
        /// width). Returns 0.0 for a shape with no geometry at all.
        static double local_width_at(const Shape &shape, const Point &point)
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

        /// @brief Like `contains`, but returns the single rect/polygon/path
        /// piece that contains `point` - both a copy of its geometry (as
        /// its own one-piece Shape, same `layer_name`) and `kind`/`index`
        /// identifying exactly where it lives in `shape`'s own rects/
        /// polygons/paths vector (see HitPiece) - `shape` can bundle
        /// several rects/polygons/paths together (e.g. several RECT
        /// statements in one LEF PORT, or an OBS's array of rects), and
        /// click/hover hit-testing must identify only the one piece
        /// actually under `point`, not the whole group (UPDATES.md 7.1 -
        /// highlighting every rect in the group when only one was under
        /// the cursor was a real reported bug). nullopt if no piece
        /// contains `point`.
        ///
        /// Polygons/paths are pre-checked against a cheap bbox (see
        /// point_in_rect above, and bbox_of for paths - already accounts
        /// for width) before the real `bg::within`/`path_to_polygons`
        /// test - a real measured cost, not speculation: BM_HitTestPoint
        /// (pipeline_benchmark.cpp) against the 1M-shape stress design
        /// initially measured ~17ms/call without it (dominated by
        /// path_to_polygons - a real Boost.Geometry buffer operation -
        /// running on every visible path regardless of whether the query
        /// point was anywhere near it), clearly too slow to run on every
        /// pointer-move event (see le_set_mouse_position). With the
        /// pre-check, most candidates are rejected by four integer
        /// comparisons before any Boost.Geometry call - see
        /// BENCHMARKS.md for the before/after numbers.
        static std::optional<HitPiece> find_hit_piece(const Shape &shape, const Point &point)
        {
            for (size_t i = 0; i < shape.rects.size(); ++i)
            {
                if (point_in_rect(point, shape.rects[i]))
                    return HitPiece{.kind = PieceKind::RECT, .index = i, .outline = Shape{.layer_name = shape.layer_name, .rects = {shape.rects[i]}}};
            }

            for (size_t i = 0; i < shape.polygons.size(); ++i)
            {
                if (!point_in_rect(point, bbox_of(shape.polygons[i])))
                    continue;

                if (bg::within(point, to_boost_polygon(shape.polygons[i])))
                    return HitPiece{.kind = PieceKind::POLYGON, .index = i, .outline = Shape{.layer_name = shape.layer_name, .polygons = {shape.polygons[i]}}};
            }

            for (size_t i = 0; i < shape.paths.size(); ++i)
            {
                if (!point_in_rect(point, bbox_of(shape.paths[i])))
                    continue;

                for (const auto &part : path_to_polygons(shape.paths[i]))
                {
                    if (bg::within(point, to_boost_polygon(part)))
                        return HitPiece{.kind = PieceKind::PATH, .index = i, .outline = Shape{.layer_name = shape.layer_name, .paths = {shape.paths[i]}}};
                }
            }

            return std::nullopt;
        }

        /// @brief True if `point` falls within `shape`'s actual drawn
        /// geometry (any rect/polygon/path piece) - used where only a
        /// yes/no answer is needed. See find_hit_piece for the same test
        /// when the specific piece itself is also needed (e.g. hover
        /// highlighting).
        static bool contains(const Shape &shape, const Point &point)
        {
            return find_hit_piece(shape, point).has_value();
        }

        /// @brief Extracts just the one rect/polygon/path piece at
        /// `index` within `kind`'s own vector of `shape`, as its own
        /// one-piece Shape (same `layer_name`, same convention
        /// find_hit_piece's own `outline` uses) - for rendering/moving a
        /// single selected piece of a Shape that may bundle several
        /// together, without pulling in its siblings. Returns an empty
        /// (no rects/polygons/paths) one-piece Shape if `index` is out of
        /// range for `kind` - e.g. a stale selection after the shape's
        /// own geometry shrank since it was selected - drawing/moving
        /// nothing rather than crashing or silently substituting a
        /// different piece.
        static Shape extract_piece(const Shape &shape, PieceKind kind, size_t index)
        {
            Shape piece{.layer_name = shape.layer_name};
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
            }
            return piece;
        }

        /// @brief True if `extract_piece(shape, kind, index)` would
        /// return a real (non-empty) piece - i.e. `index` is in range for
        /// `kind`'s own vector. Lets a caller distinguish "this piece
        /// really has no geometry" from "the index is stale" without
        /// extracting a whole Shape copy just to check.
        static bool piece_in_range(const Shape &shape, PieceKind kind, size_t index)
        {
            switch (kind)
            {
            case PieceKind::RECT:
                return index < shape.rects.size();
            case PieceKind::POLYGON:
                return index < shape.polygons.size();
            case PieceKind::PATH:
                return index < shape.paths.size();
            }
            return false;
        }

        /// @brief Translates just the one rect/polygon/path piece at
        /// `index` within `kind`'s own vector of `data`, in place,
        /// leaving every other piece (including other entries of the
        /// same kind) untouched. A no-op if `index` is out of range for
        /// `kind` (see piece_in_range - check first if the caller needs
        /// to know whether anything actually happened). UPDATES.md item
        /// 21's per-piece Move uses this to move exactly the selected
        /// piece, not the whole Shape.
        static void transform_piece_in_place(Shape &data, PieceKind kind, size_t index, const Point &offset)
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
            }
        }

        /// @brief True if `shape`'s bbox is entirely inside `container` -
        /// used for rubber-band drag-select (UPDATES.md 7.1 item 5:
        /// "completely enclosed by the selection rectangle"). Exact, not
        /// an approximation: for an axis-aligned rectangle, "every point
        /// of shape is inside container" and "shape's tightest bounding
        /// box is inside container" are equivalent - if the bbox fits, so
        /// does everything inside it; if any point of the shape were
        /// outside, the bbox (being the tightest fit around the shape)
        /// would be too. So no per-rect/polygon/path testing is needed
        /// here, unlike `contains`. False for a shape with no geometry
        /// (no bbox).
        static bool fully_enclosed(const Rect &container, const Shape &shape)
        {
            const auto bbox = Geometry::bbox(shape);
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
        /// qualify, not just whether the bundle's own combined bbox does
        /// (UPDATES.md 7.1 item 5's rule 5 rectangle-select).
        static std::vector<HitPiece> fully_enclosed_pieces(const Rect &container, const Shape &shape)
        {
            auto bbox_enclosed = [&](const Rect &bbox)
            {
                return bbox.ll.x >= container.ll.x && bbox.ll.y >= container.ll.y &&
                       bbox.ur.x <= container.ur.x && bbox.ur.y <= container.ur.y;
            };

            std::vector<HitPiece> result;

            for (size_t i = 0; i < shape.rects.size(); ++i)
                if (bbox_enclosed(shape.rects[i]))
                    result.push_back(HitPiece{.kind = PieceKind::RECT, .index = i, .outline = Shape{.layer_name = shape.layer_name, .rects = {shape.rects[i]}}});

            for (size_t i = 0; i < shape.polygons.size(); ++i)
                if (bbox_enclosed(bbox_of(shape.polygons[i])))
                    result.push_back(HitPiece{.kind = PieceKind::POLYGON, .index = i, .outline = Shape{.layer_name = shape.layer_name, .polygons = {shape.polygons[i]}}});

            for (size_t i = 0; i < shape.paths.size(); ++i)
                if (bbox_enclosed(bbox_of(shape.paths[i])))
                    result.push_back(HitPiece{.kind = PieceKind::PATH, .index = i, .outline = Shape{.layer_name = shape.layer_name, .paths = {shape.paths[i]}}});

            return result;
        }
    };
}
