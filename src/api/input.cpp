#include "api_internal.hpp"
#include "../geometry/geometry.hpp"
#include "generated/api/snapshot_appliers.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace le::api_detail;

namespace
{
    // apply_shape_snapshot, plus Shape.vias/via_iterates and the DEF masks
    // (rect_/polygon_/path_masks, index-parallel to their pieces) - the
    // generated update_shape (and so the generated snapshot applier)
    // carries none of them, and Move/Delete change them. Used for every
    // Shape edit this file records.
    bool apply_shape_snapshot_with_vias(le::Root &root, le::ShapeId id, const le::ShapeData &data)
    {
        if (!le::apply_shape_snapshot(root, id, data))
            return false;
        if (le::ShapeData *shape = root.get_shape(id))
        {
            shape->vias = data.vias;
            shape->via_iterates = data.via_iterates;
            shape->rect_masks = data.rect_masks; // Delete removes a piece's mask with it
            shape->polygon_masks = data.polygon_masks;
            shape->path_masks = data.path_masks;
            root.note_shape_changed(id); // written through the pointer - not in the change log otherwise
        }
        return true;
    }

    // apply_wire_snapshot, plus the Wire's segments, vias and widths,
    // which the generated update_wire doesn't carry.
    bool apply_wire_snapshot_with_lists(le::Root &root, le::WireId id, const le::WireData &data)
    {
        if (!le::apply_wire_snapshot(root, id, data))
            return false;
        if (le::WireData *wire = root.get_wire(id))
        {
            wire->segments = data.segments;
            wire->vias = data.vias;
            wire->widths = data.widths;
            root.note_wire_changed(id); // written through the pointer - not in the change log otherwise
        }
        return true;
    }

    // A piece owner's geometry as one Shape - a Shape's own, or a Wire's
    // le::wire_to_shape - for the edits below, which work on Shapes.
    std::optional<le::ShapeData> owner_geometry(const le::Root &root, le::GeometryId owner)
    {
        le::ShapeData scratch;
        const le::ShapeData *shape = le::geometry_shape(root, owner, scratch);
        if (!shape)
            return std::nullopt;
        return shape == &scratch ? std::move(scratch) : *shape;
    }

    // Stores `after` as `owner`'s geometry (`before` being what
    // owner_geometry read) and records it into the current transaction. A
    // Wire takes it back through le::shape_to_wire; one that no longer
    // fits a Wire is left unchanged - false.
    bool store_owner_geometry(LeHandle *handle, le::GeometryId owner, const le::ShapeData &before, const le::ShapeData &after)
    {
        le::Root &root = handle->root;
        le::editing::Transaction *txn = handle->command_history.current();
        if (owner.shape.valid())
        {
            if (!apply_shape_snapshot_with_vias(root, owner.shape, after))
                return false;
            if (txn)
                txn->record_update<le::ShapeId, le::ShapeData>(owner.shape, before, after, &apply_shape_snapshot_with_vias);
            return true;
        }
        const le::WireData *wire = root.get_wire(owner.wire);
        if (!wire)
            return false;
        std::optional<le::WireData> stored = le::shape_to_wire(root, after, wire->route);
        if (!stored)
        {
            spdlog::error("A Wire holds Manhattan segments and defined vias only - edit not applied.");
            return false;
        }
        const le::WireData wire_before = *wire;
        apply_wire_snapshot_with_lists(root, owner.wire, *stored);
        if (txn)
            txn->record_update<le::WireId, le::WireData>(owner.wire, wire_before, *stored, &apply_wire_snapshot_with_lists);
        return true;
    }

    // Deletes `owner` (a Shape or a Wire), recording it for undo.
    // Deletes every Shape and Wire in `owners`, each class as one batch so
    // the child lists they leave are compacted once.
    void delete_owners(LeHandle *handle, const std::vector<le::GeometryId> &owners)
    {
        std::vector<le::ShapeId> shapes;
        std::vector<le::WireId> wires;
        for (const le::GeometryId &owner : owners)
        {
            if (owner.shape.valid())
                shapes.push_back(owner.shape);
            else
                wires.push_back(owner.wire);
        }
        le::editing::Transaction *txn = handle->command_history.current();
        le::editing::delete_objects(handle->root, txn, std::span<const le::ShapeId>(shapes));
        le::editing::delete_objects(handle->root, txn, std::span<const le::WireId>(wires));
    }
}

namespace le::api_detail
{
    // Where each of `placements` (LeHandle::moving_placements()) would land
    // for a Placement Move by `raw_delta` right now - the handle's own
    // snap mode applied (le::plan_placement_move).
    // Shared by the ghost preview and the commit so both always agree.
    // Empty outside a Layout view.
    std::vector<le::PlacementMoveTarget> plan_moving_placements_unlocked(const LeHandle *handle, const std::vector<le::PlacementId> &placements, le::Point raw_delta)
    {
        if (!handle->current_layout().valid())
            return {};
        const int remaining_depth = std::max(0, handle->hierarchy_depth() - 1);
        return le::plan_placement_move(handle->root, handle->current_layout(), placements, le::Orientation::N,
                                       raw_delta, handle->placement_snap_mode(), remaining_depth);
    }
}

namespace
{
    // --- Resize ---

    // How close (screen pixels) a press must land to a selected piece's
    // edge/segment to grab it.
    constexpr double kResizeGrabTolerancePx = 6.0;

    // What a resize of a `kind` piece on `layer` snaps against right now -
    // the handle's per-kind snap mode plus the grids it may need; tracks
    // only for a path in TRACKS mode (its own layer's).
    le::ShapeSnapContext shape_snap_context_unlocked(const LeHandle *handle, le::PieceKind kind, le::LayerId layer)
    {
        le::ShapeSnapContext context;
        context.mode = handle->shape_snap_mode(kind);
        context.user_grid = handle->minor_grid_spacing();
        context.manufacturing_grid = le::technology_manufacturing_grid(handle->root);
        context.fin_grid = le::technology_fin_grid(handle->root);
        if (context.mode == le::ShapeSnapMode::TRACKS && le::shape_snap_mode_applies(kind, le::ShapeSnapMode::TRACKS))
            context.tracks = le::layer_track_grids(handle->root, handle->current_layout(), layer);
        return context;
    }
}

namespace le::api_detail
{
    // Pieces Move snaps one by one -
    // routes' paths, vias and via arrays - rather than by the shared
    // user-grid delta.
    bool snaps_individually_when_moved(le::PieceKind kind)
    {
        return kind == le::PieceKind::PATH || kind == le::PieceKind::VIA || kind == le::PieceKind::VIA_ITERATE;
    }

