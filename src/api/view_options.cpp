#include "api_internal.hpp"
#include "../geometry/geometry.hpp"
#include "../geometry/row_geometry.hpp"
#include "le/extension.hpp"
#include "le/extension_overlay.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace le::api_detail;

namespace le::api_detail
{
    // Rebuilds handle->view_layers for its currently-selected Technology
    // and stamps view_layers_built_at_version so ensure_view_layers_current()
    // (below) can tell it's current again. The one place this used to
    // happen inline (le_read_lef) now calls this too, so the two can't
    // drift out of sync.
    void rebuild_view_layers(LeHandle *handle, le::TechnologyId technology_id)
    {
        handle->view_layers = le::ViewLayerSet::build_for_technology(handle->root, technology_id);
        handle->view_layers.apply_color_overrides(handle->layer_color_overrides()); // the user's picked layer colors
        handle->view_layers_built_at_version = handle->root.mutation_version();
    }

    // Rebuilds handle->view_layers if Root has changed since it was last
    // built - a layer created directly (le_create_layer, generated CRUD)
    // rather than via a LEF read must still show up. Called at the top of
    // le_layer_count/le_layer_at/le_purpose_count/le_purpose_at and of
    // every hit-test entry point (le_mouse_up) -
    // cheap when already current (one integer compare).
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
}

namespace
{
    // The values an LeObjectFilter offers; null for an unknown one.
    const std::vector<std::string> *object_filter_values(const LeHandle *handle, int32_t filter)
    {
        switch (filter)
        {
        case LE_OBJECT_FILTER_PLACEMENT_TYPE:
            return &handle->placement_type_values();
        case LE_OBJECT_FILTER_ROUTE_USE:
            return &le::route_use_values();
        case LE_OBJECT_FILTER_PLACEMENT_STATUS:
            return &le::placement_status_values();
        case LE_OBJECT_FILTER_ROUTING_STATUS:
            return &le::routing_status_values();
        case LE_OBJECT_FILTER_ROUTE_SHAPE:
            return &le::route_shape_values();
        default:
            return nullptr;
        }
    }

    // The selected placements' flightlines for view_render_options_for,
    // from LeHandle::flightline_cache -
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
            cache.lines_mutation_version != mutation_version || cache.lines_layout != layout_id ||
            cache.lines_max_fanout != handle->flightline_max_fanout())
        {
            cache.lines = le::placement_flightlines(handle->root, cache.index, selected_placements_unlocked(handle), handle->flightline_max_fanout());
            cache.lines_valid = true;
            cache.lines_max_fanout = handle->flightline_max_fanout();
            cache.lines_selection_version = handle->selection_version();
            cache.lines_mutation_version = mutation_version;
            cache.lines_layout = layout_id;
            ++cache.version;
        }
        return {cache.lines, cache.version};
    }
}

