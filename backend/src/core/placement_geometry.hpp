#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include <optional>
#include <vector>

namespace le
{
    /// @brief A Placement's own world-space bbox, plus the free functions
    /// (relocated from src/instancing/instance_renderer.hpp - see E1's own
    /// plan) that compute it, and the E1 (BUGS_AND_ENHANCEMENTS.md)
    /// top-level Placement hit-test built on top.
    ///
    /// Relocated out of InstanceRenderer specifically so both it and
    /// render's own BuildSelectionOverlayPictureStage can call the exact
    /// same implementation: `render` links `database geometry scene
    /// view_style core skia`, not `pipeline`/`instancing` (see
    /// CMakeLists.txt), so InstanceRenderer's own private statics -
    /// living in `src/instancing/` - were structurally unreachable from
    /// render's own selection-overlay stage. `core` sits below both, so
    /// this is a pure extraction (every function here was already
    /// `static`, no instance state) - InstanceRenderer calls these same
    /// free functions now instead of its own private copies.

    /// The dispatch rule for design_id's own current view (Layout,
    /// recursed, if remaining_depth allows it; otherwise Abstract;
    /// Kind::None if neither resolves) - the single source of truth
    /// design_is_resolvable/resolved_local_bbox below are now thin
    /// callers of, and HierarchyResolver's own node-key scheme
    /// (hierarchy_resolver.hpp NodeKey/discover_layout_children) is built
    /// on directly. Previously this exact if/else was hand-duplicated
    /// three ways (build_design_picture, design_is_resolvable,
    /// resolved_local_bbox) - a real drift risk; now three (four,
    /// counting HierarchyResolver's own discovery) callers of one
    /// function can never disagree about which view a given
    /// {DesignId, remaining_depth} resolves to.
    struct DesignTarget
    {
        enum class Kind
        {
            None,
            Layout,
            Abstract
        } kind = Kind::None;
        LayoutId layout_id;
        AbstractId abstract_id;
    };

    inline DesignTarget resolve_design_target(const Root &root, DesignId design_id, int remaining_depth)
    {
        const LayoutId layout_id = root.get_design_layout(design_id);
        if (remaining_depth > 0 && layout_id.valid())
            return DesignTarget{.kind = DesignTarget::Kind::Layout, .layout_id = layout_id, .abstract_id = {}};

        const AbstractId abstract_id = root.get_design_abstract(design_id);
        if (abstract_id.valid())
            return DesignTarget{.kind = DesignTarget::Kind::Abstract, .layout_id = {}, .abstract_id = abstract_id};

        return DesignTarget{};
    }