    // The delta each of move().moving_pieces moves by right now (parallel
    // to it) - shared by the ghost and the commit so they always agree.
    // A path/via/via array snaps on its own (le::snap_moved_piece_delta,
    // under the one routing snap mode they share, from the raw mouse
    // offset); every other
    // piece moves by the shared user-grid delta (a Placement's entry is
    // unused - plan_moving_placements_unlocked places those). nullopt until
    // anchored.
    std::optional<std::vector<le::Point>> moving_piece_deltas_unlocked(const LeHandle *handle)
    {
        const std::optional<le::Point> delta = handle->move_delta(handle->move_free_form());
        if (!delta)
            return std::nullopt;
        const std::optional<le::Point> raw_delta = handle->move_raw_delta(handle->move_free_form());

        // One snap context per (snap slot, layer) - a TRACKS context
        // resolves the layer's track grids, not worth redoing per piece.
        std::map<std::pair<le::PieceKind, le::LayerId>, le::ShapeSnapContext> contexts;
        std::vector<le::Point> deltas;
        deltas.reserve(handle->move().moving_pieces.size());
        for (const LeHandle::SelectedObject &selected : handle->move().moving_pieces)
        {
            le::Point piece_delta = *delta;
            const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
            le::ShapeData scratch;
            if (piece && raw_delta && snaps_individually_when_moved(piece->piece_kind))
                if (const le::ShapeData *data = le::geometry_shape(handle->root, piece->owner(), scratch);
                    data && le::Geometry::piece_in_range(*data, piece->piece_kind, piece->piece_index))
                {
                    const auto key = std::make_pair(le::shape_snap_slot(piece->piece_kind), data->layer);
                    auto it = contexts.find(key);
                    if (it == contexts.end())
                        it = contexts.emplace(key, shape_snap_context_unlocked(handle, piece->piece_kind, data->layer)).first;
                    piece_delta = le::snap_moved_piece_delta(le::Geometry::extract_piece(*data, piece->piece_kind, piece->piece_index),
                                                             piece->piece_kind, *raw_delta, it->second);
                }
            deltas.push_back(piece_delta);
        }
        return deltas;
    }
}

namespace
{
    // The grabbed piece resized to the mouse at dbu `current` - the ghost
    // while dragging, the committed geometry on release.
    le::ShapeData resized_piece_unlocked(const LeHandle *handle, le::Point current)
    {
        const LeHandle::ResizeGrab &grab = *handle->resize().grab;
        const le::Point delta{.x = current.x - grab.start.x, .y = current.y - grab.start.y};
        return le::resize_piece(grab.original, grab.handle, delta,
                                shape_snap_context_unlocked(handle, grab.piece.piece_kind, grab.original.layer));
    }
}

namespace le::api_detail
{
    // Every Shape the current grab would change if committed at dbu
    // `current` - shared by the ghost and the commit so they always agree.
    // A moved path segment drags along any other path point sitting on
    // one of its original endpoints (le::follow_moved_path_segment): in its
    // own Shape or Wire, and in its Route's other Wires and Shapes on the
    // same layer - a DEF route's wire runs are separate Paths that only
    // share coordinates.
    std::vector<ResizeEdit> plan_resize_unlocked(const LeHandle *handle, le::Point current)
    {
        const LeHandle::ResizeGrab &grab = *handle->resize().grab;
        const std::optional<le::ShapeData> existing = owner_geometry(handle->root, grab.piece.owner());
        if (!existing || !le::Geometry::piece_in_range(*existing, grab.piece.piece_kind, grab.piece.piece_index))
            return {};

        const le::ShapeData resized = resized_piece_unlocked(handle, current);
        std::vector<ResizeEdit> edits;
        ResizeEdit own{.owner = grab.piece.owner(), .before = *existing, .after = *existing, .ghost_pieces = {resized}};
        le::replace_piece(own.after, grab.piece.piece_kind, grab.piece.piece_index, resized);

        if (grab.piece.piece_kind == le::PieceKind::PATH && !grab.original.paths.empty() && !resized.paths.empty())
        {
            const auto &from = grab.original.paths.front().polygon.points;
            const auto &to = resized.paths.front().polygon.points;
            const size_t e = grab.handle.edge;
            if (e + 1 < from.size() && e + 1 < to.size())
            {
                const auto follow = [&](le::ShapeData &data, std::optional<size_t> skip, std::vector<le::ShapeData> &ghost)
                {
                    const std::vector<size_t> changed = le::follow_moved_path_segment(data, skip, from[e], to[e], from[e + 1], to[e + 1]);
                    for (const size_t i : changed)
                        ghost.push_back(le::Geometry::extract_piece(data, le::PieceKind::PATH, i));
                    return !changed.empty();
                };
                follow(own.after, grab.piece.piece_index, own.ghost_pieces);

                const le::RouteId route = le::geometry_route(handle->root, grab.piece.owner());
                std::vector<le::GeometryId> siblings;
                if (route.valid())
                {
                    for (const le::WireId wire_id : handle->root.get_route_wires(route))
                        siblings.push_back(le::GeometryId::of(wire_id));
                    for (const le::ShapeId shape_id : handle->root.get_route_shapes(route))
                        siblings.push_back(le::GeometryId::of(shape_id));
                }
                for (const le::GeometryId sibling_id : siblings)
                {
                    if (sibling_id == grab.piece.owner())
                        continue;
                    const std::optional<le::ShapeData> sibling = owner_geometry(handle->root, sibling_id);
                    if (!sibling || sibling->layer != existing->layer)
                        continue;
                    ResizeEdit edit{.owner = sibling_id, .before = *sibling, .after = *sibling, .ghost_pieces = {}};
                    if (follow(edit.after, std::nullopt, edit.ghost_pieces))
                        edits.push_back(std::move(edit));
                }
            }
        }
        edits.insert(edits.begin(), std::move(own));
        return edits;
    }
}

namespace
{
    void update_resize_hover_unlocked(LeHandle *handle);