namespace le::api_detail
{
    // Builds a ViewRenderOptions snapshot of `handle`'s own current
    // root/view state, for le_render_pixel_buffer's own
    // view_render_pipeline.run() call. LeHandle's own pan/scale/viewport-size convention
    // (pixel = (dbu - pan) * scale, LeHandle::pixel_to_dbu's own comment)
    // maps directly onto ViewRenderOptions::viewport/scale: pan is
    // exactly the dbu point at the viewport's own bottom-left pixel
    // corner - viewport.ll - and the top-right corner is pan plus the
    // pixel size converted to dbu via the same scale.
    le::ViewRenderOptions view_render_options_for(const LeHandle *handle)
    {
        le::ViewRenderOptions options;
        options.root = &handle->root;
        options.root_mutation_version = handle->root.mutation_version();
        options.hierarchy_depth = handle->hierarchy_depth();
        options.scale = handle->scale();
        options.view_flip = handle->view_flip();
        options.layer_name_visible = handle->layer_name_visibility();
        options.purpose_visible = handle->purpose_visibility();
        options.hidden_objects = handle->hidden_objects();
        options.layer_color_overrides = handle->layer_color_overrides();

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

        if (handle->is_dragging())
        {
            options.drag_rect_dbu = handle->drag_rect_dbu();
            options.drag_is_zoom = handle->drag_kind() == LeHandle::DragKind::ZOOM;
        }

        options.mouse_version = handle->mouse_version();
        options.cursor_snapped_position_dbu = handle->snapped_mouse_position();

        if (handle->resize().hover)
            options.resize_hover_segment_dbu = std::array<le::Point, 2>{handle->resize().hover->a, handle->resize().hover->b};
        if (handle->resize().grab)
        {
            // Resize - the grabbed
            // piece as it would be committed right now, pre-placed.
            if (const std::optional<le::Point> mouse = handle->mouse_dbu_position())
                for (ResizeEdit &edit : plan_resize_unlocked(handle, *mouse))
                    for (le::ShapeData &piece : edit.ghost_pieces)
                        options.move_ghost_pieces_dbu.push_back(std::move(piece));
        }
        else if (const std::vector<le::PlacementId> placements = handle->moving_placements(); !placements.empty())
        {
            // Placement Move - each
            // placement snaps independently, so the ghost is pre-placed
            // geometry (offset 0) rather than one shared translation; any
            // shape pieces moving alongside are pre-translated by their own
            // (user-grid) delta to match.
            if (const std::optional<le::Point> raw_delta = handle->move_raw_delta(handle->move_free_form()))
            {
                const std::optional<std::vector<le::Point>> deltas = moving_piece_deltas_unlocked(handle);
                const std::vector<le::ShapeData> &geometry = handle->move().moving_geometry;
                for (size_t i = 0; i < geometry.size(); ++i)
                    if (!geometry[i].rects.empty() || !geometry[i].polygons.empty() || !geometry[i].paths.empty())
                        options.move_ghost_pieces_dbu.push_back(le::Geometry::translate_rects_polygons_paths(geometry[i], deltas && i < deltas->size() ? (*deltas)[i] : le::Point{}));
                for (const le::PlacementMoveTarget &target : plan_moving_placements_unlocked(handle, placements, *raw_delta))
                    options.move_ghost_pieces_dbu.push_back(le::placement_move_ghost(target));
            }
        }
        else if (const std::optional<std::vector<le::Point>> deltas = moving_piece_deltas_unlocked(handle))
        {
            // One shared offset unless some piece snaps on its own
            // - then each is pre-translated by its own delta.
            const std::vector<LeHandle::SelectedObject> &pieces = handle->move().moving_pieces;
            const bool per_piece = std::ranges::any_of(pieces, [](const LeHandle::SelectedObject &selected)
                                                       {
                                                           const LeHandle::ShapePiece *piece = std::get_if<LeHandle::ShapePiece>(&selected);
                                                           return piece && snaps_individually_when_moved(piece->piece_kind);
                                                       });
            const std::vector<le::ShapeData> &geometry = handle->move().moving_geometry;
            if (!per_piece)
            {
                options.move_ghost_pieces_dbu = geometry;
                options.move_ghost_offset_dbu = handle->move_delta(handle->move_free_form()).value_or(le::Point{});
            }
            else
                for (size_t i = 0; i < geometry.size() && i < deltas->size(); ++i)
                    options.move_ghost_pieces_dbu.push_back(le::Geometry::translate_rects_polygons_paths(geometry[i], (*deltas)[i]));
        }

        // Extensions' overlays, each drawn with an OverlayContext over this
        // handle (the render holds its lock for reading).
        for (const auto &[extension, draw] : le::ext::registry().overlays())
            options.extension_overlays.push_back([handle, extension = extension, draw = draw](BLContext &ctx, const le::OverlayFrame &frame) {
                le::ext::OverlayContext overlay(ctx, frame, const_cast<LeHandle *>(handle), extension);
                draw(overlay);
            });
        options.extension_overlay_version = handle->extension_overlay_version.load(std::memory_order_relaxed);

        options.ruler_version = handle->ruler_version();
        options.ruler_dbu_per_um = technology_dbu_per_um(handle->root);
        options.ruler_label_size_px = handle->ruler_label_size_px();
        options.label_min_size_px = handle->label_min_size_px();
        options.label_max_size_px = handle->label_max_size_px();
        options.ruler_polylines_dbu.reserve(handle->rulers().size());
        for (const LeHandle::Ruler &ruler : handle->rulers())
            options.ruler_polylines_dbu.push_back(ruler.points);

        // The live ghost segment - only meaningful in Ruler mode, only
        // ever extends the *last* ruler if it exists, isn't finished, and
        // already has a committed point (LeHandle::ruler_next_point's own
        // "the last entry is the active ruler" invariant).
        if (handle->mode() == LeHandle::Mode::RULER && !handle->rulers().empty() &&
            !handle->rulers().back().finished && !handle->rulers().back().points.empty())
        {
            options.ruler_ghost_point_dbu = handle->ruler_next_point(handle->ruler_free_form());
        }

        options.minor_grid_spacing_dbu = handle->minor_grid_spacing();
        options.major_grid_spacing_dbu = handle->major_grid_spacing();
        options.axis_origin_dbu = display_origin_unlocked(handle);
        if (const le::AbstractId *abstract_id = std::get_if<le::AbstractId>(&options.top_level))
            if (const le::AbstractData *abstract = handle->root.get_abstract(*abstract_id))
            {
                // Where the placement point lands in the macro: -ORIGIN.
                const le::Point origin = abstract->origin.value_or(le::Point{});
                options.abstract_origin_dbu = le::Point{-origin.x, -origin.y};
            }

        return options;
    }

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

