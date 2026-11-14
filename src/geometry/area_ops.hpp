#pragma once
#include "geometry_core.hpp"
#include <vector>

namespace le
{
    /// @brief Area geometry in the only two forms a Shape can store it -
    /// Polygon has no hole representation, so a region with holes is
    /// emitted as exact rects instead (see Geometry::to_area_geometry).
    struct AreaGeometry
    {
        std::vector<Rect> rects;
        std::vector<Polygon> polygons;

        bool empty() const { return rects.empty() && polygons.empty(); }
    };

    /// @brief Operations on a Shape's merged area: booleans, union, conversions to polygons/rects/outlines, and sizing.
    class AreaOps : public GeometryCore
    {
    public:
        static std::optional<std::vector<Polygon>> union_shapes(const std::vector<const ShapeData *> &shapes)
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

        // --- Shape boolean operations and conversions (shape_* TCL commands) ---
        //
        // Every operation below works on a Shape's *area*: the union of all
        // its own rects/polygons/stroked paths, holes preserved - so e.g.
        // shape_to_rects never emits overlapping rects for a Shape whose own
        // entries overlap. Unexpanded *_iterates and vias are ignored (a
        // caller wanting iterates expands them first).

        /// @brief Boolean combination of two shape groups' areas (OR/AND/NOT, NOT = a minus b).
        static AreaGeometry boolean_shapes(const std::vector<const ShapeData *> &a, const std::vector<const ShapeData *> &b, BooleanOp op)
        {
            BgArea area_a = shapes_area(a);
            BgArea area_b = shapes_area(b);

            return to_area_geometry(combine(std::move(area_a), std::move(area_b), op));
        }

        /// @brief `shape`'s area as polygons only. A region with holes (which
        /// a Polygon can't represent) is fractured into exact rects, each
        /// emitted as its own 4-corner polygon.
        static std::vector<Polygon> shape_to_polygons(const ShapeData &shape)
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
        static std::vector<Rect> shape_to_rects(const ShapeData &shape, FractureDirection direction)
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
        static std::optional<AreaGeometry> size_shape(const ShapeData &shape, int64_t dx, int64_t dy)
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
        static std::vector<Path> shape_outline_paths(const ShapeData &shape, int64_t width)
        {
            ShapeData area_only = shape;
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

    protected:
        // Groups `boxes` into connected components of overlapping-or-touching
        // boxes (bulk-loaded R-tree + union-find). Two components' contents
        // can never overlap or touch, so an overlay only ever needs to run
        // within one - Boost's own overlay is superlinear in how many
        // polygons each operand holds (one union of two 2,500-polygon
        // sets alone takes ~1.45s).
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
        // O(N * result size) - measured 51x slower than this at 1k rects
        // and 182x slower at 10k.
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

        static void append_shape_parts(std::vector<BgArea> &parts, const ShapeData &shape)
        {
            for (const Rect &rect : shape.rects)
                append_part(parts, rect_to_bg(rect));
            for (const Polygon &polygon : shape.polygons)
                append_part(parts, to_boost_polygon(polygon));
            for (const Path &path : shape.paths)
                for (BgPolygon &polygon : path_to_area(path))
                    append_part(parts, std::move(polygon));
        }

        static BgArea shape_area(const ShapeData &shape)
        {
            std::vector<BgArea> parts;
            append_shape_parts(parts, shape);
            return union_all(std::move(parts));
        }

        static BgArea shapes_area(const std::vector<const ShapeData *> &shapes)
        {
            std::vector<BgArea> parts;
            for (const ShapeData *shape : shapes)
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
        // Intersecting every slab with the whole polygon instead would
        // cost (slabs x vertices) - 5.4s for 2,500 holes.
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
        // overlays are superlinear in that hole count.
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