    inline Rect layout_declared_bbox(const Root &root, LayoutId layout_id)
    {
        if (const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id)))
            if (auto b = Geometry::bbox(*diearea))
                return *b;
        return Rect{};
    }

    // AbstractData.origin is deliberately NOT applied here - a known,
    // accepted gap, no ORIGIN-bearing test data driving it yet (see
    // Geometry::instance_transform's own doc comment).
    inline Rect abstract_declared_bbox(const Root &root, AbstractId abstract_id)
    {
        const AbstractData *abstract = root.get_abstract(abstract_id);
        if (abstract && abstract->size)
            return Rect{.ll = Point{0, 0}, .ur = *abstract->size};

        if (const Shape *boundary = root.get_shape(root.get_abstract_boundary(abstract_id)))
            if (auto b = Geometry::bbox(*boundary))
                return *b;

        return Rect{};
    }

    /// Whether design_id's own current view (Layout, recursed, if
    /// remaining_depth allows it; otherwise Abstract) resolves to
    /// anything drawable at all, without actually building/recording
    /// anything. A placement whose reference_design fails this check
    /// must not be treated as a real (if sub-pixel) instance elsewhere:
    /// placement_world_bbox's own nullopt return for "unresolved" is
    /// otherwise indistinguishable from a legitimately zero-sized
    /// declared bbox.
    inline bool design_is_resolvable(const Root &root, DesignId design_id, int remaining_depth)
    {
        return resolve_design_target(root, design_id, remaining_depth).kind != DesignTarget::Kind::None;
    }

    // Resolves design_id's own current view's own declared bbox, without
    // building/recording anything - used both for a placing parent's own
    // Geometry::instance_transform orientation math and for
    // placement_world_bbox below. Thin caller of resolve_design_target
    // above, so this and design_is_resolvable can never disagree about
    // which view (Layout vs. Abstract) a given {DesignId, remaining_depth}
    // resolves to.
    inline Rect resolved_local_bbox(const Root &root, DesignId design_id, int remaining_depth)
    {
        const DesignTarget target = resolve_design_target(root, design_id, remaining_depth);
        switch (target.kind)
        {
        case DesignTarget::Kind::Layout:
            return layout_declared_bbox(root, target.layout_id);
        case DesignTarget::Kind::Abstract:
            return abstract_declared_bbox(root, target.abstract_id);
        case DesignTarget::Kind::None:
        default:
            return Rect{};
        }
    }

    /// A Placement's own world-space bbox (its reference_design's own
    /// declared bbox, transformed by its orientation + location) -
    /// nullopt if the placement itself doesn't resolve (no location, no
    /// reference_design) or its reference_design isn't resolvable
    /// (design_is_resolvable above) - never a degenerate Rect{} standing
    /// in for "nothing here", which would be indistinguishable from a
    /// legitimately zero-sized declared bbox.
    inline std::optional<Rect> placement_world_bbox(const Root &root, PlacementId placement_id, int remaining_depth)
    {
        const PlacementData *placement = root.get_placement(placement_id);
        if (!placement || !placement->location || !placement->reference_design.valid())
            return std::nullopt;
        if (!design_is_resolvable(root, placement->reference_design, remaining_depth))
            return std::nullopt;

        const Rect child_local_bbox = resolved_local_bbox(root, placement->reference_design, remaining_depth);
        const Orientation orientation = placement->orientation.value_or(Orientation::N);
        const Geometry::InstanceTransform transform = Geometry::instance_transform(orientation, child_local_bbox, *placement->location);
        return Geometry::transform_bbox(transform, child_local_bbox);
    }

    /// E1 (BUGS_AND_ENHANCEMENTS.md) - top-level Placement hit-test, the
    /// "separate, bbox-only mechanism" Pipeline::hit_test_point's own doc
    /// comment already anticipated (a Placement never enters the
    /// RenderedShape map at all, by design - see
    /// GenerateLayoutShapesStage's own comment). Iterates
    /// `layout_id`'s own direct placements **in reverse** - matches
    /// BuildLayoutPictureStage::run's own draw order (own_shapes first,
    /// then instances, so a placement is always topmost) - and returns
    /// the first (topmost) one whose world bbox contains `dbu_point`.
    /// Never recurses into a placement's own reference_design - E1's own
    /// "top-level of hierarchy only" scope.
    inline std::optional<PlacementId> hit_test_placements_point(const Root &root, LayoutId layout_id, int remaining_depth, Point dbu_point)
    {
        const auto &placements = root.get_layout_placements(layout_id);
        for (auto it = placements.rbegin(); it != placements.rend(); ++it)
        {
            const std::optional<Rect> bbox = placement_world_bbox(root, *it, remaining_depth);
            if (bbox && dbu_point.x >= bbox->ll.x && dbu_point.x <= bbox->ur.x && dbu_point.y >= bbox->ll.y && dbu_point.y <= bbox->ur.y)
                return *it;
        }
        return std::nullopt;
    }

    /// Rubber-band counterpart to hit_test_placements_point above - every
    /// top-level placement whose own world bbox is fully enclosed by
    /// `dbu_rect` (same "all layers, no topmost-only restriction"
    /// semantics as Pipeline::hit_test_rect), in no particular order.
    inline std::vector<PlacementId> hit_test_placements_rect(const Root &root, LayoutId layout_id, int remaining_depth, Rect dbu_rect)
    {
        std::vector<PlacementId> result;
        for (PlacementId placement_id : root.get_layout_placements(layout_id))
        {
            const std::optional<Rect> bbox = placement_world_bbox(root, placement_id, remaining_depth);
            if (bbox && bbox->ll.x >= dbu_rect.ll.x && bbox->ll.y >= dbu_rect.ll.y && bbox->ur.x <= dbu_rect.ur.x && bbox->ur.y <= dbu_rect.ur.y)
                result.push_back(placement_id);
        }
        return result;
    }
}