        // Undo the rasterizer's Y-flip to get from the caller's image-pixel
        // (x, y) - top-left origin, y down - to the dbu point it currently
        // shows, using the *old* scale/pan (pan/scale describe the
        // pre-flip transform while (x, y) here is post-flip).
        // A mirrored view's pixel is first taken back to the unmirrored one.
        const auto [ux, uy] = handle->unmirrored_pixel(x, y);
        const double dbu_x = static_cast<double>(old_pan.x) + ux / old_scale;
        const double dbu_y = static_cast<double>(old_pan.y) + (viewport_height - uy) / old_scale;

        // Re-solve pan so that same dbu point still lands under (x, y) at
        // the new scale, keeping the zoom visually anchored there.
        const double pan_x_double = dbu_x - ux / new_scale;
        const double pan_y_double = dbu_y - (viewport_height - uy) / new_scale;

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

    // -1 on an axis the view mirrors, else 1.
    double screen_x_sign(const LeHandle *handle) { return handle->view_flip() == le::ViewFlip::HORIZONTAL ? -1.0 : 1.0; }
    double screen_y_sign(const LeHandle *handle) { return handle->view_flip() == le::ViewFlip::VERTICAL ? -1.0 : 1.0; }

    void pan_unlocked(LeHandle *handle, double x_factor, double y_factor)
    {
        const double scale = handle->scale();
        const le::Point pan = handle->pan();

        const int64_t dx = static_cast<int64_t>(x_factor * handle->viewport_width_px() / scale);
        const int64_t dy = static_cast<int64_t>(y_factor * handle->viewport_height_px() / scale);

        handle->set_pan(le::Point{.x = pan.x + dx, .y = pan.y + dy});
    }
}

namespace
{
    // The open view's boundary: a Layout's diearea bbox, else the open
    // Abstract's declared bbox (abstract_declared_bbox, what a parent sizes
    // its placement by) - declared sizes, O(1), rather than a union of
    // everything in the view. nullopt if the view has none.
    std::optional<le::Rect> view_boundary_unlocked(const LeHandle *handle)
    {
        // A Layout view has no current_abstract() (the two "current view"
        // trackers are mutually exclusive).
        if (handle->current_layout().valid())
        {
            const le::ShapeData *diearea = handle->root.get_shape(handle->root.get_layout_diearea(handle->current_layout()));
            return diearea ? le::Geometry::bbox(*diearea) : std::nullopt;
        }
        return le::abstract_declared_bbox(handle->root, handle->current_abstract());
    }
}

