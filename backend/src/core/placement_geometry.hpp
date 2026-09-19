#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../view_style/view_style.hpp"
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
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

    /// @brief One piece of Abstract-view content hit-tested by
    /// hit_test_abstract_point/_rect below - exactly the rect/polygon/
    /// path (`piece_kind`/`piece_index`) of `shape_id`'s own raw
    /// `ShapeData` that was actually hit, plus a copy of just that one
    /// piece's own geometry (`outline`) for a caller that wants to render
    /// it (a hover highlight) without a second Root lookup.
    struct AbstractHitPiece
    {
        ShapeId shape_id;
        PieceKind piece_kind;
        size_t piece_index;
        Shape outline;
    };

    /// @brief Whether a given ViewLayer (by layer name + purpose) should
    /// be considered for hit-testing at all - a caller-supplied predicate
    /// (`LeHandle::is_view_layer_selectable`) rather than a `LeHandle`
    /// reference/pointer, so this header (`core`) doesn't take a
    /// dependency on `api`.
    using ViewLayerSelectablePredicate = std::function<bool(const std::string &, ViewLayerPurpose)>;

    /// @brief True when `piece`'s own bbox is under 1 on-screen pixel in
    /// both dimensions at `scale` - the exact same "invisible, don't draw
    /// it" test `draw_helpers.hpp`'s own `bbox_is_sub_pixel` applies at
    /// render time (duplicated here, not shared, since that function
    /// lives in `pipelines`, below `core` in this project's own layering -
    /// see `backend/CLAUDE.md`). A piece the renderer would skip entirely
    /// must not be hit-testable either - `ApiFixture.
    /// SubPixelShapeIsNotRenderedAndIsNotSelectable` is a real,
    /// intentional test of exactly this: a click landing on a shape too
    /// small to see must not select it.
    inline bool abstract_piece_is_sub_pixel(const Shape &piece, double scale)
    {
        const std::optional<Rect> bbox = Geometry::bbox(piece);
        if (!bbox)
            return true;
        const double width = static_cast<double>(bbox->ur.x - bbox->ll.x);
        const double height = static_cast<double>(bbox->ur.y - bbox->ll.y);
        return width * scale < 1.0 && height * scale < 1.0;
    }

    /// @brief Abstract-view analog of hit_test_placements_point above -
    /// the pre-restart `pipelines.old/hit_test.hpp`'s own
    /// `hit_test_point`, rewritten directly against `Root`'s raw
    /// Terminal-port/Obstruction `ShapeData` instead of pipeline-rendered
    /// `RenderedShape` output: a piece a caller selects/moves must be
    /// addressable in `Root` by `(shape_id, piece_kind, piece_index)`
    /// (see `LeHandle::HoverTarget`'s own comment for why a Rasterize
    /// stage's own iterate-expanded geometry isn't always addressable
    /// that way, the same reason the pre-restart design already
    /// re-hit-tested against raw `ShapeData` for select/Move rather than
    /// reusing its own `RenderedShape` hit). E1's own scope - only
    /// Terminal/Obstruction pieces are selectable in an Abstract view, no
    /// BOUNDARY shape (never has an `origin` the same way a pipeline-
    /// rendered one wouldn't either).
    ///
    /// Topmost-selectable-ViewLayer-first (`view_layers.all()` is
    /// bottom-to-top insertion order - see that method's own doc comment -
    /// so reverse means topmost first), first-match-within-a-layer wins
    /// for two overlapping shapes on the same layer - an accepted MVP
    /// limitation, unchanged from the pre-restart version. Every
    /// Terminal-port/Obstruction Shape always carries a real, valid
    /// `Shape.layer` (unlike a BOUNDARY/PLACEMENT_BLOCKAGE Shape, which
    /// resolves its own ViewLayer via `Shape.purpose` instead - see
    /// `HierarchyResolverStage::resolve_view_layer`'s own comment) so
    /// `view_layers.find(shape.layer, purpose)` alone is enough here,
    /// with no fallback-by-purpose branch needed.
    inline std::optional<AbstractHitPiece> hit_test_abstract_point(
        const Root &root, const ViewLayerSet &view_layers, AbstractId abstract_id, Point dbu_point,
        double scale, const ViewLayerSelectablePredicate &is_selectable)
    {
        std::unordered_map<ViewLayerId, std::vector<ShapeId>> by_layer;

        for (TerminalId terminal_id : root.get_abstract_terminals(abstract_id))
            for (TerminalPortId port_id : root.get_terminal_ports(terminal_id))
                for (ShapeId shape_id : root.get_terminal_port_shapes(port_id))
                {
                    const Shape *shape = root.get_shape(shape_id);
                    if (!shape || !shape->layer.valid())
                        continue;
                    by_layer[view_layers.find(shape->layer, ViewLayerPurpose::TERMINAL)].push_back(shape_id);
                }

        for (ObstructionId obstruction_id : root.get_abstract_obstructions(abstract_id))
            for (ShapeId shape_id : root.get_obstruction_shapes(obstruction_id))
            {
                const Shape *shape = root.get_shape(shape_id);
                if (!shape || !shape->layer.valid())
                    continue;
                by_layer[view_layers.find(shape->layer, ViewLayerPurpose::OBSTRUCTION)].push_back(shape_id);
            }

        const std::vector<ViewLayerId> order = view_layers.all();
        for (auto layer_it = order.rbegin(); layer_it != order.rend(); ++layer_it)
        {
            const auto group_it = by_layer.find(*layer_it);
            if (group_it == by_layer.end())
                continue;

            const ViewLayerData *data = view_layers.get(*layer_it);
            if (data && !is_selectable(data->layer_name, data->purpose))
                continue;

            for (ShapeId shape_id : group_it->second)
            {
                const Shape *shape = root.get_shape(shape_id);
                if (!shape)
                    continue;
                if (auto piece = Geometry::find_hit_piece(*shape, dbu_point))
                {
                    if (abstract_piece_is_sub_pixel(piece->outline, scale))
                        continue; // invisible at this scale - not rendered, so not selectable either
                    return AbstractHitPiece{.shape_id = shape_id, .piece_kind = piece->kind, .piece_index = piece->index, .outline = piece->outline};
                }
            }
        }
        return std::nullopt;
    }

    /// @brief Rubber-band counterpart to hit_test_abstract_point above -
    /// every Terminal/Obstruction piece fully enclosed by `dbu_rect`,
    /// scanning every selectable ViewLayer (no topmost-only restriction,
    /// same "all layers" semantics as the pre-restart `hit_test_rect`),
    /// in no particular order. One `AbstractHitPiece` per enclosed piece
    /// - a Shape bundling several rects/polygons/paths only reports the
    /// pieces actually enclosed, not the whole Shape.
    inline std::vector<AbstractHitPiece> hit_test_abstract_rect(
        const Root &root, const ViewLayerSet &view_layers, AbstractId abstract_id, Rect dbu_rect,
        double scale, const ViewLayerSelectablePredicate &is_selectable)
    {
        std::vector<AbstractHitPiece> result;

        auto collect = [&](ShapeId shape_id, ViewLayerPurpose purpose)
        {
            const Shape *shape = root.get_shape(shape_id);
            if (!shape || !shape->layer.valid())
                return;

            const ViewLayerId view_layer = view_layers.find(shape->layer, purpose);
            const ViewLayerData *data = view_layers.get(view_layer);
            if (data && !is_selectable(data->layer_name, data->purpose))
                return;

            for (const HitPiece &piece : Geometry::fully_enclosed_pieces(dbu_rect, *shape))
            {
                if (abstract_piece_is_sub_pixel(piece.outline, scale))
                    continue; // invisible at this scale - not rendered, so not selectable either
                result.push_back(AbstractHitPiece{.shape_id = shape_id, .piece_kind = piece.kind, .piece_index = piece.index, .outline = piece.outline});
            }
        };

        for (TerminalId terminal_id : root.get_abstract_terminals(abstract_id))
            for (TerminalPortId port_id : root.get_terminal_ports(terminal_id))
                for (ShapeId shape_id : root.get_terminal_port_shapes(port_id))
                    collect(shape_id, ViewLayerPurpose::TERMINAL);

        for (ObstructionId obstruction_id : root.get_abstract_obstructions(abstract_id))
            for (ShapeId shape_id : root.get_obstruction_shapes(obstruction_id))
                collect(shape_id, ViewLayerPurpose::OBSTRUCTION);

        return result;
    }

    /// @brief Layout-view analog of hit_test_abstract_point above - same
    /// re-hit-test-against-raw-ShapeData design (a Route/PhysicalPort
    /// piece a caller selects/moves must be addressable in `Root` by
    /// `(shape_id, piece_kind, piece_index)`, not through the pipeline's
    /// own cached RenderShape output, which deliberately drops shape_id -
    /// see render_shape.hpp's own doc comment), reusing the same
    /// AbstractHitPiece return type (its fields were never Abstract-
    /// specific - shape_id/piece_kind/piece_index/outline apply equally
    /// to a Route's or PhysicalPort's own Shape).
    ///
    /// Scope: only Route and PhysicalPort own-shapes, matching Terminal/
    /// Obstruction's own two-owner-kind scope in the Abstract view above.
    /// Blockage/Row/Region own-shape hit-testing remains a separate,
    /// still-deferred gap (see select_in_layout_view_unlocked's own
    /// comment, api.cpp) - Row/Region in particular have no backing
    /// Shape at all (synthesized geometry only), so they'd need their
    /// own bare-id hit-test, not an extension of this function.
    ///
    /// A PhysicalPortSegment's own Shape always carries a real Shape.layer
    /// (DEF PIN geometry), so ViewLayerPurpose::TERMINAL here is always
    /// used as the real per-Layer row's own fallback purpose, exactly
    /// matching how HierarchyResolverStage::collect_layout_content
    /// resolves the same shapes for rendering (resolve_view_layer,
    /// hierarchy_resolver_stage.hpp) - a PhysicalPort's own selectability
    /// therefore already rides the same TERMINAL-purpose gating a
    /// Terminal has, not a separate PHYSICAL_PORT purpose (there isn't
    /// one - view_style.hpp's own ViewLayerPurpose enum).
    inline std::optional<AbstractHitPiece> hit_test_layout_point(
        const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, Point dbu_point,
        double scale, const ViewLayerSelectablePredicate &is_selectable)
    {
        std::unordered_map<ViewLayerId, std::vector<ShapeId>> by_layer;

        for (RouteId route_id : root.get_layout_routes(layout_id))
            for (ShapeId shape_id : root.get_route_shapes(route_id))
            {
                const Shape *shape = root.get_shape(shape_id);
                if (!shape || !shape->layer.valid())
                    continue;
                by_layer[view_layers.find(shape->layer, ViewLayerPurpose::ROUTE)].push_back(shape_id);
            }

        for (PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
            for (PhysicalPortSegmentId segment_id : root.get_physical_port_segments(port_id))
                for (ShapeId shape_id : root.get_physical_port_segment_shapes(segment_id))
                {
                    const Shape *shape = root.get_shape(shape_id);
                    if (!shape || !shape->layer.valid())
                        continue;
                    by_layer[view_layers.find(shape->layer, ViewLayerPurpose::TERMINAL)].push_back(shape_id);
                }

        const std::vector<ViewLayerId> order = view_layers.all();
        for (auto layer_it = order.rbegin(); layer_it != order.rend(); ++layer_it)
        {
            const auto group_it = by_layer.find(*layer_it);
            if (group_it == by_layer.end())
                continue;

            const ViewLayerData *data = view_layers.get(*layer_it);
            if (data && !is_selectable(data->layer_name, data->purpose))
                continue;

            for (ShapeId shape_id : group_it->second)
            {
                const Shape *shape = root.get_shape(shape_id);
                if (!shape)
                    continue;
                if (auto piece = Geometry::find_hit_piece(*shape, dbu_point))
                {
                    if (abstract_piece_is_sub_pixel(piece->outline, scale))
                        continue; // invisible at this scale - not rendered, so not selectable either
                    return AbstractHitPiece{.shape_id = shape_id, .piece_kind = piece->kind, .piece_index = piece->index, .outline = piece->outline};
                }
            }
        }
        return std::nullopt;
    }

    /// @brief Rubber-band counterpart to hit_test_layout_point above -
    /// every Route/PhysicalPort piece fully enclosed by `dbu_rect`,
    /// scanning every selectable ViewLayer, in no particular order - see
    /// hit_test_abstract_rect's own comment for the shared semantics.
    inline std::vector<AbstractHitPiece> hit_test_layout_rect(
        const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, Rect dbu_rect,
        double scale, const ViewLayerSelectablePredicate &is_selectable)
    {
        std::vector<AbstractHitPiece> result;

        auto collect = [&](ShapeId shape_id, ViewLayerPurpose purpose)
        {
            const Shape *shape = root.get_shape(shape_id);
            if (!shape || !shape->layer.valid())
                return;

            const ViewLayerId view_layer = view_layers.find(shape->layer, purpose);
            const ViewLayerData *data = view_layers.get(view_layer);
            if (data && !is_selectable(data->layer_name, data->purpose))
                return;

            for (const HitPiece &piece : Geometry::fully_enclosed_pieces(dbu_rect, *shape))
            {
                if (abstract_piece_is_sub_pixel(piece.outline, scale))
                    continue; // invisible at this scale - not rendered, so not selectable either
                result.push_back(AbstractHitPiece{.shape_id = shape_id, .piece_kind = piece.kind, .piece_index = piece.index, .outline = piece.outline});
            }
        };

        for (RouteId route_id : root.get_layout_routes(layout_id))
            for (ShapeId shape_id : root.get_route_shapes(route_id))
                collect(shape_id, ViewLayerPurpose::ROUTE);

        for (PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
            for (PhysicalPortSegmentId segment_id : root.get_physical_port_segments(port_id))
                for (ShapeId shape_id : root.get_physical_port_segment_shapes(segment_id))
                    collect(shape_id, ViewLayerPurpose::TERMINAL);

        return result;
    }
}
