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

    /// @brief Which combination Geometry::boolean_shapes computes between
    /// its two input groups (NOT is "a minus b").
    enum class BooleanOp
    {
        Or,
        And,
        Not,
    };

    /// @brief Cut-line direction for Geometry::shape_to_rects. Horizontal
    /// cuts with horizontal lines, giving horizontal strips (each rect as
    /// wide as the shape allows); Vertical is the transpose.
    enum class FractureDirection
    {
        Horizontal,
        Vertical,
    };

    /// @brief Area geometry in the only two forms a Shape can store it -
    /// Polygon has no hole representation, so a region with holes is
    /// emitted as exact rects instead (see Geometry::to_area_geometry).
    struct AreaGeometry
    {
        std::vector<Rect> rects;
        std::vector<Polygon> polygons;

        bool empty() const { return rects.empty() && polygons.empty(); }
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
            return Path{.width = path.width, .polygon = transform(path.polygon, offset)};
        }

        // Translates every rect/polygon/path in `data` by `offset` -
        // every other field (layer, spacing, ...) is copied
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

        /// @brief The linear (rotate/mirror-only, about local (0,0)) part
        /// of one of the 8 standard LEF/DEF placement orientations:
        /// x' = a*x + b*y ; y' = c*x + d*y. Coefficients are always in
        /// {-1,0,1} - Migration Step 3's own placement-instancing math
        /// (instance_transform below) relies on this being exact (no
        /// rounding) even after being scaled to pixel space, since a
        /// {-1,0,1} coefficient commutes exactly with a uniform scalar
        /// multiply.
        struct LinearTransform2D
        {
            int64_t a, b, c, d;
        };

        static LinearTransform2D orientation_linear(Orientation orientation)
        {
            switch (orientation)
            {
            case Orientation::N:
                return LinearTransform2D{1, 0, 0, 1};
            case Orientation::W:
                return LinearTransform2D{0, -1, 1, 0};
            case Orientation::S:
                return LinearTransform2D{-1, 0, 0, -1};
            case Orientation::E:
                return LinearTransform2D{0, 1, -1, 0};
            case Orientation::FN:
                return LinearTransform2D{-1, 0, 0, 1};
            case Orientation::FS:
                return LinearTransform2D{1, 0, 0, -1};
            case Orientation::FE:
                return LinearTransform2D{0, -1, -1, 0};
            case Orientation::FW:
                return LinearTransform2D{0, 1, 1, 0};
            }
            return LinearTransform2D{1, 0, 0, 1};
        }

        static Point apply_linear(const LinearTransform2D &m, Point p)
        {
            return Point{.x = m.a * p.x + m.b * p.y, .y = m.c * p.x + m.d * p.y};
        }

        /// @brief The dbu-space transform (rotate/mirror + translate) that
        /// places one placed instance's own local content into its
        /// parent's dbu space (Migration Step 3, Placement -> Design
        /// rendering): `linear` is `orientation_linear(orientation)`;
        /// `translation` is derived, not `placement_location` directly -
        /// every corner of `local_bbox` is run through `linear`, the
        /// transformed bbox's own lower-left corner is found
        /// (componentwise min, not a memorized per-orientation W/H-swap
        /// table - this is what correctly handles a 90-degree orientation
        /// swapping which local axis becomes the transformed width), and
        /// `translation = placement_location - transformed_bbox_ll` - so
        /// applying {linear, translation} to `local_bbox` itself lands its
        /// own lower-left corner exactly at `placement_location`, matching
        /// Placement.location's own schema.py doc comment ("the location
        /// of the lower-left corner of this instance ... after
        /// orientation is applied").
        ///
        /// `local_bbox` is the instance's own untransformed content bbox
        /// in its own raw dbu coordinates - AbstractData.size-derived
        /// {ll=(0,0), ur=size} for an Abstract-backed Design
        /// (AbstractData.origin is deliberately NOT applied here - a
        /// known, accepted gap, no ORIGIN-bearing test data driving it
        /// yet), or Geometry::bbox(the Layout's own diearea Shape) for a
        /// Layout-backed Design (which can have a non-zero ll).
        struct InstanceTransform
        {
            LinearTransform2D linear;
            Point translation;
        };

        static InstanceTransform instance_transform(Orientation orientation, Rect local_bbox, Point placement_location)
        {
            const LinearTransform2D linear = orientation_linear(orientation);
            const Point corners[4] = {
                local_bbox.ll,
                Point{.x = local_bbox.ur.x, .y = local_bbox.ll.y},
                Point{.x = local_bbox.ll.x, .y = local_bbox.ur.y},
                local_bbox.ur,
            };

            Point transformed_ll = apply_linear(linear, corners[0]);
            for (int i = 1; i < 4; i++)
            {
                const Point transformed = apply_linear(linear, corners[i]);
                transformed_ll.x = std::min(transformed_ll.x, transformed.x);
                transformed_ll.y = std::min(transformed_ll.y, transformed.y);
            }

            return InstanceTransform{
                .linear = linear,
                .translation = Point{.x = placement_location.x - transformed_ll.x, .y = placement_location.y - transformed_ll.y},
            };
        }

        /// @brief The world-space axis-aligned bbox `local_bbox` occupies
        /// once every corner is run through `t` ({linear, translation},
        /// e.g. from instance_transform above) - useful both for sizing a
        /// cached instance picture's own recorder bounds (InstanceRenderer,
        /// src/instancing/) and, in the future, whole-placement hit-testing
        /// (Migration Step 3's own locked-in "whole-placement only"
        /// selection scope for instanced content).
        static Rect transform_bbox(const InstanceTransform &t, Rect local_bbox)
        {
            auto world = [&](Point p)
            {
                const Point rotated = apply_linear(t.linear, p);
                return Point{.x = rotated.x + t.translation.x, .y = rotated.y + t.translation.y};
            };

            const Point corners[4] = {
                local_bbox.ll,
                Point{.x = local_bbox.ur.x, .y = local_bbox.ll.y},
                Point{.x = local_bbox.ll.x, .y = local_bbox.ur.y},
                local_bbox.ur,
            };

            Point lo = world(corners[0]);
            Point hi = lo;
            for (int i = 1; i < 4; i++)
            {
                const Point p = world(corners[i]);
                lo.x = std::min(lo.x, p.x);
                lo.y = std::min(lo.y, p.y);
                hi.x = std::max(hi.x, p.x);
                hi.y = std::max(hi.y, p.y);
            }
            return Rect{.ll = lo, .ur = hi};
        }

        /// @brief Composes two InstanceTransforms into one equivalent to
        /// applying `inner` first, then `outer` - i.e.
        /// apply_linear(compose(outer, inner).linear, p) + compose(outer,
        /// inner).translation == outer applied to (inner applied to p),
        /// for any point p. Needed to walk a Placement hierarchy more than
        /// one level deep without returning to Root: a nested placement's
        /// own InstanceTransform (ViewPlacementData::transform,
        /// src/pipelines/stages/hierarchy_resolver_stage.hpp) maps its own
        /// content into its *immediate* parent's local space only: to
        /// place that content directly into a further ancestor's own
        /// space (e.g. a viewport-culling stage's own running accumulated
        /// transform while recursing top-down), that ancestor's own
        /// transform must be composed with each descendant's own, one
        /// level at a time, exactly as this function does.
        /// @brief The identity InstanceTransform (maps every point to
        /// itself) - the correct starting point for composing down from a
        /// hierarchy's own top level (e.g. viewport culling's own running
        /// accumulated transform before any placement has been applied).
        /// Deliberately not just `InstanceTransform{}` - a default-
        /// constructed one has `linear = LinearTransform2D{0, 0, 0, 0}`
        /// (every point collapses to the origin), not the identity matrix
        /// `orientation_linear(Orientation::N)` produces.
        static InstanceTransform identity_transform()
        {
            return InstanceTransform{.linear = orientation_linear(Orientation::N), .translation = Point{0, 0}};
        }

        static InstanceTransform compose(const InstanceTransform &outer, const InstanceTransform &inner)
        {
            const LinearTransform2D &a = outer.linear;
            const LinearTransform2D &b = inner.linear;
            const Point rotated_inner_translation = apply_linear(a, inner.translation);
            return InstanceTransform{
                .linear = LinearTransform2D{
                    .a = a.a * b.a + a.b * b.c,
                    .b = a.a * b.b + a.b * b.d,
                    .c = a.c * b.a + a.d * b.c,
                    .d = a.c * b.b + a.d * b.d,
                },
                .translation = Point{
                    .x = rotated_inner_translation.x + outer.translation.x,
                    .y = rotated_inner_translation.y + outer.translation.y,
                },
            };
        }

        /// @brief The inverse of `t` - composing the result with `t`
        /// (in either order) yields identity_transform(). Every
        /// LinearTransform2D this codebase ever produces
        /// (orientation_linear's 8 cases) is orthogonal with {-1,0,1}
        /// entries, so its inverse is exactly its transpose - no
        /// determinant/division needed, and no precision loss. Needed to
        /// bring a world-space rect (e.g. a viewport) into one node's own
        /// local space by applying the *inverse* of that node's own
        /// accumulated transform, the cheap direction when there are many
        /// local bboxes to test against one world-space rect and only one
        /// rect to transform, rather than transforming every local bbox
        /// out to world space instead (ViewportCullStage's own use).
        static InstanceTransform invert(const InstanceTransform &t)
        {
            const LinearTransform2D transposed{.a = t.linear.a, .b = t.linear.c, .c = t.linear.b, .d = t.linear.d};
            const Point negated_translation{.x = -t.translation.x, .y = -t.translation.y};
            return InstanceTransform{
                .linear = transposed,
                .translation = apply_linear(transposed, negated_translation),
            };
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
        template <typename ShapeLike>
        static Point get_label_location(const ShapeLike &shape)
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

        /// @brief Like `contains`, but returns the single rect/polygon/path
        /// piece that contains `point` - both a copy of its geometry (as
        /// its own one-piece Shape, same `layer`) and `kind`/`index`
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
                    return HitPiece{.kind = PieceKind::RECT, .index = i, .outline = Shape{.layer = shape.layer, .rects = {shape.rects[i]}}};
            }

            for (size_t i = 0; i < shape.polygons.size(); ++i)
            {
                if (!point_in_rect(point, bbox_of(shape.polygons[i])))
                    continue;

                if (bg::within(point, to_boost_polygon(shape.polygons[i])))
                    return HitPiece{.kind = PieceKind::POLYGON, .index = i, .outline = Shape{.layer = shape.layer, .polygons = {shape.polygons[i]}}};
            }

            for (size_t i = 0; i < shape.paths.size(); ++i)
            {
                if (!point_in_rect(point, bbox_of(shape.paths[i])))
                    continue;

                for (const auto &part : path_to_polygons(shape.paths[i]))
                {
                    if (bg::within(point, to_boost_polygon(part)))
                        return HitPiece{.kind = PieceKind::PATH, .index = i, .outline = Shape{.layer = shape.layer, .paths = {shape.paths[i]}}};
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
        /// one-piece Shape (same `layer`, same convention
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
            Shape piece{.layer = shape.layer};
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
                    result.push_back(HitPiece{.kind = PieceKind::RECT, .index = i, .outline = Shape{.layer = shape.layer, .rects = {shape.rects[i]}}});

            for (size_t i = 0; i < shape.polygons.size(); ++i)
                if (bbox_enclosed(bbox_of(shape.polygons[i])))
                    result.push_back(HitPiece{.kind = PieceKind::POLYGON, .index = i, .outline = Shape{.layer = shape.layer, .polygons = {shape.polygons[i]}}});

            for (size_t i = 0; i < shape.paths.size(); ++i)
                if (bbox_enclosed(bbox_of(shape.paths[i])))
                    result.push_back(HitPiece{.kind = PieceKind::PATH, .index = i, .outline = Shape{.layer = shape.layer, .paths = {shape.paths[i]}}});

            return result;
        }

        /// @brief Expands RECT/PATH/POLYGON ITERATE (raw LEF storage) into
        /// concrete rects/paths/polygons on a copy of `shape`. An iterate
        /// with a non-positive or implausibly large count is skipped.
        static Shape expand_iterates(Shape shape)
        {
            constexpr int kMaxReasonableCount = 1'000'000;
            auto valid = [](const auto &it)
            { return it.num_x > 0 && it.num_y > 0 && it.num_x <= kMaxReasonableCount && it.num_y <= kMaxReasonableCount; };

            for (const RectIterate &it : shape.rect_iterates)
            {
                if (!valid(it))
                    continue;
                shape.rects.reserve(shape.rects.size() + static_cast<std::size_t>(it.num_x) * static_cast<std::size_t>(it.num_y));
                for (int ix = 0; ix < it.num_x; ix++)
                    for (int iy = 0; iy < it.num_y; iy++)
                        shape.rects.push_back(Rect{
                            .ll = Point{.x = it.rect.ll.x + ix * it.space_x, .y = it.rect.ll.y + iy * it.space_y},
                            .ur = Point{.x = it.rect.ur.x + ix * it.space_x, .y = it.rect.ur.y + iy * it.space_y},
                        });
            }
            shape.rect_iterates.clear();

            for (const PathIterate &it : shape.path_iterates)
            {
                if (!valid(it))
                    continue;
                shape.paths.reserve(shape.paths.size() + static_cast<std::size_t>(it.num_x) * static_cast<std::size_t>(it.num_y));
                for (int ix = 0; ix < it.num_x; ix++)
                    for (int iy = 0; iy < it.num_y; iy++)
                    {
                        const Point offset{.x = ix * it.space_x, .y = iy * it.space_y};
                        shape.paths.push_back(Path{.width = it.path.width, .polygon = transform(it.path.polygon, offset)});
                    }
            }
            shape.path_iterates.clear();

            for (const PolygonIterate &it : shape.polygon_iterates)
            {
                if (!valid(it))
                    continue;
                shape.polygons.reserve(shape.polygons.size() + static_cast<std::size_t>(it.num_x) * static_cast<std::size_t>(it.num_y));
                for (int ix = 0; ix < it.num_x; ix++)
                    for (int iy = 0; iy < it.num_y; iy++)
                    {
                        const Point offset{.x = ix * it.space_x, .y = iy * it.space_y};
                        shape.polygons.push_back(transform(it.polygon, offset));
                    }
            }
            shape.polygon_iterates.clear();

            return shape;
        }

        // --- Shape boolean operations and conversions (shape_* TCL commands) ---
        //
        // Every operation below works on a Shape's *area*: the union of all
        // its own rects/polygons/stroked paths, holes preserved - so e.g.
        // shape_to_rects never emits overlapping rects for a Shape whose own
        // entries overlap. Unexpanded *_iterates and vias are ignored (a
        // caller wanting iterates expands them first).

        /// @brief Boolean combination of two shape groups' areas (OR/AND/NOT, NOT = a minus b).
        static AreaGeometry boolean_shapes(const std::vector<const Shape *> &a, const std::vector<const Shape *> &b, BooleanOp op)
        {
            BgArea area_a = shapes_area(a);
            BgArea area_b = shapes_area(b);

            return to_area_geometry(combine(std::move(area_a), std::move(area_b), op));
        }

        /// @brief `shape`'s area as polygons only. A region with holes (which
        /// a Polygon can't represent) is fractured into exact rects, each
        /// emitted as its own 4-corner polygon.
        static std::vector<Polygon> shape_to_polygons(const Shape &shape)
        {
            AreaGeometry geometry = to_area_geometry(shape_area(shape));
            std::vector<Polygon> result = std::move(geometry.polygons);
            for (const Rect &rect : geometry.rects)
                result.push_back(rect_to_polygon(rect));
            return result;
        }

        /// @brief `shape`'s area as non-overlapping rects. Exact for
        /// rectilinear geometry; a diagonal edge is approximated by its
        /// slab's bbox (over-covering), since a Rect can't represent it.
        static std::vector<Rect> shape_to_rects(const Shape &shape, FractureDirection direction)
        {
            std::vector<Rect> result;
            for (const BgPolygon &polygon : shape_area(shape))
            {
                std::vector<Rect> rects = fracture_to_rects(polygon, direction);
                result.insert(result.end(), rects.begin(), rects.end());
            }
            return result;
        }

        /// @brief `shape`'s area grown (positive) or shrunk (negative) by dx
        /// in X and dy in Y. Exact for rectilinear geometry with any dx/dy;
        /// non-rectilinear geometry only supports dx == dy (an isotropic
        /// buffer) - std::nullopt otherwise. An empty result (shrunk away
        /// entirely) is a valid, empty AreaGeometry, not nullopt.
        static std::optional<AreaGeometry> size_shape(const Shape &shape, int64_t dx, int64_t dy)
        {
            const BgArea area = shape_area(shape);
            if (area.empty())
                return AreaGeometry{};

            if (is_rectilinear(area))
                return to_area_geometry(size_rectilinear(area, dx, dy));

            if (dx != dy)
                return std::nullopt;

            BgArea out;
            bg::strategy::buffer::distance_symmetric<double> distance_strategy(static_cast<double>(dx));
            bg::strategy::buffer::join_miter join_strategy(5);
            bg::strategy::buffer::end_flat end_strategy;
            bg::strategy::buffer::point_square point_strategy;
            bg::strategy::buffer::side_straight side_strategy;
            bg::buffer(area, out, distance_strategy, side_strategy, join_strategy, end_strategy, point_strategy);
            return to_area_geometry(out);
        }

        /// @brief One closed Path of `width` along every ring (outer
        /// boundary and each hole) of `shape`'s area, plus each of its own
        /// input Paths' centerlines re-stroked at `width`.
        static std::vector<Path> shape_outline_paths(const Shape &shape, int64_t width)
        {
            Shape area_only = shape;
            area_only.paths.clear();

            std::vector<Path> result;
            auto add_ring = [&](const auto &ring)
            {
                std::vector<Point> points = simplify_ring(std::vector<Point>(ring.begin(), ring.end()));
                if (points.size() >= 4) // closed: 3 distinct corners + the repeated first point
                    result.push_back(Path{.width = width, .polygon = Polygon{.points = std::move(points)}});
            };
            for (const BgPolygon &polygon : shape_area(area_only))
            {
                add_ring(polygon.outer());
                for (const auto &inner : polygon.inners())
                    add_ring(inner);
            }
            for (const Path &path : shape.paths)
                result.push_back(Path{.width = width, .polygon = path.polygon});
            return result;
        }

    private:
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

        // Groups `boxes` into connected components of overlapping-or-touching
        // boxes (bulk-loaded R-tree + union-find). Two components' contents
        // can never overlap or touch, so an overlay only ever needs to run
        // within one - Boost's own overlay is superlinear in how many
        // polygons each operand holds (BENCHMARKS.md 2026-09-24: one union
        // of two 2,500-polygon sets alone took ~1.45s).
        static std::vector<std::vector<size_t>> bbox_components(const std::vector<Rect> &boxes)
        {
            namespace bgi = bg::index;
            std::vector<std::pair<Rect, size_t>> entries;
            entries.reserve(boxes.size());
            for (size_t i = 0; i < boxes.size(); ++i)
                entries.emplace_back(boxes[i], i);
            const bgi::rtree<std::pair<Rect, size_t>, bgi::rstar<16>> tree(entries.begin(), entries.end());

            std::vector<size_t> parent(boxes.size());
            std::iota(parent.begin(), parent.end(), size_t{0});
            auto find = [&](size_t i)
            {
                while (parent[i] != i)
                    i = parent[i] = parent[parent[i]];
                return i;
            };
            std::vector<std::pair<Rect, size_t>> hits;
            for (size_t i = 0; i < boxes.size(); ++i)
            {
                hits.clear();
                tree.query(bgi::intersects(boxes[i]), std::back_inserter(hits));
                for (const auto &hit : hits)
                    parent[find(hit.second)] = find(i);
            }

            std::vector<std::vector<size_t>> components;
            std::vector<size_t> component_of(boxes.size(), SIZE_MAX);
            for (size_t i = 0; i < boxes.size(); ++i)
            {
                size_t &slot = component_of[find(i)];
                if (slot == SIZE_MAX)
                {
                    slot = components.size();
                    components.emplace_back();
                }
                components[slot].push_back(i);
            }
            return components;
        }

        static Rect envelope_of(const auto &geometry)
        {
            Rect box;
            bg::envelope(geometry, box);
            return box;
        }

        // Unions every part, but only within each connected component of
        // their bboxes (bbox_components) - disjoint clusters never meet in
        // one overlay - then concatenates the components' results.
        static BgArea union_all(std::vector<BgArea> parts)
        {
            if (parts.size() <= 1)
                return parts.empty() ? BgArea{} : std::move(parts.front());
            std::vector<Rect> boxes;
            boxes.reserve(parts.size());
            for (const BgArea &part : parts)
                boxes.push_back(envelope_of(part));

            BgArea result;
            for (const std::vector<size_t> &component : bbox_components(boxes))
            {
                std::vector<BgArea> group;
                group.reserve(component.size());
                for (size_t i : component)
                    group.push_back(std::move(parts[i]));
                for (BgPolygon &polygon : union_balanced(std::move(group)))
                    result.push_back(std::move(polygon));
            }
            return result;
        }

        // `op` applied component by component over both sides' polygons
        // together (same reasoning as union_all). A component holding only
        // one side's polygons needs no overlay at all.
        static BgArea combine(BgArea a, BgArea b, BooleanOp op)
        {
            std::vector<Rect> boxes;
            boxes.reserve(a.size() + b.size());
            for (const BgPolygon &polygon : a)
                boxes.push_back(envelope_of(polygon));
            for (const BgPolygon &polygon : b)
                boxes.push_back(envelope_of(polygon));

            BgArea result;
            for (const std::vector<size_t> &component : bbox_components(boxes))
            {
                BgArea from_a, from_b;
                for (size_t i : component)
                {
                    if (i < a.size())
                        from_a.push_back(std::move(a[i]));
                    else
                        from_b.push_back(std::move(b[i - a.size()]));
                }
                BgArea piece;
                if (from_b.empty())
                {
                    if (op != BooleanOp::And)
                        piece = std::move(from_a);
                }
                else if (from_a.empty())
                {
                    if (op == BooleanOp::Or)
                        piece = std::move(from_b);
                }
                else if (op == BooleanOp::Or)
                    bg::union_(from_a, from_b, piece);
                else if (op == BooleanOp::And)
                    bg::intersection(from_a, from_b, piece);
                else
                    bg::difference(from_a, from_b, piece);
                for (BgPolygon &polygon : piece)
                    result.push_back(std::move(polygon));
            }
            return result;
        }

        // Balanced pairwise reduction, not a left fold: folding N parts one
        // at a time into an ever-growing accumulator costs roughly
        // O(N * result size) - BENCHMARKS.md 2026-09-24 measured the left
        // fold (Geometry::union_shapes) 51x slower at 1k rects and 182x
        // slower at 10k.
        static BgArea union_balanced(std::vector<BgArea> parts)
        {
            if (parts.empty())
                return {};
            while (parts.size() > 1)
            {
                std::vector<BgArea> next;
                next.reserve((parts.size() + 1) / 2);
                for (size_t i = 0; i + 1 < parts.size(); i += 2)
                {
                    BgArea merged;
                    bg::union_(parts[i], parts[i + 1], merged);
                    next.push_back(std::move(merged));
                }
                if (parts.size() % 2 == 1)
                    next.push_back(std::move(parts.back()));
                parts = std::move(next);
            }
            return std::move(parts.front());
        }

        static void append_part(std::vector<BgArea> &parts, BgPolygon polygon)
        {
            if (bg::area(polygon) == 0)
                return; // degenerate (zero-width rect, collinear polygon) - contributes no area
            parts.push_back(BgArea{std::move(polygon)});
        }

        static void append_shape_parts(std::vector<BgArea> &parts, const Shape &shape)
        {
            for (const Rect &rect : shape.rects)
                append_part(parts, rect_to_bg(rect));
            for (const Polygon &polygon : shape.polygons)
                append_part(parts, to_boost_polygon(polygon));
            for (const Path &path : shape.paths)
                for (BgPolygon &polygon : path_to_area(path))
                    append_part(parts, std::move(polygon));
        }

        static BgArea shape_area(const Shape &shape)
        {
            std::vector<BgArea> parts;
            append_shape_parts(parts, shape);
            return union_all(std::move(parts));
        }

        static BgArea shapes_area(const std::vector<const Shape *> &shapes)
        {
            std::vector<BgArea> parts;
            for (const Shape *shape : shapes)
                if (shape)
                    append_shape_parts(parts, *shape);
            return union_all(std::move(parts));
        }

        // 128-bit: two int64 dbu deltas multiplied can exceed int64 range.
        static bool collinear(const Point &a, const Point &b, const Point &c)
        {
            const __int128 cross = static_cast<__int128>(b.x - a.x) * (c.y - b.y) - static_cast<__int128>(b.y - a.y) * (c.x - b.x);
            return cross == 0;
        }

        // Drops repeated and collinear vertices from a closed ring (first ==
        // last), returning it closed again - boolean results routinely keep
        // a vertex where two merged rects used to meet, which would
        // otherwise make a plain rectangle look like a 6-point polygon.
        static std::vector<Point> simplify_ring(std::vector<Point> ring)
        {
            if (ring.size() >= 2 && ring.front().x == ring.back().x && ring.front().y == ring.back().y)
                ring.pop_back();

            bool changed = true;
            while (changed && ring.size() >= 3)
            {
                changed = false;
                for (size_t i = 0; i < ring.size() && ring.size() >= 3; ++i)
                {
                    const Point &prev = ring[(i + ring.size() - 1) % ring.size()];
                    const Point &next = ring[(i + 1) % ring.size()];
                    if (collinear(prev, ring[i], next))
                    {
                        ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
                        changed = true;
                        break;
                    }
                }
            }
            if (!ring.empty())
                ring.push_back(ring.front());
            return ring;
        }

        static bool ring_is_rectilinear(const auto &ring)
        {
            for (size_t i = 0; i + 1 < ring.size(); ++i)
                if (bg::get<0>(ring[i]) != bg::get<0>(ring[i + 1]) && bg::get<1>(ring[i]) != bg::get<1>(ring[i + 1]))
                    return false;
            return true;
        }

        static bool is_rectilinear(const BgArea &area)
        {
            for (const BgPolygon &polygon : area)
            {
                if (!ring_is_rectilinear(polygon.outer()))
                    return false;
                for (const auto &inner : polygon.inners())
                    if (!ring_is_rectilinear(inner))
                        return false;
            }
            return true;
        }

        // Exact slab decomposition of a rectilinear polygon (outer ring and
        // holes), cut with horizontal lines at every distinct vertex y: a
        // single sweep over its vertical edges, keeping the edges that span
        // the current slab in x order - pairing them up (even-odd) gives
        // the slab's inside intervals directly. Each interval merges into
        // the previous slab's rect with the same x-extent (two-pointer, both
        // sides in x order), so a plain rectangle comes back as one rect.
        // Replaced intersecting every slab with the whole polygon, which
        // costs (slabs x vertices) - 5.4s for 2,500 holes (BENCHMARKS.md
        // 2026-09-24).
        static std::vector<Rect> fracture_rectilinear_horizontal(const std::vector<std::vector<Point>> &rings)
        {
            struct Edge
            {
                int64_t x;
                int64_t ylo;
                int64_t yhi;
            };
            std::vector<Edge> edges;
            std::vector<int64_t> cuts;
            for (const std::vector<Point> &ring : rings)
                for (size_t i = 0; i + 1 < ring.size(); ++i)
                {
                    cuts.push_back(ring[i].y);
                    if (ring[i].x == ring[i + 1].x && ring[i].y != ring[i + 1].y)
                        edges.push_back(Edge{ring[i].x, std::min(ring[i].y, ring[i + 1].y), std::max(ring[i].y, ring[i + 1].y)});
                }
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

            std::vector<size_t> by_lo(edges.size()), by_hi(edges.size());
            std::iota(by_lo.begin(), by_lo.end(), size_t{0});
            std::iota(by_hi.begin(), by_hi.end(), size_t{0});
            std::sort(by_lo.begin(), by_lo.end(), [&](size_t a, size_t b)
                      { return edges[a].ylo < edges[b].ylo; });
            std::sort(by_hi.begin(), by_hi.end(), [&](size_t a, size_t b)
                      { return edges[a].yhi < edges[b].yhi; });

            std::multiset<std::pair<int64_t, size_t>> active; // (x, edge) spanning the current slab
            std::vector<Rect> result;
            std::vector<size_t> open; // result rects ending at the current slab's bottom, in x order
            std::vector<size_t> next_open;
            size_t next_lo = 0;
            size_t next_hi = 0;
            for (size_t c = 0; c + 1 < cuts.size(); ++c)
            {
                const int64_t y0 = cuts[c];
                const int64_t y1 = cuts[c + 1];
                for (; next_hi < by_hi.size() && edges[by_hi[next_hi]].yhi <= y0; ++next_hi)
                    active.erase(active.find({edges[by_hi[next_hi]].x, by_hi[next_hi]}));
                for (; next_lo < by_lo.size() && edges[by_lo[next_lo]].ylo <= y0; ++next_lo)
                    active.insert({edges[by_lo[next_lo]].x, by_lo[next_lo]});

                next_open.clear();
                size_t o = 0;
                for (auto it = active.begin(); it != active.end();)
                {
                    const int64_t x0 = (it++)->first;
                    if (it == active.end())
                        break;
                    const int64_t x1 = (it++)->first;
                    if (x1 == x0)
                        continue; // two edges at one x (e.g. corner-touching) - no area between them
                    while (o < open.size() && result[open[o]].ll.x < x0)
                        ++o;
                    if (o < open.size() && result[open[o]].ll.x == x0 && result[open[o]].ur.x == x1)
                    {
                        result[open[o]].ur.y = y1;
                        next_open.push_back(open[o++]);
                    }
                    else
                    {
                        result.push_back(Rect{.ll = {x0, y0}, .ur = {x1, y1}});
                        next_open.push_back(result.size() - 1);
                    }
                }
                open.swap(next_open);
            }
            return result;
        }

        // Non-overlapping rects covering `polygon`: the exact edge sweep
        // above for rectilinear geometry (LEF/DEF's norm), transposed for
        // vertical cuts; fracture_by_intersection otherwise.
        static std::vector<Rect> fracture_to_rects(const BgPolygon &polygon, FractureDirection direction)
        {
            bool rectilinear = ring_is_rectilinear(polygon.outer());
            for (const auto &inner : polygon.inners())
                rectilinear = rectilinear && ring_is_rectilinear(inner);
            if (!rectilinear)
                return fracture_by_intersection(polygon, direction);

            const bool vertical = direction == FractureDirection::Vertical;
            auto ring_points = [&](const auto &ring)
            {
                std::vector<Point> points;
                points.reserve(ring.size());
                for (const Point &p : ring)
                    points.push_back(vertical ? Point{.x = p.y, .y = p.x} : p);
                return points;
            };
            std::vector<std::vector<Point>> rings{ring_points(polygon.outer())};
            for (const auto &inner : polygon.inners())
                rings.push_back(ring_points(inner));

            std::vector<Rect> rects = fracture_rectilinear_horizontal(rings);
            if (vertical)
                for (Rect &r : rects)
                    r = Rect{.ll = {r.ll.y, r.ll.x}, .ur = {r.ur.y, r.ur.x}};
            return rects;
        }

        // Slab decomposition by intersecting each slab with the whole
        // polygon - the fallback for non-rectilinear geometry, where each
        // piece is approximated by its own bbox (over-covering a diagonal
        // edge). Cut lines at every distinct vertex coordinate of the outer
        // ring *and* every hole (unlike the label-placement-only
        // fracture_into_rects above, which ignores holes and picks its own
        // direction). Adjacent pieces with an identical cross-extent are
        // merged back together.
        static std::vector<Rect> fracture_by_intersection(const BgPolygon &polygon, FractureDirection direction)
        {
            const bool horizontal = direction == FractureDirection::Horizontal;
            Rect bbox;
            bg::envelope(polygon, bbox);

            std::vector<int64_t> cuts;
            auto add_cuts = [&](const auto &ring)
            {
                for (const auto &pt : ring)
                    cuts.push_back(horizontal ? bg::get<1>(pt) : bg::get<0>(pt));
            };
            add_cuts(polygon.outer());
            for (const auto &inner : polygon.inners())
                add_cuts(inner);
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

            std::vector<Rect> result;
            std::vector<size_t> open; // indices into result ending exactly at the current slab's near edge
            for (size_t i = 0; i + 1 < cuts.size(); ++i)
            {
                const Rect strip = horizontal
                    ? Rect{.ll = {bbox.ll.x, cuts[i]}, .ur = {bbox.ur.x, cuts[i + 1]}}
                    : Rect{.ll = {cuts[i], bbox.ll.y}, .ur = {cuts[i + 1], bbox.ur.y}};

                BgArea pieces;
                bg::intersection(polygon, rect_to_bg(strip), pieces);

                std::vector<size_t> next_open;
                for (const BgPolygon &piece : pieces)
                {
                    if (bg::area(piece) == 0)
                        continue;
                    Rect rect;
                    bg::envelope(piece, rect);

                    auto merge = std::find_if(open.begin(), open.end(), [&](size_t j)
                                              {
                        const Rect &prev = result[j];
                        return horizontal
                            ? prev.ll.x == rect.ll.x && prev.ur.x == rect.ur.x && prev.ur.y == rect.ll.y
                            : prev.ll.y == rect.ll.y && prev.ur.y == rect.ur.y && prev.ur.x == rect.ll.x; });
                    if (merge != open.end())
                    {
                        if (horizontal)
                            result[*merge].ur.y = rect.ur.y;
                        else
                            result[*merge].ur.x = rect.ur.x;
                        next_open.push_back(*merge);
                        open.erase(merge);
                    }
                    else
                    {
                        result.push_back(rect);
                        next_open.push_back(result.size() - 1);
                    }
                }
                open = std::move(next_open);
            }
            return result;
        }

        // Minkowski sum with the rect [-gx,gx] x [-gy,gy]: distributes over
        // union, so it's exactly the union of every fractured rect grown by
        // (gx, gy) - rectilinear input only (fracture_to_rects is exact
        // only there).
        static BgArea grow_rectilinear(const BgArea &area, int64_t gx, int64_t gy)
        {
            std::vector<BgArea> parts;
            for (const BgPolygon &polygon : area)
                for (const Rect &rect : fracture_to_rects(polygon, FractureDirection::Horizontal))
                    append_part(parts, rect_to_bg(Rect{.ll = {rect.ll.x - gx, rect.ll.y - gy}, .ur = {rect.ur.x + gx, rect.ur.y + gy}}));
            return union_all(std::move(parts));
        }

        // Erosion by the same rect, as the complement of the complement's
        // growth: A shrunk = A minus (bbox(A)+margin minus A) grown. The
        // margin (> the shrink amount) makes the complement include a frame
        // all the way round A, so A's outer boundary erodes too.
        //
        // Done one polygon at a time: `area`'s polygons are disjoint (it's
        // already merged), and erosion never adds area, so no polygon's
        // result depends on any other's - one small complement each instead
        // of one plate-sized complement with a hole per polygon, whose
        // overlays are superlinear in that hole count (BENCHMARKS.md
        // 2026-09-24).
        static BgArea shrink_rectilinear(const BgArea &area, int64_t sx, int64_t sy)
        {
            BgArea result;
            for (const BgPolygon &polygon : area)
            {
                const Rect bounds = envelope_of(polygon);
                const Rect box{.ll = {bounds.ll.x - sx - 1, bounds.ll.y - sy - 1}, .ur = {bounds.ur.x + sx + 1, bounds.ur.y + sy + 1}};
                const BgArea single{polygon};
                BgArea complement;
                bg::difference(BgArea{rect_to_bg(box)}, single, complement);
                BgArea shrunk;
                bg::difference(single, grow_rectilinear(complement, sx, sy), shrunk);
                for (BgPolygon &piece : shrunk)
                    result.push_back(std::move(piece));
            }
            return result;
        }

        // Growing by (gx,0) then (0,gy) equals growing by (gx,gy) at once
        // (the kernels' own Minkowski sum is that rect), and likewise for
        // erosion - so a mixed-sign size is just the two axes applied in
        // turn, each as a grow or a shrink by its own sign.
        static BgArea size_rectilinear(const BgArea &area, int64_t dx, int64_t dy)
        {
            if (dx >= 0 && dy >= 0)
                return grow_rectilinear(area, dx, dy);
            if (dx <= 0 && dy <= 0)
                return shrink_rectilinear(area, -dx, -dy);
            const BgArea x_sized = dx >= 0 ? grow_rectilinear(area, dx, 0) : shrink_rectilinear(area, -dx, 0);
            return dy >= 0 ? grow_rectilinear(x_sized, 0, dy) : shrink_rectilinear(x_sized, 0, -dy);
        }

        // A hole-free result polygon becomes a Rect if it's an axis-aligned
        // rectangle, else a Polygon; one with holes (which a Polygon can't
        // represent) is fractured into exact rects instead.
        static AreaGeometry to_area_geometry(const BgArea &area)
        {
            AreaGeometry out;
            for (const BgPolygon &polygon : area)
            {
                if (!polygon.inners().empty())
                {
                    std::vector<Rect> rects = fracture_to_rects(polygon, FractureDirection::Horizontal);
                    out.rects.insert(out.rects.end(), rects.begin(), rects.end());
                    continue;
                }
                std::vector<Point> ring = simplify_ring(std::vector<Point>(polygon.outer().begin(), polygon.outer().end()));
                if (ring.size() < 4)
                    continue; // degenerate after simplification - no area
                if (ring.size() == 5 && ring_is_rectilinear(ring))
                {
                    Rect rect;
                    bg::envelope(ring, rect);
                    out.rects.push_back(rect);
                }
                else
                {
                    out.polygons.push_back(Polygon{.points = std::move(ring)});
                }
            }
            return out;
        }
    };
}
