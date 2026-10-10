#pragma once
#include "../database/database.hpp"
#include "../database/wire_helpers.hpp"
#include "../geometry/geometry.hpp"
#include "../geometry/placement_geometry.hpp"
#include "../pipelines/view_style.hpp"
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace le
{
    /// @brief Selection hit-tests: top-level Placements by world bbox, and
    /// shape pieces in the Abstract and Layout views.

    /// Top-level Placement hit-test, by world bbox. Iterates `layout_id`'s
    /// direct placements **in reverse** (the newest is drawn last, so is
    /// topmost) and returns the first one whose world bbox contains
    /// `dbu_point`. Never recurses into a placement's reference_design:
    /// only the top level of the hierarchy is selectable.
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

    /// Every top-level placement whose world bbox contains `dbu_point`,
    /// topmost first (hit_test_placements_point's own order) - for
    /// click-cycling through overlapping placements.
    ///
    /// `candidates` (api.cpp's layout_candidates, in id order), when
    /// given, are the only placements tested - newest id first.
    inline std::vector<PlacementId> hit_test_placements_point_all(const Root &root, LayoutId layout_id, int remaining_depth, Point dbu_point,
                                                                  const std::vector<PlacementId> *candidates = nullptr)
    {
        std::vector<PlacementId> hits;
        const auto &placements = candidates ? *candidates : root.get_layout_placements(layout_id);
        for (auto it = placements.rbegin(); it != placements.rend(); ++it)
        {
            const std::optional<Rect> bbox = placement_world_bbox(root, *it, remaining_depth);
            if (bbox && dbu_point.x >= bbox->ll.x && dbu_point.x <= bbox->ur.x && dbu_point.y >= bbox->ll.y && dbu_point.y <= bbox->ur.y)
                hits.push_back(*it);
        }
        return hits;
    }

    /// Rubber-band counterpart to hit_test_placements_point above - every
    /// top-level placement whose own world bbox is fully enclosed by
    /// `dbu_rect` (all of them, not just the topmost), in no particular
    /// order.
    inline std::vector<PlacementId> hit_test_placements_rect(const Root &root, LayoutId layout_id, int remaining_depth, Rect dbu_rect,
                                                             const std::vector<PlacementId> *candidates = nullptr)
    {
        std::vector<PlacementId> result;
        for (PlacementId placement_id : candidates ? *candidates : root.get_layout_placements(layout_id))
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
        WireId wire_id; // instead of shape_id, for a piece of a Wire (of le::wire_to_shape's Shape)
        PieceKind piece_kind;
        size_t piece_index;
        ShapeData outline;
    };

    /// @brief Whether a given ViewLayer (by layer name + purpose) should
    /// be considered for hit-testing at all - a caller-supplied predicate
    /// (`LeHandle::is_view_layer_selectable`) rather than a `LeHandle`
    /// reference/pointer, so this header (`core`) doesn't take a
    /// dependency on `api`.
    using ViewLayerSelectablePredicate = std::function<bool(const std::string &, ViewLayerPurpose)>;

    /// @brief True when `piece`'s own bbox is under 1 on-screen pixel in
    /// both dimensions at `scale` - the exact same "invisible, don't draw
    /// it" test `draw_helpers.hpp`'s `bbox_is_sub_pixel` applies at
    /// render time (duplicated rather than shared because draw_helpers
    /// lives in `pipelines`, which `core` can't depend on). A piece the
    /// renderer skips must not be hit-testable either: a click landing on
    /// a shape too small to see must not select it
    /// (`ApiFixture.SubPixelShapeIsNotRenderedAndIsNotSelectable`).
    inline bool abstract_piece_is_sub_pixel(const ShapeData &piece, double scale)
    {
        const std::optional<Rect> bbox = Geometry::bbox(piece);
        if (!bbox)
            return true;
        const double width = static_cast<double>(bbox->ur.x - bbox->ll.x);
        const double height = static_cast<double>(bbox->ur.y - bbox->ll.y);
        return width * scale < 1.0 && height * scale < 1.0;
    }

    /// @brief The shared walk behind hit_test_abstract_point_all/
    /// hit_test_layout_point_all: every selectable piece of the shapes in
    /// `by_layer` containing `dbu_point`, topmost ViewLayer first
    /// (`view_layers.all()` is bottom-to-top), each shape's pieces in
    /// find_hit_pieces order. Sub-pixel pieces aren't rendered, so they
    /// aren't hit either. `first_only` stops at the first hit.
    inline std::vector<AbstractHitPiece> point_hits_topmost_first(
        const Root &root, const ViewLayerSet &view_layers, const std::unordered_map<ViewLayerId, std::vector<GeometryId>> &by_layer,
        Point dbu_point, double scale, const ViewLayerSelectablePredicate &is_selectable, bool first_only)
    {
        std::vector<AbstractHitPiece> hits;
        const std::vector<ViewLayerId> order = view_layers.all();
        for (auto layer_it = order.rbegin(); layer_it != order.rend(); ++layer_it)
        {
            const auto group_it = by_layer.find(*layer_it);
            if (group_it == by_layer.end())
                continue;

            const ViewLayerData *data = view_layers.get(*layer_it);
            if (data && !is_selectable(data->layer_name, data->purpose))
                continue;

            ShapeData scratch;
            for (const GeometryId id : group_it->second)
            {
                const ShapeData *shape = geometry_shape(root, id, scratch);
                if (!shape)
                    continue;
                for (HitPiece &piece : Geometry::find_hit_pieces(*shape, dbu_point))
                {
                    if (abstract_piece_is_sub_pixel(piece.outline, scale))
                        continue; // invisible at this scale - not rendered, so not selectable either
                    hits.push_back(AbstractHitPiece{.shape_id = id.shape, .wire_id = id.wire, .piece_kind = piece.kind, .piece_index = piece.index, .outline = std::move(piece.outline)});
                    if (first_only)
                        return hits;
                }
            }
        }
        return hits;
    }

    /// @brief Abstract-view analog of hit_test_placements_point above.
    /// Tests `Root`'s raw Terminal-port/Obstruction `ShapeData` rather than
    /// rendered output: a piece a caller selects or moves must be
    /// addressable in `Root` by `(shape_id, piece_kind, piece_index)`, and
    /// the Rasterize stage's iterate-expanded geometry has more entries
    /// than Root's raw rects/polygons/paths. Only Terminal/Obstruction
    /// pieces are selectable in an Abstract view, never the BOUNDARY
    /// shape.
    ///
    /// Topmost-selectable-ViewLayer-first (`view_layers.all()` is
    /// bottom-to-top insertion order - see that method's own doc comment -
    /// so reverse means topmost first), first-match-within-a-layer wins
    /// for two overlapping shapes on the same layer (an accepted
    /// limitation). Every
    /// Terminal-port/Obstruction Shape always carries a real, valid
    /// `Shape.layer` (unlike a BOUNDARY/PLACEMENT_BLOCKAGE Shape, which
    /// resolves its own ViewLayer via `Shape.purpose` instead - see
    /// `HierarchyResolverStage::resolve_view_layer`'s own comment) so
    /// `view_layers.find(shape.layer, purpose)` alone is enough here,
    /// with no fallback-by-purpose branch needed.
    inline std::vector<AbstractHitPiece> hit_test_abstract_point_all(
        const Root &root, const ViewLayerSet &view_layers, AbstractId abstract_id, Point dbu_point,
        double scale, const ViewLayerSelectablePredicate &is_selectable, bool first_only = false)
    {
        std::unordered_map<ViewLayerId, std::vector<GeometryId>> by_layer;

        for (TerminalId terminal_id : root.get_abstract_terminals(abstract_id))
            for (TerminalPortId port_id : root.get_terminal_ports(terminal_id))
                for (ShapeId shape_id : root.get_terminal_port_shapes(port_id))
                {
                    const ShapeData *shape = root.get_shape(shape_id);
                    if (!shape || !shape->layer.valid())
                        continue;
                    by_layer[view_layers.find(shape->layer, ViewLayerPurpose::TERMINAL)].push_back(GeometryId::of(shape_id));
                }

        for (ObstructionId obstruction_id : root.get_abstract_obstructions(abstract_id))
            for (ShapeId shape_id : root.get_obstruction_shapes(obstruction_id))
            {
                const ShapeData *shape = root.get_shape(shape_id);
                if (!shape || !shape->layer.valid())
                    continue;
                by_layer[view_layers.find(shape->layer, ViewLayerPurpose::OBSTRUCTION)].push_back(GeometryId::of(shape_id));
            }

        return point_hits_topmost_first(root, view_layers, by_layer, dbu_point, scale, is_selectable, first_only);
    }

    /// @brief The topmost selectable piece under `dbu_point`, if any -
    /// hit_test_abstract_point_all's first hit.
    inline std::optional<AbstractHitPiece> hit_test_abstract_point(
        const Root &root, const ViewLayerSet &view_layers, AbstractId abstract_id, Point dbu_point,
        double scale, const ViewLayerSelectablePredicate &is_selectable)
    {
        std::vector<AbstractHitPiece> hits = hit_test_abstract_point_all(root, view_layers, abstract_id, dbu_point, scale, is_selectable, /*first_only=*/true);
        if (hits.empty())
            return std::nullopt;
        return std::move(hits.front());
    }

    /// @brief Rubber-band counterpart to hit_test_abstract_point above -
    /// every Terminal/Obstruction piece fully enclosed by `dbu_rect`,
    /// scanning every selectable ViewLayer (no topmost-only restriction),
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
            const ShapeData *shape = root.get_shape(shape_id);
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
    /// Calls `visit(id, purpose)` for `layout_id`'s route Wires and
    /// Shapes (ROUTE) then its physical-port shapes (TERMINAL) - all of
    /// them, or only those in `candidates` (layout_candidates', which
    /// holds exactly these), in the same routes-then-ports order.
    /// Then each renderable class's shapes (renderable_classes.hpp), with
    /// the class's purpose.
    template <typename Visit>
    void for_each_layout_hit_shape(const Root &root, LayoutId layout_id, const std::vector<GeometryId> *candidates, Visit &&visit)
    {
        if (candidates)
        {
            for (const GeometryId id : *candidates)
                if (geometry_route(root, id).valid())
                    visit(id, ViewLayerPurpose::ROUTE);
            for (const GeometryId id : *candidates)
                if (const ShapeData *shape = root.get_shape(id.shape); shape && !shape->route().valid() && shape->physical_port_segment().valid())
                    visit(id, ViewLayerPurpose::TERMINAL);
            renderable::for_each([&]<class R>(R) {
                for (const GeometryId id : *candidates)
                    if (const ShapeData *shape = root.get_shape(id.shape); shape && R::owner_of(*shape).valid())
                        visit(id, R::purpose);
            });
            return;
        }
        for (RouteId route_id : root.get_layout_routes(layout_id))
        {
            for (WireId wire_id : root.get_route_wires(route_id))
                visit(GeometryId::of(wire_id), ViewLayerPurpose::ROUTE);
            for (ShapeId shape_id : root.get_route_shapes(route_id))
                visit(GeometryId::of(shape_id), ViewLayerPurpose::ROUTE);
        }
        for (PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
            for (PhysicalPortSegmentId segment_id : root.get_physical_port_segments(port_id))
                for (ShapeId shape_id : root.get_physical_port_segment_shapes(segment_id))
                    visit(GeometryId::of(shape_id), ViewLayerPurpose::TERMINAL);
        renderable::for_each([&]<class R>(R) {
            for (const typename R::Id object : R::in_layout(root, layout_id))
                for (const ShapeId shape_id : R::shapes(root, object))
                    visit(GeometryId::of(shape_id), R::purpose);
        });
    }

    /// @brief The view layer a Layout-view hit shape draws on: where a
    /// renderable class puts it (ViewLayerSet::renderable_view_layer), else
    /// its layer's `purpose` column; invalid if it has neither.
    inline ViewLayerId layout_hit_view_layer(const ViewLayerSet &view_layers, const ShapeData &shape, ViewLayerPurpose purpose)
    {
        std::optional<ViewLayerId> renderable;
        renderable::for_each([&]<class R>(R) {
            if (purpose == R::purpose)
                renderable = view_layers.renderable_view_layer<R>(shape);
        });
        if (renderable)
            return *renderable;
        return shape.layer.valid() ? view_layers.find(shape.layer, purpose) : ViewLayerId{};
    }

    inline std::vector<AbstractHitPiece> hit_test_layout_point_all(
        const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, Point dbu_point,
        double scale, const ViewLayerSelectablePredicate &is_selectable, bool first_only = false,
        const std::vector<GeometryId> *candidates = nullptr)
    {
        std::unordered_map<ViewLayerId, std::vector<GeometryId>> by_layer;
        for_each_layout_hit_shape(root, layout_id, candidates, [&](GeometryId id, ViewLayerPurpose purpose)
                                  {
            if (const WireData *wire = root.get_wire(id.wire))
            {
                by_layer[view_layers.find(wire->layer, purpose)].push_back(id);
                return;
            }
            const ShapeData *shape = root.get_shape(id.shape);
            if (!shape)
                return;
            if (const ViewLayerId view_layer = layout_hit_view_layer(view_layers, *shape, purpose); view_layer.valid())
                by_layer[view_layer].push_back(id); });
        return point_hits_topmost_first(root, view_layers, by_layer, dbu_point, scale, is_selectable, first_only);
    }

    /// @brief The topmost selectable piece under `dbu_point`, if any -
    /// hit_test_layout_point_all's first hit.
    inline std::optional<AbstractHitPiece> hit_test_layout_point(
        const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, Point dbu_point,
        double scale, const ViewLayerSelectablePredicate &is_selectable)
    {
        std::vector<AbstractHitPiece> hits = hit_test_layout_point_all(root, view_layers, layout_id, dbu_point, scale, is_selectable, /*first_only=*/true);
        if (hits.empty())
            return std::nullopt;
        return std::move(hits.front());
    }

    /// @brief Rubber-band counterpart to hit_test_layout_point above -
    /// every Route/PhysicalPort piece fully enclosed by `dbu_rect`,
    /// scanning every selectable ViewLayer, in no particular order - see
    /// hit_test_abstract_rect's own comment for the shared semantics.
    inline std::vector<AbstractHitPiece> hit_test_layout_rect(
        const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, Rect dbu_rect,
        double scale, const ViewLayerSelectablePredicate &is_selectable, const std::vector<GeometryId> *candidates = nullptr)
    {
        std::vector<AbstractHitPiece> result;

        ShapeData scratch;
        auto collect = [&](GeometryId id, ViewLayerPurpose purpose)
        {
            const ShapeData *shape = geometry_shape(root, id, scratch);
            if (!shape)
                return;
            const ViewLayerId view_layer = layout_hit_view_layer(view_layers, *shape, purpose);
            if (!view_layer.valid())
                return;
            const ViewLayerData *data = view_layers.get(view_layer);
            if (data && !is_selectable(data->layer_name, data->purpose))
                return;

            for (const HitPiece &piece : Geometry::fully_enclosed_pieces(dbu_rect, *shape))
            {
                if (abstract_piece_is_sub_pixel(piece.outline, scale))
                    continue; // invisible at this scale - not rendered, so not selectable either
                result.push_back(AbstractHitPiece{.shape_id = id.shape, .wire_id = id.wire, .piece_kind = piece.kind, .piece_index = piece.index, .outline = piece.outline});
            }
        };

        for_each_layout_hit_shape(root, layout_id, candidates, collect);
        return result;
    }
}
