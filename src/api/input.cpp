#include "api_internal.hpp"
#include "interaction_types.hpp"
#include "edit_ops.hpp"
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

        OwnStep step(*handle, "resize");
        for (const ResizeEdit &edit : edits)
            store_owner_geometry(handle, edit.owner, edit.before, edit.after);
        handle->root.bump_mutation_version();
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
        int32_t mask = (1 << static_cast<int32_t>(le::OrientationOp::ROTATE_CCW)) | (1 << static_cast<int32_t>(le::OrientationOp::FLIP_HORIZONTAL)) |
                       (1 << static_cast<int32_t>(le::OrientationOp::FLIP_VERTICAL));
        for (const le::PlacementId id : placements)
        {
            const le::PlacementData *placement = handle->root.get_placement(id);
            if (!placement || !placement->location)
                continue;
            const le::AbstractData *abstract = handle->root.get_abstract(handle->root.get_design_abstract(placement->reference_design));
            for (int32_t op = static_cast<int32_t>(le::OrientationOp::ROTATE_CCW); op <= static_cast<int32_t>(le::OrientationOp::FLIP_VERTICAL); ++op)
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

    // The tooltip's text, one per InteractionMode. Edit mode lists
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

        std::optional<OwnStep> step(std::in_place, *handle, "move");
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
        step.reset();
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
        std::optional<OwnStep> step(std::in_place, *handle, "delete");
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
        if (deleted == 0)
            step->fail();
        step.reset();
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

namespace
{
    // Whether `selected` is a piece of a kind Resize can change (a via has
    // no edges to drag).
    const LeHandle::ShapePiece *resizable_piece(const LeHandle::SelectedObject &selected)
    {
        const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
        return piece && piece->piece_kind != le::PieceKind::VIA && piece->piece_kind != le::PieceKind::VIA_ITERATE ? piece : nullptr;
    }
}

namespace le::edit
{
    std::expected<void, std::string> add_ruler(LeHandle &handle, std::span<const Point> points)
    {
        if (points.size() < 2)
            return std::unexpected("a ruler needs at least 2 points");
        handle.add_ruler(std::vector<Point>(points.begin(), points.end()));
        return {};
    }

    void finish_ruler(LeHandle &handle) { handle.finish_active_ruler(); }

    void clear_rulers(LeHandle &handle) { handle.clear_rulers(); }

    InteractionMode mode(const LeHandle &handle) { return handle.mode(); }

    void set_mode(LeHandle &handle, InteractionMode mode)
    {
        if (mode == InteractionMode::RULER)
            handle.reset_ruler_mode();
        else
            handle.set_mode(mode);
    }

    std::string_view tooltip(const LeHandle &handle)
    {
        switch (handle.mode())
        {
        case InteractionMode::EDIT:
            return kEditModeTooltip;
        case InteractionMode::RULER:
            return kRulerModeTooltip;
        case InteractionMode::SELECT:
            break;
        }
        return kSelectModeTooltip;
    }

    void arm_move(LeHandle &handle) { arm_move_unlocked(&handle); }

    void cancel_move(LeHandle &handle) { handle.end_move(); }

    bool move_armed(const LeHandle &handle) { return handle.move().armed; }

    bool move_anchored(const LeHandle &handle) { return handle.move().anchor.has_value(); }

    void set_placement_snap_mode(LeHandle &handle, PlacementSnapMode mode) { handle.set_placement_snap_mode(mode); }

    PlacementSnapMode placement_snap_mode(const LeHandle &handle) { return handle.placement_snap_mode(); }

    bool placement_snap_mode_available(const LeHandle &handle, PlacementSnapMode mode)
    {
        return PlacementSnapper::available(handle.root, handle.current_layout(), mode);
    }

    size_t delete_selected_pieces(LeHandle &handle) { return static_cast<size_t>(delete_selected_pieces_unlocked(&handle)); }

    size_t selected_locked_placement_count(const LeHandle &handle) { return static_cast<size_t>(locked_placement_count_unlocked(&handle)); }

    bool selected_placements_all_fixed(const LeHandle &handle)
    {
        const std::vector<PlacementId> placements = selected_placements_unlocked(&handle);
        return !placements.empty() && std::ranges::all_of(placements, [&](PlacementId id)
                                                           {
                                                               const PlacementData *placement = handle.root.get_placement(id);
                                                               return placement && placement->placement_status == PlacementStatus::FIXED;
                                                           });
    }

    uint32_t placement_orientation_ops_enabled(const LeHandle &handle)
    {
        return static_cast<uint32_t>(orientation_ops_enabled_unlocked(&handle, selected_placements_unlocked(&handle)));
    }

    std::expected<void, OrientationOpError> apply_placement_orientation_op(LeHandle &handle, OrientationOp op)
    {
        const std::vector<PlacementId> placements = selected_placements_unlocked(&handle);
        if (placements.empty())
            return std::unexpected(OrientationOpError::NOTHING_SELECTED);
        if (any_placement_locked(&handle, placements))
            return std::unexpected(OrientationOpError::LOCKED);
        if (!(orientation_ops_enabled_unlocked(&handle, placements) & (1 << static_cast<int32_t>(op))))
            return std::unexpected(OrientationOpError::NOT_AVAILABLE);

        // Rotate or flip in place, about each placement's own bbox center,
        // with no snapping: a zero-delta, SNAP_NONE move plan.
        const int remaining_depth = std::max(0, handle.hierarchy_depth() - 1);
        const std::vector<PlacementMoveTarget> targets = plan_placement_move(handle.root, handle.current_layout(), placements, orientation_for_op(op),
                                                                             Point{}, PlacementSnapMode::NONE, remaining_depth);

        OwnStep step(handle, op == OrientationOp::ROTATE_CCW ? "rotate" : "flip");
        for (const PlacementMoveTarget &target : targets)
        {
            const PlacementData before = *handle.root.get_placement(target.id);
            handle.root.update_placement(target.id, before.layout, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                         target.location, target.orientation, std::nullopt, std::nullopt);
            handle.root.bump_mutation_version();
            if (editing::Transaction *txn = handle.command_history.current())
                txn->record_update<PlacementId, PlacementData>(target.id, before, *handle.root.get_placement(target.id), &apply_placement_snapshot);
        }
        return {};
    }

    std::string_view describe(OrientationOpError error)
    {
        switch (error)
        {
        case OrientationOpError::NOTHING_SELECTED:
            return "no placement is selected";
        case OrientationOpError::NOT_AVAILABLE:
            return "the selection can't take that orientation right now (a Move is under way, no Layout is open, or its rows' sites don't allow it)";
        case OrientationOpError::LOCKED:
            return "a selected placement is FIXED or COVER";
        }
        return "unknown error";
    }

    void arm_resize(LeHandle &handle) { arm_resize_unlocked(&handle); }

    bool resize_armed(const LeHandle &handle) { return handle.resize().armed; }

    std::optional<ResizeAxis> resize_hover_axis(const LeHandle &handle)
    {
        if (!handle.resize().hover)
            return std::nullopt;
        return handle.resize().hover->axis;
    }

    bool set_shape_snap_mode(LeHandle &handle, PieceKind kind, ShapeSnapMode mode)
    {
        if (!shape_snap_mode_applies(kind, mode))
            return false;
        handle.set_shape_snap_mode(kind, mode);
        return true;
    }

    ShapeSnapMode shape_snap_mode(const LeHandle &handle, PieceKind kind) { return handle.shape_snap_mode(kind); }

    bool shape_snap_mode_available(const LeHandle &handle, PieceKind kind, ShapeSnapMode mode)
    {
        if (!shape_snap_mode_applies(kind, mode))
            return false;
        switch (mode)
        {
        case ShapeSnapMode::NONE:
        case ShapeSnapMode::USER_GRID:
            return true;
        case ShapeSnapMode::MANUFACTURING_GRID:
            return technology_manufacturing_grid(handle.root).has_value();
        case ShapeSnapMode::FIN_GRID:
            return technology_fin_grid(handle.root).has_value();
        case ShapeSnapMode::TRACKS:
            for (const LeHandle::SelectedObject &selected : handle.selection())
                if (const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
                    piece && shape_snap_slot(piece->piece_kind) == shape_snap_slot(kind))
                {
                    ShapeData scratch;
                    if (const ShapeData *data = geometry_shape(handle.root, piece->owner(), scratch);
                        data && !layer_track_grids(handle.root, handle.current_layout(), data->layer).empty())
                        return true;
                }
            return false;
        }
        return false;
    }

    uint32_t selected_piece_kinds(const LeHandle &handle)
    {
        uint32_t mask = 0;
        for (const LeHandle::SelectedObject &selected : handle.selection())
            if (const LeHandle::ShapePiece *piece = resizable_piece(selected))
                mask |= 1u << static_cast<uint32_t>(piece->piece_kind);
        return mask;
    }

    uint32_t selected_move_snap_piece_kinds(const LeHandle &handle)
    {
        uint32_t mask = 0;
        for (const LeHandle::SelectedObject &selected : handle.selection())
            if (const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
                piece && snaps_individually_when_moved(piece->piece_kind))
                mask |= 1u << static_cast<uint32_t>(shape_snap_slot(piece->piece_kind));
        return mask;
    }

    void set_mouse_position(LeHandle &handle, int32_t x, int32_t y)
    {
        handle.set_mouse_position(x, y);
        update_resize_hover_unlocked(&handle);
    }

    void clear_mouse_position(LeHandle &handle) { handle.clear_mouse_position(); }

    std::optional<Point> snapped_mouse_position(const LeHandle &handle) { return handle.snapped_mouse_position(); }

    void key_down(LeHandle &handle, Key key)
    {
        LeHandle *const h = &handle;
        handle.press_key(static_cast<int32_t>(key));
        const bool ctrl = handle.is_key_held(static_cast<int32_t>(Key::CTRL));
        const bool shift = handle.is_key_held(static_cast<int32_t>(Key::SHIFT));
        handle.set_ruler_free_form(shift);
        handle.set_move_free_form(shift);

        // Each action fires only for its own modifier combination, so an
        // extra modifier makes it a no-op rather than another action (Ctrl-S
        // must not switch modes). ZOOM gives every combination a meaning;
        // FINISH_RULER (Escape) ignores modifiers, since a free-form ruler
        // often ends with Shift still held.
        switch (key)
        {
        case Key::ZOOM:
            if (ctrl)
            {
                if (shift)
                    report_replay(h, handle.command_history.redo(), "redo");
                else
                    report_replay(h, handle.command_history.undo(), "undo");
                refresh_armed_move_geometry_unlocked(h);
                break;
            }
            zoom_unlocked(h, shift ? -kKeyZoomFactor : kKeyZoomFactor, handle.mouse_x_px(), handle.mouse_y_px());
            break;
        case Key::FIT:
            if (shift)
                break;
            if (ctrl)
                fit_selected_unlocked(h, kKeyFitPaddingPx);
            else
                fit_scene_unlocked(h, kKeyFitPaddingPx);
            break;
        // Arrow keys pan in screen directions, so a mirrored axis pans the
        // other way in dbu.
        case Key::PAN_LEFT:
            if (!ctrl && !shift)
                pan_unlocked(h, -kKeyPanFactor * screen_x_sign(h), 0.0);
            break;
        case Key::PAN_RIGHT:
            if (!ctrl && !shift)
                pan_unlocked(h, kKeyPanFactor * screen_x_sign(h), 0.0);
            break;
        case Key::PAN_UP:
            if (!ctrl && !shift)
                pan_unlocked(h, 0.0, kKeyPanFactor * screen_y_sign(h));
            break;
        case Key::PAN_DOWN:
            if (!ctrl && !shift)
                pan_unlocked(h, 0.0, -kKeyPanFactor * screen_y_sign(h));
            break;
        case Key::DIGIT_1:
        case Key::DIGIT_2:
        case Key::DIGIT_3:
        case Key::DIGIT_4:
        case Key::DIGIT_5:
        case Key::DIGIT_6:
        case Key::DIGIT_7:
        case Key::DIGIT_8:
        case Key::DIGIT_9:
        {
            // Ctrl moves the digits to the 11th..19th routing layers.
            if (shift)
                break;
            const int base_index = static_cast<int>(key) - static_cast<int>(Key::DIGIT_1);
            toggle_routing_layer_visibility_unlocked(h, ctrl ? base_index + 10 : base_index);
            break;
        }
        case Key::DIGIT_0:
            if (!ctrl && !shift)
                toggle_routing_layer_visibility_unlocked(h, 9); // the 10th routing layer
            break;
        case Key::DESELECT_ALL:
            if (handle.mode() == InteractionMode::SELECT && ctrl && !shift)
                handle.clear_selection();
            break;
        case Key::MOVE:
            if (ctrl && !shift)
                arm_move_unlocked(h);
            break;
        case Key::DELETE:
            if (handle.mode() == InteractionMode::EDIT && !ctrl && !shift)
                delete_selected_pieces_unlocked(h);
            break;
        case Key::SELECT_MODE:
            if (!ctrl && !shift)
                handle.set_mode(InteractionMode::SELECT);
            break;
        case Key::EDIT_MODE:
            if (!ctrl && !shift)
                handle.set_mode(InteractionMode::EDIT);
            break;
        case Key::RULER_MODE:
            if (!ctrl && !shift)
                handle.reset_ruler_mode();
            else if (ctrl && !shift)
                arm_resize_unlocked(h);
            break;
        case Key::FINISH_RULER:
            // One gesture per press: a rubber-band drag in progress is
            // cancelled on its own.
            if (handle.is_dragging())
            {
                handle.end_drag();
                break;
            }
            handle.finish_active_ruler();
            handle.end_move();
            if (handle.resize().grab)
            {
                handle.end_resize_grab();
                update_resize_hover_unlocked(h);
            }
            else
                handle.end_resize();
            break;
        case Key::SHIFT:
        case Key::CTRL:
            break;
        }
    }

    void key_up(LeHandle &handle, Key key)
    {
        handle.release_key(static_cast<int32_t>(key));
        const bool shift = handle.is_key_held(static_cast<int32_t>(Key::SHIFT));
        handle.set_ruler_free_form(shift);
        handle.set_move_free_form(shift);
    }

    bool key_held(const LeHandle &handle, Key key) { return handle.is_key_held(static_cast<int32_t>(key)); }

    void clear_keys(LeHandle &handle) { handle.clear_all_keys(); }

    void mouse_down(LeHandle &handle, int32_t x, int32_t y) { handle.begin_drag(x, y); }

    void zoom_drag_down(LeHandle &handle, int32_t x, int32_t y) { handle.begin_drag(x, y, LeHandle::DragKind::ZOOM); }

    void cancel_drag(LeHandle &handle)
    {
        if (handle.is_dragging())
            handle.end_drag();
    }

    void mouse_up(LeHandle &handle, int32_t x, int32_t y)
    {
        if (!handle.is_dragging())
            return;
        LeHandle *const h = &handle;
        ensure_view_layers_current(h); // click and drag-select hit-test against view_layers

        const int32_t dx = x - handle.drag_start_x_px();
        const int32_t dy = y - handle.drag_start_y_px();
        const bool is_click = dx * dx + dy * dy < kClickDragThresholdPx * kClickDragThresholdPx;

        if (handle.drag_kind() == LeHandle::DragKind::ZOOM)
        {
            // Rectangle zoom leaves the selection alone; a click-sized
            // rectangle would zoom absurdly far, so it does nothing.
            if (!is_click)
            {
                const Point start = handle.pixel_to_dbu(handle.drag_start_x_px(), handle.drag_start_y_px());
                const Point end = handle.pixel_to_dbu(x, y);
                const Rect zoom_rect{
                    .ll = Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                    .ur = Point{std::max(start.x, end.x), std::max(start.y, end.y)},
                };
                handle.fit_to_content(zoom_rect, 0);
            }
            handle.end_drag();
            return;
        }

        switch (handle.mode())
        {
        case InteractionMode::SELECT:
        {
            const bool shift = handle.is_key_held(static_cast<int32_t>(Key::SHIFT));
            const std::vector<LeHandle::SelectedObject> previous = handle.selection();
            if (!shift)
                handle.clear_selection();
            if (is_click)
                click_select_unlocked(h, handle.pixel_to_dbu(x, y), previous, shift);
            // A Layout view hit-tests the Layout's own content, never
            // whatever Abstract content happens to exist.
            else if (handle.current_layout().valid())
                select_in_layout_view_unlocked(h, x, y);
            else
                select_in_abstract_view_unlocked(h, x, y);
            break;
        }
        case InteractionMode::RULER:
            // A click adds a ruler point; a drag does nothing.
            if (is_click)
                handle.add_ruler_point(handle.is_key_held(static_cast<int32_t>(Key::SHIFT)));
            break;
        case InteractionMode::EDIT:
            // A click grabs or places a Resize, else anchors or commits a
            // Move (if armed); a drag does nothing.
            if (is_click && !resize_click_unlocked(h, x, y))
                move_click_unlocked(h);
            break;
        }
        handle.end_drag();
    }
}

// The C API's values are the C++ enums'.
static_assert(LE_MODE_SELECT == static_cast<int32_t>(le::InteractionMode::SELECT) && LE_MODE_EDIT == static_cast<int32_t>(le::InteractionMode::EDIT) &&
              LE_MODE_RULER == static_cast<int32_t>(le::InteractionMode::RULER));
static_assert(LE_KEY_SHIFT == static_cast<int32_t>(le::Key::SHIFT) && LE_KEY_ZOOM == static_cast<int32_t>(le::Key::ZOOM) &&
              LE_KEY_FIT == static_cast<int32_t>(le::Key::FIT) && LE_KEY_PAN_LEFT == static_cast<int32_t>(le::Key::PAN_LEFT) &&
              LE_KEY_PAN_RIGHT == static_cast<int32_t>(le::Key::PAN_RIGHT) && LE_KEY_PAN_UP == static_cast<int32_t>(le::Key::PAN_UP) &&
              LE_KEY_PAN_DOWN == static_cast<int32_t>(le::Key::PAN_DOWN) && LE_KEY_CTRL == static_cast<int32_t>(le::Key::CTRL) &&
              LE_KEY_1 == static_cast<int32_t>(le::Key::DIGIT_1) && LE_KEY_9 == static_cast<int32_t>(le::Key::DIGIT_9) &&
              LE_KEY_DESELECT_ALL == static_cast<int32_t>(le::Key::DESELECT_ALL) && LE_KEY_0 == static_cast<int32_t>(le::Key::DIGIT_0) &&
              LE_KEY_SELECT_MODE == static_cast<int32_t>(le::Key::SELECT_MODE) && LE_KEY_EDIT_MODE == static_cast<int32_t>(le::Key::EDIT_MODE) &&
              LE_KEY_RULER_MODE == static_cast<int32_t>(le::Key::RULER_MODE) && LE_KEY_FINISH_RULER == static_cast<int32_t>(le::Key::FINISH_RULER) &&
              LE_KEY_MOVE == static_cast<int32_t>(le::Key::MOVE) && LE_KEY_DELETE == static_cast<int32_t>(le::Key::DELETE));
static_assert(LE_PLACEMENT_SNAP_NONE == static_cast<int32_t>(le::PlacementSnapMode::NONE) &&
              LE_PLACEMENT_SNAP_MANUFACTURING_GRID == static_cast<int32_t>(le::PlacementSnapMode::MANUFACTURING_GRID));
static_assert(LE_ORIENTATION_OP_ROTATE_CCW == static_cast<int32_t>(le::OrientationOp::ROTATE_CCW) &&
              LE_ORIENTATION_OP_FLIP_VERTICAL == static_cast<int32_t>(le::OrientationOp::FLIP_VERTICAL));
static_assert(LE_SHAPE_SNAP_NONE == static_cast<int32_t>(le::ShapeSnapMode::NONE) && LE_SHAPE_SNAP_TRACKS == static_cast<int32_t>(le::ShapeSnapMode::TRACKS));
static_assert(LE_PIECE_KIND_RECT == static_cast<int32_t>(le::PieceKind::RECT) && LE_PIECE_KIND_VIA_ITERATE == static_cast<int32_t>(le::PieceKind::VIA_ITERATE));

namespace
{
    bool valid_piece_kind(int32_t kind) { return kind >= LE_PIECE_KIND_RECT && kind <= LE_PIECE_KIND_VIA_ITERATE; }
    bool valid_shape_snap_mode(int32_t mode) { return mode >= LE_SHAPE_SNAP_NONE && mode <= LE_SHAPE_SNAP_TRACKS; }
    bool valid_placement_snap_mode(int32_t mode) { return mode >= LE_PLACEMENT_SNAP_NONE && mode <= LE_PLACEMENT_SNAP_MANUFACTURING_GRID; }
}

extern "C"
{
    int32_t le_get_mode(LeHandle *handle)
    {
        if (!handle)
            return LE_MODE_SELECT;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(le::edit::mode(*handle));
    }

    void le_set_mode(LeHandle *handle, int32_t mode)
    {
        if (!handle || mode < LE_MODE_SELECT || mode > LE_MODE_RULER)
            return;
        HandleWriteLock lock(handle);
        le::edit::set_mode(*handle, static_cast<le::InteractionMode>(mode));
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
        le::edit::finish_ruler(*handle);
    }

    void le_clear_rulers(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::clear_rulers(*handle);
    }

    void le_arm_move(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::arm_move(*handle);
    }

    void le_cancel_move(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::cancel_move(*handle);
    }

    int32_t le_is_move_armed(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return le::edit::move_armed(*handle) ? 1 : 0;
    }

    int32_t le_is_move_anchored(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return le::edit::move_anchored(*handle) ? 1 : 0;
    }

    void le_set_placement_snap_mode(LeHandle *handle, int32_t mode)
    {
        if (!handle || !valid_placement_snap_mode(mode))
            return;
        HandleWriteLock lock(handle);
        le::edit::set_placement_snap_mode(*handle, static_cast<le::PlacementSnapMode>(mode));
    }

    int32_t le_get_placement_snap_mode(LeHandle *handle)
    {
        if (!handle)
            return LE_PLACEMENT_SNAP_SITE;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(le::edit::placement_snap_mode(*handle));
    }

    int32_t le_is_placement_snap_mode_available(LeHandle *handle, int32_t mode)
    {
        if (!handle || !valid_placement_snap_mode(mode))
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return le::edit::placement_snap_mode_available(*handle, static_cast<le::PlacementSnapMode>(mode)) ? 1 : 0;
    }

    int32_t le_delete_selected_pieces(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return static_cast<int32_t>(le::edit::delete_selected_pieces(*handle));
    }

    int32_t le_selected_locked_placement_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(le::edit::selected_locked_placement_count(*handle));
    }

    int32_t le_selected_placements_all_fixed(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return le::edit::selected_placements_all_fixed(*handle) ? 1 : 0;
    }

    int32_t le_placement_orientation_ops_enabled(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(le::edit::placement_orientation_ops_enabled(*handle));
    }

    int32_t le_apply_placement_orientation_op(LeHandle *handle, int32_t op)
    {
        if (!handle || op < LE_ORIENTATION_OP_ROTATE_CCW || op > LE_ORIENTATION_OP_FLIP_VERTICAL)
            return -1;
        HandleWriteLock lock(handle);
        const std::expected<void, le::OrientationOpError> applied = le::edit::apply_placement_orientation_op(*handle, static_cast<le::OrientationOp>(op));
        if (applied)
            return 0;
        switch (applied.error())
        {
        case le::OrientationOpError::NOTHING_SELECTED:
            return 1;
        case le::OrientationOpError::NOT_AVAILABLE:
            return 2;
        case le::OrientationOpError::LOCKED:
            return 3;
        }
        return -1;
    }

    void le_arm_resize(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::arm_resize(*handle);
    }

    int32_t le_is_resize_armed(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return le::edit::resize_armed(*handle) ? 1 : 0;
    }

    int32_t le_resize_hover_axis(LeHandle *handle)
    {
        if (!handle)
            return LE_RESIZE_AXIS_NONE;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const std::optional<le::ResizeAxis> axis = le::edit::resize_hover_axis(*handle);
        if (!axis)
            return LE_RESIZE_AXIS_NONE;
        switch (*axis)
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
        if (!handle || !valid_piece_kind(kind) || !valid_shape_snap_mode(mode))
            return;
        HandleWriteLock lock(handle);
        le::edit::set_shape_snap_mode(*handle, static_cast<le::PieceKind>(kind), static_cast<le::ShapeSnapMode>(mode));
    }

    int32_t le_get_shape_snap_mode(LeHandle *handle, int32_t kind)
    {
        if (!handle || !valid_piece_kind(kind))
            return LE_SHAPE_SNAP_USER_GRID;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(le::edit::shape_snap_mode(*handle, static_cast<le::PieceKind>(kind)));
    }

    int32_t le_is_shape_snap_mode_available(LeHandle *handle, int32_t kind, int32_t mode)
    {
        if (!handle || !valid_piece_kind(kind) || !valid_shape_snap_mode(mode))
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return le::edit::shape_snap_mode_available(*handle, static_cast<le::PieceKind>(kind), static_cast<le::ShapeSnapMode>(mode)) ? 1 : 0;
    }

    int32_t le_selected_piece_kinds(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(le::edit::selected_piece_kinds(*handle));
    }

    int32_t le_selected_move_snap_piece_kinds(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(le::edit::selected_move_snap_piece_kinds(*handle));
    }

    void le_set_mouse_position(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::set_mouse_position(*handle, x, y);
    }

    void le_clear_mouse_position(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::clear_mouse_position(*handle);
    }

    LeSnappedMousePosition le_snapped_mouse_position(LeHandle *handle)
    {
        constexpr LeSnappedMousePosition kNone{.x_um = 0.0, .y_um = 0.0, .has_position = 0};
        if (!handle)
            return kNone;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const std::optional<le::Point> snapped = le::edit::snapped_mouse_position(*handle);
        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!snapped || !dbu_per_um)
            return kNone;
        const auto [x, y] = display_point_unlocked(handle, *snapped);
        return LeSnappedMousePosition{.x_um = x / *dbu_per_um, .y_um = y / *dbu_per_um, .has_position = 1};
    }

    void le_key_down(LeHandle *handle, int32_t key_code)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::key_down(*handle, static_cast<le::Key>(key_code));
    }

    void le_key_up(LeHandle *handle, int32_t key_code)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::key_up(*handle, static_cast<le::Key>(key_code));
    }

    int32_t le_is_key_held(LeHandle *handle, int32_t key_code)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return le::edit::key_held(*handle, static_cast<le::Key>(key_code)) ? 1 : 0;
    }

    void le_clear_all_keys(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::clear_keys(*handle);
    }

    void le_mouse_down(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::mouse_down(*handle, x, y);
    }

    void le_cancel_drag(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::cancel_drag(*handle);
    }

    void le_zoom_drag_down(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::zoom_drag_down(*handle, x, y);
    }

    void le_mouse_up(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        le::edit::mouse_up(*handle, x, y);
    }

    const char *le_tooltip_message(LeHandle *handle)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return le::edit::tooltip(*handle).data(); // static storage, null-terminated
    }
}
