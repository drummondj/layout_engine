#pragma once
#include "geometry_core.hpp"

namespace le
{
    /// @brief Translations and the 8 LEF/DEF orientations: placement instancing transforms, composed and inverted.
    class GeometryTransforms : public GeometryCore
    {
    public:
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
        // preview (see draw_move_ghost).
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

        /// @brief `data` with its rects, polygons and paths translated by
        /// `offset`; its vias, iterates, texts and every other field are
        /// copied unchanged (transform_piece_in_place moves a via piece).
        static ShapeData translate_rects_polygons_paths(const ShapeData &data, const Point &offset)
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
        /// {-1,0,1} - placement-instancing math
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
        /// parent's dbu space (Placement -> Design rendering): `linear`
        /// is `orientation_linear(orientation)`;
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
        /// in its own raw dbu coordinates - abstract_declared_bbox's
        /// {-origin, size - origin} for an Abstract-backed Design (LEF
        /// MACRO ORIGIN), or Geometry::bbox(the Layout's own diearea
        /// Shape) for a Layout-backed Design. Either can have a non-zero ll.
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
        /// e.g. from instance_transform above) - a placed instance's world
        /// bbox, used for culling and whole-placement hit-testing.
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

        /// @brief A DEF PIN's (or 5.7+ PORT's) placement as a transform:
        /// DEF gives pin geometry
        /// relative to its PLACED point, rotated/mirrored by its
        /// orientation about that point - not realigned to a bbox the way
        /// instance_transform places a cell. Identity for an unplaced pin.
        static InstanceTransform pin_transform(const std::optional<Point> &location, const std::optional<Orientation> &orientation)
        {
            if (!location)
                return identity_transform();
            return InstanceTransform{.linear = orientation_linear(orientation.value_or(Orientation::N)), .translation = *location};
        }

        static Point apply(const InstanceTransform &t, Point p)
        {
            const Point rotated = apply_linear(t.linear, p);
            return Point{.x = rotated.x + t.translation.x, .y = rotated.y + t.translation.y};
        }

        /// @brief Every rect, polygon and path point and via origin of
        /// `data` through `t` - rects re-normalized (a rotation can swap
        /// their corners); widths, masks and every other field copied.
        /// Via geometry itself isn't rotated, only placed. Iterates and
        /// texts aren't moved.
        static ShapeData transform_geometry_and_vias(const ShapeData &data, const InstanceTransform &t)
        {
            ShapeData out = data;
            for (Rect &r : out.rects)
            {
                const Point a = apply(t, r.ll);
                const Point b = apply(t, r.ur);
                r = Rect{.ll = Point{.x = std::min(a.x, b.x), .y = std::min(a.y, b.y)}, .ur = Point{.x = std::max(a.x, b.x), .y = std::max(a.y, b.y)}};
            }
            for (Polygon &polygon : out.polygons)
                for (Point &p : polygon.points)
                    p = apply(t, p);
            for (Path &path : out.paths)
                for (Point &p : path.polygon.points)
                    p = apply(t, p);
            for (auto &via : out.vias)
                via.origin = apply(t, via.origin);
            for (auto &via : out.via_iterates)
                via.origin = apply(t, via.origin);
            return out;
        }
    };
}
