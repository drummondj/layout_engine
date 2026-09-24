#include "api.hpp"
#include "../database/database.hpp"
#include "../database/filter.hpp"
#include "../database/schematic_layout_linker.hpp"
#include "../database/rename_propagation.hpp"
#include "../editing/editing.hpp"
#include "../geometry/geometry.hpp"
#include "../geometry/shape_ops.hpp"
#include "../core/placement_geometry.hpp"
#include "../core/row_geometry.hpp"
#include "../io/lef_reader.hpp"
#include "../io/def_reader.hpp"
#include "../sv/sv_reader.hpp"
#include "../sv/verilog_stub_writer.hpp"
#include "../io/lef_writer.hpp"
#include "../io/def_writer.hpp"
#include "../view_style/view_style.hpp"
#include "../pipelines/view_render_pipeline.hpp"
#include "../pipelines/pipeline_options.hpp"
#include "le_handle.hpp"
// Generated apply_<snake>_snapshot(Root&, <Klass>Id, const <Klass>Data&)
// helpers (UPDATES.md item 21) - a real standalone header, unlike every
// other generated_tcl/*.inc fragment, so it's included here with the
// rest of api.cpp's top-level includes rather than spliced into a
// specific scope. Never edit generated_tcl/snapshot_appliers.hpp
// directly - regenerate via the regen-tcl skill.
#include "generated_tcl/snapshot_appliers.hpp"
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <oneapi/tbb/global_control.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <unordered_set>
#include <vector>
#include <unistd.h>

namespace
{
    // Tracy's FrameMarkStart/FrameMarkEnd send the raw pointer value of
    // `name`, not a copy of the string (see TracyProfiler.hpp's
    // SendFrameMark) - the server pairs a Start with its matching End by
    // that pointer, so both call sites in le_render_pixel_buffer must pass
    // the exact same pointer, not two separately-compiled string literals
    // (which the compiler is free, but not guaranteed, to pool into one
    // address). A single named constant guarantees that regardless of
    // compiler/optimization level.
    constexpr const char *kRenderFrameName = "le_render_pixel_buffer";

    // Rebuilds handle->view_layers for its currently-selected Technology
    // and stamps view_layers_built_at_version so ensure_view_layers_current()
    // (below) can tell it's current again. The one place this used to
    // happen inline (le_read_lef) now calls this too, so the two can't
    // drift out of sync.
    void rebuild_view_layers(LeHandle *handle, le::TechnologyId technology_id)
    {
        handle->view_layers = le::ViewLayerSet::build_for_technology(handle->root, technology_id);
        handle->view_layers_built_at_version = handle->root.mutation_version();
    }

    // handle->view_layers used to only ever get rebuilt inside le_read_lef
    // - a layer created directly (le_create_layer, generated CRUD) rather
    // than via a LEF read silently never showed up in it, an easy-to-miss
    // staleness bug (confirmed: le_layer_count/le_layer_at/le_purpose_count/
    // le_purpose_at all read view_layers directly with no freshness check
    // at all). Called at the top of those four read-only accessors -
    // cheap when already current (one integer compare), rebuilds via the
    // same ViewLayerSet::build_for_technology call le_read_lef's own
    // rebuild already made unconditionally on every read regardless of
    // whether anything new was actually added, so this isn't a new cost
    // class, just a new trigger for an existing one.
    //
    // Scope note: this fixes the four read accessors specifically (the
    // ones with failing test coverage) - hit_test_abstract_point/_rect
    // and select_in_abstract_view_unlocked's own direct view_layers reads
    // elsewhere in this file have the same underlying staleness exposure
    // (a layer created via le_create_layer, then hit-tested/rendered
    // before any subsequent le_read_lef call) but aren't covered by any
    // failing test today and are deliberately left untouched here - a
    // real, separate gap, not silently papered over.
    void ensure_view_layers_current(LeHandle *handle)
    {
        if (!handle->current_technology_id.valid())
            return;
        if (handle->view_layers_built_at_version == handle->root.mutation_version())
            return;
        rebuild_view_layers(handle, handle->current_technology_id);
    }

    // The read-only half of ensure_view_layers_current's own staleness
    // check, split out so le_layer_count/_at/le_purpose_count/_at (below)
    // can use a double-checked locking pattern: check this under a
    // shared_lock first (the common case - view_layers only ever goes
    // stale right after a real edit, not during a steady render), and
    // only escalate to a unique_lock (needed for the rebuild itself,
    // which mutates handle->view_layers/view_layers_built_at_version) if
    // it's actually stale. Without this split, those four accessors would
    // need a unique_lock unconditionally - safe, but exactly the kind of
    // "looks read-only, secretly writes" hazard that would otherwise
    // force them to block behind an in-progress render for its entire
    // duration (le_handle.hpp's own mutex_ doc comment) the same way
    // every other read already would without this file's own shared_lock
    // work.
    bool view_layers_already_current(const LeHandle *handle)
    {
        return !handle->current_technology_id.valid() ||
               handle->view_layers_built_at_version == handle->root.mutation_version();
    }