    // arm_resize/le_arm_resize's body - Edit mode, with at least one
    // selected piece (Row/Placement/Region selections have no edges to
    // drag).
    void arm_resize_unlocked(LeHandle *handle)
    {
        if (handle->mode() != LeHandle::Mode::EDIT)
            return;
        const auto &selection = handle->selection();
        // A placement has no edges to resize, and resizing the shapes
        // alongside one would leave it behind - the GUI's Resize button is
        // disabled to match.
        if (std::ranges::any_of(selection, [](const LeHandle::SelectedObject &s)
                                { return std::holds_alternative<le::PlacementId>(s); }))
            return;
        if (std::ranges::none_of(selection, [](const LeHandle::SelectedObject &s)
                                 {
                                     const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&s);
                                     return piece && piece->piece_kind != le::PieceKind::VIA && piece->piece_kind != le::PieceKind::VIA_ITERATE; // a via has no edges to drag
                                 }))
            return;
        handle->arm_resize();
        update_resize_hover_unlocked(handle);
    }

    // The edge/segment of a selected piece nearest dbu `p` (within
    // kResizeGrabTolerancePx) - what the hover indicator shows and a first
    // click grabs. nullopt if there's none that close.
    std::optional<LeHandle::ResizeGrab> find_resize_target_unlocked(const LeHandle *handle, le::Point p)
    {
        const int64_t tolerance = static_cast<int64_t>(std::ceil(kResizeGrabTolerancePx / handle->scale()));
        std::optional<LeHandle::ResizeGrab> best;
        for (const LeHandle::SelectedObject &selected : handle->selection())
        {
            const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
            if (!piece)
                continue;
            le::ShapeData scratch;
            const le::ShapeData *data = le::geometry_shape(handle->root, piece->owner(), scratch);
            if (!data || !le::Geometry::piece_in_range(*data, piece->piece_kind, piece->piece_index))
                continue;
            le::ShapeData original = le::Geometry::extract_piece(*data, piece->piece_kind, piece->piece_index);
            const std::optional<le::ResizeHandle> hit = le::find_resize_handle(original, p, tolerance);
            if (hit && (!best || hit->distance < best->handle.distance))
                best = LeHandle::ResizeGrab{.piece = *piece, .handle = *hit, .original = std::move(original), .start = p};
        }
        return best;
    }

    // le_set_mouse_position with Resize armed and nothing grabbed: the
    // hover indicator for whatever is now under the mouse.
    void update_resize_hover_unlocked(LeHandle *handle)
    {
        std::optional<le::ResizeHandleSegment> hover;
        if (handle->mode() == LeHandle::Mode::EDIT && handle->resize().armed && !handle->resize().grab)
            if (const std::optional<le::Point> mouse = handle->mouse_dbu_position())
                if (const std::optional<LeHandle::ResizeGrab> target = find_resize_target_unlocked(handle, *mouse))
                    hover = le::resize_handle_segment(target->original, target->handle);
        handle->set_resize_hover(hover);
    }

    // Applies plan_resize_unlocked at dbu `p` - every changed Shape - as
    // one undoable "resize". Resize stays armed.
    void commit_resize_unlocked(LeHandle *handle, le::Point p)
    {
        const std::vector<ResizeEdit> edits = plan_resize_unlocked(handle, p);
        if (edits.empty())
            return;

        handle->command_history.begin("resize");
        for (const ResizeEdit &edit : edits)
            store_owner_geometry(handle, edit.owner, edit.before, edit.after);
        handle->root.bump_mutation_version();
        handle->command_history.end(/*succeeded=*/true);
    }

    // le_mouse_up's Edit-mode click with Resize armed - Resize's two-click
    // gesture: the first click grabs the edge/segment under it (a click
    // on nothing grabbable does nothing), the second commits the ghost.
    // Returns false if Resize isn't armed, so the click falls through to
    // Move.
    bool resize_click_unlocked(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle->resize().armed)
            return false;
        const le::Point p = handle->pixel_to_dbu(x, y);
        if (!handle->resize().grab)
        {
            if (std::optional<LeHandle::ResizeGrab> target = find_resize_target_unlocked(handle, p))
                handle->begin_resize_grab(std::move(*target));
            return true;
        }
        commit_resize_unlocked(handle, p);
        handle->end_resize_grab();
        update_resize_hover_unlocked(handle);
        return true;
    }

    // le_placement_orientation_ops_enabled's own body - a bit (1 << op)
    // per LeOrientationOp that every selected placement allows right now:
    // none while a Move is under way - anchored by its first click, ghost
    // showing (merely arming Move doesn't count) - or outside a Layout view; otherwise each op the
    // PlacementSnapper permits for every one of them (only ever restricted
    // under SITE snapping, by the row's Site symmetry).
    // Whether any of `placements` is FIXED or COVER.
    bool any_placement_locked(const LeHandle *handle, const std::vector<le::PlacementId> &placements)
    {
        return std::ranges::any_of(placements, [&](le::PlacementId id)
                                   {
                                       const le::PlacementData *placement = handle->root.get_placement(id);
                                       return placement && le::placement_is_locked(placement->placement_status);
                                   });
    }

    int32_t orientation_ops_enabled_unlocked(const LeHandle *handle, const std::vector<le::PlacementId> &placements)
    {
        if (placements.empty() || handle->move().anchor || !handle->current_layout().valid() || any_placement_locked(handle, placements))
            return 0;

        const le::PlacementSnapper snapper(handle->root, handle->current_layout(), handle->placement_snap_mode());
        int32_t mask = (1 << LE_ORIENTATION_OP_ROTATE_CCW) | (1 << LE_ORIENTATION_OP_FLIP_HORIZONTAL) | (1 << LE_ORIENTATION_OP_FLIP_VERTICAL);
        for (const le::PlacementId id : placements)
        {
            const le::PlacementData *placement = handle->root.get_placement(id);
            if (!placement || !placement->location)
                continue;
            const le::AbstractData *abstract = handle->root.get_abstract(handle->root.get_design_abstract(placement->reference_design));
            for (int32_t op = LE_ORIENTATION_OP_ROTATE_CCW; op <= LE_ORIENTATION_OP_FLIP_VERTICAL; ++op)
                if (!snapper.permits(static_cast<le::OrientationOp>(op), *placement->location, abstract))
                    mask &= ~(1 << op);
        }
        return mask;
    }

    // Below this many pixels of down-to-up movement, le_mouse_up treats a
    // gesture as a click rather than a drag-select - small enough that an
    // intended click with a little hand tremor still registers as one,
    // large enough that a real rubber-band drag never gets misread as a
    // click. Not exposed/configurable - an implementation detail of the
    // click-vs-drag decision, not a persistent LeHandle concern.
    constexpr int32_t kClickDragThresholdPx = 4;

    // Fixed step sizes for the keyboard-triggered canvas-navigation
    // commands (le_key_down's LE_KEY_ZOOM/LE_KEY_FIT/LE_KEY_PAN_*) - not
    // exposed/configurable, same reasoning as kClickDragThresholdPx.
    constexpr double kKeyZoomFactor = 0.3;
    constexpr int32_t kKeyFitPaddingPx = 10;
    constexpr double kKeyPanFactor = 0.25;

    // le_tooltip_message's text, one per LeHandle::Mode. Edit mode lists
    // every key le_key_down acts on in that mode, separated by a blank
    // line ("\n\n"); the Info panel wraps each to its width.
    constexpr const char *kSelectModeTooltip =
        "Left click to select. Shift for multi-select. Left click and drag for rectangle multi-select.";
    constexpr const char *kEditModeTooltip =
        "Edits the current selection (S switches to Select mode to change it).\n\n"
        "Ctrl-M: arm Move. Click to set the start point, move the mouse, click again to commit. Stays armed until Esc.\n\n"
        "Shift (held while moving): free-form, non-orthogonal move.\n\n"
        "Ctrl-R: arm Resize. Click an edge or path segment of a selected shape, then click again to place it. "
        "Stays armed until Esc.\n\n"
        "Delete: delete the selected shapes.\n\n"
        "Esc: cancel the move or resize in progress, or disarm it.\n\n"
        "Ctrl-Z / Ctrl-Shift-Z: undo / redo.\n\n"
        "Z / Shift-Z: zoom in / out at the mouse.\n\n"
        "F: fit the design.\n\n"
        "Ctrl-F: fit the selection.\n\n"
        "Arrows: pan.\n\n"
        "1-9, 0: show/hide routing layers 1-10 (Ctrl-1..9 for 11-19).\n\n"
        "S / R: switch to Select / Ruler mode.";
    constexpr const char *kRulerModeTooltip =
        "Click to add a ruler point. Shift for a non-orthogonal segment. Esc to finish the ruler.";

    // Builds the one-piece ghost-preview geometry for a single selected
    // piece - a fresh Root lookup plus
    // Geometry::extract_piece, shared by arm_move_unlocked and
    // refresh_armed_move_geometry_unlocked. An empty one-piece Shape
    // (drawing nothing) if the shape itself or the piece index has gone
    // stale since it was selected, rather than crashing or substituting
    // the wrong piece. Also empty for the Row/Placement/Region
    // alternatives: Rows/Regions aren't movable, and a Placement's ghost
    // is planned per frame instead (plan_moving_placements_unlocked -
    // its snapped position depends on the live mouse, not one shared
    // offset).
    le::ShapeData move_ghost_piece_unlocked(LeHandle *handle, const LeHandle::SelectedObject &selected)
    {
        const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
        if (!piece)
            return le::ShapeData{};

        le::ShapeData scratch;
        const le::ShapeData *data = le::geometry_shape(handle->root, piece->owner(), scratch);
        if (!data)
            return le::ShapeData{};
        return drawable_piece(handle->root, *data, piece->piece_kind, piece->piece_index, handle->current_layout());
    }

    // How many selected placements are FIXED or COVER - placements Move
    // leaves alone.
    int32_t locked_placement_count_unlocked(const LeHandle *handle)
    {
        return static_cast<int32_t>(std::ranges::count_if(selected_placements_unlocked(handle), [&](le::PlacementId id)
                                                          {
                                                              const le::PlacementData *placement = handle->root.get_placement(id);
                                                              return placement && le::placement_is_locked(placement->placement_status);
                                                          }));
    }

    // LE_KEY_MOVE/le_arm_move's own body - unlocked
    // variant, same reasoning as fit_selected_unlocked
    // below (called from inside le_key_down, which already holds
    // handle->mutex_). Only meaningful in Edit mode with a non-empty
    // selection - the Mode::EDIT check lives here rather than inside
    // LeHandle::arm_move itself (LeHandle's own Move state stays mode-
    // agnostic - see LeHandle::set_mode's own comment on the analogous
    // Ruler-mode split).
    // Snapshots each selected *piece's* current geometry for the ghost
    // overlay (LeHandle::MoveState::moving_geometry), piece-granular -
    // Move moves exactly whichever
    // pieces are selected, not necessarily every piece of their owning
    // Shapes.
    void arm_move_unlocked(LeHandle *handle)
    {
        if (handle->mode() != LeHandle::Mode::EDIT)
            return;
        // Placements snap to sites/rows and shapes to their own grids, so a
        // Move of both at once has no one right answer - refused, and the
        // GUI's Move button is disabled to match (the same rule as
        // Resize).
        const auto &selection = handle->selection();
        const auto is_placement = [](const LeHandle::SelectedObject &s)
        { return std::holds_alternative<le::PlacementId>(s); };
        if (std::ranges::any_of(selection, is_placement) && !std::ranges::all_of(selection, is_placement))
            return;
        // FIXED and COVER placements stay where they are until unlocked.
        if (locked_placement_count_unlocked(handle) > 0)
            return;

        std::vector<le::ShapeData> geometry;
        geometry.reserve(handle->selection().size());
        for (const LeHandle::SelectedObject &selected : handle->selection())
            geometry.push_back(move_ghost_piece_unlocked(handle, selected));

        handle->arm_move(std::move(geometry));
    }
}

