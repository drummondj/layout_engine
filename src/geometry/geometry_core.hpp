#pragma once
#include "../database/database.hpp"
#include "shape_op_types.hpp"
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
#include <boost/geometry/index/rtree.hpp>
#include <cmath>
#include <numeric>
#include <limits>
#include <optional>
#include <set>

namespace bg = boost::geometry;

BOOST_GEOMETRY_REGISTER_POINT_2D(le::Point, int64_t, bg::cs::cartesian, x, y)
BOOST_GEOMETRY_REGISTER_BOX(le::Rect, le::Point, ll, ur)
BOOST_GEOMETRY_REGISTER_LINESTRING(std::vector<le::Point>)

namespace le
{
    /// @brief Bounding boxes and the conversions between the database's Polygon/Path and Boost.Geometry's polygons that every other Geometry part builds on.
    class GeometryCore
    {
    public:
        template <typename ShapeLike>
        static std::optional<Rect> bbox(const ShapeLike &shape)
        {
            std::optional<Rect> out;
            accumulate_bbox(out, shape);
            return out;
        }

        template <typename ShapeLike>
        static std::optional<Rect> bbox(const std::vector<ShapeLike> &shapes)
        {
            std::optional<Rect> out;

            for (const auto &shape : shapes)
                accumulate_bbox(out, shape);

            return out;
        }

        static std::optional<Rect> bbox(const std::vector<const ShapeData *> &shapes)
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

        // A Path's own DEF/LEF-standard default end cap: each of its two
        // free ends extends by half the path's own width beyond its raw
        // centerline coordinate, rather than butting flush to it - the
        // usual convention for how a router draws/connects PATH segments
        // (a genuinely free-standing end is meant to look "capped", not
        // cut off exactly at its own point; adjacent NEW-delimited path
        // segments of a route - each its own separate Path, not one
        // continuous polyline - are meant to visually flush-join at a
        // shared corner with no gap between them). Boost's own buffer end
        // strategies only offer end_flat (no extension, the bug this
        // fixes) or end_round (a semicircular cap) - neither matches - so
        // the extension is baked into a throwaway copy of the centerline
        // used only for this buffering call; the caller's own
        // path.polygon.points (the stored centerline) is never touched.
        // Interior vertices of a multi-point Path are left exactly as
        // given - those already get a proper corner via join_miter below,
        // not an end cap.
        static std::vector<Point> extend_path_ends_for_buffering(const std::vector<Point> &points, int64_t width)
        {
            if (points.size() < 2 || width <= 0)
                return points;

            std::vector<Point> extended = points;
            const double half_width = static_cast<double>(width) / 2.0;

            auto extend_endpoint = [half_width](Point &end, const Point &neighbor)
            {
                const double dx = static_cast<double>(end.x - neighbor.x);
                const double dy = static_cast<double>(end.y - neighbor.y);
                const double length = std::sqrt(dx * dx + dy * dy);
                if (length <= 0.0)
                    return; // coincident with its neighbor - no direction to extend along
                end.x += static_cast<int64_t>(std::llround(dx / length * half_width));
                end.y += static_cast<int64_t>(std::llround(dy / length * half_width));
            };

            extend_endpoint(extended.front(), extended[1]);
            extend_endpoint(extended.back(), extended[extended.size() - 2]);

            return extended;
        }

        // Outer rings only (from_boost_polygon drops holes) - a closed-loop
        // Path's own buffered outline has a hole, which this loses; use
        // path_to_area where the true region matters (the shape_* boolean/
        // conversion operations below).
        static std::vector<Polygon> path_to_polygons(const Path &path)
        {
            const auto out = path_to_area(path);

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

    protected:
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

        // Templated on the shape-like type (duck-typed on .rects/.polygons/
        // .paths, mirroring draw_helpers.hpp's own stroke_piece_outline
        // convention) rather than hardcoded to Shape, so it - and every
        // public bbox()/get_label_location()/local_width_at() overload
        // built on it below - also accepts pipelines/render_shape.hpp's
        // own leaner RenderShape without a second, duplicated
        // implementation. Adding this dependency-free (no #include of
        // render_shape.hpp - geometry has no dependency on pipelines, and
        // this keeps it that way) is exactly why it's a template rather
        // than an overload named at RenderShape directly.
        template <typename ShapeLike>
        static void accumulate_bbox(std::optional<Rect> &out, const ShapeLike &shape)
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
        // decomposition along its dominant axis:
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
        // rediscover it. Without this shortcut the intersection dominates
        // this function's cost on large designs, and most real LEF
        // POLYGON geometry and any straight buffered Path is already
        // exactly its own bbox, hitting this path.
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



        using BgPolygon = bg::model::polygon<Point>;
        using BgArea = bg::model::multi_polygon<BgPolygon>;

        static BgArea path_to_area(const Path &path)
        {
            BgArea out;
            // distance_symmetric buffers by this distance on EACH side of the
            // centerline (total width = 2x), but LEF's PATH WIDTH is the total
            // trace width, so halve it here rather than doubling every path.
            bg::strategy::buffer::distance_symmetric<double> distance_strategy(path.width / 2.0);
            bg::strategy::buffer::join_miter join_strategy(5);
            bg::strategy::buffer::end_flat end_strategy;
            bg::strategy::buffer::point_square circle_strategy;
            bg::strategy::buffer::side_straight side_strategy;

            const std::vector<Point> buffering_points = extend_path_ends_for_buffering(path.polygon.points, path.width);
            bg::buffer(buffering_points, out, distance_strategy, side_strategy, join_strategy, end_strategy, circle_strategy);
            return out;
        }

        static BgPolygon rect_to_bg(const Rect &rect)
        {
            return to_boost_polygon(rect_to_polygon(rect));
        }
    };
}
