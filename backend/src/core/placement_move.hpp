#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "fin_grid.hpp"
#include "placement_geometry.hpp"
#include "row_geometry.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace le
{
    /// @brief Placement Move snapping (NEW_FEATURES_SEPT_2026.md item 2) -
    /// what a moving Placement's own location (the lower-left of its
    /// placed bbox, DEF's own convention) snaps to. Values are the C API's
    /// own LE_PLACEMENT_SNAP_* numbering.
    enum class PlacementSnapMode : int32_t
    {
        NONE = 0,
        SITE = 1,
        FIN_GRID = 2,
        MANUFACTURING_GRID = 3,
    };

    /// @brief One orientation edit applied to a moving Placement, in world
    /// space: a 90-degree counterclockwise rotation (N -> W, DEF's own
    /// sense), a horizontal flip (mirror in X, N -> FN) or a vertical flip
    /// (mirror in Y, N -> FS).
    enum class OrientationOp : int32_t
    {
        ROTATE_CCW = 0,
        FLIP_HORIZONTAL = 1,
        FLIP_VERTICAL = 2,
    };

    inline Orientation orientation_from_linear(const Geometry::LinearTransform2D &m)
    {
        for (const Orientation o : {Orientation::N, Orientation::W, Orientation::S, Orientation::E,
                                    Orientation::FN, Orientation::FS, Orientation::FE, Orientation::FW})
        {
            const Geometry::LinearTransform2D c = Geometry::orientation_linear(o);
            if (c.a == m.a && c.b == m.b && c.c == m.c && c.d == m.d)
                return o;
        }
        return Orientation::N;
    }

    /// @brief `outer` applied after `inner` (matrix product outer * inner).
    inline Orientation compose_orientation(Orientation outer, Orientation inner)
    {
        const Geometry::LinearTransform2D o = Geometry::orientation_linear(outer);
        const Geometry::LinearTransform2D i = Geometry::orientation_linear(inner);
        return orientation_from_linear(Geometry::LinearTransform2D{
            .a = o.a * i.a + o.b * i.c,
            .b = o.a * i.b + o.b * i.d,
            .c = o.c * i.a + o.d * i.c,
            .d = o.c * i.b + o.d * i.d,
        });
    }

    inline Orientation orientation_for_op(OrientationOp op)
    {
        switch (op)
        {
        case OrientationOp::ROTATE_CCW:
            return Orientation::W;
        case OrientationOp::FLIP_HORIZONTAL:
            return Orientation::FN;
        case OrientationOp::FLIP_VERTICAL:
            return Orientation::FS;
        }
        return Orientation::N;
    }

    /// @brief Whether `site`'s SYMMETRY permits `op`: R90 a rotation, Y
    /// (symmetric about the Y axis) a horizontal flip, X a vertical one.
    /// False for a Site with no symmetry at all.
    inline bool site_permits(const SiteData &site, OrientationOp op)
    {
        if (!site.symmetry)
            return false;
        switch (op)
        {
        case OrientationOp::ROTATE_CCW:
            return site.symmetry->r90;
        case OrientationOp::FLIP_HORIZONTAL:
            return site.symmetry->y;
        case OrientationOp::FLIP_VERTICAL:
            return site.symmetry->x;
        }
        return false;
    }

    /// @brief Every orientation a cell may take in a row of `row_orientation`
    /// built from `site`: the row's own, closed under whichever
    /// OrientationOps the Site's symmetry permits - so any sequence of
    /// permitted rotate/flip presses stays legal for that row.
    inline std::vector<Orientation> row_allowed_orientations(Orientation row_orientation, const SiteData &site)
    {
        std::vector<Orientation> allowed{row_orientation};
        for (size_t i = 0; i < allowed.size(); ++i)
            for (const OrientationOp op : {OrientationOp::ROTATE_CCW, OrientationOp::FLIP_HORIZONTAL, OrientationOp::FLIP_VERTICAL})
            {
                if (!site_permits(site, op))
                    continue;
                const Orientation next = compose_orientation(orientation_for_op(op), allowed[i]);
                if (std::ranges::find(allowed, next) == allowed.end())
                    allowed.push_back(next);
            }
        return allowed;
    }

    inline bool orientation_swaps_axes(Orientation o)
    {
        return Geometry::orientation_linear(o).a == 0;
    }

    /// @brief Whether `abstract` is a standard cell that site snapping
    /// applies to - LEF MACRO CLASS CORE, including its subclasses
    /// ("CORE TIEHIGH", ...). Blocks/pads/etc. never snap to a SITE.
    inline bool abstract_is_core(const AbstractData &abstract)
    {
        if (!abstract.type)
            return false;
        const std::string_view type = *abstract.type;
        if (type.size() < 4)
            return false;
        for (size_t i = 0; i < 4; ++i)
            if (std::toupper(static_cast<unsigned char>(type[i])) != "CORE"[i])
                return false;
        return type.size() == 4 || std::isspace(static_cast<unsigned char>(type[4]));
    }

    /// @brief `v` rounded to the nearest `offset + k * pitch`.
    inline int64_t snap_to_grid(int64_t v, int64_t pitch, int64_t offset = 0)
    {
        if (pitch <= 0)
            return v;
        return offset + static_cast<int64_t>(std::llround(static_cast<double>(v - offset) / static_cast<double>(pitch))) * pitch;
    }

    /// @brief Where one moving Placement would land - its new location
    /// (placed-bbox lower-left) and orientation, plus the local bbox the
    /// placed transform is built from (for ghost drawing).
    struct PlacementMoveTarget
    {
        PlacementId id;
        Point location;
        Orientation orientation = Orientation::N;
        Rect local_bbox;
    };

    /// @brief Everything plan_placement_move needs besides the Placements
    /// themselves - resolved once per call, not per Placement.
    class PlacementSnapper
    {
    public:
        PlacementSnapper(const Root &root, LayoutId layout_id, PlacementSnapMode mode)
            : root_(root), mode_(mode), fin_grid_(technology_fin_grid(root)),
              manufacturing_grid_(technology_manufacturing_grid(root))
        {
            if (mode_ != PlacementSnapMode::SITE)
                return;
            for (const RowId row_id : root.get_layout_rows(layout_id))
            {
                const RowData *row = root.get_row(row_id);
                if (!row || !row->origin)
                    continue;
                const SiteId site_id = root.get_site_by_name(row->site_name);
                const SiteData *site = site_id.valid() ? root.get_site(site_id) : nullptr;
                if (!site || !site->size || site->size->x <= 0 || site->size->y <= 0)
                    continue;
                RowInfo info{
                    .row = row,
                    .site = site,
                    .step_x = row->step_x.value_or(site->size->x),
                    .step_y = row->step_y.value_or(site->size->y),
                    .num_x = std::max(1, row->num_x.value_or(1)),
                    .num_y = std::max(1, row->num_y.value_or(1)),
                };
                if (const auto bbox = row_footprint_bbox(root, row_id))
                    info.footprint = *bbox;
                rows_.push_back(info);
            }
            std::ranges::sort(rows_, {}, [](const RowInfo &r)
                              { return r.row->origin->y; });
        }

        /// @brief Whether `mode` has what it needs to snap anything at
        /// all - rows for SITE, a fin grid for FIN_GRID, a manufacturing
        /// grid for MANUFACTURING_GRID. NONE is always available.
        static bool available(const Root &root, LayoutId layout_id, PlacementSnapMode mode)
        {
            switch (mode)
            {
            case PlacementSnapMode::NONE:
                return true;
            case PlacementSnapMode::SITE:
                return !root.get_layout_rows(layout_id).empty();
            case PlacementSnapMode::FIN_GRID:
                return technology_fin_grid(root).has_value();
            case PlacementSnapMode::MANUFACTURING_GRID:
                return technology_manufacturing_grid(root).has_value();
            }
            return false;
        }

        /// @brief Snaps `target`'s location (and, for SITE, orientation)
        /// in place. SITE only applies to a CORE Abstract (abstract_is_core)
        /// with a row to land in - using only rows built from the
        /// Abstract's own SITE, if it declares one - and otherwise falls
        /// back to the manufacturing grid (or nothing, if there's none).
        /// The site grid is the row's own origin + k * step, clamped to
        /// the row; the orientation is forced to one the row allows
        /// (row_allowed_orientations - the same Site-symmetry rule
        /// permits() gates the rotate/flip buttons on) - kept if already
        /// legal, else vertically flipped into the row's family, else the
        /// row's own.
        void snap(PlacementMoveTarget &target, const AbstractData *abstract) const
        {
            switch (mode_)
            {
            case PlacementSnapMode::NONE:
                return;
            case PlacementSnapMode::MANUFACTURING_GRID:
                snap_manufacturing(target.location);
                return;
            case PlacementSnapMode::FIN_GRID:
                if (!fin_grid_)
                    return;
                if (fin_grid_->horizontal)
                {
                    target.location.y = snap_to_grid(target.location.y, fin_grid_->pitch, fin_grid_->offset);
                    if (manufacturing_grid_)
                        target.location.x = snap_to_grid(target.location.x, *manufacturing_grid_);
                }
                else
                {
                    target.location.x = snap_to_grid(target.location.x, fin_grid_->pitch, fin_grid_->offset);
                    if (manufacturing_grid_)
                        target.location.y = snap_to_grid(target.location.y, *manufacturing_grid_);
                }
                return;
            case PlacementSnapMode::SITE:
                if (!abstract || !abstract_is_core(*abstract) || !snap_site(target, *abstract))
                    snap_manufacturing(target.location);
                return;
            }
        }

        /// @brief Whether a rotate/flip (`op`) is allowed for a placement at
        /// `location` of `abstract` - only ever restricted under SITE
        /// snapping, for a CORE cell with a row to sit in: then the row's
        /// own Site's symmetry must permit it (site_permits). Anything
        /// site snapping wouldn't apply to is unrestricted.
        bool permits(OrientationOp op, Point location, const AbstractData *abstract) const
        {
            if (mode_ != PlacementSnapMode::SITE || !abstract || !abstract_is_core(*abstract))
                return true;
            const RowInfo *row = nearest_row(location, abstract->site);
            return !row || site_permits(*row->site, op);
        }

    private:
        struct RowInfo
        {
            const RowData *row = nullptr;
            const SiteData *site = nullptr;
            int64_t step_x = 0;
            int64_t step_y = 0;
            int num_x = 1;
            int num_y = 1;
            Rect footprint;
        };

        void snap_manufacturing(Point &p) const
        {
            if (!manufacturing_grid_)
                return;
            p.x = snap_to_grid(p.x, *manufacturing_grid_);
            p.y = snap_to_grid(p.y, *manufacturing_grid_);
        }

        // Distance from `p` to `r`'s footprint - vertical distance to the
        // nearest site-row y, plus how far `p` sits outside the row in X.
        static int64_t row_distance(const RowInfo &r, Point p)
        {
            int64_t dy;
            if (p.y <= r.footprint.ll.y)
                dy = r.footprint.ll.y - p.y;
            else if (r.num_y > 1 && r.step_y > 0 && p.y <= r.row->origin->y + static_cast<int64_t>(r.num_y - 1) * r.step_y)
                dy = std::llabs(snap_to_grid(p.y, r.step_y, r.row->origin->y) - p.y);
            else
                dy = std::llabs(p.y - (r.row->origin->y + static_cast<int64_t>(r.num_y - 1) * r.step_y));
            int64_t dx = 0;
            if (p.x < r.footprint.ll.x)
                dx = r.footprint.ll.x - p.x;
            else if (p.x > r.footprint.ur.x)
                dx = p.x - r.footprint.ur.x;
            return dy + dx;
        }

        const RowInfo *nearest_row(Point p, const std::optional<std::string> &site_name) const
        {
            const RowInfo *best = nullptr;
            int64_t best_distance = std::numeric_limits<int64_t>::max();
            const auto consider = [&](const RowInfo &r)
            {
                if (site_name && r.row->site_name != *site_name)
                    return;
                const int64_t d = row_distance(r, p);
                if (d < best_distance)
                {
                    best_distance = d;
                    best = &r;
                }
            };

            // rows_ is sorted by origin y - walk outward from `p.y`,
            // stopping each way once the origin-y gap alone exceeds the
            // best distance found (rows spanning several y repeats are
            // rare, so a row below p.y by more than best_distance can
            // still only matter if it's multi-row - checked by its own
            // footprint top instead).
            const auto split = std::ranges::lower_bound(rows_, p.y, {}, [](const RowInfo &r)
                                                        { return r.row->origin->y; });
            for (auto it = split; it != rows_.end(); ++it)
            {
                if (it->row->origin->y - p.y > best_distance)
                    break;
                consider(*it);
            }
            for (auto it = split; it != rows_.begin();)
            {
                --it;
                if (p.y - it->footprint.ur.y > best_distance && it->num_y == 1)
                    break;
                consider(*it);
            }
            return best;
        }

        bool snap_site(PlacementMoveTarget &target, const AbstractData &abstract) const
        {
            const RowInfo *row = nearest_row(target.location, abstract.site);
            if (!row)
                return false;

            const Point origin = *row->row->origin;
            const auto snap_index = [](int64_t v, int64_t o, int64_t step, int count)
            {
                if (step <= 0)
                    return o;
                const int64_t k = std::clamp<int64_t>(std::llround(static_cast<double>(v - o) / static_cast<double>(step)), 0, count - 1);
                return o + k * step;
            };
            target.location.x = snap_index(target.location.x, origin.x, row->step_x, row->num_x);
            target.location.y = snap_index(target.location.y, origin.y, row->step_y, row->num_y);

            const Orientation row_orientation = row->row->orientation;
            const std::vector<Orientation> allowed_orientations = row_allowed_orientations(row_orientation, *row->site);
            const auto allowed = [&](Orientation o)
            { return std::ranges::find(allowed_orientations, o) != allowed_orientations.end(); };
            if (allowed(target.orientation))
                return true;
            const Orientation flipped = compose_orientation(Orientation::FS, target.orientation);
            target.orientation = allowed(flipped) ? flipped : row_orientation;
            return true;
        }

        const Root &root_;
        PlacementSnapMode mode_;
        std::optional<FinGrid> fin_grid_;
        std::optional<int64_t> manufacturing_grid_;
        std::vector<RowInfo> rows_;
    };

    /// @brief Plans a Placement Move: for each of `placements` (skipping any
    /// that no longer resolve - no location/reference design), applies
    /// `pending` (an orientation change, see compose_orientation - N for a
    /// plain move, orientation_for_op for a rotate/flip) on top of its
    /// current orientation, keeping its placed bbox's own center fixed,
    /// then translates by `delta` and snaps each Placement
    /// independently per `mode` (PlacementSnapper::snap). `remaining_depth`
    /// picks which view of the reference design sizes the bbox - the same
    /// meaning placement_world_bbox gives it.
    inline std::vector<PlacementMoveTarget> plan_placement_move(const Root &root, LayoutId layout_id, std::span<const PlacementId> placements,
                                                                Orientation pending, Point delta, PlacementSnapMode mode, int remaining_depth)
    {
        const PlacementSnapper snapper(root, layout_id, mode);
        std::vector<PlacementMoveTarget> targets;
        targets.reserve(placements.size());
        for (const PlacementId id : placements)
        {
            const PlacementData *placement = root.get_placement(id);
            if (!placement || !placement->location || !placement->reference_design.valid() ||
                !design_is_resolvable(root, placement->reference_design, remaining_depth))
                continue;

            const Rect local_bbox = resolved_local_bbox(root, placement->reference_design, remaining_depth);
            const int64_t w = local_bbox.ur.x - local_bbox.ll.x;
            const int64_t h = local_bbox.ur.y - local_bbox.ll.y;
            const Orientation original = placement->orientation.value_or(Orientation::N);
            const Orientation oriented = compose_orientation(pending, original);

            // Placed-bbox extents before/after - swapped for a 90-degree
            // orientation. Keeping the center fixed means shifting the
            // lower-left by half the extent change.
            const int64_t old_w = orientation_swaps_axes(original) ? h : w;
            const int64_t old_h = orientation_swaps_axes(original) ? w : h;
            const int64_t new_w = orientation_swaps_axes(oriented) ? h : w;
            const int64_t new_h = orientation_swaps_axes(oriented) ? w : h;

            PlacementMoveTarget target{
                .id = id,
                .location = Point{
                    .x = placement->location->x + (old_w - new_w) / 2 + delta.x,
                    .y = placement->location->y + (old_h - new_h) / 2 + delta.y,
                },
                .orientation = oriented,
                .local_bbox = local_bbox,
            };
            snapper.snap(target, root.get_abstract(root.get_design_abstract(placement->reference_design)));
            targets.push_back(target);
        }
        return targets;
    }

    /// @brief The dbu-space ghost outline of one planned placement: its
    /// placed bbox, plus an orientation marker - a right triangle at the
    /// placed image of the local lower-left corner, twice as long along
    /// the local X axis as along local Y, so each of the 8 orientations
    /// draws distinguishably.
    inline Shape placement_move_ghost(const PlacementMoveTarget &target)
    {
        const Geometry::InstanceTransform t = Geometry::instance_transform(target.orientation, target.local_bbox, target.location);
        const auto world = [&](Point p)
        {
            const Point r = Geometry::apply_linear(t.linear, p);
            return Point{.x = r.x + t.translation.x, .y = r.y + t.translation.y};
        };

        Shape ghost;
        ghost.rects.push_back(Geometry::transform_bbox(t, target.local_bbox));

        const Point ll = target.local_bbox.ll;
        const int64_t m = std::min(target.local_bbox.ur.x - ll.x, target.local_bbox.ur.y - ll.y) / 4;
        if (m > 0)
            ghost.polygons.push_back(Polygon{.points = {world(ll), world(Point{.x = ll.x + 2 * m, .y = ll.y}), world(Point{.x = ll.x, .y = ll.y + m})}});
        return ghost;
    }
}