namespace le::api_detail
{
    void fit_scene_unlocked(LeHandle *handle, int32_t padding_px)
    {
        handle->fit_to_content(view_boundary_unlocked(handle), padding_px);
    }

    // The dbu point shown to the user as (0, 0): the centre of the view's
    // boundary in a mirrored view (le_set_view_flip), so the boundary's
    // bottom-left on screen reads (-w/2, -h/2); else, or with no boundary,
    // the database origin.
    std::array<double, 2> display_origin_unlocked(const LeHandle *handle)
    {
        if (handle->view_flip() == le::ViewFlip::NONE)
            return {0.0, 0.0};
        const std::optional<le::Rect> boundary = view_boundary_unlocked(handle);
        if (!boundary)
            return {0.0, 0.0};
        return {(static_cast<double>(boundary->ll.x) + static_cast<double>(boundary->ur.x)) / 2.0,
                (static_cast<double>(boundary->ll.y) + static_cast<double>(boundary->ur.y)) / 2.0};
    }

    // Where dbu point `p` is shown to the user, in dbu: relative to
    // display_origin_unlocked, and mirrored with the view.
    std::array<double, 2> display_point_unlocked(const LeHandle *handle, le::Point p)
    {
        const auto [cx, cy] = display_origin_unlocked(handle);
        const double x = static_cast<double>(p.x) - cx;
        const double y = static_cast<double>(p.y) - cy;
        switch (handle->view_flip())
        {
        case le::ViewFlip::HORIZONTAL:
            return {-x, y};
        case le::ViewFlip::VERTICAL:
            return {x, -y};
        default:
            return {x, y};
        }
    }
}

namespace
{
    // Widens `bbox` to also enclose `r` - a plain min/max union, same
    // shape as Geometry::expand_bbox (private to that class - this is
    // the small hand-rolled equivalent api.cpp needs at its own call
    // sites for the bare-id (no Shape) selectable kinds).
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
}

