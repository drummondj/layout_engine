#pragma once
#include "../database/database.hpp"
#include "geometry.hpp"
#include <optional>

namespace le
{
    /// @brief Which view a placed Design resolves to and its declared and
    /// world-space bboxes - shared by HierarchyResolverStage (pipelines)
    /// and selection/Move (api).

    /// The dispatch rule for design_id's own current view (Layout,
    /// recursed, if remaining_depth allows it; otherwise Abstract;
    /// Kind::None if neither resolves). The single source of truth for
    /// that choice: design_is_resolvable, resolved_local_bbox and
    /// HierarchyResolverStage all call this, so they can never disagree
    /// about which view a given {DesignId, remaining_depth} resolves to.
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
        if (const ShapeData *diearea = root.get_shape(root.get_layout_diearea(layout_id)))
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

        if (const ShapeData *boundary = root.get_shape(root.get_abstract_boundary(abstract_id)))
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
}