namespace le::api_detail
{
    // le_undo/le_redo's own follow-up - unlocked
    // variant, called right after handle->command_history.undo()/redo()
    // succeeds. If Move is currently armed (including the "stays armed
    // after a commit" case - see move_click_unlocked), the moving
    // pieces' geometry may have just changed out from under its own
    // moving_geometry snapshot (taken at the last arm/re-arm, not
    // continuously) - re-snapshot it via LeHandle::refresh_move_geometry so
    // a subsequent move's ghost preview starts from the actual
    // post-undo/redo position rather than a stale one. A no-op (via
    // refresh_move_geometry's own guard) if Move isn't armed.
    void refresh_armed_move_geometry_unlocked(LeHandle *handle)
    {
        if (!handle->move().armed)
            return;

        std::vector<le::ShapeData> geometry;
        geometry.reserve(handle->move().moving_pieces.size());
        for (const LeHandle::SelectedObject &selected : handle->move().moving_pieces)
            geometry.push_back(move_ghost_piece_unlocked(handle, selected));
        handle->refresh_move_geometry(std::move(geometry));
    }
}

namespace
{
    // Shows `status` in the Layers panel's placement-status filter, so a
    // placement just given it doesn't disappear.
    void reveal_placement_status_unlocked(LeHandle *handle, le::PlacementStatus status)
    {
        if (!handle->is_object_filter_visible(LE_OBJECT_FILTER_PLACEMENT_STATUS, le::to_string(status)))
            handle->set_object_filter_visible(LE_OBJECT_FILTER_PLACEMENT_STATUS, le::to_string(status), true);
    }

    // le_mouse_up's Edit-mode branch - unlocked
    // variant, called with handle->mutex_ already held. First click (no
    // anchor yet) sets the move's anchor (LeHandle::move_set_anchor, which
    // - like LeHandle::add_ruler_point's own click handling just below in
    // le_mouse_up - reads the separately-tracked *stored* mouse position,
    // not an x/y passed in here); second click (anchor already set)
    // computes the delta and commits: for each moving *piece*, re-fetches
    // its owning Shape's *current* full geometry from Root (not the
    // arm-time snapshot, which is ghost-rendering-only - see
    // arm_move_unlocked), translates only that one piece in place
    // (Geometry::transform_piece_in_place - leaving every sibling piece,
    // including other entries of the same owning Shape, untouched),
    // applies it via Root::update_shape (bypassing the micron-conversion
    // C API layer - Move already works in dbu), and records the whole
    // set as one undo/redo transaction. A no-op if Move isn't armed.
    //
    // Stays armed after a successful commit (re-arms with each moved
    // piece's now-current geometry, ready for an immediate follow-up
    // move) rather than fully clearing Move state - only Escape
    // (le_cancel_move/LE_KEY_FINISH_RULER) or leaving Edit mode
    // (LeHandle::set_mode's own end_move() call) actually disarms it, so a
    // user moving several pieces in sequence doesn't have to re-press
    // the Move button/Ctrl-M between each one.
    void move_click_unlocked(LeHandle *handle)
    {
        if (!handle->move().armed)
            return;

        if (!handle->move().anchor)
        {
            handle->move_set_anchor();
            return;
        }

        const std::optional<std::vector<le::Point>> deltas = moving_piece_deltas_unlocked(handle);
        const std::vector<le::PlacementId> placements = handle->moving_placements();
        const std::optional<le::Point> raw_delta = handle->move_raw_delta(handle->move_free_form());
        if (!deltas || (!placements.empty() && !raw_delta))
        {
            handle->end_move();
            return;
        }
        // Planned before any Shape below changes Root - the ghost was
        // planned against the same pre-move state.
        const std::vector<le::PlacementMoveTarget> placement_targets =
            placements.empty() ? std::vector<le::PlacementMoveTarget>{} : plan_moving_placements_unlocked(handle, placements, *raw_delta);

        handle->command_history.begin("move");
        const std::vector<LeHandle::SelectedObject> moving_pieces = handle->move().moving_pieces;
        for (size_t moving_index = 0; moving_index < moving_pieces.size() && moving_index < deltas->size(); ++moving_index)
        {
            const LeHandle::SelectedObject &selected = moving_pieces[moving_index];
            // Only ShapePieces here - Placements commit from
            // placement_targets below, Rows/Regions aren't movable.
            const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
            if (!piece)
                continue;

            const std::optional<le::ShapeData> before = owner_geometry(handle->root, piece->owner());
            if (!before || !le::Geometry::piece_in_range(*before, piece->piece_kind, piece->piece_index))
                continue; // stale shape or piece index - skip rather than corrupt an unrelated piece

            le::ShapeData after = *before;
            le::Geometry::transform_piece_in_place(after, piece->piece_kind, piece->piece_index, (*deltas)[moving_index]);
            store_owner_geometry(handle, piece->owner(), *before, after); // a via piece moves its origin
            handle->root.bump_mutation_version();
        }
        // Placement Move - location
        // and orientation (the toolbar's pending rotate/flip, possibly
        // forced by site snapping) land together, in the same transaction.
        // A moved UNPLACED placement becomes PLACED.
        bool placed_any = false;
        for (const le::PlacementMoveTarget &target : placement_targets)
        {
            const le::PlacementData *existing = handle->root.get_placement(target.id);
            if (!existing || le::placement_is_locked(existing->placement_status)) // locked while the Move was armed
                continue;
            const le::PlacementData before = *existing;
            const bool place = before.placement_status == le::PlacementStatus::UNPLACED;
            placed_any = placed_any || place;
            handle->root.update_placement(target.id, before.layout, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                          place ? std::optional(le::PlacementStatus::PLACED) : std::nullopt, target.location,
                                          target.orientation, std::nullopt, std::nullopt);
            handle->root.bump_mutation_version();
            if (le::editing::Transaction *txn = handle->command_history.current())
                txn->record_update<le::PlacementId, le::PlacementData>(target.id, before, *handle->root.get_placement(target.id), &le::apply_placement_snapshot);
        }
        handle->command_history.end(/*succeeded=*/true);
        if (placed_any)
            reveal_placement_status_unlocked(handle, le::PlacementStatus::PLACED);

        std::vector<le::ShapeData> geometry;
        geometry.reserve(moving_pieces.size());
        for (const LeHandle::SelectedObject &selected : moving_pieces)
            geometry.push_back(move_ghost_piece_unlocked(handle, selected));
        handle->arm_move(std::move(geometry));
    }