namespace le::api_detail
{
    // LE_KEY_FIT's Ctrl-held branch - fits the viewport
    // to the current selection's own combined bbox instead of the whole
    // design's. One Root::get_shape(selected.shape_id) lookup per
    // ShapePiece selection entry (owned by `root`, outlives this call, no
    // copy needed), then a single Geometry::bbox call unions them. The
    // bare-id kinds (Row/Region/Placement - no backing Shape) resolve
    // their own bbox directly and union in via expand_bbox_unlocked
    // instead. A no-op (view unchanged) if nothing is selected, unlike
    // fit_scene_unlocked, which always has the whole design to fall back
    // to.
    void fit_selected_unlocked(LeHandle *handle, int32_t padding_px)
    {
        std::vector<const le::ShapeData *> shape_ptrs;
        std::deque<le::ShapeData> converted_wires; // stable addresses for shape_ptrs
        std::optional<le::Rect> bbox;
        const int remaining_depth = std::max(0, handle->hierarchy_depth() - 1);

        for (const LeHandle::SelectedObject &selected : handle->selection())
        {
            std::visit([&](const auto &s)
                       {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, LeHandle::ShapePiece>)
                {
                    if (const le::ShapeData *shape = handle->root.get_shape(s.shape_id))
                        shape_ptrs.push_back(shape);
                    else if (const le::WireData *wire = handle->root.get_wire(s.wire_id))
                        shape_ptrs.push_back(&converted_wires.emplace_back(le::wire_to_shape(handle->root, *wire)));
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
}

extern "C"
{
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
        return read_view_layers(handle, [](const le::ViewLayerSet &view_layers)
                                { return static_cast<int32_t>(view_layers.rows().size()); });
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
                .name = hold_string("le_layer_at", row.name),
                .color_r = first_column ? first_column->style.outline_color.r : uint8_t{0},
                .color_g = first_column ? first_column->style.outline_color.g : uint8_t{0},
                .color_b = first_column ? first_column->style.outline_color.b : uint8_t{0},
                .has_physical_layer = (first_column && first_column->layer.valid()) ? 1 : 0,
            };
        };

        return read_view_layers(handle, row_at);
    }

    int32_t le_purpose_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        return read_view_layers(handle, [](const le::ViewLayerSet &view_layers)
                                { return static_cast<int32_t>(view_layers.purposes().size()); });
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

        return read_view_layers(handle, purpose_at);
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

    int32_t le_purpose_has_selectable_objects(int32_t purpose)
    {
        if (purpose < 0 || static_cast<size_t>(purpose) >= le::kViewLayerPurposeCount)
            return 0;
        return le::purpose_has_selectable_objects(static_cast<le::ViewLayerPurpose>(purpose)) ? 1 : 0;
    }

    int32_t le_purpose_kind_count(void) { return static_cast<int32_t>(le::kViewLayerPurposeCount); }

    const char *le_purpose_name(int32_t purpose)
    {
        if (purpose < 0 || static_cast<size_t>(purpose) >= le::kViewLayerPurposeCount)
            return nullptr;
        // Labels are string literals in the generated table, so their data
        // is null-terminated and lives for the whole process.
        return le::kViewLayerPurposes[static_cast<size_t>(purpose)].label.data();
    }

    int32_t le_purpose_visible_by_default(int32_t purpose)
    {
        if (purpose < 0 || static_cast<size_t>(purpose) >= le::kViewLayerPurposeCount)
            return 0;
        return le::kViewLayerPurposes[static_cast<size_t>(purpose)].visible_by_default ? 1 : 0;
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

    int32_t le_object_filter_value_count(LeHandle *handle, int32_t filter)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const std::vector<std::string> *values = object_filter_values(handle, filter);
        return values ? static_cast<int32_t>(values->size()) : 0;
    }

    const char *le_object_filter_value_at(LeHandle *handle, int32_t filter, int32_t index)
    {
        if (!handle || index < 0)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const std::vector<std::string> *values = object_filter_values(handle, filter);
        if (!values || static_cast<size_t>(index) >= values->size())
            return nullptr;
        return hold_string(__func__, (*values)[static_cast<size_t>(index)]);
    }

    int32_t le_is_object_filter_value_visible(LeHandle *handle, int32_t filter, const char *value)
    {
        if (!handle || !value)
            return 1;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->is_object_filter_visible(filter, value) ? 1 : 0;
    }

    void le_set_object_filter_value_visible(LeHandle *handle, int32_t filter, const char *value, int32_t visible)
    {
        if (!handle || !value)
            return;
        HandleWriteLock lock(handle);
        handle->set_object_filter_visible(filter, value, visible != 0);
    }

    int32_t le_is_object_filter_value_selectable(LeHandle *handle, int32_t filter, const char *value)
    {
        if (!handle || !value)
            return 1;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->is_object_filter_selectable(filter, value) ? 1 : 0;
    }

    void le_set_object_filter_value_selectable(LeHandle *handle, int32_t filter, const char *value, int32_t selectable)
    {
        if (!handle || !value)
            return;
        HandleWriteLock lock(handle);
        handle->set_object_filter_selectable(filter, value, selectable != 0);
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

    int32_t le_set_view_flip(LeHandle *handle, int32_t flip)
    {
        if (!handle || flip < LE_VIEW_FLIP_NONE || flip > LE_VIEW_FLIP_VERTICAL)
            return 1;
        HandleWriteLock lock(handle);
        handle->set_view_flip(static_cast<le::ViewFlip>(flip));
        return 0;
    }

    int32_t le_view_flip(LeHandle *handle)
    {
        if (!handle)
            return LE_VIEW_FLIP_NONE;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->view_flip());
    }

    void le_set_viewport_size(LeHandle *handle, int32_t width_px, int32_t height_px)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_viewport_size(width_px, height_px);
    }

    int32_t le_viewport_width_px(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->viewport_width_px();
    }

    int32_t le_viewport_height_px(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->viewport_height_px();
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
}