    // Builds a ViewRenderOptions snapshot of `handle`'s own current
    // root/view state, for le_render_pixel_buffer's own
    // view_render_pipeline.run() call. LeHandle's own pan/scale/viewport-size convention
    // (pixel = (dbu - pan) * scale, LeHandle::pixel_to_dbu's own comment)
    // maps directly onto ViewRenderOptions::viewport/scale: pan is
    // exactly the dbu point at the viewport's own bottom-left pixel
    // corner - viewport.ll - and the top-right corner is pan plus the
    // pixel size converted to dbu via the same scale.
    // Resolves one LeHandle::SelectedObject alternative to its own
    // dbu-space outline geometry, for ComposeStage's own selection
    // overlay - the exact per-kind resolution the pre-restart
    // pipelines.old/stages/selection_overlay_stage.hpp used against a
    // live Scene/Root pair, ported here since that stage (and the
    // pipeline it lived in) were both deleted with the rest of that
    // module. `remaining_depth` is only meaningful for a PlacementId
    // alternative (Layout-view top-level selection, E1) - Abstract-view
    // selection never produces one (see LeHandle::SelectionRef's own
    // comment for why a Placement never becomes a ShapePiece either).
    std::optional<le::Shape> resolve_selected_outline(const LeHandle *handle, const LeHandle::SelectedObject &selected, int remaining_depth)
    {
        return std::visit(
            [&](const auto &s) -> std::optional<le::Shape>
            {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, LeHandle::ShapePiece>)
                {
                    if (const le::ShapeData *data = handle->root.get_shape(s.shape_id))
                        return le::Geometry::extract_piece(*data, s.piece_kind, s.piece_index);
                    return std::nullopt;
                }
                else if constexpr (std::is_same_v<T, le::RowId>)
                {
                    if (auto bbox = le::row_footprint_bbox(handle->root, s))
                        return le::Shape{.rects = {*bbox}};
                    return std::nullopt;
                }
                else if constexpr (std::is_same_v<T, le::RegionId>)
                {
                    if (const le::RegionData *region = handle->root.get_region(s))
                        return le::Shape{.rects = region->rects};
                    return std::nullopt;
                }
                else // le::PlacementId
                {
                    if (!handle->current_layout().valid())
                        return std::nullopt;
                    if (auto bbox = le::placement_world_bbox(handle->root, s, remaining_depth))
                        return le::Shape{.rects = {*bbox}};
                    return std::nullopt;
                }
            },
            selected);
    }

    // Database units per micron for the handle's own (singleton)
    // Technology - every ruler distance/tick-spacing computation
    // (ComposeStage::draw_ruler_overlay) works in real microns, not raw
    // dbu. 0.0 (rather than std::optional) means "unavailable" (no
    // Technology read yet, or a non-positive value) - matches
    // ViewRenderOptions::ruler_dbu_per_um's own "<=0 means skip ruler
    // drawing entirely" convention.
    double technology_dbu_per_um(const le::Root &root)
    {
        const auto technology_ids = root.get_technology_ids();
        if (technology_ids.empty())
            return 0.0;
        const le::TechnologyData *technology = root.get_technology(technology_ids.front());
        if (!technology || technology->database_units_microns <= 0.0)
            return 0.0;
        return technology->database_units_microns;
    }

    // The SelectionRef a hover hit's own shape_id belongs to (LeHandle::
    // HoverTarget::origin) - a Terminal-port Shape resolves to its owning
    // Terminal (not the TerminalPortId itself - SelectionRef's own
    // variant only has TerminalId, matching hover/selection always
    // operating at Terminal granularity even though a Terminal can have
    // several ports), an Obstruction Shape resolves directly (Shape.
    // obstruction already is one). nullopt for anything else (a Shape
    // hit-testing itself already only ever returns must be one of these
    // two in an Abstract view, but this stays defensive rather than
    // assuming).
    std::optional<LeHandle::SelectionRef> shape_selection_ref(const le::Root &root, le::ShapeId shape_id)
    {
        const le::ShapeData *shape = root.get_shape(shape_id);
        if (!shape)
            return std::nullopt;
        if (shape->terminal_port.valid())
        {
            if (const le::TerminalPortData *port = root.get_terminal_port(shape->terminal_port))
                return LeHandle::SelectionRef{port->terminal};
        }
        if (shape->obstruction.valid())
            return LeHandle::SelectionRef{shape->obstruction};
        return std::nullopt;
    }

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

    // --- Resize (NEW_FEATURES_SEPT_2026.md item 3) ---

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
        if (kind == le::PieceKind::PATH && context.mode == le::ShapeSnapMode::TRACKS)
            context.tracks = le::layer_track_grids(handle->root, handle->current_layout(), layer);
        return context;
    }

    // The grabbed piece resized to the mouse at dbu `current` - the ghost
    // while dragging, the committed geometry on release.
    le::Shape resized_piece_unlocked(const LeHandle *handle, le::Point current)
    {
        const LeHandle::ResizeGrab &grab = *handle->resize().grab;
        const le::Point delta{.x = current.x - grab.start.x, .y = current.y - grab.start.y};
        return le::resize_piece(grab.original, grab.handle, delta,
                                shape_snap_context_unlocked(handle, grab.piece.piece_kind, grab.original.layer));
    }

    // arm_resize/le_arm_resize's body - Edit mode, with at least one
    // selected piece (Row/Placement/Region selections have no edges to
    // drag).
    void arm_resize_unlocked(LeHandle *handle)
    {
        if (handle->mode() != LeHandle::Mode::EDIT)
            return;
        const auto &selection = handle->selection();
        if (std::ranges::none_of(selection, [](const LeHandle::SelectedObject &s)
                                 { return std::holds_alternative<LeHandle::ShapePiece>(s); }))
            return;
        handle->arm_resize();
    }

    // le_mouse_down with Resize armed: grabs the edge/segment of a selected
    // piece nearest the press (within kResizeGrabTolerancePx), if any.
    bool try_begin_resize_grab_unlocked(LeHandle *handle, int32_t x, int32_t y)
    {
        const le::Point p = handle->pixel_to_dbu(x, y);
        const int64_t tolerance = static_cast<int64_t>(std::ceil(kResizeGrabTolerancePx / handle->scale()));
        std::optional<LeHandle::ResizeGrab> best;
        for (const LeHandle::SelectedObject &selected : handle->selection())
        {
            const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
            if (!piece)
                continue;
            const le::ShapeData *data = handle->root.get_shape(piece->shape_id);
            if (!data || !le::Geometry::piece_in_range(*data, piece->piece_kind, piece->piece_index))
                continue;
            le::Shape original = le::Geometry::extract_piece(*data, piece->piece_kind, piece->piece_index);
            const std::optional<le::ResizeHandle> hit = le::find_resize_handle(original, p, tolerance);
            if (hit && (!best || hit->distance < best->handle.distance))
                best = LeHandle::ResizeGrab{.piece = *piece, .handle = *hit, .original = std::move(original), .start = p};
        }
        if (!best)
            return false;
        handle->begin_resize_grab(std::move(*best));
        return true;
    }

    // le_mouse_up after a real drag of a grab: writes the resized piece
    // back into its Shape, as one undoable "resize". Resize stays armed.
    void commit_resize_unlocked(LeHandle *handle, int32_t x, int32_t y)
    {
        const LeHandle::ResizeGrab &grab = *handle->resize().grab;
        const le::ShapeData *existing = handle->root.get_shape(grab.piece.shape_id);
        if (!existing || !le::Geometry::piece_in_range(*existing, grab.piece.piece_kind, grab.piece.piece_index))
            return;

        const le::ShapeData before = *existing;
        le::ShapeData after = before;
        le::replace_piece(after, grab.piece.piece_kind, grab.piece.piece_index, resized_piece_unlocked(handle, handle->pixel_to_dbu(x, y)));

        handle->command_history.begin("resize");
        handle->root.update_shape(grab.piece.shape_id, after.layer, after.purpose, after.paths, after.polygons, after.rects,
                                  after.spacing, after.design_rule_width, after.except_pg_net);
        handle->root.bump_mutation_version();
        if (le::editing::Transaction *txn = handle->command_history.current())
            txn->record_update<le::ShapeId, le::ShapeData>(grab.piece.shape_id, before, after, &le::apply_shape_snapshot);
        handle->command_history.end(/*succeeded=*/true);
    }

    // Every PlacementId in the current selection, in selection order.
    std::vector<le::PlacementId> selected_placements_unlocked(const LeHandle *handle)
    {
        std::vector<le::PlacementId> ids;
        for (const LeHandle::SelectedObject &selected : handle->selection())
            if (const le::PlacementId *id = std::get_if<le::PlacementId>(&selected))
                ids.push_back(*id);
        return ids;
    }

    // le_placement_orientation_ops_enabled's own body - a bit (1 << op)
    // per LeOrientationOp that every selected placement allows right now:
    // none while a Move is under way - anchored by its first click, ghost
    // showing (merely arming Move doesn't count) - or outside a Layout view; otherwise each op the
    // PlacementSnapper permits for every one of them (only ever restricted
    // under SITE snapping, by the row's Site symmetry).
    int32_t orientation_ops_enabled_unlocked(const LeHandle *handle, const std::vector<le::PlacementId> &placements)
    {
        if (placements.empty() || handle->move().anchor || !handle->current_layout().valid())
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

    // The selected placements' flightlines (NEW_FEATURES_SEPT_2026.md item
    // 5) for view_render_options_for, from LeHandle::flightline_cache -
    // nothing (and version 0) unless the FLIGHTLINE row and purpose are
    // both visible in a Layout view, so the hidden-by-default feature
    // costs nothing. Returns {lines, version}.
    std::pair<std::vector<le::Flightline>, uint64_t> flightlines_for(const LeHandle *handle)
    {
        const le::LayoutId layout_id = handle->current_layout();
        if (!layout_id.valid() || !handle->is_view_layer_visible("FLIGHTLINE", le::ViewLayerPurpose::FLIGHTLINE))
            return {{}, 0};

        LeHandle::FlightlineCache &cache = handle->flightline_cache;
        std::lock_guard<std::mutex> lock(cache.mutex);
        const uint64_t mutation_version = handle->root.mutation_version();
        if (!cache.index_valid || cache.index_mutation_version != mutation_version || cache.index_layout != layout_id)
        {
            cache.index = le::NetEndpointIndex(handle->root, layout_id);
            cache.index_valid = true;
            cache.index_mutation_version = mutation_version;
            cache.index_layout = layout_id;
            cache.lines_valid = false;
        }
        if (!cache.lines_valid || cache.lines_selection_version != handle->selection_version() ||
            cache.lines_mutation_version != mutation_version || cache.lines_layout != layout_id)
        {
            cache.lines = le::placement_flightlines(handle->root, cache.index, selected_placements_unlocked(handle));
            cache.lines_valid = true;
            cache.lines_selection_version = handle->selection_version();
            cache.lines_mutation_version = mutation_version;
            cache.lines_layout = layout_id;
            ++cache.version;
        }
        return {cache.lines, cache.version};
    }

    le::ViewRenderOptions view_render_options_for(const LeHandle *handle)
    {
        le::ViewRenderOptions options;
        options.root = &handle->root;
        options.root_mutation_version = handle->root.mutation_version();
        options.hierarchy_depth = handle->hierarchy_depth();
        options.scale = handle->scale();
        options.antialiasing_enabled = handle->antialiasing_enabled();
        options.layer_name_visible = handle->layer_name_visibility();
        options.purpose_visible = handle->purpose_visibility();

        options.selection_version = handle->selection_version();
        std::tie(options.flightlines_dbu, options.flightline_version) = flightlines_for(handle);
        options.flightline_color = le::ViewLayerSet::flightline_style().outline_color;
        options.selected_piece_outlines.reserve(handle->selection().size());
        const int remaining_depth = std::max(0, handle->hierarchy_depth() - 1);
        for (const LeHandle::SelectedObject &selected : handle->selection())
            if (auto outline = resolve_selected_outline(handle, selected, remaining_depth))
                options.selected_piece_outlines.push_back(std::move(*outline));

        if (handle->current_layout().valid())
            options.top_level = handle->current_layout();
        else
            options.top_level = handle->current_abstract();

        const le::Point pan = handle->pan();
        const double width_dbu = handle->viewport_width_px() / handle->scale();
        const double height_dbu = handle->viewport_height_px() / handle->scale();
        options.viewport = le::Rect{
            .ll = pan,
            .ur = le::Point{.x = pan.x + static_cast<int64_t>(width_dbu), .y = pan.y + static_cast<int64_t>(height_dbu)},
        };

        if (handle->is_dragging() && handle->drag_kind() != LeHandle::DragKind::RESIZE)
        {
            options.drag_rect_dbu = handle->drag_rect_dbu();
            options.drag_is_zoom = handle->drag_kind() == LeHandle::DragKind::ZOOM;
        }

        options.mouse_version = handle->mouse_version();
        options.cursor_snapped_position_dbu = handle->snapped_mouse_position();
        if (handle->hover().has_value())
            options.hover_outline_dbu = handle->hover()->outline;

        if (handle->resize().grab)
        {
            // Resize (NEW_FEATURES_SEPT_2026.md item 3) - the grabbed
            // piece as it would be committed right now, pre-placed.
            if (const std::optional<le::Point> mouse = handle->mouse_dbu_position())
                options.move_ghost_pieces_dbu.push_back(resized_piece_unlocked(handle, *mouse));
        }
        else if (const std::vector<le::PlacementId> placements = handle->moving_placements(); !placements.empty())
        {
            // Placement Move (NEW_FEATURES_SEPT_2026.md item 2) - each
            // placement snaps independently, so the ghost is pre-placed
            // geometry (offset 0) rather than one shared translation; any
            // shape pieces moving alongside are pre-translated by their own
            // (user-grid) delta to match.
            if (const std::optional<le::Point> raw_delta = handle->move_raw_delta(handle->move_free_form()))
            {
                const std::optional<le::Point> shape_delta = handle->move_delta(handle->move_free_form());
                for (const le::Shape &piece : handle->move().moving_geometry)
                    if (!piece.rects.empty() || !piece.polygons.empty() || !piece.paths.empty())
                        options.move_ghost_pieces_dbu.push_back(le::Geometry::transform(piece, shape_delta.value_or(le::Point{})));
                for (const le::PlacementMoveTarget &target : plan_moving_placements_unlocked(handle, placements, *raw_delta))
                    options.move_ghost_pieces_dbu.push_back(le::placement_move_ghost(target));
            }
        }
        else if (const std::optional<le::Point> delta = handle->move_delta(handle->move_free_form()))
        {
            options.move_ghost_pieces_dbu = handle->move().moving_geometry;
            options.move_ghost_offset_dbu = *delta;
        }

        options.ruler_version = handle->ruler_version();
        options.ruler_dbu_per_um = technology_dbu_per_um(handle->root);
        options.ruler_label_size_px = handle->ruler_label_size_px();
        options.ruler_polylines_dbu.reserve(handle->rulers().size());
        for (const LeHandle::Ruler &ruler : handle->rulers())
            options.ruler_polylines_dbu.push_back(ruler.points);

        // The live ghost segment - only meaningful in Ruler mode, only
        // ever extends the *last* ruler if it exists, isn't finished, and
        // already has a committed point (LeHandle::ruler_next_point's own
        // "the last entry is the active ruler" invariant - mirrors
        // pipelines.old's own MouseOverlayStage gating exactly).
        if (handle->mode() == LeHandle::Mode::RULER && !handle->rulers().empty() &&
            !handle->rulers().back().finished && !handle->rulers().back().points.empty())
        {
            options.ruler_ghost_point_dbu = handle->ruler_next_point(handle->ruler_free_form());
        }

        options.minor_grid_spacing_dbu = handle->minor_grid_spacing();
        options.major_grid_spacing_dbu = handle->major_grid_spacing();
        if (const le::AbstractId *abstract_id = std::get_if<le::AbstractId>(&options.top_level))
            if (const le::AbstractData *abstract = handle->root.get_abstract(*abstract_id))
                options.abstract_origin_dbu = abstract->origin.value_or(le::Point{});

        return options;
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

    // LE_KEY_SELECT_ALL's own cap (UPDATES.md 9.1) - a design can have
    // far more selectable shapes than are reasonable to hold in the
    // selection at once (LeHandle::select() is O(1) average per call, but
    // the resulting selection itself, and every later FFI round-trip
    // over it, still scales with however many objects are in it).
    constexpr int32_t kMaxSelectAllCount = 10000;

    // le_tooltip_message's own text (UPDATES.md item 7.3), one constant
    // per LeHandle::Mode (UPDATES.md item 11) - le_tooltip_message branches
    // on the current mode rather than returning a single fixed string.
    constexpr const char *kSelectModeTooltip =
        "Left click to select. Shift for multi-select. Left click and drag for rectangle multi-select.";
    // UPDATES.md item 21 - Move is the only real editing semantic
    // implemented so far (Resize/Rotate/Align/Delete remain inert UI
    // stubs), so this text describes only that flow.
    constexpr const char *kEditModeTooltip =
        "Ctrl-M or the Move button to arm a move. Click to set the start point, move the mouse, click again "
        "to commit - stays armed for another move until Esc. Shift for free-form (non-orthogonal).";
    constexpr const char *kRulerModeTooltip =
        "Click to add a ruler point. Shift for a non-orthogonal segment. Esc to finish the ruler.";

    // Maps le::PropertyValue::Type (generated/property.hpp) to the C API's
    // LePropertyType - kept as an explicit switch rather than a bare
    // static_cast so a future reordering of either enum fails to compile
    // here instead of silently mislabeling a row's type.
    int32_t to_c_property_type(le::PropertyValue::Type type)
    {
        switch (type)
        {
        case le::PropertyValue::Type::STRING:
            return LE_PROPERTY_TYPE_STRING;
        case le::PropertyValue::Type::INT:
            return LE_PROPERTY_TYPE_INT;
        case le::PropertyValue::Type::DOUBLE:
            return LE_PROPERTY_TYPE_DOUBLE;
        }
        return LE_PROPERTY_TYPE_STRING;
    }

    // LeProperty conversion - shared by every by-id property accessor
    // (le_terminal_property_at et al) and le_object_property_at, so they
    // all build the same row shape from a le::PropertyValue without
    // duplicating the field-by-field mapping.
    LeProperty to_c(const le::PropertyValue &property)
    {
        return LeProperty{
            .name = property.name.c_str(),
            .type = to_c_property_type(property.type),
            .string_value = property.string_value.c_str(),
            .int_value = property.int_value,
            .double_value = property.double_value,
        };
    }

    // UPDATES.md item 19.1's `-filter` validation (every le_get_* function
    // below) - a hand-maintained allowlist of each class's filterable leaf
    // fields and hops, cross-checked directly against each class's own
    // generated get_field()/match_hop() (src/database/generated/*.hpp).
    // Hand-duplicated from schema.py rather than adding a cmg-generated
    // runtime enumeration - a short static list not worth a cross-repo
    // codegen change for. Deliberately narrower than what get_field/
    // match_hop actually dispatch: excludes hops into non-pooled
    // value-list/embedded types (Abstract's bbox/boundary/densities/
    // foreigns/origin/properties/site_placements/size/symmetry; Terminal's
    // antenna_*/properties; Shape's paths/polygons/rects/texts/vias/
    // *_iterates; Design's schematic hop, Schematic not being one of the
    // seven get_* types) - a filter naming one of these is rejected by
    // get_* even though it still works via the unscoped le_search_terminal/
    // etc. escape hatch (item 17).
    struct FilterFieldTable
    {
        std::unordered_set<std::string> leaf_fields;
        std::unordered_map<std::string, std::string> hops; // hop name -> target class name
    };

    const std::unordered_map<std::string, FilterFieldTable> &filter_field_tables()
    {
        static const std::unordered_map<std::string, FilterFieldTable> tables =
#include "generated_tcl/filter_tables.inc"
            return tables;
    }

    // Walks one Comparison's path (the last segment must be a leaf field of
    // whatever class the path has hopped to by then; every earlier segment
    // must be a hop of the class it's checked against, advancing the
    // "current class" to the hop's target). filter.hpp itself never
    // validates field/hop names - an unrecognized one just silently
    // evaluates to no-match (get_field returns nullopt / match_hop returns
    // false) - this is what turns that into a real, reported error instead.
    std::optional<std::string> validate_filter_path(const std::string &root_class, const std::vector<std::string> &path)
    {
        std::string current_class = root_class;
        for (size_t i = 0; i < path.size(); ++i)
        {
            const auto table_it = filter_field_tables().find(current_class);
            if (table_it == filter_field_tables().end())
                return fmt::format("unknown class '{}'", current_class);

            const std::string &segment = path[i];
            const bool is_last = (i + 1 == path.size());
            if (is_last)
            {
                if (table_it->second.leaf_fields.count(segment))
                    return std::nullopt;
                return fmt::format("unknown field '{}' on {}", segment, current_class);
            }

            const auto hop_it = table_it->second.hops.find(segment);
            if (hop_it == table_it->second.hops.end())
                return fmt::format("unknown hop '{}' on {}", segment, current_class);
            current_class = hop_it->second;
        }
        return std::string("empty field path");
    }

    // Recurses through a parsed FilterExpr's And/Or tree, validating every
    // leaf Comparison's path against root_class via validate_filter_path.
    std::optional<std::string> validate_filter_expr(const std::string &root_class, const le::FilterExpr &expr)
    {
        switch (expr.kind)
        {
        case le::FilterExpr::Kind::Comparison:
            return validate_filter_path(root_class, expr.path);
        case le::FilterExpr::Kind::And:
        case le::FilterExpr::Kind::Or:
            for (const le::FilterExpr &child : expr.children)
            {
                if (auto error = validate_filter_expr(root_class, child))
                    return error;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    // Shared parse+validate+log sequence every le_get_* function below
    // needs for its `-filter` axis - std::nullopt with `ok` left true
    // means "no -filter given" (skip that axis entirely); std::nullopt
    // with `ok` set false means a parse or validation error already
    // logged via spdlog::error (caller returns -1). `handle` is unused
    // now (spdlog needs no LeHandle) but kept in the signature - every
    // generated le_get_<type> call site (search_inc_j2.py) passes it
    // positionally, and there's no reason to touch that codegen template
    // just to drop one now-decorative parameter.
    std::optional<le::FilterExpr> parse_and_validate_filter(LeHandle *, const char *caller, const std::string &root_class, const char *filter_expression, bool &ok)
    {
        ok = true;
        if (!filter_expression || filter_expression[0] == '\0')
            return std::nullopt;

        auto parsed = le::parse_filter_expression(filter_expression);
        if (!parsed)
        {
            spdlog::error("{}: {}", caller, parsed.error());
            ok = false;
            return std::nullopt;
        }
        if (auto error = validate_filter_expr(root_class, *parsed))
        {
            spdlog::error("{}: {}", caller, *error);
            ok = false;
            return std::nullopt;
        }
        return std::move(*parsed);
    }

    // Single shared/global Technology, same assumption
    // le_snapped_mouse_position's own lookup makes - nullopt if none has
    // been read yet (le_read_lef) or it has no usable scale.
    std::optional<double> database_units_microns(const le::Root &root)
    {
        const auto technology_ids = root.get_technology_ids();
        if (technology_ids.empty())
            return std::nullopt;

        const le::TechnologyData *technology = root.get_technology(technology_ids.front());
        if (!technology || technology->database_units_microns <= 0.0)
            return std::nullopt;

        return technology->database_units_microns;
    }

    // Rounds to the nearest dbu rather than truncating - a caller passing
    // e.g. 0.1um at 1000 dbu/um should get exactly 100 dbu, not silently
    // lose precision to a fractional-dbu rounding direction they didn't
    // choose.
    int64_t to_dbu(double value_um, double dbu_per_um)
    {
        return static_cast<int64_t>(std::llround(value_um * dbu_per_um));
    }

    // A `dbu` field's own dbu-per-micron ratio, for property tables built
    // before any Technology has been loaded (or with an invalid scale) -
    // falls back to 1.0 (no scaling, same raw magnitude the field would
    // have shown before dbu-aware formatting existed) rather than leaving
    // every build_X_properties() call site to invent its own fallback.
    double display_dbu_per_um(const le::Root &root)
    {
        return database_units_microns(root).value_or(1.0);
    }

    // Looks up `name` among an object's already-built to_properties()-
    // style rows - the same rows a bare `get_properties $token` (no
    // property name) already shows. Used by every le_X_property_path for
    // a single-segment (non-chained) path, so a name like Shape's "rects"
    // resolves the same way there - the -filter DSL's get_field() (what
    // resolve_property_path() uses for everything else) only recognizes
    // scalar leaf fields, not list-of-object fields like rects/polygons/
    // paths, so a bare `.rects` used to fail with "unknown field" even
    // though `get_properties $token` (no name) happily showed it.
    std::optional<le::PropertyValue> find_property_by_name(const std::vector<le::PropertyValue> &properties, std::string_view name)
    {
        for (const le::PropertyValue &property : properties)
        {
            if (property.name == name)
                return property;
        }
        return std::nullopt;
    }

    // Generated TCL property-reading surface (internal helpers only -
    // build_X_properties/to_c/from_c overloads, for every TCL-readable
    // class - these stay inside this anonymous namespace since they're
    // never called from another translation unit; see
    // generated_tcl/property_accessors_public.inc, included later in
    // this file inside extern "C", for the externally-linked
    // le_X_property_count/_at/_path etc. build_terminal_properties/
    // build_library_properties/etc.'s own derived "<field>_count" rows
    // (e.g. Terminal's "ports_count") come from here now too - matches
    // every other is_child-field-derived count row exactly (was
    // hand-abbreviated to "port_count" before this migration; see
    // api_test.cpp). Never edit generated_tcl/property_accessors_internal.inc
    // directly - regenerate via the regen-tcl skill instead.
#include "generated_tcl/property_accessors_internal.inc"

    // Lock-free bodies of le_zoom/le_pan/le_fit_scene - factored out so
    // le_key_down's LE_KEY_ZOOM/FIT/PAN_* handling can call them directly
    // while it's already holding handle->mutex_ (std::mutex isn't
    // recursive - calling back into le_zoom/le_pan/le_fit_scene itself
    // from inside le_key_down would deadlock the calling thread against
    // itself). Callers must have already null-checked `handle` and locked
    // its mutex; the public le_zoom/le_pan/le_fit_scene below do exactly
    // that and then delegate here, so the real logic exists in exactly
    // one place either way.
    void zoom_unlocked(LeHandle *handle, double factor, int32_t x, int32_t y)
    {
        const double old_scale = handle->scale();
        const double new_scale = old_scale * (1.0 + factor);
        if (new_scale <= 0.0)
            return;

        const le::Point old_pan = handle->pan();
        const double viewport_height = handle->viewport_height_px();

        // Undo rasterize()'s Y-flip to get from the caller's image-pixel
        // (x, y) - top-left origin, y down - to the dbu point it currently
        // shows, using the *old* scale/pan (see render.hpp's PixelShape /
        // Renderer::rasterize comments for why pan/scale describe the
        // pre-flip transform while (x, y) here is post-flip).
        const double dbu_x = static_cast<double>(old_pan.x) + static_cast<double>(x) / old_scale;
        const double dbu_y = static_cast<double>(old_pan.y) + (viewport_height - static_cast<double>(y)) / old_scale;

        // Re-solve pan so that same dbu point still lands under (x, y) at
        // the new scale, keeping the zoom visually anchored there.
        const double pan_x_double = dbu_x - static_cast<double>(x) / new_scale;
        const double pan_y_double = dbu_y - (viewport_height - static_cast<double>(y)) / new_scale;

        // A `factor` close enough to -1.0 (an ordinary finite double, not
        // just the already-rejected exact -1.0 above) drives new_scale
        // toward zero, blowing up x/new_scale - reject before casting to
        // int64_t below, since casting an out-of-range (or non-finite)
        // double to an integer type is undefined behavior in C++, not a
        // safe wrap or saturation. Matches new_scale <= 0.0's existing
        // "invalid zoom, no-op" behavior rather than clamping to some
        // arbitrary minimum scale.
        constexpr double kInt64Max = static_cast<double>(std::numeric_limits<int64_t>::max());
        if (!std::isfinite(pan_x_double) || !std::isfinite(pan_y_double) ||
            std::abs(pan_x_double) >= kInt64Max || std::abs(pan_y_double) >= kInt64Max)
            return;

        const int64_t pan_x = static_cast<int64_t>(pan_x_double);
        const int64_t pan_y = static_cast<int64_t>(pan_y_double);

        handle->set_scale(new_scale);
        handle->set_pan(le::Point{.x = pan_x, .y = pan_y});
    }

    void pan_unlocked(LeHandle *handle, double x_factor, double y_factor)
    {
        const double scale = handle->scale();
        const le::Point pan = handle->pan();

        const int64_t dx = static_cast<int64_t>(x_factor * handle->viewport_width_px() / scale);
        const int64_t dy = static_cast<int64_t>(y_factor * handle->viewport_height_px() / scale);

        handle->set_pan(le::Point{.x = pan.x + dx, .y = pan.y + dy});
    }

    void fit_scene_unlocked(LeHandle *handle, int32_t padding_px)
    {
        // A Layout view has no current_abstract() (Phase C's own
        // convention: the two "current view" trackers are mutually
        // exclusive) - generate_shapes against an invalid AbstractId
        // returns nothing, so fit_to_content(nullopt, ...) used to reset
        // to scale=1.0/pan={0,0} instead of framing the Layout's own
        // content. Uses the Layout's own declared diearea bbox (same
        // "declared size" convention as layout_declared_bbox in
        // instance_renderer.hpp/layout_die_area_bbox in
        // generate_layout_shapes_stage.hpp) rather than unioning every
        // Placement's own transformed bbox - O(1) instead of
        // O(placement count), and diearea is the DEF-standard bound of
        // everything in it anyway.
        if (handle->current_layout().valid())
        {
            const le::Shape *diearea = handle->root.get_shape(handle->root.get_layout_diearea(handle->current_layout()));
            handle->fit_to_content(diearea ? le::Geometry::bbox(*diearea) : std::nullopt, padding_px);
            return;
        }

        // Same "declared size, not a union of every generated shape"
        // convention as the Layout branch above, now that this stage's
        // own shape generation lives in the new pipelines module (Warm
        // tier) instead of AbstractShapePipeline - abstract_declared_bbox
        // (core/placement_geometry.hpp) is the exact bbox a *parent*
        // already uses to size its own placement of this Abstract, so
        // it's the right "whole content" bound here too, and O(1)
        // regardless of how many Terminal/Obstruction shapes it has.
        handle->fit_to_content(le::abstract_declared_bbox(handle->root, handle->current_abstract()), padding_px);
    }

    // Widens `bbox` to also enclose `r` - a plain min/max union, same
    // shape as Geometry::expand_bbox (private to that class - this is
    // the small hand-rolled equivalent api.cpp needs at its own call
    // sites for the bare-id (no Shape) selectable kinds, E1).
    void expand_bbox_unlocked(std::optional<le::Rect> &bbox, const le::Rect &r)
    {
        if (!bbox)
        {
            bbox = r;
            return;
        }
        bbox->ll.x = std::min(bbox->ll.x, r.ll.x);
        bbox->ll.y = std::min(bbox->ll.y, r.ll.y);
        bbox->ur.x = std::max(bbox->ur.x, r.ur.x);
        bbox->ur.y = std::max(bbox->ur.y, r.ur.y);
    }

    // LE_KEY_FIT's Ctrl-held branch (UPDATES.md 9.6) - fits the viewport
    // to the current selection's own combined bbox instead of the whole
    // design's. One Root::get_shape(selected.shape_id) lookup per
    // ShapePiece selection entry (owned by `root`, outlives this call, no
    // copy needed), then a single Geometry::bbox call unions them -
    // mirrors fit_scene_unlocked's own shape_ptrs pattern above. E1's own
    // bare-id kinds (Row/Region/Placement - no backing Shape) resolve
    // their own bbox directly and union in via expand_bbox_unlocked
    // instead. A no-op (view unchanged) if nothing is selected, unlike
    // fit_scene_unlocked, which always has the whole design to fall back
    // to.
    void fit_selected_unlocked(LeHandle *handle, int32_t padding_px)
    {
        std::vector<const le::Shape *> shape_ptrs;
        std::optional<le::Rect> bbox;
        const int remaining_depth = std::max(0, handle->hierarchy_depth() - 1);

        for (const LeHandle::SelectedObject &selected : handle->selection())
        {
            std::visit([&](const auto &s)
                       {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, LeHandle::ShapePiece>)
                {
                    if (const le::Shape *shape = handle->root.get_shape(s.shape_id))
                        shape_ptrs.push_back(shape);
                }
                else if constexpr (std::is_same_v<T, le::RowId>)
                {
                    if (auto row_bbox = le::row_footprint_bbox(handle->root, s))
                        expand_bbox_unlocked(bbox, *row_bbox);
                }
                else if constexpr (std::is_same_v<T, le::RegionId>)
                {
                    if (const le::RegionData *region = handle->root.get_region(s))
                        for (const le::Rect &r : region->rects)
                            expand_bbox_unlocked(bbox, r);
                }
                else if constexpr (std::is_same_v<T, le::PlacementId>)
                {
                    if (auto placement_bbox = le::placement_world_bbox(handle->root, s, remaining_depth))
                        expand_bbox_unlocked(bbox, *placement_bbox);
                } }, selected);
        }

        if (const std::optional<le::Rect> shapes_bbox = le::Geometry::bbox(shape_ptrs))
            expand_bbox_unlocked(bbox, *shapes_bbox);

        if (!bbox)
            return;

        handle->fit_to_content(bbox, padding_px);
    }

    // Builds the one-piece ghost-preview geometry for a single selected
    // piece (UPDATES.md item 21) - a fresh Root lookup plus
    // Geometry::extract_piece, shared by arm_move_unlocked and
    // refresh_armed_move_geometry_unlocked. An empty one-piece Shape
    // (drawing nothing) if the shape itself or the piece index has gone
    // stale since it was selected, rather than crashing or substituting
    // the wrong piece. Also empty for E1's Row/Placement/Region
    // alternatives: Rows/Regions aren't movable, and a Placement's ghost
    // is planned per frame instead (plan_moving_placements_unlocked -
    // its snapped position depends on the live mouse, not one shared
    // offset).
    le::Shape move_ghost_piece_unlocked(LeHandle *handle, const LeHandle::SelectedObject &selected)
    {
        const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
        if (!piece)
            return le::Shape{};

        const le::ShapeData *data = handle->root.get_shape(piece->shape_id);
        if (!data)
            return le::Shape{};
        return le::Geometry::extract_piece(*data, piece->piece_kind, piece->piece_index);
    }

    // LE_KEY_MOVE/le_arm_move's own body (UPDATES.md item 21) - unlocked
    // variant, same reasoning as fit_selected_unlocked/select_all_unlocked
    // below (called from inside le_key_down, which already holds
    // handle->mutex_). Only meaningful in Edit mode with a non-empty
    // selection - the Mode::EDIT check lives here rather than inside
    // LeHandle::arm_move itself (LeHandle's own Move state stays mode-
    // agnostic - see LeHandle::set_mode's own comment on the analogous
    // Ruler-mode split).
    // Snapshots each selected *piece's* current geometry for the ghost
    // overlay (LeHandle::MoveState::moving_geometry), piece-granular
    // (UPDATES.md item 21's follow-up) - Move moves exactly whichever
    // pieces are selected, not necessarily every piece of their owning
    // Shapes.
    void arm_move_unlocked(LeHandle *handle)
    {
        if (handle->mode() != LeHandle::Mode::EDIT)
            return;

        std::vector<le::Shape> geometry;
        geometry.reserve(handle->selection().size());
        for (const LeHandle::SelectedObject &selected : handle->selection())
            geometry.push_back(move_ghost_piece_unlocked(handle, selected));

        handle->arm_move(std::move(geometry));
    }

    // le_undo/le_redo's own follow-up (UPDATES.md item 21) - unlocked
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

        std::vector<le::Shape> geometry;
        geometry.reserve(handle->move().moving_pieces.size());
        for (const LeHandle::SelectedObject &selected : handle->move().moving_pieces)
            geometry.push_back(move_ghost_piece_unlocked(handle, selected));
        handle->refresh_move_geometry(std::move(geometry));
    }

    // le_mouse_up's Edit-mode branch (UPDATES.md item 21) - unlocked
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

        const std::optional<le::Point> delta = handle->move_delta(handle->move_free_form());
        const std::vector<le::PlacementId> placements = handle->moving_placements();
        const std::optional<le::Point> raw_delta = handle->move_raw_delta(handle->move_free_form());
        if (!delta || (!placements.empty() && !raw_delta))
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
        for (const LeHandle::SelectedObject &selected : moving_pieces)
        {
            // Only ShapePieces here - Placements commit from
            // placement_targets below, Rows/Regions aren't movable.
            const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
            if (!piece)
                continue;

            const le::ShapeData *existing = handle->root.get_shape(piece->shape_id);
            if (!existing || !le::Geometry::piece_in_range(*existing, piece->piece_kind, piece->piece_index))
                continue; // stale shape or piece index - skip rather than corrupt an unrelated piece

            const le::ShapeData before = *existing;
            le::ShapeData after = before;
            le::Geometry::transform_piece_in_place(after, piece->piece_kind, piece->piece_index, *delta);
            handle->root.update_shape(piece->shape_id, after.layer, after.purpose, after.paths, after.polygons, after.rects,
                                      after.spacing, after.design_rule_width, after.except_pg_net);
            handle->root.bump_mutation_version();

            if (le::editing::Transaction *txn = handle->command_history.current())
                txn->record_update<le::ShapeId, le::ShapeData>(piece->shape_id, before, after, &le::apply_shape_snapshot);
        }
        // Placement Move (NEW_FEATURES_SEPT_2026.md item 2) - location
        // and orientation (the toolbar's pending rotate/flip, possibly
        // forced by site snapping) land together, in the same transaction.
        for (const le::PlacementMoveTarget &target : placement_targets)
        {
            const le::PlacementData *existing = handle->root.get_placement(target.id);
            if (!existing)
                continue;
            const le::PlacementData before = *existing;
            handle->root.update_placement(target.id, before.layout, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                          target.location, target.orientation, std::nullopt, std::nullopt);
            handle->root.bump_mutation_version();
            if (le::editing::Transaction *txn = handle->command_history.current())
                txn->record_update<le::PlacementId, le::PlacementData>(target.id, before, *handle->root.get_placement(target.id), &le::apply_placement_snapshot);
        }
        handle->command_history.end(/*succeeded=*/true);

        std::vector<le::Shape> geometry;
        geometry.reserve(moving_pieces.size());
        for (const LeHandle::SelectedObject &selected : moving_pieces)
            geometry.push_back(move_ghost_piece_unlocked(handle, selected));
        handle->arm_move(std::move(geometry));
    }

    // LE_KEY_SELECT_ALL's own body (UPDATES.md 9.1) - unlocked variant,
    // same reasoning as zoom_unlocked/pan_unlocked/fit_scene_unlocked
    // above (called from inside le_key_down, which already holds
    // handle->mutex_). Deliberately walks Root's own raw Terminal-port/
    // Obstruction ShapeData directly, *not* hit_test_abstract_rect - a
    // geometric containment test would need to also bypass its own
    // sub-pixel cull (select-all means "select everything regardless of
    // viewport or on-screen size", the same viewport-independence
    // fit_scene_unlocked's own generate_shapes-direct call needs, for the
    // same reason - a hidden-by-being-tiny shape should still be
    // select-all'able even though a *click* on it correctly can't hit
    // it), so there's nothing a geometric hit-test actually buys here -
    // select every rect/polygon/path piece of every ShapeId on a layer
    // that's both visible and selectable, directly.
    void select_all_unlocked(LeHandle *handle)
    {
        const le::AbstractId abstract_id = handle->current_abstract();
        size_t selected_count = 0;
        bool capped = false;

        const auto select_shape_pieces = [&](le::ShapeId shape_id, le::ViewLayerPurpose purpose)
        {
            const le::Shape *shape = handle->root.get_shape(shape_id);
            if (!shape || !shape->layer.valid())
                return;

            const le::ViewLayerId view_layer = handle->view_layers.find(shape->layer, purpose);
            const le::ViewLayerData *data = handle->view_layers.get(view_layer);
            if (data && (!handle->is_layer_name_visible(data->layer_name) || !handle->is_purpose_visible(data->purpose) ||
                         !handle->is_view_layer_selectable(data->layer_name, data->purpose)))
                return;

            const auto select_piece = [&](le::PieceKind kind, size_t index)
            {
                if (selected_count >= static_cast<size_t>(kMaxSelectAllCount))
                {
                    capped = true;
                    return;
                }
                handle->select(shape_id, kind, index);
                ++selected_count;
            };
            for (size_t i = 0; i < shape->rects.size(); ++i)
                select_piece(le::PieceKind::RECT, i);
            for (size_t i = 0; i < shape->polygons.size(); ++i)
                select_piece(le::PieceKind::POLYGON, i);
            for (size_t i = 0; i < shape->paths.size(); ++i)
                select_piece(le::PieceKind::PATH, i);
        };

        for (le::TerminalId terminal_id : handle->root.get_abstract_terminals(abstract_id))
            for (le::TerminalPortId port_id : handle->root.get_terminal_ports(terminal_id))
                for (le::ShapeId shape_id : handle->root.get_terminal_port_shapes(port_id))
                    select_shape_pieces(shape_id, le::ViewLayerPurpose::TERMINAL);

        for (le::ObstructionId obstruction_id : handle->root.get_abstract_obstructions(abstract_id))
            for (le::ShapeId shape_id : handle->root.get_obstruction_shapes(obstruction_id))
                select_shape_pieces(shape_id, le::ViewLayerPurpose::OBSTRUCTION);

        if (capped)
            spdlog::warn("select_all: selection capped at {} pieces", kMaxSelectAllCount);
    }

    // Every ROUTING-type layer in `technology_id`'s own declaration
    // order (UPDATES.md 9.4) - LE_KEY_1 maps to index 0 here, LE_KEY_2
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
    // (UPDATES.md 9.4 - LEF has no distinct "VIA" layer type, vias are
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

    // LE_KEY_0..LE_KEY_9's own body (UPDATES.md 9.4/9.7) - unlocked-style
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
        // "both visible" (UPDATES.md 9.4). Recomputed as a full pass,
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

    // --- Generic LeObjectRef dispatch (UPDATES.md 7.2's database-hierarchy
    // Property Viewer redesign) - le_object_property_count/_at/
    // le_object_parent/le_selected_object_ref's own internal machinery.
    // Every ref's `index`/`generation` pair is exactly one of the seven
    // LeXxxId structs' own fields, so converting is a bare field copy. ---

    LeObjectRef invalid_object_ref()
    {
        return LeObjectRef{.kind = LE_OBJECT_KIND_LIBRARY, .index = UINT32_MAX, .generation = 0};
    }

    bool same_object_ref(LeObjectRef a, LeObjectRef b)
    {
        return a.kind == b.kind && a.index == b.index && a.generation == b.generation;
    }

    template <typename IdT>
    IdT id_from_ref(LeObjectRef ref)
    {
        return IdT{.index = ref.index, .generation = ref.generation};
    }

    LeObjectRef ref_from_id(LeObjectKind kind, le::LibraryId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::DesignId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::AbstractId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::TerminalId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::TerminalPortId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::ObstructionId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::ShapeId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    // E1 (BUGS_AND_ENHANCEMENTS.md) - the six top-level Layout-view kinds
    // LeHandLeHandle::SelectedObject's own variant grew, plus PhysicalPortSegment
    // (an intermediate parent-hop node only, mirroring TerminalPort).
    LeObjectRef ref_from_id(LeObjectKind kind, le::RowId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::PlacementId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::BlockageId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::RouteId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::PhysicalPortId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::RegionId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::PhysicalPortSegmentId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::LayoutId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }

    // Dispatches to the same by-id property builder each class's own
    // le_X_property_count/_at already uses (build_library_properties et
    // al, all lock-free, defined earlier in this file) - never the public
    // le_X_property_at functions themselves, which each take
    // handle->mutex_ on their own; le_object_property_count/_at take it
    // exactly once, so calling back into a lock-taking function here
    // would self-deadlock (std::mutex isn't recursive).
    std::vector<le::PropertyValue> build_object_properties(const le::Root &root, LeObjectRef ref)
    {
        switch (static_cast<LeObjectKind>(ref.kind))
        {
        case LE_OBJECT_KIND_LIBRARY:
            return build_library_properties(root, id_from_ref<le::LibraryId>(ref));
        case LE_OBJECT_KIND_DESIGN:
            return build_design_properties(root, id_from_ref<le::DesignId>(ref));
        case LE_OBJECT_KIND_ABSTRACT:
            return build_abstract_properties(root, id_from_ref<le::AbstractId>(ref));
        case LE_OBJECT_KIND_TERMINAL:
            return build_terminal_properties(root, id_from_ref<le::TerminalId>(ref));
        case LE_OBJECT_KIND_TERMINAL_PORT:
            return build_terminal_port_properties(root, id_from_ref<le::TerminalPortId>(ref));
        case LE_OBJECT_KIND_OBSTRUCTION:
            return build_obstruction_properties(root, id_from_ref<le::ObstructionId>(ref));
        case LE_OBJECT_KIND_SHAPE:
            return build_shape_properties(root, id_from_ref<le::ShapeId>(ref));
        case LE_OBJECT_KIND_ROW:
            return build_row_properties(root, id_from_ref<le::RowId>(ref));
        case LE_OBJECT_KIND_PLACEMENT:
            return build_placement_properties(root, id_from_ref<le::PlacementId>(ref));
        case LE_OBJECT_KIND_BLOCKAGE:
            return build_blockage_properties(root, id_from_ref<le::BlockageId>(ref));
        case LE_OBJECT_KIND_ROUTE:
            return build_route_properties(root, id_from_ref<le::RouteId>(ref));
        case LE_OBJECT_KIND_PHYSICAL_PORT:
            return build_physical_port_properties(root, id_from_ref<le::PhysicalPortId>(ref));
        case LE_OBJECT_KIND_REGION:
            return build_region_properties(root, id_from_ref<le::RegionId>(ref));
        case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
            return build_physical_port_segment_properties(root, id_from_ref<le::PhysicalPortSegmentId>(ref));
        case LE_OBJECT_KIND_LAYOUT:
            return build_layout_properties(root, id_from_ref<le::LayoutId>(ref));
        }
        return {};
    }

    // `ref`'s immediate parent - the same parent-hop graph
    // filter_field_tables() already declares for -filter validation
    // (Shape->terminal_port/obstruction, TerminalPort->terminal,
    // Terminal/Obstruction->abstract, Abstract->design, Design->library),
    // read directly off each class's own schema parent field. Library has
    // no parent. Degrades to invalid_object_ref() if `ref` doesn't
    // resolve to a real object (rather than asserting) - same graceful-
    // degradation convention as every other lookup in this file.
    LeObjectRef object_ref_parent(const le::Root &root, LeObjectRef ref)
    {
        switch (static_cast<LeObjectKind>(ref.kind))
        {
        case LE_OBJECT_KIND_LIBRARY:
            return invalid_object_ref();
        case LE_OBJECT_KIND_DESIGN:
        {
            const le::DesignData *design = root.get_design(id_from_ref<le::DesignId>(ref));
            return design ? ref_from_id(LE_OBJECT_KIND_LIBRARY, design->library) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_ABSTRACT:
        {
            const le::AbstractData *abstract = root.get_abstract(id_from_ref<le::AbstractId>(ref));
            return abstract ? ref_from_id(LE_OBJECT_KIND_DESIGN, abstract->design) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_TERMINAL:
        {
            const le::TerminalData *terminal = root.get_terminal(id_from_ref<le::TerminalId>(ref));
            return terminal ? ref_from_id(LE_OBJECT_KIND_ABSTRACT, terminal->abstract) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_TERMINAL_PORT:
        {
            const le::TerminalPortData *port = root.get_terminal_port(id_from_ref<le::TerminalPortId>(ref));
            return port ? ref_from_id(LE_OBJECT_KIND_TERMINAL, port->terminal) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_OBSTRUCTION:
        {
            const le::ObstructionData *obstruction = root.get_obstruction(id_from_ref<le::ObstructionId>(ref));
            return obstruction ? ref_from_id(LE_OBJECT_KIND_ABSTRACT, obstruction->abstract) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_SHAPE:
        {
            const le::ShapeData *shape = root.get_shape(id_from_ref<le::ShapeId>(ref));
            if (!shape)
                return invalid_object_ref();
            if (shape->terminal_port.valid())
                return ref_from_id(LE_OBJECT_KIND_TERMINAL_PORT, shape->terminal_port);
            if (shape->obstruction.valid())
                return ref_from_id(LE_OBJECT_KIND_OBSTRUCTION, shape->obstruction);
            // E1 (BUGS_AND_ENHANCEMENTS.md) - the three Layout-view
            // multi-parent fields Shape gained alongside terminal_port/
            // obstruction (schema.py).
            if (shape->blockage.valid())
                return ref_from_id(LE_OBJECT_KIND_BLOCKAGE, shape->blockage);
            if (shape->route.valid())
                return ref_from_id(LE_OBJECT_KIND_ROUTE, shape->route);
            if (shape->physical_port_segment.valid())
                return ref_from_id(LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT, shape->physical_port_segment);
            // Free-standing shapes (Abstract/Layout.free_shapes) and the
            // one boundary/diearea Shape - all owned directly by the
            // Abstract/Layout itself.
            if (shape->in_abstract.valid())
                return ref_from_id(LE_OBJECT_KIND_ABSTRACT, shape->in_abstract);
            if (shape->in_layout.valid())
                return ref_from_id(LE_OBJECT_KIND_LAYOUT, shape->in_layout);
            if (shape->abstract.valid())
                return ref_from_id(LE_OBJECT_KIND_ABSTRACT, shape->abstract);
            if (shape->layout.valid())
                return ref_from_id(LE_OBJECT_KIND_LAYOUT, shape->layout);
            return invalid_object_ref(); // shouldn't happen - mutually exclusive per schema.py - degrade gracefully anyway
        }
        case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
        {
            const le::PhysicalPortSegmentData *segment = root.get_physical_port_segment(id_from_ref<le::PhysicalPortSegmentId>(ref));
            return segment ? ref_from_id(LE_OBJECT_KIND_PHYSICAL_PORT, segment->physical_port) : invalid_object_ref();
        }
        // E1 - Row/Placement/Blockage/Route/PhysicalPort/Region's own
        // parent is a Layout, which now has its own LeObjectKind (see
        // LE_OBJECT_KIND_LAYOUT below) so this hop continues on up to
        // Design -> Library instead of dead-ending here.
        case LE_OBJECT_KIND_ROW:
        {
            const le::RowData *row = root.get_row(id_from_ref<le::RowId>(ref));
            return row ? ref_from_id(LE_OBJECT_KIND_LAYOUT, row->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_PLACEMENT:
        {
            const le::PlacementData *placement = root.get_placement(id_from_ref<le::PlacementId>(ref));
            return placement ? ref_from_id(LE_OBJECT_KIND_LAYOUT, placement->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_BLOCKAGE:
        {
            const le::BlockageData *blockage = root.get_blockage(id_from_ref<le::BlockageId>(ref));
            return blockage ? ref_from_id(LE_OBJECT_KIND_LAYOUT, blockage->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_ROUTE:
        {
            const le::RouteData *route = root.get_route(id_from_ref<le::RouteId>(ref));
            return route ? ref_from_id(LE_OBJECT_KIND_LAYOUT, route->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_PHYSICAL_PORT:
        {
            const le::PhysicalPortData *port = root.get_physical_port(id_from_ref<le::PhysicalPortId>(ref));
            return port ? ref_from_id(LE_OBJECT_KIND_LAYOUT, port->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_REGION:
        {
            const le::RegionData *region = root.get_region(id_from_ref<le::RegionId>(ref));
            return region ? ref_from_id(LE_OBJECT_KIND_LAYOUT, region->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_LAYOUT:
        {
            const le::LayoutData *layout = root.get_layout(id_from_ref<le::LayoutId>(ref));
            return layout ? ref_from_id(LE_OBJECT_KIND_DESIGN, layout->design) : invalid_object_ref();
        }
        }
        return invalid_object_ref();
    }

    // --- shape_* operations (NEW_FEATURES_SEPT_2026.md item 1) ---

        std::vector<le::ShapeId> shape_ids_from_c(const LeShapeId *ids, int32_t count)
        {
            std::vector<le::ShapeId> out;
            if (!ids || count <= 0)
                return out;
            out.reserve(static_cast<size_t>(count));
            for (int32_t i = 0; i < count; ++i)
                out.push_back(from_c(ids[i]));
            return out;
        }

        // A shape_* command's target: a real layer, else a layer-less purpose
        // (e.g. "DEBUG"), else nullopt to keep each input's own; an unknown
        // purpose string is an error.
        std::expected<std::optional<le::shape_ops::LayerOrPurpose>, std::string> target_from_c(LeLayerId layer, const char *purpose)
        {
            if (layer.index != UINT32_MAX)
                return le::shape_ops::LayerOrPurpose{.layer = from_c(layer)};
            if (purpose && purpose[0])
            {
                const std::optional<le::ShapePurpose> parsed = le::shape_purpose_from_string(purpose);
                if (!parsed)
                    return std::unexpected(fmt::format("unknown purpose \"{}\"", purpose));
                return le::shape_ops::LayerOrPurpose{.purpose = *parsed};
            }
            return std::optional<le::shape_ops::LayerOrPurpose>{};
        }

        // An explicit parent (any kind shape_ops::ShapeParent supports),
        // else the current view's own Abstract/Layout free-shapes list -
        // the GUI's "current view" (LeHandle::current_abstract()/
        // current_layout()) first, then the TCL-level current_abstract/
        // current_layout (has_current_access) for a script that never
        // opened a view.
        std::optional<le::shape_ops::ShapeParent> resolve_shape_parent(LeHandle *handle, LeObjectRef parent, std::string &error)
        {
            if (parent.index != UINT32_MAX)
            {
                switch (parent.kind)
                {
                case LE_OBJECT_KIND_ABSTRACT:
                    return id_from_ref<le::AbstractId>(parent);
                case LE_OBJECT_KIND_LAYOUT:
                    return id_from_ref<le::LayoutId>(parent);
                case LE_OBJECT_KIND_OBSTRUCTION:
                    return id_from_ref<le::ObstructionId>(parent);
                case LE_OBJECT_KIND_TERMINAL_PORT:
                    return id_from_ref<le::TerminalPortId>(parent);
                case LE_OBJECT_KIND_ROUTE:
                    return id_from_ref<le::RouteId>(parent);
                case LE_OBJECT_KIND_BLOCKAGE:
                    return id_from_ref<le::BlockageId>(parent);
                case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
                    return id_from_ref<le::PhysicalPortSegmentId>(parent);
                default:
                    error = "-parent must be an abstract, layout, obstruction, terminal_port, route, blockage or physical_port_segment";
                    return std::nullopt;
                }
            }
            if (handle->root.get_abstract(handle->current_abstract()))
                return handle->current_abstract();
            if (handle->root.get_layout(handle->current_layout()))
                return handle->current_layout();
            if (handle->root.get_abstract(handle->current_abstract_id))
                return handle->current_abstract_id;
            if (handle->root.get_layout(handle->current_layout_id))
                return handle->current_layout_id;
            error = "no -parent given and no current Abstract or Layout is open";
            return std::nullopt;
        }

        // Stores `result`'s ids for le_shape_op_result_at, bumps the
        // mutation version and records each create for undo - the same
        // steps the generated le_create_shape takes - or logs its error.
        int32_t finish_shape_op(LeHandle *handle, const char *command, const le::shape_ops::Result &result)
        {
            handle->shape_op_results.clear();
            if (!result)
            {
                spdlog::error("{}: {}", command, result.error());
                return -1;
            }
            handle->shape_op_results = *result;
            if (result->empty())
                return 0;
            handle->root.bump_mutation_version();
            if (handle->command_history.is_recording())
            {
                for (le::ShapeId id : *result)
                    handle->command_history.current()->record_create<le::ShapeId, le::ShapeData>(
                        id, *handle->root.get_shape(id),
                        [](le::Root &r, const le::ShapeData &d) { return r.create_shape(d); },
                        [](le::Root &r, le::ShapeId i) { return r.delete_shape(i); });
            }
            return static_cast<int32_t>(result->size());
        }

        // Every per-input shape_* operation's own common prologue.
        template <typename Op>
        int32_t run_shape_op(LeHandle *handle, const char *command, LeLayerId layer, const char *purpose, LeObjectRef parent, Op &&op)
        {
            if (!handle)
                return -1;
            HandleWriteLock lock(handle);
            auto fail = [&](const std::string &error)
            {
                handle->shape_op_results.clear();
                spdlog::error("{}: {}", command, error);
                return -1;
            };
            const auto target = target_from_c(layer, purpose);
            if (!target)
                return fail(target.error());
            std::string error;
            const std::optional<le::shape_ops::ShapeParent> resolved = resolve_shape_parent(handle, parent, error);
            if (!resolved)
                return fail(error);
            return finish_shape_op(handle, command, op(*target, *resolved));
        }
}

extern "C"
{
    // Generated TCL property-reading surface (public, externally-linked
    // functions - le_X_property_count/_at/_path, friendly-id-by-name
    // lookups, is_child-field enumeration - declared in api.hpp's own
    // generated_tcl/declarations.inc, called from le_tcl_shim.cpp) for
    // every TCL-readable class not already covered by hand-written code.
    // Must live inside this extern "C" block, not the anonymous namespace
    // above (internal linkage there would make these unresolvable from
    // other translation units) - see
    // generated_tcl/property_accessors_public.inc's own header comment.
    // Never edit that file directly - regenerate via the regen-tcl skill.
#include "generated_tcl/property_accessors_public.inc"

    // Generated get_<type> search (le_get_X/le_search_result_X_at) for
    // every TCL-readable class - same external-linkage requirement as
    // property_accessors_public.inc above. Never edit that file directly
    // - regenerate via the regen-tcl skill.
#include "generated_tcl/search.inc"

    LeHandle *le_create(void)
    {
        return new LeHandle();
    }

    void le_destroy(LeHandle *handle)
    {
        delete handle;
    }

    int le_read_lef(LeHandle *handle, const char *path, const char *library_name)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!path)
        {
            spdlog::error("read_lef: path is null");
            return 1;
        }
        if (!library_name || !library_name[0])
        {
            spdlog::error("read_lef: a library name is required");
            return 1;
        }

        // UPDATES.md 10 - snapshot the Technology's own layer count before
        // this read, so the default-visibility pass below (after the read)
        // can tell which physical layers this specific call newly
        // introduced, as opposed to ones a prior le_read_lef call already
        // defaulted - LEF layers are only ever appended to a Technology
        // within a session (see LEFReader's own is_technology_empty()-
        // gated create-vs-reuse), so get_technology_layers' existing
        // declaration-order prefix stays stable across reads and re-
        // running the default over it would silently re-hide a layer the
        // user has since made visible via the layer manager.
        size_t old_layer_count = 0;
        {
            const auto existing_technology_ids = handle->root.get_technology_ids();
            if (!existing_technology_ids.empty())
                old_layer_count = handle->root.get_technology_layers(existing_technology_ids.front()).size();
        }

        const std::filesystem::path lef_path(path);
        le::LEFReader reader;
        const int result = reader.read_lef(lef_path.string(), handle->root, library_name);
        if (result != 0)
            return result;

        // Rebuilt after every successful read, not just the first, so a
        // later LEF file's own new physical layers (e.g. a second macro
        // file with inline LAYER declarations) are picked up too - cheap
        // relative to a full LEF parse, so correctness here wins over the
        // small extra cost without needing a benchmark to justify it.
        const auto technology_ids = handle->root.get_technology_ids();
        if (!technology_ids.empty())
        {
            // UPDATES.md 10 - every physical layer this read newly
            // introduced defaults to hidden unless it's ROUTING or CUT.
            // BOUNDARY isn't a physical layer at all (a Shape with
            // purpose=BOUNDARY instead - see Shape.layer/.purpose's own
            // schema.py comments), so it never reaches this loop - stays
            // visible via LeHandle::is_layer_name_visible's own default-
            // true-until-toggled behavior, same as it always has.
            const auto &layers = handle->root.get_technology_layers(technology_ids.front());
            for (size_t i = old_layer_count; i < layers.size(); ++i)
            {
                const le::LayerData *layer = handle->root.get_layer(layers[i]);
                if (layer && layer->type != "ROUTING" && layer->type != "CUT")
                    handle->set_layer_name_visible(layer->name, false);
            }

            rebuild_view_layers(handle, technology_ids.front());

            // Also selects the singleton Technology as the current one for
            // the generated TCL current-instance mechanism (see
            // codegen/codegen/tcl_scope.py's own module docstring for why
            // get_layers/get_vias/etc.'s default scope needs this) -
            // Technology has no separate "open" step the way a Design/
            // Abstract does (le_tcl_procs.tcl's open_design), it's
            // implicitly read alongside everything else in a LEF file, so
            // this is the one natural chokepoint - shared by every caller
            // (Dart FFI direct, or TCL's own read_lef, which calls this
            // same function), not just the TCL-facing shim. Idempotent to
            // repeat across multiple le_read_lef calls on the same handle.
            handle->current_technology_id = technology_ids.front();
        }

        return 0;
    }

    int le_read_def(LeHandle *handle, const char *path, const char *library_name)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!path)
        {
            spdlog::error("read_def: path is null");
            return 1;
        }
        if (!library_name || !library_name[0])
        {
            spdlog::error("read_def: a library name is required");
            return 1;
        }

        const std::filesystem::path def_path(path);
        le::DEFReader reader;
        const int result = reader.read_def(def_path.string(), handle->root, library_name);
        if (result != 0)
            return result;

        // NONDEFAULTRULES may have resolved/created the shared Technology
        // (DEFReader::technology_id_, same reuse-or-create pattern
        // le_read_lef's own LEFReader uses) even if no LEF has been read
        // into this handle yet - mirror le_read_lef's own
        // current_technology_id selection so the generated TCL
        // current-instance mechanism still works. Layer-visibility
        // defaults and ViewLayerSet aren't touched here - DEF doesn't
        // introduce new physical Layers of its own (see this function's
        // own api.hpp comment).
        const auto technology_ids = handle->root.get_technology_ids();
        if (!technology_ids.empty())
            handle->current_technology_id = technology_ids.front();

        return 0;
    }

    int le_read_verilog(LeHandle *handle, const char *const *filenames, int32_t filename_count, int32_t is_netlist, const char *library_name)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!filenames || filename_count <= 0)
        {
            spdlog::error("read_verilog: filenames is null or empty");
            return 1;
        }
        if (!library_name || !library_name[0])
        {
            spdlog::error("read_verilog: a library name is required");
            return 1;
        }

        std::vector<std::string> filename_strings;
        filename_strings.reserve(static_cast<size_t>(filename_count));
        for (int32_t i = 0; i < filename_count; ++i)
            filename_strings.emplace_back(filenames[i] ? filenames[i] : "");

        // Netlist flavor only - the RTL flavor (read_rtl) never
        // elaborates against a real slang::ast::Compilation at all (see
        // SVReader's own class comment), so it has no "unknown module"
        // failure mode a stub could address. Automatically covers every
        // LEF-only Design (Abstract but no Schematic) already in this
        // handle's Root, from any read_lef so far - a Design that
        // already has a Schematic (a prior real Verilog read) is
        // skipped by generate_verilog_stubs itself, so re-running this
        // on a later read_verilog call never produces a duplicate
        // module definition. See verilog_stub_writer.hpp's own
        // top-of-file comment for why this has to be a real file
        // appended to the same read (not a separate prior read_verilog
        // call) - only files elaborated together share one Compilation.
        std::string stub_path;
        if (is_netlist)
        {
            std::string stub_source;
            for (le::LibraryId library_id : handle->root.get_library_ids())
                stub_source += le::generate_verilog_stubs(handle->root, library_id);

            if (!stub_source.empty())
            {
                const std::filesystem::path candidate = std::filesystem::temp_directory_path() /
                    fmt::format("le_verilog_stubs_{}_{}.v", getpid(), reinterpret_cast<uintptr_t>(handle));
                std::ofstream stub_file(candidate);
                if (stub_file)
                {
                    stub_file << stub_source;
                    stub_file.close();
                    stub_path = candidate.string();
                    filename_strings.push_back(stub_path);
                }
                else
                {
                    spdlog::warn("read_verilog: could not write temporary stub file {} - LEF-only leaf cells may fail to elaborate",
                                 candidate.string());
                }
            }
        }

        le::SVReader reader;
        const int result = is_netlist
            ? reader.read_netlist(filename_strings, handle->root, library_name)
            : reader.read_rtl(filename_strings, handle->root, library_name);

        if (!stub_path.empty())
        {
            std::error_code ec;
            std::filesystem::remove(stub_path, ec);
        }

        return result;
    }

    int le_write_verilog_stubs(LeHandle *handle, const char *path, LeLibraryId library_id_c)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!path)
        {
            spdlog::error("write_verilog_stubs: path is null");
            return 1;
        }

        le::LibraryId library_id{.index = library_id_c.index, .generation = library_id_c.generation};
        if (library_id.index == UINT32_MAX)
        {
            const auto library_ids = handle->root.get_library_ids();
            if (library_ids.size() != 1)
            {
                spdlog::error("write_verilog_stubs: no -library given and {} Libraries exist - need exactly 1 to default to",
                               library_ids.size());
                return 1;
            }
            library_id = library_ids.front();
        }
        else if (!handle->root.get_library(library_id))
        {
            spdlog::error("write_verilog_stubs: unknown library");
            return 1;
        }

        const std::string stub_source = le::generate_verilog_stubs(handle->root, library_id);
        std::ofstream out(path);
        if (!out)
        {
            spdlog::error("write_verilog_stubs: could not open {} for writing", path);
            return 1;
        }
        out << stub_source;
        return 0;
    }

    int32_t le_link_unresolved_instances(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        const size_t resolved = le::SVReader::link_unresolved_instances(handle->root);

        // Physical-side linking (Placement/Route/PhysicalPort <-> sibling
        // Schematic, LINKING_STRATEGY_RESEARCH.md sections 1/2) runs
        // *after* the instance-resolution call above reaches its own
        // fixed point - an Instance whose own reference_design is still
        // unresolved can't be reached by link_physical's own hierarchical
        // descent (see that function's own "ordering dependency" comment,
        // schematic_layout_linker.hpp). Its own counts don't change this
        // function's return value (still Instance-resolution-count only,
        // matching sv_reader_test.cpp's own existing assertions) -
        // link_physical logs its own WARNING/ERROR messages via spdlog
        // directly, same as LEFReader/DEFReader.
        le::link_physical(handle->root);

        return static_cast<int32_t>(resolved);
    }

    // --- Phase 5 mutation side-effects (LINKING_STRATEGY_RESEARCH.md
    // section 5) - unlike link_physical/link_unresolved_instances above
    // (bulk, re-derivable, deliberately not undoable), these are direct
    // user edits, so each is batched into one undo/redo transaction the
    // same way move_click_unlocked is: raw Root:: calls (not the
    // generated public le_update_x/le_delete_x - those can't express
    // "clear this reference field back to unset", which the Net-delete
    // cascade specifically needs) under one held lock, with every step
    // manually recorded via Transaction::record_update/record_delete.
    // handle->command_history.begin/end is called unconditionally in
    // each - a no-op if a caller (le_repl_eval) already has one open,
    // and otherwise starts/ends one itself (le_shell's own console isn't
    // wired through le_repl_eval - see le_tcl_procs.tcl's own comment).

    int le_delete_net_cascade(LeHandle *handle, LeNetId id)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        const le::NetId net_id = from_c(id);
        const le::NetData *existing_net = handle->root.get_net(net_id);
        if (!existing_net)
        {
            spdlog::error("delete_net: unknown id - no such Net exists");
            return 1;
        }
        const le::NetData net_snapshot = *existing_net;
        const le::SchematicId schematic_id = net_snapshot.schematic;

        handle->command_history.begin("delete_net");
        le::editing::Transaction *txn = handle->command_history.current();
        // Captured before the Net itself is deleted below, so a Route's
        // own record_delete create_fn can repoint its `.net` at whatever
        // id the Net ends up with on undo (recreated with a *new* id,
        // per Pool::create()'s own "never reuses an id" rule - see
        // IdCell's own comment) rather than the stale one snapshotted in
        // route_snapshot.net.
        le::editing::IdCellPtr<le::NetId> net_cell = txn ? txn->id_cell_for(net_id) : nullptr;

        const le::SchematicData *schematic = handle->root.get_schematic(schematic_id);
        if (schematic)
        {
            for (const le::InstanceId instance_id : handle->root.get_schematic_instances(schematic_id))
            {
                for (const le::PinId pin_id : handle->root.get_instance_pins(instance_id))
                {
                    const le::PinData *pin = handle->root.get_pin(pin_id);
                    if (!pin || pin->net != net_id)
                        continue;
                    const le::PinData before = *pin;
                    handle->root.update_pin(pin_id, le::InstanceId{}, std::optional<le::NetId>(le::NetId{}),
                                             std::nullopt, std::nullopt, std::nullopt);
                    if (txn)
                        txn->record_update<le::PinId, le::PinData>(pin_id, before, *handle->root.get_pin(pin_id),
                                                                    &le::apply_pin_snapshot);
                }
            }

            for (const le::PortId port_id : handle->root.get_schematic_ports(schematic_id))
            {
                const le::PortData *port = handle->root.get_port(port_id);
                if (!port || port->net != net_id)
                    continue;
                const le::PortData before = *port;
                handle->root.update_port(port_id, le::SchematicId{}, std::nullopt, std::optional<le::NetId>(le::NetId{}),
                                          std::nullopt, std::nullopt, std::nullopt);
                if (txn)
                    txn->record_update<le::PortId, le::PortData>(port_id, before, *handle->root.get_port(port_id),
                                                                  &le::apply_port_snapshot);
            }
        }

        const le::LayoutId layout_id = schematic ? handle->root.get_design_layout(schematic->design) : le::LayoutId{};
        if (layout_id.valid())
        {
            std::vector<le::RouteId> routes_to_delete;
            for (const le::RouteId route_id : handle->root.get_layout_routes(layout_id))
            {
                const le::RouteData *route = handle->root.get_route(route_id);
                if (route && route->net == net_id)
                    routes_to_delete.push_back(route_id);
            }
            for (const le::RouteId route_id : routes_to_delete)
            {
                const le::RouteData route_snapshot = *handle->root.get_route(route_id);
                le::editing::IdCellPtr<le::RouteId> route_cell = txn ? txn->id_cell_for(route_id) : nullptr;

                for (const le::ShapeId shape_id : handle->root.get_route_shapes(route_id))
                {
                    const le::ShapeData shape_snapshot = *handle->root.get_shape(shape_id);
                    handle->root.delete_shape(shape_id);
                    if (txn)
                    {
                        txn->record_delete<le::ShapeId, le::ShapeData>(
                            shape_id, shape_snapshot,
                            [route_cell](le::Root &r, const le::ShapeData &d)
                            {
                                le::ShapeData fixed = d;
                                fixed.route = route_cell->id;
                                return r.create_shape(fixed);
                            },
                            [](le::Root &r, le::ShapeId i) { return r.delete_shape(i); });
                    }
                }

                handle->root.delete_route(route_id);
                if (txn)
                {
                    txn->record_delete<le::RouteId, le::RouteData>(
                        route_id, route_snapshot,
                        [net_cell](le::Root &r, const le::RouteData &d)
                        {
                            le::RouteData fixed = d;
                            fixed.net = net_cell->id;
                            return r.create_route(fixed);
                        },
                        [](le::Root &r, le::RouteId i) { return r.delete_route(i); });
                }
            }

            for (const le::PhysicalPortId port_id : handle->root.get_layout_physical_ports(layout_id))
            {
                const le::PhysicalPortData *port = handle->root.get_physical_port(port_id);
                if (!port || port->net != net_id)
                    continue;
                const le::PhysicalPortData before = *port;
                handle->root.update_physical_port(port_id, le::LayoutId{}, std::optional<le::NetId>(le::NetId{}),
                                                   std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                                   std::nullopt, std::nullopt);
                if (txn)
                    txn->record_update<le::PhysicalPortId, le::PhysicalPortData>(
                        port_id, before, *handle->root.get_physical_port(port_id), &le::apply_physical_port_snapshot);
            }
        }

        handle->root.delete_net(net_id);
        handle->root.bump_mutation_version();
        if (txn)
        {
            txn->record_delete<le::NetId, le::NetData>(
                net_id, net_snapshot, [](le::Root &r, const le::NetData &d) { return r.create_net(d); },
                [](le::Root &r, le::NetId i) { return r.delete_net(i); });
        }
        handle->command_history.end(/*succeeded=*/true);
        return 0;
    }

    int le_rename_net_propagate(LeHandle *handle, LeNetId id, const char *new_name)
    {
        if (!handle)
            return 1;
        if (!new_name || !new_name[0])
        {
            spdlog::error("update_net: -name may not be empty");
            return 1;
        }
        HandleWriteLock lock(handle);

        const le::NetId net_id = from_c(id);
        const le::NetData *existing_net = handle->root.get_net(net_id);
        if (!existing_net)
        {
            spdlog::error("update_net: unknown id - no such Net exists");
            return 1;
        }
        const le::SchematicId schematic_id = existing_net->schematic;

        handle->command_history.begin("update_net");
        le::editing::Transaction *txn = handle->command_history.current();

        const le::NetData before_net = *existing_net;
        const bool renamed =
            handle->root.update_net(net_id, le::SchematicId{}, std::nullopt, std::optional<std::string>(new_name),
                                     std::nullopt);
        if (!renamed)
        {
            spdlog::error("update_net: a sibling Net with this name already exists");
            handle->command_history.end(false);
            return 1;
        }
        if (txn)
            txn->record_update<le::NetId, le::NetData>(net_id, before_net, *handle->root.get_net(net_id),
                                                        &le::apply_net_snapshot);

        const le::SchematicData *schematic = handle->root.get_schematic(schematic_id);
        const le::LayoutId layout_id = schematic ? handle->root.get_design_layout(schematic->design) : le::LayoutId{};
        if (layout_id.valid())
        {
            for (const le::RouteId route_id : handle->root.get_layout_routes(layout_id))
            {
                const le::RouteData *route = handle->root.get_route(route_id);
                if (!route || route->net != net_id)
                    continue;
                const le::RouteData before = *route;
                if (!handle->root.update_route(route_id, le::LayoutId{}, std::nullopt,
                                                std::optional<std::string>(new_name), std::nullopt, std::nullopt,
                                                std::nullopt, std::nullopt))
                {
                    spdlog::warn(
                        "update_net: linked Route could not be renamed to '{}' - a sibling Route with that name already exists",
                        new_name);
                    continue;
                }
                if (txn)
                    txn->record_update<le::RouteId, le::RouteData>(route_id, before, *handle->root.get_route(route_id),
                                                                    &le::apply_route_snapshot);
            }
            for (const le::PhysicalPortId port_id : handle->root.get_layout_physical_ports(layout_id))
            {
                const le::PhysicalPortData *port = handle->root.get_physical_port(port_id);
                if (!port || port->net != net_id)
                    continue;
                const le::PhysicalPortData before = *port;
                if (!handle->root.update_physical_port(port_id, le::LayoutId{}, std::nullopt,
                                                        std::optional<std::string>(new_name), std::nullopt, std::nullopt,
                                                        std::nullopt, std::nullopt, std::nullopt, std::nullopt))
                {
                    spdlog::warn(
                        "update_net: linked PhysicalPort could not be renamed to '{}' - a sibling PhysicalPort with that name already exists",
                        new_name);
                    continue;
                }
                if (txn)
                    txn->record_update<le::PhysicalPortId, le::PhysicalPortData>(
                        port_id, before, *handle->root.get_physical_port(port_id), &le::apply_physical_port_snapshot);
            }
        }

        handle->root.bump_mutation_version();
        handle->command_history.end(/*succeeded=*/true);
        return 0;
    }

    int le_rename_instance_propagate(LeHandle *handle, LeInstanceId id, const char *new_name)
    {
        if (!handle)
            return 1;
        if (!new_name || !new_name[0])
        {
            spdlog::error("update_instance: -name may not be empty");
            return 1;
        }
        HandleWriteLock lock(handle);

        const le::InstanceId instance_id = from_c(id);
        const le::InstanceData *existing = handle->root.get_instance(instance_id);
        if (!existing)
        {
            spdlog::error("update_instance: unknown id - no such Instance exists");
            return 1;
        }

        handle->command_history.begin("update_instance");
        le::editing::Transaction *txn = handle->command_history.current();

        const le::InstanceData before = *existing;
        const bool renamed = handle->root.update_instance(instance_id, le::SchematicId{}, std::nullopt,
                                                            std::optional<std::string>(new_name), std::nullopt,
                                                            std::nullopt, std::nullopt, std::nullopt, std::nullopt);
        if (!renamed)
        {
            spdlog::error("update_instance: a sibling Instance with this name already exists");
            handle->command_history.end(false);
            return 1;
        }
        if (txn)
            txn->record_update<le::InstanceId, le::InstanceData>(
                instance_id, before, *handle->root.get_instance(instance_id), &le::apply_instance_snapshot);

        const auto record_placement = [&](le::PlacementId placement_id, const le::PlacementData &b, const le::PlacementData &a)
        {
            if (txn)
                txn->record_update<le::PlacementId, le::PlacementData>(placement_id, b, a, &le::apply_placement_snapshot);
        };
        const auto record_route = [&](le::RouteId route_id, const le::RouteData &b, const le::RouteData &a)
        {
            if (txn)
                txn->record_update<le::RouteId, le::RouteData>(route_id, b, a, &le::apply_route_snapshot);
        };
        const auto record_physical_port =
            [&](le::PhysicalPortId port_id, const le::PhysicalPortData &b, const le::PhysicalPortData &a)
        {
            if (txn)
                txn->record_update<le::PhysicalPortId, le::PhysicalPortData>(port_id, b, a,
                                                                              &le::apply_physical_port_snapshot);
        };
        le::propagate_instance_rename(handle->root, instance_id, record_placement, record_route, record_physical_port);

        handle->root.bump_mutation_version();
        handle->command_history.end(/*succeeded=*/true);
        return 0;
    }

    int le_write_lef(LeHandle *handle, const char *path,
                     const LeAbstractId *abstract_ids_c, int32_t abstract_id_count,
                     LeLibraryId library_id_c, int32_t layer_write_mode)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!path)
        {
            spdlog::error("write_lef: path is null");
            return 1;
        }

        le::LEFWriter::LayerWriteMode mode;
        switch (layer_write_mode)
        {
        case LE_LEF_LAYER_WRITE_MODE_INCLUDE_WITH_ABSTRACT:
            mode = le::LEFWriter::LayerWriteMode::IncludeWithAbstract;
            break;
        case LE_LEF_LAYER_WRITE_MODE_TECHNOLOGY_ONLY:
            mode = le::LEFWriter::LayerWriteMode::TechnologyOnly;
            break;
        default:
            mode = le::LEFWriter::LayerWriteMode::None;
            break;
        }

        // BUGS_AND_ENHANCEMENTS.md E28.b resolution order - see this
        // function's own api.hpp doc comment for the full 4-step
        // rationale. Skipped entirely in TechnologyOnly mode. Reads
        // handle->current_abstract_id directly rather than calling
        // le_current_abstract(handle) - that function takes
        // handle->mutex_ itself, and this function already holds it
        // (non-recursive std::mutex, so re-locking here would deadlock).
        std::vector<le::AbstractId> abstract_ids;
        if (mode != le::LEFWriter::LayerWriteMode::TechnologyOnly)
        {
            if (abstract_id_count > 0)
            {
                abstract_ids.reserve(static_cast<size_t>(abstract_id_count));
                for (int32_t i = 0; i < abstract_id_count; i++)
                    abstract_ids.push_back(le::AbstractId{.index = abstract_ids_c[i].index, .generation = abstract_ids_c[i].generation});
            }
            else
            {
                const le::LibraryId library_id{.index = library_id_c.index, .generation = library_id_c.generation};
                if (library_id.index != UINT32_MAX)
                {
                    for (const le::DesignId design_id : handle->root.get_library_designs(library_id))
                    {
                        const le::AbstractId design_abstract_id = handle->root.get_design_abstract(design_id);
                        if (design_abstract_id.index != UINT32_MAX)
                            abstract_ids.push_back(design_abstract_id);
                    }
                }
                else if (handle->current_abstract_id.index != UINT32_MAX)
                {
                    abstract_ids.push_back(handle->current_abstract_id);
                }
                else
                {
                    spdlog::error("write_lef: no Abstract or Library given and no current Abstract set");
                    return 1;
                }
            }
        }

        le::LEFWriter writer;
        return writer.write_lef(path, handle->root, abstract_ids, mode);
    }

    int le_write_def(LeHandle *handle, const char *path, LeLayoutId layout_id_c)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!path)
        {
            spdlog::error("write_def: path is null");
            return 1;
        }

        // Same "invalid id means use current" convention as le_write_lef
        // above - read handle->current_layout_id directly rather than
        // calling le_current_layout(handle), which would re-lock
        // handle->mutex_ and deadlock (see le_write_lef's own comment).
        le::LayoutId layout_id{.index = layout_id_c.index, .generation = layout_id_c.generation};
        if (layout_id.index == UINT32_MAX)
        {
            layout_id = handle->current_layout_id;
        }
        if (layout_id.index == UINT32_MAX)
        {
            spdlog::error("write_def: no Layout given and no current Layout set");
            return 1;
        }

        le::DEFWriter writer;
        return writer.write_def(path, handle->root, layout_id);
    }

    int32_t le_design_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return static_cast<int32_t>(handle->root.get_design_size());
    }

    const char *le_design_name(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return nullptr;
        HandleWriteLock lock(handle);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return nullptr;

        const le::DesignData *design = handle->root.get_design(design_ids[static_cast<size_t>(index)]);
        return design ? design->name.c_str() : nullptr;
    }

    int32_t le_property_path_failed(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return handle->last_property_path_failed ? 1 : 0;
    }

    int le_set_current_design_abstract(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return 1;
        HandleWriteLock lock(handle);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return 1;

        // Moves the Abstract-view "current view" trackers together - two
        // genuinely separate concepts that happen to both live on
        // LeHandle now: `handle->current_abstract()` (formerly Scene's
        // own, drives GUI rendering) and the generated has_current_access
        // one (handle->current_abstract_id, what get_terminals/get_shapes/
        // etc.'s own default -of-omitted scope and resolve_terminal_id
        // derive from - see current_abstract_id's own declaration
        // comment). Selecting a Design from either FFI caller (a
        // Dart-driven GUI) or a TCL script (open_design, itself calling
        // le_set_current_design_abstract_by_id below - the same shared
        // entry point) should mean the same thing to both.
        // Clears current_layout_id and handle->current_layout()
        // (Migration Step 3 Phase C): only one view is "open" at a time
        // (see le_set_current_design_layout's own comment) - selecting
        // the Abstract view deactivates the Layout one, same as the
        // reverse.
        const le::DesignId design_id = design_ids[static_cast<size_t>(index)];
        const le::AbstractId abstract_id = handle->root.get_design_abstract(design_id);
        handle->set_current_abstract(abstract_id);
        handle->set_current_layout(le::LayoutId{});
        handle->current_abstract_id = abstract_id;
        handle->current_layout_id = le::LayoutId{};
        return 0;
    }

    int32_t le_library_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_library_size());
    }

    LeLibraryInfo le_library_at(LeHandle *handle, int32_t index)
    {
        const LeLibraryInfo invalid{.id = {UINT32_MAX, 0}, .name = nullptr};
        if (!handle || index < 0)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const auto library_ids = handle->root.get_library_ids();
        if (static_cast<size_t>(index) >= library_ids.size())
            return invalid;

        const le::LibraryId id = library_ids[static_cast<size_t>(index)];
        const le::LibraryData *library = handle->root.get_library(id);
        return LeLibraryInfo{.id = to_c(id), .name = library ? library->name.c_str() : nullptr};
    }

    int32_t le_library_design_count(LeHandle *handle, int32_t library_index)
    {
        if (!handle || library_index < 0)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const auto library_ids = handle->root.get_library_ids();
        if (static_cast<size_t>(library_index) >= library_ids.size())
            return 0;

        return static_cast<int32_t>(handle->root.get_library_designs(library_ids[static_cast<size_t>(library_index)]).size());
    }

    LeDesignInfo le_library_design_at(LeHandle *handle, int32_t library_index, int32_t design_index)
    {
        const LeDesignInfo invalid{.library_id = {UINT32_MAX, 0}, .id = {UINT32_MAX, 0}, .abstract_id = {UINT32_MAX, 0}, .layout_id = {UINT32_MAX, 0}, .name = nullptr};
        if (!handle || library_index < 0 || design_index < 0)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const auto library_ids = handle->root.get_library_ids();
        if (static_cast<size_t>(library_index) >= library_ids.size())
            return invalid;

        const le::LibraryId library_id = library_ids[static_cast<size_t>(library_index)];
        const auto &design_ids = handle->root.get_library_designs(library_id);
        if (static_cast<size_t>(design_index) >= design_ids.size())
            return invalid;

        const le::DesignId design_id = design_ids[static_cast<size_t>(design_index)];
        const le::DesignData *design = handle->root.get_design(design_id);
        return LeDesignInfo{
            .library_id = to_c(library_id),
            .id = to_c(design_id),
            .abstract_id = to_c(handle->root.get_design_abstract(design_id)),
            .layout_id = to_c(handle->root.get_design_layout(design_id)),
            .name = design ? design->name.c_str() : nullptr,
        };
    }

    // Technology is a single shared per-session instance, same assumption
    // database_units_microns() already makes (root.get_technology_ids().front()).
    // Hand-written, not generated - it's a session/singleton lookup, not
    // per-class CRUD (see backend/CLAUDE.md's TCL section).
    LeTechnologyId le_technology_id(LeHandle *handle)
    {
        const LeTechnologyId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle)
            return invalid;
        HandleWriteLock lock(handle);

        const auto technology_ids = handle->root.get_technology_ids();
        if (technology_ids.empty())
            return invalid;
        return to_c(technology_ids.front());
    }

    int le_set_current_design_abstract_by_id(LeHandle *handle, LeDesignId design_id)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        const le::DesignId id = from_c(design_id);
        if (!handle->root.get_design(id))
            return 1;

        // See le_set_current_design_abstract's own comment above - the
        // Abstract-view trackers move together, current_layout_id clears.
        const le::AbstractId abstract_id = handle->root.get_design_abstract(id);
        handle->set_current_abstract(abstract_id);
        handle->set_current_layout(le::LayoutId{});
        handle->current_abstract_id = abstract_id;
        handle->current_layout_id = le::LayoutId{};
        return 0;
    }

    int le_set_current_design_layout(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return 1;
        HandleWriteLock lock(handle);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return 1;

        // Mirror image of le_set_current_design_abstract: activates the
        // Layout view's own current-instance tracker (what get_rows/
        // get_placements/get_blockages/etc.'s own default -of-omitted
        // scope derives from) and handle->current_layout() (Migration
        // Step 3 Phase C - what le_render_pixel_buffer now actually
        // renders, via InstanceRenderer::render_layout_frame), and
        // deactivates the Abstract-view ones - only one view is "open" at
        // a time, matching a real GUI showing one editor.
        const le::DesignId design_id = design_ids[static_cast<size_t>(index)];
        const le::LayoutId layout_id = handle->root.get_design_layout(design_id);
        handle->set_current_layout(layout_id);
        handle->set_current_abstract(le::AbstractId{});
        handle->current_layout_id = layout_id;
        handle->current_abstract_id = le::AbstractId{};
        return 0;
    }

    int le_set_current_design_layout_by_id(LeHandle *handle, LeDesignId design_id)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        const le::DesignId id = from_c(design_id);
        if (!handle->root.get_design(id))
            return 1;

        // See le_set_current_design_layout's own comment above.
        const le::LayoutId layout_id = handle->root.get_design_layout(id);
        handle->set_current_layout(layout_id);
        handle->set_current_abstract(le::AbstractId{});
        handle->current_layout_id = layout_id;
        handle->current_abstract_id = le::AbstractId{};
        return 0;
    }

    int32_t le_hierarchy_depth(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->hierarchy_depth());
    }

    void le_set_hierarchy_depth(LeHandle *handle, int32_t depth)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_hierarchy_depth(depth);
    }

    int32_t le_layer_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        // Double-checked - see view_layers_already_current's own comment.
        {
            std::shared_lock<std::shared_mutex> read_lock(handle->mutex_);
            if (view_layers_already_current(handle))
                return static_cast<int32_t>(handle->view_layers.rows().size());
        }
        HandleWriteLock write_lock(handle);
        ensure_view_layers_current(handle);
        return static_cast<int32_t>(handle->view_layers.rows().size());
    }

    LeLayerRow le_layer_at(LeHandle *handle, int32_t row_index)
    {
        const LeLayerRow invalid{.name = nullptr, .color_r = 0, .color_g = 0, .color_b = 0, .has_physical_layer = 0};
        if (!handle || row_index < 0)
            return invalid;

        // Shared by both the shared_lock fast path and the unique_lock
        // slow path below (view_layers_already_current's own comment) -
        // avoids the awkwardness of unlocking a shared_lock and
        // re-locking it after a rebuild just to reuse this same
        // read-only logic; each path just calls this under whichever
        // lock it's already holding.
        auto row_at = [&](const le::ViewLayerSet &view_layers) -> LeLayerRow
        {
            const auto &rows = view_layers.rows();
            if (static_cast<size_t>(row_index) >= rows.size())
                return invalid;
            const le::ViewLayerRow &row = rows[static_cast<size_t>(row_index)];
            const le::ViewLayerData *first_column = row.columns.empty() ? nullptr : view_layers.get(row.columns.front().id);
            return LeLayerRow{
                .name = row.name.c_str(),
                .color_r = first_column ? first_column->style.outline_color.r : uint8_t{0},
                .color_g = first_column ? first_column->style.outline_color.g : uint8_t{0},
                .color_b = first_column ? first_column->style.outline_color.b : uint8_t{0},
                .has_physical_layer = (first_column && first_column->layer.valid()) ? 1 : 0,
            };
        };

        {
            std::shared_lock<std::shared_mutex> read_lock(handle->mutex_);
            if (view_layers_already_current(handle))
                return row_at(handle->view_layers);
        }
        HandleWriteLock write_lock(handle);
        ensure_view_layers_current(handle);
        return row_at(handle->view_layers);
    }

    int32_t le_purpose_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        // Double-checked - see view_layers_already_current's own comment.
        {
            std::shared_lock<std::shared_mutex> read_lock(handle->mutex_);
            if (view_layers_already_current(handle))
                return static_cast<int32_t>(handle->view_layers.purposes().size());
        }
        HandleWriteLock write_lock(handle);
        ensure_view_layers_current(handle);
        return static_cast<int32_t>(handle->view_layers.purposes().size());
    }

    int32_t le_purpose_at(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return -1;

        // Shared by both lock paths - le_layer_at's own comment above.
        auto purpose_at = [&](const le::ViewLayerSet &view_layers) -> int32_t
        {
            const auto purposes = view_layers.purposes();
            if (static_cast<size_t>(index) >= purposes.size())
                return -1;
            return static_cast<int32_t>(purposes[static_cast<size_t>(index)]);
        };

        {
            std::shared_lock<std::shared_mutex> read_lock(handle->mutex_);
            if (view_layers_already_current(handle))
                return purpose_at(handle->view_layers);
        }
        HandleWriteLock write_lock(handle);
        ensure_view_layers_current(handle);
        return purpose_at(handle->view_layers);
    }

    bool le_is_layer_name_visible(LeHandle *handle, const char *layer_name)
    {
        if (!handle || !layer_name)
            return true;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->is_layer_name_visible(layer_name);
    }

    void le_set_layer_name_visible(LeHandle *handle, const char *layer_name, bool visible)
    {
        if (!handle || !layer_name)
            return;
        HandleWriteLock lock(handle);
        handle->set_layer_name_visible(layer_name, visible);
    }

    bool le_is_antialiasing_enabled(LeHandle *handle)
    {
        if (!handle)
            return false;
        HandleWriteLock lock(handle);
        return handle->antialiasing_enabled();
    }

    void le_set_antialiasing_enabled(LeHandle *handle, bool enabled)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_antialiasing_enabled(enabled);
    }

    int32_t le_max_concurrency(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return handle->max_concurrency_;
    }

    void le_set_max_concurrency(LeHandle *handle, int32_t max_concurrency)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        const int32_t clamped = std::max(2, max_concurrency);
        if (clamped == handle->max_concurrency_)
            return;
        handle->max_concurrency_ = clamped;
        // No setter on global_control itself - destroy the old limit
        // before constructing the new one (a live global_control's own
        // limit is the min across every currently-constructed instance,
        // so leaving the old one alive while constructing a new one could
        // never raise the effective limit, only lower it).
        handle->concurrency_control_.reset();
        handle->concurrency_control_.emplace(oneapi::tbb::global_control::max_allowed_parallelism, static_cast<size_t>(clamped));
    }

    int32_t le_is_purpose_visible(LeHandle *handle, int32_t purpose)
    {
        if (!handle)
            return 1;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->is_purpose_visible(static_cast<le::ViewLayerPurpose>(purpose)) ? 1 : 0;
    }

    void le_set_purpose_visible(LeHandle *handle, int32_t purpose, int32_t visible)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_purpose_visible(static_cast<le::ViewLayerPurpose>(purpose), visible != 0);
    }

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
        HandleWriteLock lock(handle);
        return static_cast<int32_t>(handle->rulers().size());
    }

    int32_t le_ruler_point_count(LeHandle *handle, int32_t ruler_index)
    {
        if (!handle || ruler_index < 0)
            return 0;
        HandleWriteLock lock(handle);
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
        HandleWriteLock lock(handle);
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

    // --- Editing / undo-redo (UPDATES.md item 21) ---

    void le_begin_command(LeHandle *handle, const char *label)
    {
        if (!handle || !label)
            return;
        HandleWriteLock lock(handle);
        handle->command_history.begin(label);
    }

    void le_end_command(LeHandle *handle, int32_t succeeded)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->command_history.end(succeeded != 0);
    }

    int32_t le_undo(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        const bool undone = handle->command_history.undo(handle->root);
        if (undone)
            refresh_armed_move_geometry_unlocked(handle);
        return undone ? 1 : 0;
    }

    int32_t le_redo(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        const bool redone = handle->command_history.redo(handle->root);
        if (redone)
            refresh_armed_move_geometry_unlocked(handle);
        return redone ? 1 : 0;
    }

    int32_t le_can_undo(LeHandle *handle)
    {
        return handle && handle->command_history.can_undo() ? 1 : 0;
    }

    int32_t le_can_redo(LeHandle *handle)
    {
        return handle && handle->command_history.can_redo() ? 1 : 0;
    }

    int32_t le_command_history_count(LeHandle *handle)
    {
        return handle ? static_cast<int32_t>(handle->command_history.recall_count()) : 0;
    }

    const char *le_command_history_at(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0 || static_cast<size_t>(index) >= handle->command_history.recall_count())
            return nullptr;
        return handle->command_history.recall_at(static_cast<size_t>(index)).c_str();
    }

    void le_select_all(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        select_all_unlocked(handle);
    }

    void le_deselect_all(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->clear_selection();
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

    int32_t le_selected_placement_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(selected_placements_unlocked(handle).size());
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

    void le_set_shape_snap_mode(LeHandle *handle, int32_t kind, int32_t mode)
    {
        if (!handle || kind < LE_PIECE_KIND_RECT || kind > LE_PIECE_KIND_PATH || mode < LE_SHAPE_SNAP_NONE || mode > LE_SHAPE_SNAP_TRACKS)
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
        if (!handle || kind < LE_PIECE_KIND_RECT || kind > LE_PIECE_KIND_PATH)
            return LE_SHAPE_SNAP_USER_GRID;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->shape_snap_mode(static_cast<le::PieceKind>(kind)));
    }

    int32_t le_is_shape_snap_mode_available(LeHandle *handle, int32_t kind, int32_t mode)
    {
        if (!handle || kind < LE_PIECE_KIND_RECT || kind > LE_PIECE_KIND_PATH || mode < LE_SHAPE_SNAP_NONE || mode > LE_SHAPE_SNAP_TRACKS)
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
                    piece && piece->piece_kind == le::PieceKind::PATH)
                    if (const le::ShapeData *data = handle->root.get_shape(piece->shape_id);
                        data && !le::layer_track_grids(handle->root, handle->current_layout(), data->layer).empty())
                        return 1;
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
            if (const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected))
                mask |= 1 << static_cast<int32_t>(piece->piece_kind);
        return mask;
    }

    int32_t le_is_layer_name_selectable(LeHandle *handle, const char *layer_name)
    {
        if (!handle || !layer_name)
            return 1;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->is_layer_name_selectable(layer_name) ? 1 : 0;
    }

    void le_set_layer_name_selectable(LeHandle *handle, const char *layer_name, int32_t selectable)
    {
        if (!handle || !layer_name)
            return;
        HandleWriteLock lock(handle);
        handle->set_layer_name_selectable(layer_name, selectable != 0);
    }

    int32_t le_is_purpose_selectable(LeHandle *handle, int32_t purpose)
    {
        if (!handle)
            return 1;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->is_purpose_selectable(static_cast<le::ViewLayerPurpose>(purpose)) ? 1 : 0;
    }

    void le_set_purpose_selectable(LeHandle *handle, int32_t purpose, int32_t selectable)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_purpose_selectable(static_cast<le::ViewLayerPurpose>(purpose), selectable != 0);
    }

    void le_zoom(LeHandle *handle, double factor, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        zoom_unlocked(handle, factor, x, y);
    }

    void le_pan(LeHandle *handle, double x_factor, double y_factor)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        pan_unlocked(handle, x_factor, y_factor);
    }

    void le_set_viewport_size(LeHandle *handle, int32_t width_px, int32_t height_px)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_viewport_size(width_px, height_px);
    }

    void le_fit_scene(LeHandle *handle, int32_t padding_px)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        fit_scene_unlocked(handle, padding_px);
    }

    void le_fit_rect(LeHandle *handle, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um, int32_t padding_px)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        const double dbu_per_um = display_dbu_per_um(handle->root);
        handle->fit_to_content(le::Rect{.ll = {.x = to_dbu(ll_x_um, dbu_per_um), .y = to_dbu(ll_y_um, dbu_per_um)}, .ur = {.x = to_dbu(ur_x_um, dbu_per_um), .y = to_dbu(ur_y_um, dbu_per_um)}}, padding_px);
    }

    int64_t le_minor_grid_spacing(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return handle->minor_grid_spacing();
    }

    void le_set_minor_grid_spacing(LeHandle *handle, int64_t dbu)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_minor_grid_spacing(dbu);
    }

    int64_t le_major_grid_spacing(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return handle->major_grid_spacing();
    }

    void le_set_major_grid_spacing(LeHandle *handle, int64_t dbu)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_major_grid_spacing(dbu);
    }

    double le_ruler_label_size(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return handle->ruler_label_size_px();
    }

    void le_set_ruler_label_size(LeHandle *handle, double px)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_ruler_label_size_px(px);
    }

    void le_set_mouse_position(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);

        handle->set_mouse_position(x, y);

        // Hover is a Select-mode-only affordance (LeHandle::set_mode's
        // own comment - it signals "this is a selection candidate",
        // meaningless while placing ruler points or editing) and, for
        // now, an Abstract-view-only one - Layout-view own-shape
        // hit-testing (Row/Region/Blockage/Route/PhysicalPort) is a
        // separate, not-yet-built gap (whole-placement hover has no
        // HoverTarget path at all - a Placement never enters
        // SelectionRef, see its own comment). Reuses the exact same
        // hit_test_abstract_point select_in_abstract_view_unlocked's own
        // click path already calls - hover is just a point hit-test with
        // no click/selection side effect.
        if (handle->mode() != LeHandle::Mode::SELECT || handle->current_layout().valid())
        {
            handle->clear_hover();
            return;
        }

        const le::AbstractId abstract_id = handle->current_abstract();
        const le::Point dbu_point = handle->pixel_to_dbu(x, y);
        // Both axes, not selectability alone - select_all_unlocked's own
        // three-condition check (above) already establishes this as the
        // real convention: a hidden ViewLayer (visibility off) must not
        // be click/drag/hover-selectable even when it's still marked
        // selectable=true (the default for most purposes) - selectable
        // means "eligible to be selected when visible", not "selectable
        // regardless of visibility". is_view_layer_visible already ANDs
        // its own two axes (layer-name/purpose) the same way
        // is_view_layer_selectable does.
        const auto is_selectable = [handle](const std::string &layer_name, le::ViewLayerPurpose purpose)
        { return handle->is_view_layer_visible(layer_name, purpose) && handle->is_view_layer_selectable(layer_name, purpose); };

        const auto hit = le::hit_test_abstract_point(handle->root, handle->view_layers, abstract_id, dbu_point, handle->scale(), is_selectable);
        if (!hit)
        {
            handle->clear_hover();
            return;
        }

        if (const auto origin = shape_selection_ref(handle->root, hit->shape_id))
            handle->set_hover(LeHandle::HoverTarget{.origin = *origin, .outline = hit->outline, .shape_id = hit->shape_id});
        else
            handle->clear_hover();
    }

    void le_clear_mouse_position(LeHandle *handle)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->clear_mouse_position();
        handle->clear_hover();
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

        return LeSnappedMousePosition{
            .x_um = static_cast<double>(snapped->x) / technology->database_units_microns,
            .y_um = static_cast<double>(snapped->y) / technology->database_units_microns,
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
        // to the bare/Ctrl behavior. Regression: pressing 's'/'e'/'r' to
        // switch modes used to fire even with Ctrl or Shift held (e.g.
        // Ctrl-S), stealing the keystroke from whatever the modifier was
        // actually meant for. LE_KEY_ZOOM is the one exception that's
        // deliberately exhaustive instead (Ctrl and Shift each already
        // select a real, distinct action of their own - see below) and
        // LE_KEY_FINISH_RULER, which is deliberately *not* modifier-
        // gated at all (see its own case).
        switch (key_code)
        {
        case LE_KEY_ZOOM:
        {
            // UPDATES.md item 21 - Ctrl-Z/Ctrl-Shift-Z undo/redo, branched
            // here rather than a separate key code (see LE_KEY_ZOOM's own
            // api.hpp doc comment for why). Falls through to the ordinary
            // zoom action when Ctrl isn't held. Every one of the four
            // Ctrl/Shift combinations already has a real, distinct
            // meaning (undo/redo, zoom in/out), unlike every other action
            // key below - nothing to suppress here.
            if (ctrl)
            {
                if (shift)
                    handle->command_history.redo(handle->root);
                else
                    handle->command_history.undo(handle->root);
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
        case LE_KEY_PAN_LEFT:
            if (!ctrl && !shift)
                pan_unlocked(handle, -kKeyPanFactor, 0.0);
            break;
        case LE_KEY_PAN_RIGHT:
            if (!ctrl && !shift)
                pan_unlocked(handle, kKeyPanFactor, 0.0);
            break;
        case LE_KEY_PAN_UP:
            if (!ctrl && !shift)
                pan_unlocked(handle, 0.0, kKeyPanFactor);
            break;
        case LE_KEY_PAN_DOWN:
            if (!ctrl && !shift)
                pan_unlocked(handle, 0.0, -kKeyPanFactor);
            break;
        case LE_KEY_SELECT_ALL:
            // UPDATES.md item 21 - Select-mode-only, in addition to the
            // existing Ctrl-held gate (switch back to Select mode to
            // change the selection from Edit/Ruler mode). Shift has no
            // meaning here - Ctrl-Shift-A is a no-op, not "same as
            // Ctrl-A".
            if (handle->mode() == LeHandle::Mode::SELECT && ctrl && !shift)
                select_all_unlocked(handle);
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
            // UPDATES.md 9.7 - the same physical 1-9 keys address the
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
            // UPDATES.md item 21 - same Select-mode-only gate and
            // Shift-suppresses shape as LE_KEY_SELECT_ALL above.
            if (handle->mode() == LeHandle::Mode::SELECT && ctrl && !shift)
                handle->clear_selection();
            break;
        case LE_KEY_MOVE:
            if (ctrl && !shift)
                arm_move_unlocked(handle);
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
            break;
        case LE_KEY_FINISH_RULER:
            // Deliberately *not* modifier-gated, unlike every other bare
            // key above: Escape is a pure cancel/finish gesture, and a
            // real ruler-drawing sequence routinely ends with Shift
            // (free-form) still physically held right up to the Escape
            // press - suppressing it here would make "finish the ruler"
            // unreliable in exactly the workflow that uses Shift most.
            handle->finish_active_ruler();
            handle->end_move(); // UPDATES.md item 21 - Escape also cancels an in-progress move
            handle->end_resize(); // ...and disarms Resize (NEW_FEATURES_SEPT_2026.md item 3)
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
        // NEW_FEATURES_SEPT_2026.md item 3 - with Resize armed, a press on
        // a selected piece's edge/segment grabs it instead of starting a
        // rubber band.
        if (handle->mode() == LeHandle::Mode::EDIT && handle->resize().armed && try_begin_resize_grab_unlocked(handle, x, y))
        {
            handle->begin_drag(x, y, LeHandle::DragKind::RESIZE);
            return;
        }
        handle->begin_drag(x, y);
    }

    void le_zoom_drag_down(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->begin_drag(x, y, LeHandle::DragKind::ZOOM);
    }

    // le_mouse_up's Select-mode, Abstract-view branch - exactly the
    // click/drag hit-testing logic that lived directly in le_mouse_up
    // before E1 (BUGS_AND_ENHANCEMENTS.md) split it out to make room for
    // select_in_layout_view_unlocked below, which needs an entirely
    // different hit-test (Layout content has no per-piece geometry
    // addressable the same way an Abstract's Terminal/Obstruction Shapes
    // are). Called with handle->mutex_ already held, `x`/`y` the same
    // release-point le_mouse_up itself received, `is_click` its own
    // click-vs-drag threshold result.
    //
    // Ported from the pre-restart pipelines.old/hit_test.hpp's own
    // hit_test_point/hit_test_rect (this used to go through
    // abstract_shape_pipeline.run() + those functions, both deleted with
    // pipelines.old) - core/placement_geometry.hpp's own
    // hit_test_abstract_point/_rect are the direct replacement, working
    // against Root's raw ShapeData directly rather than a pipeline's own
    // rendered output (PIPELINE_REFACTOR.md's Hot tier has no per-shape
    // rendered-output cache the way the old module did, and doesn't need
    // one just for this - a click/drag is a rare, one-off query, not a
    // per-frame cost).
    void select_in_abstract_view_unlocked(LeHandle *handle, int32_t x, int32_t y, bool is_click)
    {
        const le::AbstractId abstract_id = handle->current_abstract();
        // Both visible AND selectable - see le_set_mouse_position's own
        // comment on this same predicate shape.
        const auto is_selectable = [handle](const std::string &layer_name, le::ViewLayerPurpose purpose)
        { return handle->is_view_layer_visible(layer_name, purpose) && handle->is_view_layer_selectable(layer_name, purpose); };

        if (is_click)
        {
            const le::Point dbu_point = handle->pixel_to_dbu(x, y);
            if (const auto hit = le::hit_test_abstract_point(handle->root, handle->view_layers, abstract_id, dbu_point, handle->scale(), is_selectable))
                handle->select(hit->shape_id, hit->piece_kind, hit->piece_index);
        }
        else
        {
            const le::Point start = handle->pixel_to_dbu(handle->drag_start_x_px(), handle->drag_start_y_px());
            const le::Point end = handle->pixel_to_dbu(x, y);
            const le::Rect drag_rect{
                .ll = le::Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                .ur = le::Point{std::max(start.x, end.x), std::max(start.y, end.y)},
            };

            for (const le::AbstractHitPiece &hit : le::hit_test_abstract_rect(handle->root, handle->view_layers, abstract_id, drag_rect, handle->scale(), is_selectable))
                handle->select(hit.shape_id, hit.piece_kind, hit.piece_index);
        }
    }

    // le_mouse_up's Select-mode, Layout-view branch (E1,
    // BUGS_AND_ENHANCEMENTS.md) - top-level Layout content only, never
    // recursing into a Placement's own reference_design (matches this
    // codebase's own existing, documented "whole-placement only"
    // deferral - see InstanceRenderer's own class comment). A click
    // checks own_shapes (hit_test_point - Blockage/Route/PhysicalPort/
    // Row/Region) *before* Placement (hit_test_placements_point,
    // src/core/placement_geometry.hpp) - BUGS_AND_ENHANCEMENTS.md B2:
    // hit_test_placements_point is a pure bounding-box test, not real
    // per-pixel/geometry hit-testing, so a click that lands within a
    // placement's own bbox but over a point where its own painted
    // content is actually transparent there (leaving an own_shape like a
    // Route visible underneath - exactly what mouse-hover's own
    // hit_test_point-only path, le_set_mouse_position, already shows)
    // used to still claim the click for the Placement regardless,
    // disagreeing with whatever was actually highlighted. Falling
    // through to the Placement bbox test only when own_shapes has no hit
    // at all fixes that while keeping every other case unchanged (a
    // click genuinely inside a placement's own content, away from any
    // own_shape, still finds nothing via hit_test_point and falls
    // through to it exactly as before). Blockage/Route/PhysicalPort
    // share the exact same ShapeId+piece re-resolution as the Abstract
    // branch above (they have real backing Shapes); Row/Region have no
    // Shape at all, so a hit with `origin` set but `shape_id` unset gets
    // its own small fork straight into scene.select(RowId)/
    // scene.select(RegionId) - there's no separate Root-owned geometry
    // to re-validate a piece against, the synthesized rect *is* the
    // geometry. A drag (the `else` branch below) has no such ordering
    // concern - it unions both hit_test_placements_rect and
    // hit_test_rect's own results independently rather than picking one
    // topmost target, so the same set of ids ends up selected regardless
    // of which is checked first.
    // own_shape hit-testing below currently covers Route/PhysicalPort
    // only (hit_test_layout_point/_rect, core/placement_geometry.hpp) -
    // Blockage/Row/Region remain a separate, still-deferred gap (Row/
    // Region in particular have no backing Shape at all, so they need
    // their own bare-id hit-test, not an extension of this one). A click
    // checks own_shapes *before* Placement (BUGS_AND_ENHANCEMENTS.md B2:
    // hit_test_placements_point is a pure bounding-box test, not real
    // per-pixel/geometry hit-testing, so a click that lands within a
    // placement's own bbox but over a point where its own painted
    // content is actually transparent there - leaving a Route/
    // PhysicalPort visible underneath - must still prefer the visible
    // own_shape, not the placement bbox merely covering that point).
    // Falling through to the Placement bbox test only when own_shapes
    // has no hit at all keeps every other case unchanged (a click
    // genuinely inside a placement's own content, away from any
    // own_shape, still finds nothing via hit_test_layout_point and falls
    // through to it exactly as before). Route/PhysicalPort have real
    // backing Shapes, so they ride the exact same ShapeId+piece
    // re-resolution the Abstract branch above already uses. A drag (the
    // `else` branch below) has no such ordering concern - it unions both
    // hit_test_placements_rect and hit_test_layout_rect's own results
    // independently rather than picking one topmost target, so the same
    // set of ids ends up selected regardless of which is checked first.
    void select_in_layout_view_unlocked(LeHandle *handle, int32_t x, int32_t y, bool is_click)
    {
        const le::LayoutId layout_id = handle->current_layout();
        const int remaining_depth = std::max(0, handle->hierarchy_depth() - 1);
        // Both visible AND selectable - see le_set_mouse_position's own
        // comment on this same predicate shape.
        const auto is_selectable = [handle](const std::string &layer_name, le::ViewLayerPurpose purpose)
        { return handle->is_view_layer_visible(layer_name, purpose) && handle->is_view_layer_selectable(layer_name, purpose); };

        if (is_click)
        {
            const le::Point dbu_point = handle->pixel_to_dbu(x, y);
            if (const auto hit = le::hit_test_layout_point(handle->root, handle->view_layers, layout_id, dbu_point, handle->scale(), is_selectable))
                handle->select(hit->shape_id, hit->piece_kind, hit->piece_index);
            else if (const auto placement_id = le::hit_test_placements_point(handle->root, layout_id, remaining_depth, dbu_point))
                handle->select(*placement_id);
        }
        else
        {
            const le::Point start = handle->pixel_to_dbu(handle->drag_start_x_px(), handle->drag_start_y_px());
            const le::Point end = handle->pixel_to_dbu(x, y);
            const le::Rect drag_rect{
                .ll = le::Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                .ur = le::Point{std::max(start.x, end.x), std::max(start.y, end.y)},
            };

            for (le::PlacementId placement_id : le::hit_test_placements_rect(handle->root, layout_id, remaining_depth, drag_rect))
                handle->select(placement_id);

            for (const le::AbstractHitPiece &hit : le::hit_test_layout_rect(handle->root, handle->view_layers, layout_id, drag_rect, handle->scale(), is_selectable))
                handle->select(hit.shape_id, hit.piece_kind, hit.piece_index);
        }
    }

    void le_mouse_up(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle || !handle->is_dragging())
            return;
        HandleWriteLock lock(handle);

        const int32_t dx = x - handle->drag_start_x_px();
        const int32_t dy = y - handle->drag_start_y_px();
        const bool is_click = dx * dx + dy * dy < kClickDragThresholdPx * kClickDragThresholdPx;

        if (handle->drag_kind() == LeHandle::DragKind::RESIZE)
        {
            // A click-sized release changes nothing; a real drag commits.
            if (!is_click && handle->resize().grab)
                commit_resize_unlocked(handle, x, y);
            handle->end_resize_grab();
            handle->end_drag();
            return;
        }

        if (handle->drag_kind() == LeHandle::DragKind::ZOOM)
        {
            // Rectangle-zoom (UPDATES.md 9.3) - purely navigational,
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

        // UPDATES.md item 11 - only Select mode changes the selection; in
        // Edit mode a click/drag is left for editing the existing
        // selection (behavior TBD, a later item), so this whole block -
        // including the pipeline run it only needs for hit-testing - is
        // skipped. end_drag() below stays unconditional so drag state
        // always resets regardless of mode.
        if (handle->mode() == LeHandle::Mode::SELECT)
        {
            const bool shift = handle->is_key_held(LE_KEY_SHIFT);

            if (!shift)
                handle->clear_selection();

            // E1 (BUGS_AND_ENHANCEMENTS.md) - this used to unconditionally
            // hit-test the Abstract path even in Layout view (a real bug:
            // clicking in Layout view hit whatever stale/irrelevant
            // Abstract content happened to exist, never the Layout's own).
            if (handle->current_layout().valid())
                select_in_layout_view_unlocked(handle, x, y, is_click);
            else
                select_in_abstract_view_unlocked(handle, x, y, is_click);
        }
        else if (handle->mode() == LeHandle::Mode::RULER)
        {
            // UPDATES.md item 13 - only a click (not a drag) commits a
            // ruler point; a drag in Ruler mode does nothing beyond
            // ending the gesture below.
            if (is_click)
                handle->add_ruler_point(handle->is_key_held(LE_KEY_SHIFT));
        }
        else if (handle->mode() == LeHandle::Mode::EDIT)
        {
            // UPDATES.md item 21 - only a click (not a drag) sets the
            // move's anchor / commits it; a drag in Edit mode does
            // nothing beyond ending the gesture below, same as Ruler
            // mode's own click-only handling just above. A no-op if
            // Move isn't armed (see move_click_unlocked).
            if (is_click)
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

    int32_t le_selection_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        return static_cast<int32_t>(handle->selection().size());
    }

    int64_t le_selection_version(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        return static_cast<int64_t>(handle->selection_version());
    }

    LeObjectRef le_object_invalid_ref(void)
    {
        return invalid_object_ref();
    }

    int32_t le_object_property_count(LeHandle *handle, LeObjectRef ref)
    {
        if (!handle)
            return 0;

        // Double-checked, same shape as view_layers_already_current's
        // own callers - "already cached for this exact ref" under a
        // shared_lock first (the common case, since every real caller
        // calls le_object_property_count(ref) once and then loops
        // le_object_property_at(ref, i) immediately after for the same
        // ref - api_test.cpp/property_viewer.cpp confirmed, no caller
        // anywhere calls _count twice in a row for the same ref
        // expecting a forced re-read), only escalating to a unique_lock
        // to actually rebuild when ref is genuinely different from
        // what's cached. Previously rebuilt unconditionally on every
        // call regardless of ref, unlike le_object_property_at's own
        // same_object_ref check just below - aligned with that existing
        // behavior here (a strictly narrower cache-hit condition than
        // "always rebuild", so this can only return calls that were
        // already stale-free) rather than left needing an unconditional
        // unique_lock the same "looks read-only, secretly always
        // writes" way it did before.
        {
            std::shared_lock<std::shared_mutex> read_lock(handle->mutex_);
            if (same_object_ref(handle->cached_object_property_ref, ref))
                return static_cast<int32_t>(handle->cached_object_properties.size());
        }
        HandleWriteLock write_lock(handle);
        handle->cached_object_properties = build_object_properties(handle->root, ref);
        handle->cached_object_property_ref = ref;
        return static_cast<int32_t>(handle->cached_object_properties.size());
    }

    LeProperty le_object_property_at(LeHandle *handle, LeObjectRef ref, int32_t index)
    {
        const LeProperty invalid{.name = nullptr, .type = LE_PROPERTY_TYPE_STRING, .string_value = nullptr, .int_value = 0, .double_value = 0.0};
        if (!handle || index < 0)
            return invalid;

        // Double-checked - le_object_property_count's own comment above.
        {
            std::shared_lock<std::shared_mutex> read_lock(handle->mutex_);
            if (same_object_ref(handle->cached_object_property_ref, ref))
            {
                if (static_cast<size_t>(index) >= handle->cached_object_properties.size())
                    return invalid;
                return to_c(handle->cached_object_properties[static_cast<size_t>(index)]);
            }
        }
        HandleWriteLock write_lock(handle);
        if (!same_object_ref(handle->cached_object_property_ref, ref))
        {
            handle->cached_object_properties = build_object_properties(handle->root, ref);
            handle->cached_object_property_ref = ref;
        }

        if (static_cast<size_t>(index) >= handle->cached_object_properties.size())
            return invalid;

        return to_c(handle->cached_object_properties[static_cast<size_t>(index)]);
    }

    int32_t le_object_property_cache_current(LeHandle *handle, LeObjectRef ref)
    {
        if (!handle)
            return 0;
        // Always shared_lock, never escalates - le_object_property_count's
        // own doc comment (api.hpp) explains why this is safe to call
        // unconditionally, including while a render is in progress.
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return same_object_ref(handle->cached_object_property_ref, ref) ? 1 : 0;
    }

    LeObjectRef le_object_parent(LeHandle *handle, LeObjectRef ref)
    {
        if (!handle)
            return invalid_object_ref();
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        return object_ref_parent(handle->root, ref);
    }

    LeObjectRef le_selected_object_ref(LeHandle *handle, int32_t selection_index)
    {
        if (!handle || selection_index < 0)
            return invalid_object_ref();
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const std::vector<LeHandle::SelectedObject> &selection = handle->selection();
        if (static_cast<size_t>(selection_index) >= selection.size())
            return invalid_object_ref();

        // E1 (BUGS_AND_ENHANCEMENTS.md) - dispatches every SelectedObject
        // alternative to its own LeObjectKind; ShapePiece (Terminal/
        // Obstruction/Blockage/Route/PhysicalPort) always resolves to
        // LE_OBJECT_KIND_SHAPE, unchanged from before this variant grew -
        // a Property Viewer wanting the owning Blockage/Route/PhysicalPort
        // instead walks up via le_object_parent (see object_ref_parent's
        // own new Shape->blockage/route/physical_port_segment hops).
        return std::visit([](const auto &s) -> LeObjectRef
                          {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, LeHandle::ShapePiece>)
                return ref_from_id(LE_OBJECT_KIND_SHAPE, s.shape_id);
            else if constexpr (std::is_same_v<T, le::RowId>)
                return ref_from_id(LE_OBJECT_KIND_ROW, s);
            else if constexpr (std::is_same_v<T, le::PlacementId>)
                return ref_from_id(LE_OBJECT_KIND_PLACEMENT, s);
            else if constexpr (std::is_same_v<T, le::RegionId>)
                return ref_from_id(LE_OBJECT_KIND_REGION, s); }, selection[static_cast<size_t>(selection_index)]);
    }

    int32_t le_select_object_ref(LeHandle *handle, LeObjectRef ref)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        // Shared by SHAPE/ROUTE/PHYSICAL_PORT below - selects every rect/
        // polygon/path entry of one Shape, since a bare ShapeId can't
        // express "just this one piece" (see this function's own
        // api.hpp doc comment). Returns whether it actually selected
        // anything, so a caller resolving several child Shapes (Route/
        // PhysicalPort) can tell "no geometry anywhere" apart from
        // "some/all of them had real geometry" with one shared check.
        const auto select_all_pieces_of = [&](le::ShapeId shape_id) -> bool
        {
            const le::ShapeData *shape = handle->root.get_shape(shape_id);
            if (!shape)
                return false;
            for (size_t i = 0; i < shape->rects.size(); i++)
                handle->select(shape_id, le::PieceKind::RECT, i);
            for (size_t i = 0; i < shape->polygons.size(); i++)
                handle->select(shape_id, le::PieceKind::POLYGON, i);
            for (size_t i = 0; i < shape->paths.size(); i++)
                handle->select(shape_id, le::PieceKind::PATH, i);
            return !shape->rects.empty() || !shape->polygons.empty() || !shape->paths.empty();
        };

        switch (ref.kind)
        {
        case LE_OBJECT_KIND_SHAPE:
        {
            const le::ShapeId shape_id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_shape(shape_id))
            {
                spdlog::error("select: no such Shape");
                return 1;
            }
            if (!select_all_pieces_of(shape_id))
            {
                spdlog::error("select: Shape has no geometry to select");
                return 1;
            }
            return 0;
        }
        case LE_OBJECT_KIND_ROUTE:
        {
            // A Route's own shapes ride the exact same ShapeId+piece
            // selection Terminal/Obstruction/Shape already use above -
            // Route (like Blockage/PhysicalPort) has a real backing
            // Shape per piece of routed geometry, unlike Row/Region.
            const le::RouteId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_route(id))
            {
                spdlog::error("select: no such Route");
                return 1;
            }
            bool any_selected = false;
            for (le::ShapeId shape_id : handle->root.get_route_shapes(id))
                any_selected |= select_all_pieces_of(shape_id);
            if (!any_selected)
            {
                spdlog::error("select: Route has no geometry to select");
                return 1;
            }
            return 0;
        }
        case LE_OBJECT_KIND_PHYSICAL_PORT:
        {
            const le::PhysicalPortId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_physical_port(id))
            {
                spdlog::error("select: no such PhysicalPort");
                return 1;
            }
            bool any_selected = false;
            for (le::PhysicalPortSegmentId segment_id : handle->root.get_physical_port_segments(id))
                for (le::ShapeId shape_id : handle->root.get_physical_port_segment_shapes(segment_id))
                    any_selected |= select_all_pieces_of(shape_id);
            if (!any_selected)
            {
                spdlog::error("select: PhysicalPort has no geometry to select");
                return 1;
            }
            return 0;
        }
        case LE_OBJECT_KIND_ROW:
        {
            const le::RowId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_row(id))
            {
                spdlog::error("select: no such Row");
                return 1;
            }
            handle->select(id);
            return 0;
        }
        case LE_OBJECT_KIND_PLACEMENT:
        {
            const le::PlacementId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_placement(id))
            {
                spdlog::error("select: no such Placement");
                return 1;
            }
            handle->select(id);
            return 0;
        }
        case LE_OBJECT_KIND_REGION:
        {
            const le::RegionId id{.index = ref.index, .generation = ref.generation};
            if (!handle->root.get_region(id))
            {
                spdlog::error("select: no such Region");
                return 1;
            }
            handle->select(id);
            return 0;
        }
        default:
            spdlog::error("select: unsupported object kind (only Shape/Route/PhysicalPort/Row/Placement/Region can be selected)");
            return 1;
        }
    }

    LeTerminalId le_terminal_by_name(LeHandle *handle, const char *name)
    {
        const LeTerminalId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        // handle->current_abstract_id (le_set_current_abstract/
        // le_current_abstract's own generated field), not
        // handle->current_abstract() - the latter is a genuinely
        // separate "GUI current view" tracker. le_set_current_design_abstract/
        // le_set_current_design_abstract_by_id move both together (selecting a
        // Design should mean the same thing whether it came from a
        // Dart-driven GUI or a TCL script's open_design), but they can
        // still diverge: a script that builds an Abstract from scratch
        // and calls set_current_abstract directly (no Design to
        // open_design into at all) only ever touches
        // handle->current_abstract_id, never handle->current_abstract().
        // Terminal is the one
        // class with a hand-written friendly-id resolver
        // (unique_per_parent means it can't use the generated by-name
        // lookup pair - see Field.unique_per_parent's own docstring), so
        // it's the one place reading the wrong one of the two was easy
        // to get wrong: reading handle->current_abstract() here
        // would leave resolve_terminal_id unable to find any Terminal a
        // from-scratch script just created, even though get_terminals
        // (same "current" concept, already reading current_abstract_id)
        // already sees it correctly.
        for (const le::TerminalId id : handle->root.get_abstract_terminals(handle->current_abstract_id))
        {
            const le::TerminalData *terminal = handle->root.get_terminal(id);
            if (terminal && terminal->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_terminal_name(LeHandle *handle, LeTerminalId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::TerminalData *terminal = handle->root.get_terminal(from_c(id));
        return terminal ? terminal->name.c_str() : nullptr;
    }

    LeRowId le_row_by_name(LeHandle *handle, const char *name)
    {
        const LeRowId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::RowId id : handle->root.get_layout_rows(handle->current_layout_id))
        {
            const le::RowData *row = handle->root.get_row(id);
            if (row && row->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_row_name(LeHandle *handle, LeRowId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::RowData *row = handle->root.get_row(from_c(id));
        return row ? row->name.c_str() : nullptr;
    }

    LePlacementId le_placement_by_name(LeHandle *handle, const char *name)
    {
        const LePlacementId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::PlacementId id : handle->root.get_layout_placements(handle->current_layout_id))
        {
            const le::PlacementData *placement = handle->root.get_placement(id);
            if (placement && placement->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_placement_name(LeHandle *handle, LePlacementId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::PlacementData *placement = handle->root.get_placement(from_c(id));
        return placement ? placement->name.c_str() : nullptr;
    }

    LePhysicalPortId le_physical_port_by_name(LeHandle *handle, const char *name)
    {
        const LePhysicalPortId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::PhysicalPortId id : handle->root.get_layout_physical_ports(handle->current_layout_id))
        {
            const le::PhysicalPortData *physical_port = handle->root.get_physical_port(id);
            if (physical_port && physical_port->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_physical_port_name(LeHandle *handle, LePhysicalPortId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::PhysicalPortData *physical_port = handle->root.get_physical_port(from_c(id));
        return physical_port ? physical_port->name.c_str() : nullptr;
    }

    LeRouteId le_route_by_name(LeHandle *handle, const char *name)
    {
        const LeRouteId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::RouteId id : handle->root.get_layout_routes(handle->current_layout_id))
        {
            const le::RouteData *route = handle->root.get_route(id);
            if (route && route->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_route_name(LeHandle *handle, LeRouteId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::RouteData *route = handle->root.get_route(from_c(id));
        return route ? route->name.c_str() : nullptr;
    }

    LeRegionId le_region_by_name(LeHandle *handle, const char *name)
    {
        const LeRegionId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::RegionId id : handle->root.get_layout_regions(handle->current_layout_id))
        {
            const le::RegionData *region = handle->root.get_region(id);
            if (region && region->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_region_name(LeHandle *handle, LeRegionId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::RegionData *region = handle->root.get_region(from_c(id));
        return region ? region->name.c_str() : nullptr;
    }

    LeLayoutViaId le_layout_via_by_name(LeHandle *handle, const char *name)
    {
        const LeLayoutViaId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::LayoutViaId id : handle->root.get_layout_vias(handle->current_layout_id))
        {
            const le::LayoutViaData *layout_via = handle->root.get_layout_via(id);
            if (layout_via && layout_via->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_layout_via_name(LeHandle *handle, LeLayoutViaId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::LayoutViaData *layout_via = handle->root.get_layout_via(from_c(id));
        return layout_via ? layout_via->name.c_str() : nullptr;
    }

    LePortId le_port_by_name(LeHandle *handle, const char *name)
    {
        const LePortId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::PortId id : handle->root.get_schematic_ports(handle->current_schematic_id))
        {
            const le::PortData *port = handle->root.get_port(id);
            if (port && port->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_port_name(LeHandle *handle, LePortId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::PortData *port = handle->root.get_port(from_c(id));
        return port ? port->name.c_str() : nullptr;
    }

    LeNetId le_net_by_name(LeHandle *handle, const char *name)
    {
        const LeNetId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::NetId id : handle->root.get_schematic_nets(handle->current_schematic_id))
        {
            const le::NetData *net = handle->root.get_net(id);
            if (net && net->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_net_name(LeHandle *handle, LeNetId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::NetData *net = handle->root.get_net(from_c(id));
        return net ? net->name.c_str() : nullptr;
    }

    LeInstanceId le_instance_by_name(LeHandle *handle, const char *name)
    {
        const LeInstanceId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::InstanceId id : handle->root.get_schematic_instances(handle->current_schematic_id))
        {
            const le::InstanceData *instance = handle->root.get_instance(id);
            if (instance && instance->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_instance_name(LeHandle *handle, LeInstanceId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::InstanceData *instance = handle->root.get_instance(from_c(id));
        return instance ? instance->name.c_str() : nullptr;
    }

    LePortBusId le_port_bus_by_name(LeHandle *handle, const char *name)
    {
        const LePortBusId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::PortBusId id : handle->root.get_schematic_port_buses(handle->current_schematic_id))
        {
            const le::PortBusData *bus = handle->root.get_port_bus(id);
            if (bus && bus->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_port_bus_name(LeHandle *handle, LePortBusId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::PortBusData *bus = handle->root.get_port_bus(from_c(id));
        return bus ? bus->name.c_str() : nullptr;
    }

    LeNetBusId le_net_bus_by_name(LeHandle *handle, const char *name)
    {
        const LeNetBusId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        HandleWriteLock lock(handle);

        for (const le::NetBusId id : handle->root.get_schematic_net_buses(handle->current_schematic_id))
        {
            const le::NetBusData *bus = handle->root.get_net_bus(id);
            if (bus && bus->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_net_bus_name(LeHandle *handle, LeNetBusId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::NetBusData *bus = handle->root.get_net_bus(from_c(id));
        return bus ? bus->name.c_str() : nullptr;
    }

    // le_get_instances/nets/ports_by_path: the TCL-facing hierarchical
    // path syntax (LINKING_STRATEGY_RESEARCH.md sections 3/4) -
    // get_instances/get_nets/get_ports' own thin wrapper (le_tcl_procs.tcl)
    // calls one of these instead of the plain flat le_get_<type> whenever
    // a name-expr argument contains "/", reusing the exact same shared
    // resolver `link` itself uses internally (hierarchical_resolver.hpp),
    // now including `**` recursive descent (a genuine differentiator most
    // EDA tool TCL interfaces don't offer - see that section's own note).
    // `-filter` still applies on top, via the same generic evaluator/
    // allowlist-validation helper the generated flat search already uses
    // (parse_and_validate_filter) - a path and a filter aren't mutually
    // exclusive.
    int32_t le_get_instances_by_path(LeHandle *handle, LeSchematicId of_schematic, const char *path,
                                      const char *filter_expression)
    {
        if (!handle || !path)
            return 0;
        HandleWriteLock lock(handle);

        bool ok = true;
        auto expr = parse_and_validate_filter(handle, "le_get_instances_by_path", "Instance", filter_expression, ok);
        if (!ok)
            return -1;

        const le::SchematicId requested = from_c(of_schematic);
        const le::SchematicId root_schematic =
            handle->root.get_schematic(requested) ? requested : handle->current_schematic_id;

        std::vector<le::InstanceId> results =
            le::hierarchy::resolve_instances(handle->root, root_schematic, le::hierarchy::split_path(path));
        if (expr)
        {
            std::vector<le::InstanceId> filtered;
            for (const le::InstanceId id : results)
            {
                const le::InstanceData *data = handle->root.get_instance(id);
                if (data && le::evaluate_filter(*expr, handle->root, id, *data))
                    filtered.push_back(id);
            }
            results = std::move(filtered);
        }

        handle->instance_search_results = std::move(results);
        return static_cast<int32_t>(handle->instance_search_results.size());
    }

    int32_t le_get_nets_by_path(LeHandle *handle, LeSchematicId of_schematic, const char *path,
                                 const char *filter_expression)
    {
        if (!handle || !path)
            return 0;
        HandleWriteLock lock(handle);

        bool ok = true;
        auto expr = parse_and_validate_filter(handle, "le_get_nets_by_path", "Net", filter_expression, ok);
        if (!ok)
            return -1;

        const le::SchematicId requested = from_c(of_schematic);
        const le::SchematicId root_schematic =
            handle->root.get_schematic(requested) ? requested : handle->current_schematic_id;

        std::vector<le::NetId> results =
            le::hierarchy::resolve_nets(handle->root, root_schematic, le::hierarchy::split_path(path));
        if (expr)
        {
            std::vector<le::NetId> filtered;
            for (const le::NetId id : results)
            {
                const le::NetData *data = handle->root.get_net(id);
                if (data && le::evaluate_filter(*expr, handle->root, id, *data))
                    filtered.push_back(id);
            }
            results = std::move(filtered);
        }

        handle->net_search_results = std::move(results);
        return static_cast<int32_t>(handle->net_search_results.size());
    }

    int32_t le_get_ports_by_path(LeHandle *handle, LeSchematicId of_schematic, const char *path,
                                  const char *filter_expression)
    {
        if (!handle || !path)
            return 0;
        HandleWriteLock lock(handle);

        bool ok = true;
        auto expr = parse_and_validate_filter(handle, "le_get_ports_by_path", "Port", filter_expression, ok);
        if (!ok)
            return -1;

        const le::SchematicId requested = from_c(of_schematic);
        const le::SchematicId root_schematic =
            handle->root.get_schematic(requested) ? requested : handle->current_schematic_id;

        std::vector<le::PortId> results =
            le::hierarchy::resolve_ports(handle->root, root_schematic, le::hierarchy::split_path(path));
        if (expr)
        {
            std::vector<le::PortId> filtered;
            for (const le::PortId id : results)
            {
                const le::PortData *data = handle->root.get_port(id);
                if (data && le::evaluate_filter(*expr, handle->root, id, *data))
                    filtered.push_back(id);
            }
            results = std::move(filtered);
        }

        handle->port_search_results = std::move(results);
        return static_cast<int32_t>(handle->port_search_results.size());
    }

    int32_t le_search_terminal(LeHandle *handle, const char *filter_expression)
    {
        if (!handle || !filter_expression)
            return 0;
        HandleWriteLock lock(handle);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            spdlog::error("le_search_terminal: {}", expr.error());
            return -1;
        }

        handle->terminal_search_results = handle->root.search_terminal(
            [&expr](const le::Root &root, le::TerminalId id, const le::TerminalData &data)
            { return le::evaluate_filter(*expr, root, id, data); });
        return static_cast<int32_t>(handle->terminal_search_results.size());
    }

    int32_t le_search_terminal_port(LeHandle *handle, const char *filter_expression)
    {
        if (!handle || !filter_expression)
            return 0;
        HandleWriteLock lock(handle);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            spdlog::error("le_search_terminal_port: {}", expr.error());
            return -1;
        }

        handle->terminal_port_search_results = handle->root.search_terminal_port(
            [&expr](const le::Root &root, le::TerminalPortId id, const le::TerminalPortData &data)
            { return le::evaluate_filter(*expr, root, id, data); });
        return static_cast<int32_t>(handle->terminal_port_search_results.size());
    }

    int32_t le_search_obstruction(LeHandle *handle, const char *filter_expression)
    {
        if (!handle || !filter_expression)
            return 0;
        HandleWriteLock lock(handle);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            spdlog::error("le_search_obstruction: {}", expr.error());
            return -1;
        }

        handle->obstruction_search_results = handle->root.search_obstruction(
            [&expr](const le::Root &root, le::ObstructionId id, const le::ObstructionData &data)
            { return le::evaluate_filter(*expr, root, id, data); });
        return static_cast<int32_t>(handle->obstruction_search_results.size());
    }

    int32_t le_terminal_port_shape_count(LeHandle *handle, LeTerminalPortId id)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return static_cast<int32_t>(handle->root.get_terminal_port_shapes(from_c(id)).size());
    }

    LeShapeId le_terminal_port_shape_at(LeHandle *handle, LeTerminalPortId id, int32_t index)
    {
        const LeShapeId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        HandleWriteLock lock(handle);

        const std::vector<le::ShapeId> &shapes = handle->root.get_terminal_port_shapes(from_c(id));
        if (static_cast<size_t>(index) >= shapes.size())
            return invalid;
        return to_c(shapes[static_cast<size_t>(index)]);
    }

    int32_t le_obstruction_shape_count(LeHandle *handle, LeObstructionId id)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return static_cast<int32_t>(handle->root.get_obstruction_shapes(from_c(id)).size());
    }

    LeShapeId le_obstruction_shape_at(LeHandle *handle, LeObstructionId id, int32_t index)
    {
        const LeShapeId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        HandleWriteLock lock(handle);

        const std::vector<le::ShapeId> &shapes = handle->root.get_obstruction_shapes(from_c(id));
        if (static_cast<size_t>(index) >= shapes.size())
            return invalid;
        return to_c(shapes[static_cast<size_t>(index)]);
    }

    const char *le_shape_layer_name(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return nullptr;
        HandleWriteLock lock(handle);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape)
            return nullptr;
        const le::LayerData *layer = handle->root.get_layer(shape->layer);
        return layer ? layer->name.c_str() : nullptr;
    }

    int32_t le_shape_rect_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->rects.size()) : 0;
    }

    LeRectUm le_shape_rect_at(LeHandle *handle, LeShapeId id, int32_t index)
    {
        const LeRectUm invalid{.ll_x_um = 0, .ll_y_um = 0, .ur_x_um = 0, .ur_y_um = 0};
        if (!handle || index < 0)
            return invalid;
        HandleWriteLock lock(handle);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(index) >= shape->rects.size())
            return invalid;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return invalid;

        const le::Rect &rect = shape->rects[static_cast<size_t>(index)];
        return LeRectUm{
            .ll_x_um = le::to_um(rect.ll.x, *dbu_per_um),
            .ll_y_um = le::to_um(rect.ll.y, *dbu_per_um),
            .ur_x_um = le::to_um(rect.ur.x, *dbu_per_um),
            .ur_y_um = le::to_um(rect.ur.y, *dbu_per_um),
        };
    }

    int le_remove_shape_rect(LeHandle *handle, LeShapeId id, int32_t index)
    {
        if (!handle || index < 0)
            return 1;
        HandleWriteLock lock(handle);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(index) >= shape->rects.size())
            return 1;
        shape->rects.erase(shape->rects.begin() + index);
        handle->root.bump_mutation_version();
        return 0;
    }

    int32_t le_shape_polygon_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->polygons.size()) : 0;
    }

    int32_t le_shape_polygon_point_count(LeHandle *handle, LeShapeId id, int32_t polygon_index)
    {
        if (!handle || polygon_index < 0)
            return 0;
        HandleWriteLock lock(handle);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(polygon_index) >= shape->polygons.size())
            return 0;
        return static_cast<int32_t>(shape->polygons[static_cast<size_t>(polygon_index)].points.size());
    }

    LePointUm le_shape_polygon_point_at(LeHandle *handle, LeShapeId id, int32_t polygon_index, int32_t point_index)
    {
        const LePointUm invalid{.x_um = 0, .y_um = 0};
        if (!handle || polygon_index < 0 || point_index < 0)
            return invalid;
        HandleWriteLock lock(handle);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(polygon_index) >= shape->polygons.size())
            return invalid;
        const std::vector<le::Point> &points = shape->polygons[static_cast<size_t>(polygon_index)].points;
        if (static_cast<size_t>(point_index) >= points.size())
            return invalid;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return invalid;

        const le::Point &point = points[static_cast<size_t>(point_index)];
        return LePointUm{.x_um = le::to_um(point.x, *dbu_per_um), .y_um = le::to_um(point.y, *dbu_per_um)};
    }

    int le_remove_shape_polygon(LeHandle *handle, LeShapeId id, int32_t polygon_index)
    {
        if (!handle || polygon_index < 0)
            return 1;
        HandleWriteLock lock(handle);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(polygon_index) >= shape->polygons.size())
            return 1;
        shape->polygons.erase(shape->polygons.begin() + polygon_index);
        handle->root.bump_mutation_version();
        return 0;
    }

    int32_t le_shape_path_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->paths.size()) : 0;
    }

    double le_shape_path_width_um(LeHandle *handle, LeShapeId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 0;
        HandleWriteLock lock(handle);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return 0;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return 0;
        return le::to_um(static_cast<int64_t>(shape->paths[static_cast<size_t>(path_index)].width), *dbu_per_um);
    }

    int32_t le_shape_path_point_count(LeHandle *handle, LeShapeId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 0;
        HandleWriteLock lock(handle);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return 0;
        return static_cast<int32_t>(shape->paths[static_cast<size_t>(path_index)].polygon.points.size());
    }

    LePointUm le_shape_path_point_at(LeHandle *handle, LeShapeId id, int32_t path_index, int32_t point_index)
    {
        const LePointUm invalid{.x_um = 0, .y_um = 0};
        if (!handle || path_index < 0 || point_index < 0)
            return invalid;
        HandleWriteLock lock(handle);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return invalid;
        const std::vector<le::Point> &points = shape->paths[static_cast<size_t>(path_index)].polygon.points;
        if (static_cast<size_t>(point_index) >= points.size())
            return invalid;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return invalid;

        const le::Point &point = points[static_cast<size_t>(point_index)];
        return LePointUm{.x_um = le::to_um(point.x, *dbu_per_um), .y_um = le::to_um(point.y, *dbu_per_um)};
    }

    int le_remove_shape_path(LeHandle *handle, LeShapeId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 1;
        HandleWriteLock lock(handle);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return 1;
        shape->paths.erase(shape->paths.begin() + path_index);
        handle->root.bump_mutation_version();
        return 0;
    }

    int32_t le_shape_copy(LeHandle *handle, const LeShapeId *shapes, int32_t shape_count, LeLayerId layer, const char *purpose, LeObjectRef parent)
    {
        return run_shape_op(handle, "shape_copy", layer, purpose, parent, [&](const auto &target, const le::shape_ops::ShapeParent &owner)
                            {
            if (!target)
                return le::shape_ops::Result(std::unexpected("-layer is required"));
            return le::shape_ops::copy(handle->root, shape_ids_from_c(shapes, shape_count), *target, owner); });
    }

    int32_t le_shape_boolean(LeHandle *handle, const LeShapeId *shapes_a, int32_t shape_a_count, const LeShapeId *shapes_b,
                             int32_t shape_b_count, int32_t op, LeLayerId layer, const char *purpose, LeObjectRef parent)
    {
        const char *command = op == LE_SHAPE_BOOLEAN_AND ? "shape_and" : op == LE_SHAPE_BOOLEAN_NOT ? "shape_not" : "shape_or";
        return run_shape_op(handle, command, layer, purpose, parent, [&](const auto &target, const le::shape_ops::ShapeParent &owner)
                            {
            const le::BooleanOp boolean_op = op == LE_SHAPE_BOOLEAN_AND ? le::BooleanOp::And
                                           : op == LE_SHAPE_BOOLEAN_NOT ? le::BooleanOp::Not
                                                                        : le::BooleanOp::Or;
            return le::shape_ops::boolean(handle->root, shape_ids_from_c(shapes_a, shape_a_count), shape_ids_from_c(shapes_b, shape_b_count),
                                          boolean_op, target, owner); });
    }

    int32_t le_shape_to_polygon(LeHandle *handle, const LeShapeId *shapes, int32_t shape_count, LeLayerId layer, const char *purpose, LeObjectRef parent)
    {
        return run_shape_op(handle, "shape_to_polygon", layer, purpose, parent, [&](const auto &target, const le::shape_ops::ShapeParent &owner)
                            { return le::shape_ops::to_polygons(handle->root, shape_ids_from_c(shapes, shape_count), target, owner); });
    }

    int32_t le_shape_to_rects(LeHandle *handle, const LeShapeId *shapes, int32_t shape_count, int32_t vertical, LeLayerId layer, const char *purpose,
                              LeObjectRef parent)
    {
        return run_shape_op(handle, "shape_to_rects", layer, purpose, parent, [&](const auto &target, const le::shape_ops::ShapeParent &owner)
                            {
            const le::FractureDirection direction = vertical ? le::FractureDirection::Vertical : le::FractureDirection::Horizontal;
            return le::shape_ops::to_rects(handle->root, shape_ids_from_c(shapes, shape_count), direction, target, owner); });
    }

    int32_t le_shape_size(LeHandle *handle, const LeShapeId *shapes, int32_t shape_count, double dx_um, double dy_um, LeLayerId layer,
                          const char *purpose, LeObjectRef parent)
    {
        return run_shape_op(handle, "shape_size", layer, purpose, parent, [&](const auto &target, const le::shape_ops::ShapeParent &owner)
                            {
            const std::optional<double> dbu_per_um = database_units_microns(handle->root);
            if (!dbu_per_um)
                return le::shape_ops::Result(std::unexpected("no Technology with a DATABASE MICRONS scale has been read yet"));
            return le::shape_ops::size(handle->root, shape_ids_from_c(shapes, shape_count), to_dbu(dx_um, *dbu_per_um), to_dbu(dy_um, *dbu_per_um),
                                       target, owner); });
    }

    int32_t le_shape_path(LeHandle *handle, const LeShapeId *shapes, int32_t shape_count, double width_um, LeLayerId layer, const char *purpose,
                          LeObjectRef parent)
    {
        return run_shape_op(handle, "shape_path", layer, purpose, parent, [&](const auto &target, const le::shape_ops::ShapeParent &owner)
                            {
            const std::optional<double> dbu_per_um = database_units_microns(handle->root);
            if (!dbu_per_um)
                return le::shape_ops::Result(std::unexpected("no Technology with a DATABASE MICRONS scale has been read yet"));
            return le::shape_ops::outline_paths(handle->root, shape_ids_from_c(shapes, shape_count), to_dbu(width_um, *dbu_per_um), target, owner); });
    }

    int32_t le_shape_change_layer(LeHandle *handle, const LeShapeId *shapes, int32_t shape_count, LeLayerId layer, const char *purpose)
    {
        if (!handle)
            return -1;
        HandleWriteLock lock(handle);
        const auto target = target_from_c(layer, purpose);
        if (!target || !*target)
        {
            spdlog::error("shape_change_layer: {}", target ? std::string("-layer is required") : target.error());
            return -1;
        }
        const auto changed = le::shape_ops::change_layer(handle->root, shape_ids_from_c(shapes, shape_count), **target);
        if (!changed)
        {
            spdlog::error("shape_change_layer: {}", changed.error());
            return -1;
        }
        handle->root.bump_mutation_version();
        if (handle->command_history.is_recording())
        {
            // Undo/redo restores layer and purpose exactly, both ways - the
            // generated apply_shape_snapshot can't clear an unset optional
            // purpose (see shape_ops::set_layer_or_purpose's own comment).
            using Snapshot = le::shape_ops::LayerOrPurpose;
            for (const le::shape_ops::LayerChange &entry : *changed)
                handle->command_history.current()->record_update<le::ShapeId, Snapshot>(
                    entry.id, entry.before, entry.after,
                    [](le::Root &r, le::ShapeId id, const Snapshot &snapshot)
                    {
                        le::ShapeData *shape = r.get_shape(id);
                        if (!shape)
                            return false;
                        le::shape_ops::set_layer_or_purpose(*shape, snapshot);
                        return true;
                    });
        }
        return static_cast<int32_t>(changed->size());
    }

    LeShapeId le_shape_op_result_at(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return LeShapeId{.index = UINT32_MAX, .generation = 0};
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        if (static_cast<size_t>(index) >= handle->shape_op_results.size())
            return LeShapeId{.index = UINT32_MAX, .generation = 0};
        return to_c(handle->shape_op_results[static_cast<size_t>(index)]);
    }

    LeShapeBbox le_shape_bbox(LeHandle *handle, const LeShapeId *shapes, int32_t shape_count)
    {
        LeShapeBbox out{};
        if (!handle)
            return out;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const std::expected<le::Rect, std::string> box = le::shape_ops::bbox(handle->root, shape_ids_from_c(shapes, shape_count));
        if (!box)
        {
            spdlog::error("shape_bbox: {}", box.error());
            return out;
        }
        const double dbu_per_um = display_dbu_per_um(handle->root);
        out.valid = 1;
        out.ll_x_um = static_cast<double>(box->ll.x) / dbu_per_um;
        out.ll_y_um = static_cast<double>(box->ll.y) / dbu_per_um;
        out.ur_x_um = static_cast<double>(box->ur.x) / dbu_per_um;
        out.ur_y_um = static_cast<double>(box->ur.y) / dbu_per_um;
        return out;
    }

    int frame_mark_count = 0;

    LePixelBuffer le_render_pixel_buffer(LeHandle *handle)
    {
        if (!handle)
            return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};
        // Shared (reader), not exclusive - confirmed by direct audit:
        // view_render_options_for takes a `const LeHandle*` (compiler-
        // enforced no mutation), and view_render_pipeline.run() takes a
        // `const Root*` and only ever touches handle->view_render_pipeline
        // itself, which no other exported function references - so a
        // concurrent reader elsewhere on the handle can't race anything
        // this call touches. This is exactly what le_gui.cpp's own
        // per-frame panel reads need to stay live and unblocked while a
        // render is in flight (le_handle.hpp's own mutex_ doc comment).
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        // A viewport that hasn't been sized yet (le_set_viewport_size
        // never called - LeHandle's own viewport_width_px_/
        // viewport_height_px_ both default to 0) has nothing to render -
        // short-circuit before the pipeline runs at all, rather than
        // letting it clamp a degenerate 0x0 request up to some minimum
        // internally (Blend2D itself can't construct a zero-sized
        // BLImage) and leak that clamped size back out as a misleading
        // non-empty result.
        if (handle->viewport_width_px() <= 0 || handle->viewport_height_px() <= 0)
            return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};

        // FrameMarkStart/End (named), not plain FrameMark - a frame here
        // only ever happens on demand (whenever something changed and the
        // native texture callback next pulls a frame), not once per
        // engine vsync like a game's continuous main loop, which is what
        // plain FrameMark assumes (see Tracy's own manual). A named
        // start/end pair correctly represents this as a discontinuous
        // frame set with its own explicit duration in the Tracy timeline,
        // instead of a single zero-width instant marker that would make
        // every render look free.

        FrameMarkStart(kRenderFrameName);

        // PIPELINE_REFACTOR.md's restarted pipelines module - Warm tier
        // only (basic pan/zoom, view_render_options_for's own comment on
        // how LeHandle's pan/scale/viewport-size map onto ViewRenderOptions):
        // the select/zoom drag-rectangle ghost overlay is drawn now
        // (ComposeStage's own doc comment) - selection/hover/ruler
        // overlays remain a gap, Hot tier still TBD - see
        // select_in_abstract_view_unlocked/select_in_layout_view_unlocked/
        // le_set_mouse_position's own comments for what that gap means.
        // The returned WarmOutput::frame keeps its own RasterizedFrame
        // alive via ComposeStage's own MemoizingStage cache (last_result_)
        // for exactly as long as LePixelBuffer's own "valid until the
        // next call" contract (api.hpp) already promises - no separate
        // LeHandle-owned storage needed here.
        const le::ViewRenderOptions options = view_render_options_for(handle);

        // BUGS_AND_ENHANCEMENTS.md E17 - is_rendering_'s own doc comment
        // (LeHandle) explains why this is a plain atomic, not mutex_-
        // guarded. Bracketed around only the real recompute (would_recompute()
        // true), not this whole function - a call that finds nothing
        // changed (the common case, and now the *only* case whenever
        // this is called with nothing to do, since le_gui.cpp's render
        // thread no longer calls this speculatively on a timer - see
        // le_wait_for_render_needed's own doc comment, api.hpp) never
        // sets this at all, so a poller sees an accurate, precisely-timed
        // signal instead of one smeared across cheap no-op calls too.
        // RAII, not a manual reset-before-every-return, so a future early
        // return (or a real exception, however unlikely given this
        // codebase's own "no exceptions for expected-missing-data paths"
        // convention) can't leave this stuck true.
        const bool will_recompute = handle->view_render_pipeline.would_recompute(options);
        if (will_recompute)
            handle->is_rendering_.store(true, std::memory_order_relaxed);
        struct RenderingGuard
        {
            LeHandle *handle;
            bool active;
            ~RenderingGuard()
            {
                if (active)
                    handle->is_rendering_.store(false, std::memory_order_relaxed);
            }
        } rendering_guard{handle, will_recompute};

        const le::ViewRenderPipeline::WarmOutput output = handle->view_render_pipeline.run(&handle->root, options);

        FrameMarkEnd(kRenderFrameName);

        if (!output.frame || output.frame->empty)
            return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};

        return LePixelBuffer{
            .data = output.frame->buffer.data,
            .width = output.frame->buffer.width,
            .height = output.frame->buffer.height,
            .row_bytes = static_cast<int64_t>(output.frame->buffer.row_bytes),
        };
    }

    int32_t le_is_rendering(LeHandle *handle)
    {
        // No lock - see this function's own doc comment (api.hpp) and
        // is_rendering_'s own doc comment (LeHandle) for why not.
        if (!handle)
            return 0;
        return handle->is_rendering_.load(std::memory_order_relaxed) ? 1 : 0;
    }

    void le_wait_for_render_needed(LeHandle *handle)
    {
        if (!handle)
            return;
        handle->wait_for_render_needed();
    }

    void le_cancel_render_wait(LeHandle *handle)
    {
        if (!handle)
            return;
        handle->notify_render_needed();
    }

    void le_request_show_gui(LeHandle *handle)
    {
        // No lock - see gui_show_requested_'s own doc comment (LeHandle).
        if (!handle)
            return;
        handle->gui_show_requested_.store(true, std::memory_order_relaxed);
    }

    int32_t le_take_show_gui_request(LeHandle *handle)
    {
        // exchange(false), not load() - see gui_show_requested_'s own doc
        // comment for why this consumes the request rather than just
        // peeking at it.
        if (!handle)
            return 0;
        return handle->gui_show_requested_.exchange(false, std::memory_order_relaxed) ? 1 : 0;
    }

    void le_enqueue_tcl_command(LeHandle *handle, const char *command)
    {
        if (!handle || !command)
            return;
        std::lock_guard<std::mutex> lock(handle->pending_tcl_commands_mutex_);
        handle->pending_tcl_commands_.emplace_back(command);
    }

    const char *le_take_next_pending_tcl_command(LeHandle *handle)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->pending_tcl_commands_mutex_);
        if (handle->pending_tcl_commands_.empty())
            return nullptr;
        handle->last_popped_tcl_command_ = std::move(handle->pending_tcl_commands_.front());
        handle->pending_tcl_commands_.pop_front();
        return handle->last_popped_tcl_command_.c_str();
    }
}