    // Removes piece `index` of kind `kind` from `shape` - and its DEF MASK
    // number, which rect_masks/polygon_masks/path_masks hold index-parallel.
    void erase_piece(le::ShapeData &shape, le::PieceKind kind, size_t index)
    {
        auto erase = [&](auto &pieces)
        {
            if (index < pieces.size())
                pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(index));
        };
        switch (kind)
        {
        case le::PieceKind::RECT:
            erase(shape.rects);
            erase(shape.rect_masks);
            break;
        case le::PieceKind::POLYGON:
            erase(shape.polygons);
            erase(shape.polygon_masks);
            break;
        case le::PieceKind::PATH:
            erase(shape.paths);
            erase(shape.path_masks);
            break;
        case le::PieceKind::VIA:
            erase(shape.vias);
            break;
        case le::PieceKind::VIA_ITERATE:
            erase(shape.via_iterates);
            break;
        }
    }

    // Whether `shape` has no geometry left at all.
    bool has_no_geometry(const le::ShapeData &shape)
    {
        return shape.rects.empty() && shape.polygons.empty() && shape.paths.empty() && shape.rect_iterates.empty() && shape.polygon_iterates.empty() &&
               shape.path_iterates.empty() && shape.vias.empty() && shape.via_iterates.empty();
    }

    // le_delete_selected_pieces / LE_KEY_DELETE - unlocked. Pieces of one
    // Shape or Wire are removed highest index
    // first per kind, so earlier deletions don't shift later ones. A Shape
    // or Wire left with no geometry is deleted too (its owner stays); undo
    // recreates it whole from its pre-delete snapshot.
    int32_t delete_selected_pieces_unlocked(LeHandle *handle)
    {
        std::map<le::GeometryId, std::vector<const LeHandle::ShapePiece *>> by_shape;
        for (const LeHandle::SelectedObject &selected : handle->selection())
            if (const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected))
                by_shape[piece->owner()].push_back(piece);
        if (by_shape.empty())
            return 0;

        handle->end_move();
        handle->end_resize();

        int32_t deleted = 0;
        std::vector<LeHandle::ShapePiece> removed;
        std::vector<le::GeometryId> emptied;
        // Its own undo step - unless a typed Tcl command already opened one
        // (le_repl_eval), which this then records into and leaves open.
        const bool own_transaction = !handle->command_history.is_recording();
        if (own_transaction)
            handle->command_history.begin("delete");
        for (auto &[owner, pieces] : by_shape)
        {
            const std::optional<le::ShapeData> existing = owner_geometry(handle->root, owner);
            if (!existing)
                continue;
            std::ranges::sort(pieces, [](const LeHandle::ShapePiece *a, const LeHandle::ShapePiece *b)
                              { return std::tie(a->piece_kind, b->piece_index) < std::tie(b->piece_kind, a->piece_index); }); // index descending within a kind
            const le::ShapeData &before = *existing;
            le::ShapeData after = before;
            for (const LeHandle::ShapePiece *piece : pieces)
                if (le::Geometry::piece_in_range(after, piece->piece_kind, piece->piece_index))
                {
                    erase_piece(after, piece->piece_kind, piece->piece_index);
                    removed.push_back(*piece);
                    ++deleted;
                }
            if (has_no_geometry(after))
            {
                emptied.push_back(owner);
                continue;
            }
            store_owner_geometry(handle, owner, before, after);
        }
        delete_owners(handle, emptied);
        if (own_transaction)
            handle->command_history.end(/*succeeded=*/deleted > 0);
        for (const LeHandle::ShapePiece &piece : removed)
            handle->deselect(piece);
        if (deleted > 0)
            handle->root.bump_mutation_version();
        return deleted;
    }

    // Every ROUTING-type layer in `technology_id`'s own declaration
    // order - LE_KEY_1 maps to index 0 here, LE_KEY_2
    // to index 1, etc.
    std::vector<le::LayerId> ordered_routing_layers(const le::Root &root, le::TechnologyId technology_id)
    {
        std::vector<le::LayerId> result;
        for (le::LayerId layer_id : root.get_technology_layers(technology_id))
        {
            const le::LayerData *layer = root.get_layer(layer_id);
            if (layer && layer->type == "ROUTING")
                result.push_back(layer_id);
        }
        return result;
    }

    // Every CUT-type layer strictly between `a` and `b`'s own positions
    // in root.get_technology_layers(technology_id)'s declaration order
    // (LEF has no distinct "VIA" layer type, vias are
    // TYPE CUT layers - see LeKeyCode's own doc comment). Order-
    // independent (a/b can be passed either way); usually exactly one,
    // but every CUT layer in the gap is returned, not just the first,
    // for an unusual technology that declares more than one.
    std::vector<le::LayerId> cut_layers_between(const le::Root &root, le::TechnologyId technology_id, le::LayerId a, le::LayerId b)
    {
        const auto &layers = root.get_technology_layers(technology_id);

        auto index_of = [&](le::LayerId id) -> std::optional<size_t>
        {
            for (size_t i = 0; i < layers.size(); ++i)
                if (layers[i] == id)
                    return i;
            return std::nullopt;
        };

        const auto index_a = index_of(a);
        const auto index_b = index_of(b);
        if (!index_a || !index_b)
            return {};

        const size_t lo = std::min(*index_a, *index_b);
        const size_t hi = std::max(*index_a, *index_b);

        std::vector<le::LayerId> result;
        for (size_t i = lo + 1; i < hi; ++i)
        {
            const le::LayerData *layer = root.get_layer(layers[i]);
            if (layer && layer->type == "CUT")
                result.push_back(layers[i]);
        }
        return result;
    }

    // LE_KEY_0..LE_KEY_9's own body - unlocked-style
    // helper (already inside le_key_down's held mutex, matches the other
    // *_unlocked helpers' own convention above). `routing_index` is
    // 0-based (e.g. LE_KEY_1 with Ctrl not held -> 0, LE_KEY_1 with Ctrl
    // held -> 10, LE_KEY_0 -> 9 - see le_key_down's own switch for the
    // full mapping). No-op if there's no ROUTING layer at that index or
    // no Technology has been read yet.
    void toggle_routing_layer_visibility_unlocked(LeHandle *handle, int routing_index)
    {
        if (handle->root.get_technology_ids().empty())
            return;
        const le::TechnologyId technology_id = handle->root.get_technology_ids().front();

        const auto routing_layers = ordered_routing_layers(handle->root, technology_id);
        if (routing_index < 0 || static_cast<size_t>(routing_index) >= routing_layers.size())
            return;

        const le::LayerData *toggled = handle->root.get_layer(routing_layers[static_cast<size_t>(routing_index)]);
        if (!toggled)
            return;

        handle->set_layer_name_visible(toggled->name, !handle->is_layer_name_visible(toggled->name));

        // Only on this keyboard path (never from a direct
        // le_set_layer_name_visible() call) - re-check every adjacent
        // routing-layer pair and sync the CUT layer(s) between them to
        // "both visible". Recomputed as a full pass,
        // not just the pairs touching the just-toggled layer - simpler
        // to reason about/test, and a technology has at most a few
        // dozen routing layers so the cost is trivial.
        for (size_t i = 0; i + 1 < routing_layers.size(); ++i)
        {
            const le::LayerData *first = handle->root.get_layer(routing_layers[i]);
            const le::LayerData *second = handle->root.get_layer(routing_layers[i + 1]);
            if (!first || !second)
                continue;

            const bool both_visible = handle->is_layer_name_visible(first->name) && handle->is_layer_name_visible(second->name);
            for (le::LayerId cut_id : cut_layers_between(handle->root, technology_id, routing_layers[i], routing_layers[i + 1]))
            {
                const le::LayerData *cut = handle->root.get_layer(cut_id);
                if (cut)
                    handle->set_layer_name_visible(cut->name, both_visible);
            }
        }
    }
}

extern "C"
{
    int32_t le_get_mode(LeHandle *handle)
    {
        if (!handle)
            return LE_MODE_SELECT;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->mode());
    }

    void le_set_mode(LeHandle *handle, int32_t mode)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        if (mode == LE_MODE_RULER)
            handle->reset_ruler_mode();
        else
            handle->set_mode(static_cast<LeHandle::Mode>(mode));
    }

    int32_t le_ruler_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->rulers().size());
    }

    int32_t le_ruler_point_count(LeHandle *handle, int32_t ruler_index)
    {
        if (!handle || ruler_index < 0)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const auto &rulers = handle->rulers();
        if (static_cast<size_t>(ruler_index) >= rulers.size())
            return 0;
        return static_cast<int32_t>(rulers[ruler_index].points.size());
    }

    LeRulerPoint le_ruler_point_at(LeHandle *handle, int32_t ruler_index, int32_t point_index)
    {
        constexpr LeRulerPoint kInvalid{.x_um = 0.0, .y_um = 0.0};
        if (!handle || ruler_index < 0 || point_index < 0)
            return kInvalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const auto &rulers = handle->rulers();
        if (static_cast<size_t>(ruler_index) >= rulers.size())
            return kInvalid;
        const auto &points = rulers[ruler_index].points;
        if (static_cast<size_t>(point_index) >= points.size())
            return kInvalid;
        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return kInvalid;
        const le::Point &p = points[point_index];
        return LeRulerPoint{.x_um = static_cast<double>(p.x) / *dbu_per_um, .y_um = static_cast<double>(p.y) / *dbu_per_um};
    }

    void le_finish_ruler(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->finish_active_ruler();
    }

    void le_clear_rulers(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->clear_rulers();
    }

    void le_arm_move(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        arm_move_unlocked(handle);
    }

    void le_cancel_move(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->end_move();
    }

    int32_t le_is_move_armed(LeHandle *handle)
    {
        // Pre-existing gap, fixed here rather than left as-is - see
        // le_tooltip_message's own comment just above for why (same
        // "read one field with no lock at all" shape).
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->move().armed ? 1 : 0;
    }

    int32_t le_is_move_anchored(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->move().anchor ? 1 : 0;
    }

    void le_set_placement_snap_mode(LeHandle *handle, int32_t mode)
    {
        if (!handle || mode < LE_PLACEMENT_SNAP_NONE || mode > LE_PLACEMENT_SNAP_MANUFACTURING_GRID)
            return;
        HandleWriteLock lock(handle);
        handle->set_placement_snap_mode(static_cast<le::PlacementSnapMode>(mode));
    }

    int32_t le_get_placement_snap_mode(LeHandle *handle)
    {
        if (!handle)
            return LE_PLACEMENT_SNAP_SITE;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->placement_snap_mode());
    }

    int32_t le_is_placement_snap_mode_available(LeHandle *handle, int32_t mode)
    {
        if (!handle || mode < LE_PLACEMENT_SNAP_NONE || mode > LE_PLACEMENT_SNAP_MANUFACTURING_GRID)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return le::PlacementSnapper::available(handle->root, handle->current_layout(), static_cast<le::PlacementSnapMode>(mode)) ? 1 : 0;
    }

    int32_t le_delete_selected_pieces(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return delete_selected_pieces_unlocked(handle);
    }

    int32_t le_selected_locked_placement_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return locked_placement_count_unlocked(handle);
    }

    int32_t le_selected_placements_all_fixed(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const std::vector<le::PlacementId> placements = selected_placements_unlocked(handle);
        return !placements.empty() && std::ranges::all_of(placements, [&](le::PlacementId id)
                                                           {
                                                               const le::PlacementData *placement = handle->root.get_placement(id);
                                                               return placement && placement->placement_status == le::PlacementStatus::FIXED;
                                                           })
                   ? 1
                   : 0;
    }

    int32_t le_placement_orientation_ops_enabled(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return orientation_ops_enabled_unlocked(handle, selected_placements_unlocked(handle));
    }

    int32_t le_apply_placement_orientation_op(LeHandle *handle, int32_t op)
    {
        if (!handle || op < LE_ORIENTATION_OP_ROTATE_CCW || op > LE_ORIENTATION_OP_FLIP_VERTICAL)
            return -1;
        HandleWriteLock lock(handle);

        const std::vector<le::PlacementId> placements = selected_placements_unlocked(handle);
        if (placements.empty())
            return 1;
        if (any_placement_locked(handle, placements))
            return 3;
        if (!(orientation_ops_enabled_unlocked(handle, placements) & (1 << op)))
            return 2;

        // Rotate/flip in place, about each placement's own bbox center, no
        // snapping - plan_placement_move with a zero delta and SNAP_NONE is
        // exactly that. One transaction for the whole selection.
        const int remaining_depth = std::max(0, handle->hierarchy_depth() - 1);
        const le::OrientationOp orientation_op = static_cast<le::OrientationOp>(op);
        const std::vector<le::PlacementMoveTarget> targets = le::plan_placement_move(
            handle->root, handle->current_layout(), placements, le::orientation_for_op(orientation_op), le::Point{}, le::PlacementSnapMode::NONE, remaining_depth);

        handle->command_history.begin(orientation_op == le::OrientationOp::ROTATE_CCW ? "rotate" : "flip");
        for (const le::PlacementMoveTarget &target : targets)
        {
            const le::PlacementData before = *handle->root.get_placement(target.id);
            handle->root.update_placement(target.id, before.layout, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                          target.location, target.orientation, std::nullopt, std::nullopt);
            handle->root.bump_mutation_version();
            if (le::editing::Transaction *txn = handle->command_history.current())
                txn->record_update<le::PlacementId, le::PlacementData>(target.id, before, *handle->root.get_placement(target.id), &le::apply_placement_snapshot);
        }
        handle->command_history.end(/*succeeded=*/true);
        return 0;
    }

    void le_arm_resize(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        arm_resize_unlocked(handle);
    }

    int32_t le_is_resize_armed(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->resize().armed ? 1 : 0;
    }

    int32_t le_resize_hover_axis(LeHandle *handle)
    {
        if (!handle)
            return LE_RESIZE_AXIS_NONE;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        if (!handle->resize().hover)
            return LE_RESIZE_AXIS_NONE;
        switch (handle->resize().hover->axis)
        {
        case le::ResizeAxis::X:
            return LE_RESIZE_AXIS_X;
        case le::ResizeAxis::Y:
            return LE_RESIZE_AXIS_Y;
        case le::ResizeAxis::BOTH:
            return LE_RESIZE_AXIS_BOTH;
        }
        return LE_RESIZE_AXIS_NONE;
    }

    void le_set_shape_snap_mode(LeHandle *handle, int32_t kind, int32_t mode)
    {
        if (!handle || kind < LE_PIECE_KIND_RECT || kind > LE_PIECE_KIND_VIA_ITERATE || mode < LE_SHAPE_SNAP_NONE || mode > LE_SHAPE_SNAP_TRACKS)
            return;
        const le::PieceKind piece_kind = static_cast<le::PieceKind>(kind);
        const le::ShapeSnapMode snap_mode = static_cast<le::ShapeSnapMode>(mode);
        if (!le::shape_snap_mode_applies(piece_kind, snap_mode))
            return;
        HandleWriteLock lock(handle);
        handle->set_shape_snap_mode(piece_kind, snap_mode);
    }

    int32_t le_get_shape_snap_mode(LeHandle *handle, int32_t kind)
    {
        if (!handle || kind < LE_PIECE_KIND_RECT || kind > LE_PIECE_KIND_VIA_ITERATE)
            return LE_SHAPE_SNAP_USER_GRID;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->shape_snap_mode(static_cast<le::PieceKind>(kind)));
    }

    int32_t le_is_shape_snap_mode_available(LeHandle *handle, int32_t kind, int32_t mode)
    {
        if (!handle || kind < LE_PIECE_KIND_RECT || kind > LE_PIECE_KIND_VIA_ITERATE || mode < LE_SHAPE_SNAP_NONE || mode > LE_SHAPE_SNAP_TRACKS)
            return 0;
        const le::PieceKind piece_kind = static_cast<le::PieceKind>(kind);
        const le::ShapeSnapMode snap_mode = static_cast<le::ShapeSnapMode>(mode);
        if (!le::shape_snap_mode_applies(piece_kind, snap_mode))
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        switch (snap_mode)
        {
        case le::ShapeSnapMode::NONE:
        case le::ShapeSnapMode::USER_GRID:
            return 1;
        case le::ShapeSnapMode::MANUFACTURING_GRID:
            return le::technology_manufacturing_grid(handle->root) ? 1 : 0;
        case le::ShapeSnapMode::FIN_GRID:
            return le::technology_fin_grid(handle->root) ? 1 : 0;
        case le::ShapeSnapMode::TRACKS:
            for (const LeHandle::SelectedObject &selected : handle->selection())
                if (const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
                    piece && le::shape_snap_slot(piece->piece_kind) == le::shape_snap_slot(piece_kind))
                {
                    le::ShapeData scratch;
                    if (const le::ShapeData *data = le::geometry_shape(handle->root, piece->owner(), scratch);
                        data && !le::layer_track_grids(handle->root, handle->current_layout(), data->layer).empty())
                        return 1;
                }
            return 0;
        }
        return 0;
    }

    int32_t le_selected_piece_kinds(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        int32_t mask = 0;
        for (const LeHandle::SelectedObject &selected : handle->selection())
            if (const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
                piece && piece->piece_kind != le::PieceKind::VIA && piece->piece_kind != le::PieceKind::VIA_ITERATE) // a via has nothing to resize
                mask |= 1 << static_cast<int32_t>(piece->piece_kind);
        return mask;
    }

    int32_t le_selected_move_snap_piece_kinds(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        int32_t mask = 0;
        for (const LeHandle::SelectedObject &selected : handle->selection())
            if (const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
                piece && snaps_individually_when_moved(piece->piece_kind))
                mask |= 1 << static_cast<int32_t>(le::shape_snap_slot(piece->piece_kind));
        return mask;
    }

    void le_set_mouse_position(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);

        handle->set_mouse_position(x, y);
        update_resize_hover_unlocked(handle);
    }

    void le_clear_mouse_position(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->clear_mouse_position();
    }

    LeSnappedMousePosition le_snapped_mouse_position(LeHandle *handle)
    {
        if (!handle)
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const std::optional<le::Point> snapped = handle->snapped_mouse_position();
        if (!snapped)
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};

        // Single shared/global Technology, same assumption le_read_lef's
        // own ViewLayerSet::build_for_technology(..., technology_ids.front())
        // call already makes.
        const auto technology_ids = handle->root.get_technology_ids();
        if (technology_ids.empty())
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};

        const le::TechnologyData *technology = handle->root.get_technology(technology_ids.front());
        if (!technology || technology->database_units_microns <= 0.0)
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};

        const auto [x, y] = display_point_unlocked(handle, *snapped);
        return LeSnappedMousePosition{
            .x_um = x / technology->database_units_microns,
            .y_um = y / technology->database_units_microns,
            .has_position = 1,
        };
    }

    void le_key_down(LeHandle *handle, int32_t key_code)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->press_key(key_code);
        const bool ctrl = handle->is_key_held(LE_KEY_CTRL);
        const bool shift = handle->is_key_held(LE_KEY_SHIFT);
        handle->set_ruler_free_form(shift);
        handle->set_move_free_form(shift);

        // Every case below fires only for the *exact* modifier
        // combination its own action is actually defined for - an
        // unexpected extra modifier (e.g. Shift held for a key with no
        // Shift-specific meaning) is a no-op, not a silent fall-through
        // to the bare/Ctrl behavior - e.g. Ctrl-S must not also switch
        // modes. LE_KEY_ZOOM is the one exception that's
        // deliberately exhaustive instead (Ctrl and Shift each already
        // select a real, distinct action of their own - see below) and
        // LE_KEY_FINISH_RULER, which is deliberately *not* modifier-
        // gated at all (see its own case).
        switch (key_code)
        {
        case LE_KEY_ZOOM:
        {
            // Ctrl-Z/Ctrl-Shift-Z undo/redo, branched
            // here rather than a separate key code (see LE_KEY_ZOOM's own
            // api.hpp doc comment for why). Falls through to the ordinary
            // zoom action when Ctrl isn't held. Every one of the four
            // Ctrl/Shift combinations already has a real, distinct
            // meaning (undo/redo, zoom in/out), unlike every other action
            // key below - nothing to suppress here.
            if (ctrl)
            {
                if (shift)
                    report_replay(handle, handle->command_history.redo(), "redo");
                else
                    report_replay(handle, handle->command_history.undo(), "undo");
                refresh_armed_move_geometry_unlocked(handle);
                break;
            }
            // Unlocked variants (see their own comment) - handle->mutex_
            // is already held above; le_zoom/le_fit_scene/le_pan
            // themselves would re-lock it and deadlock.
            const double factor = shift ? -kKeyZoomFactor : kKeyZoomFactor;
            zoom_unlocked(handle, factor, handle->mouse_x_px(), handle->mouse_y_px());
            break;
        }
        case LE_KEY_FIT:
            // Shift has no meaning for Fit - held at all (with or without
            // Ctrl) suppresses the action rather than falling through to
            // the bare/Ctrl behavior.
            if (shift)
                break;
            if (ctrl)
                fit_selected_unlocked(handle, kKeyFitPaddingPx);
            else
                fit_scene_unlocked(handle, kKeyFitPaddingPx);
            break;
        // Arrow keys pan in screen directions, so a mirrored axis pans the
        // other way in dbu.
        case LE_KEY_PAN_LEFT:
            if (!ctrl && !shift)
                pan_unlocked(handle, -kKeyPanFactor * screen_x_sign(handle), 0.0);
            break;
        case LE_KEY_PAN_RIGHT:
            if (!ctrl && !shift)
                pan_unlocked(handle, kKeyPanFactor * screen_x_sign(handle), 0.0);
            break;
        case LE_KEY_PAN_UP:
            if (!ctrl && !shift)
                pan_unlocked(handle, 0.0, kKeyPanFactor * screen_y_sign(handle));
            break;
        case LE_KEY_PAN_DOWN:
            if (!ctrl && !shift)
                pan_unlocked(handle, 0.0, -kKeyPanFactor * screen_y_sign(handle));
            break;
        case LE_KEY_1:
        case LE_KEY_2:
        case LE_KEY_3:
        case LE_KEY_4:
        case LE_KEY_5:
        case LE_KEY_6:
        case LE_KEY_7:
        case LE_KEY_8:
        case LE_KEY_9:
        {
            // The same physical 1-9 keys address the
            // 11th..19th ROUTING layer instead of the 1st..9th while Ctrl
            // is held (LE_KEY_0 covers the 10th - see its own case).
            // Shift has no meaning for either - held at all suppresses
            // the action.
            if (shift)
                break;
            const int base_index = key_code - LE_KEY_1; // 0-8
            const int routing_index = ctrl ? base_index + 10 : base_index;
            toggle_routing_layer_visibility_unlocked(handle, routing_index);
            break;
        }
        case LE_KEY_0:
            // Same "no modifier meaning at all" shape as the mode keys
            // below - LE_KEY_1..9's own Ctrl-held branch has no digit
            // left over to double up on for the 10th layer, so this key
            // is bare-only, not Ctrl-gated.
            if (!ctrl && !shift)
                toggle_routing_layer_visibility_unlocked(handle, 9); // the 10th ROUTING layer
            break;
        case LE_KEY_DESELECT_ALL:
            // Select-mode-only, in addition to the Ctrl-held gate (switch
            // back to Select mode to change the selection from Edit/Ruler
            // mode). Shift has no meaning here - Ctrl-Shift-D is a no-op.
            if (handle->mode() == LeHandle::Mode::SELECT && ctrl && !shift)
                handle->clear_selection();
            break;
        case LE_KEY_MOVE:
            if (ctrl && !shift)
                arm_move_unlocked(handle);
            break;
        case LE_KEY_DELETE:
            if (handle->mode() == LeHandle::Mode::EDIT && !ctrl && !shift)
                delete_selected_pieces_unlocked(handle);
            break;
        case LE_KEY_SELECT_MODE:
            if (!ctrl && !shift)
                handle->set_mode(LeHandle::Mode::SELECT);
            break;
        case LE_KEY_EDIT_MODE:
            if (!ctrl && !shift)
                handle->set_mode(LeHandle::Mode::EDIT);
            break;
        case LE_KEY_RULER_MODE:
            if (!ctrl && !shift)
                handle->reset_ruler_mode();
            else if (ctrl && !shift)
                arm_resize_unlocked(handle); // Ctrl-R
            break;
        case LE_KEY_FINISH_RULER:
            // Deliberately *not* modifier-gated, unlike every other bare
            // key above: Escape is a pure cancel/finish gesture, and a
            // real ruler-drawing sequence routinely ends with Shift
            // (free-form) still physically held right up to the Escape
            // press - suppressing it here would make "finish the ruler"
            // unreliable in exactly the workflow that uses Shift most.
            // A rubber-band drag in progress is cancelled first, on its own
            // - one gesture per press.
            if (handle->is_dragging())
            {
                handle->end_drag();
                break;
            }
            handle->finish_active_ruler();
            handle->end_move(); // Escape also cancels an in-progress move
            // Escape cancels a Resize in
            // progress (its first click), else disarms Resize.
            if (handle->resize().grab)
            {
                handle->end_resize_grab();
                update_resize_hover_unlocked(handle);
            }
            else
                handle->end_resize();
            break;
        default:
            break;
        }
    }

    void le_key_up(LeHandle *handle, int32_t key_code)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->release_key(key_code);
        handle->set_ruler_free_form(handle->is_key_held(LE_KEY_SHIFT));
        handle->set_move_free_form(handle->is_key_held(LE_KEY_SHIFT));
    }

    int32_t le_is_key_held(LeHandle *handle, int32_t key_code)
    {
        return handle && handle->is_key_held(key_code) ? 1 : 0;
    }

    void le_clear_all_keys(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->clear_all_keys();
    }

    void le_mouse_down(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->begin_drag(x, y);
    }

    void le_cancel_drag(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        if (handle->is_dragging())
            handle->end_drag();
    }

    void le_zoom_drag_down(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->begin_drag(x, y, LeHandle::DragKind::ZOOM);
    }

    void le_mouse_up(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle || !handle->is_dragging())
            return;
        HandleWriteLock lock(handle);
        ensure_view_layers_current(handle); // click and drag-select hit-test against view_layers

        const int32_t dx = x - handle->drag_start_x_px();
        const int32_t dy = y - handle->drag_start_y_px();
        const bool is_click = dx * dx + dy * dy < kClickDragThresholdPx * kClickDragThresholdPx;

        if (handle->drag_kind() == LeHandle::DragKind::ZOOM)
        {
            // Rectangle-zoom - purely navigational,
            // selection is untouched. A click-sized release is a no-op
            // (fitting to a near-zero-size rect would produce an absurd
            // scale) - same threshold used for the select gesture below.
            if (!is_click)
            {
                const le::Point start = handle->pixel_to_dbu(handle->drag_start_x_px(), handle->drag_start_y_px());
                const le::Point end = handle->pixel_to_dbu(x, y);
                const le::Rect zoom_rect{
                    .ll = le::Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                    .ur = le::Point{std::max(start.x, end.x), std::max(start.y, end.y)},
                };
                handle->fit_to_content(zoom_rect, 0);
            }
            handle->end_drag();
            return;
        }

        // Only Select mode changes the selection; in Edit mode a
        // click/drag edits the existing selection (Move/Resize below), so
        // this whole block is skipped. end_drag() below stays
        // unconditional so drag state
        // always resets regardless of mode.
        if (handle->mode() == LeHandle::Mode::SELECT)
        {
            const bool shift = handle->is_key_held(LE_KEY_SHIFT);
            const std::vector<LeHandle::SelectedObject> previous = handle->selection();

            if (!shift)
                handle->clear_selection();

            if (is_click)
            {
                click_select_unlocked(handle, handle->pixel_to_dbu(x, y), previous, shift);
                handle->end_drag();
                return;
            }

            // A Layout view hit-tests the Layout's own content, never
            // whatever Abstract content happens to exist.
            if (handle->current_layout().valid())
                select_in_layout_view_unlocked(handle, x, y);
            else
                select_in_abstract_view_unlocked(handle, x, y);
        }
        else if (handle->mode() == LeHandle::Mode::RULER)
        {
            // Only a click (not a drag) commits a
            // ruler point; a drag in Ruler mode does nothing beyond
            // ending the gesture below.
            if (is_click)
                handle->add_ruler_point(handle->is_key_held(LE_KEY_SHIFT));
        }
        else if (handle->mode() == LeHandle::Mode::EDIT)
        {
            // Only a click (not a drag) sets the
            // move's anchor / commits it; a drag in Edit mode does
            // nothing beyond ending the gesture below, same as Ruler
            // mode's own click-only handling just above. A no-op if
            // Move isn't armed (see move_click_unlocked).
            if (is_click && !resize_click_unlocked(handle, x, y))
                move_click_unlocked(handle);
        }

        handle->end_drag();
    }

    const char *le_tooltip_message(LeHandle *handle)
    {
        if (!handle)
            return nullptr;
        // Pre-existing gap, fixed here rather than left as-is now that
        // handle->mutex_ actually matters for cross-thread correctness
        // (le_handle.hpp's own mutex_ doc comment) - this read handle->mode()
        // with no lock at all before; a shared_lock costs nothing
        // (mode() is a single enum read) and closes a real, if narrow,
        // data race against a concurrent le_set_mode.
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        switch (handle->mode())
        {
        case LeHandle::Mode::EDIT:
            return kEditModeTooltip;
        case LeHandle::Mode::RULER:
            return kRulerModeTooltip;
        case LeHandle::Mode::SELECT:
        default:
            return kSelectModeTooltip;
        }
    }
}
