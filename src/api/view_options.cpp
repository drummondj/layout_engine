#include "api_internal.hpp"
#include "edit_ops.hpp"
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
#include <span>
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
    // happen inline (read_lef) now calls this too, so the two can't
    // drift out of sync.
    void rebuild_view_layers(LeHandle *handle, le::TechnologyId technology_id)
    {
        handle->view_layers = le::ViewLayerSet::build_for_technology(handle->root, technology_id);
        handle->view_layers.apply_color_overrides(handle->layer_color_overrides()); // the user's picked layer colors
        handle->view_layers_built_at_version = handle->root.mutation_version();
    }

    // Rebuilds handle->view_layers if Root has changed since it was last
    // built - a layer created directly (create_layer, generated CRUD)
    // rather than via a LEF read must still show up. Called at the top of
    // layer_rows()/layer_rows()/purposes()/purposes() and of
    // every hit-test entry point (mouse_up) -
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
    // check, split out so layer_rows()/_at/purposes()/_at (below)
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
    // The values `filter` offers; null for an unknown one.
    const std::vector<std::string> *object_filter_values(const LeHandle *handle, le::ObjectFilter filter)
    {
        switch (filter)
        {
        case le::ObjectFilter::PLACEMENT_TYPE:
            return &handle->placement_type_values();
        case le::ObjectFilter::ROUTE_USE:
            return &le::route_use_values();
        case le::ObjectFilter::PLACEMENT_STATUS:
            return &le::placement_status_values();
        case le::ObjectFilter::ROUTING_STATUS:
            return &le::routing_status_values();
        case le::ObjectFilter::ROUTE_SHAPE:
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
    // root/view state, for Renderer::render's own
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

    // Lock-free bodies of zoom/pan/fit_view - factored out so
    // key_down's Key::ZOOM/FIT/PAN_* handling can call them directly
    // while it's already holding handle->mutex_ (std::mutex isn't
    // recursive - calling back into zoom/pan/fit_view itself
    // from inside key_down would deadlock the calling thread against
    // itself). Callers must have already null-checked `handle` and locked
    // its mutex; the public zoom/pan/fit_view below do exactly
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
    // boundary in a mirrored view (set_view_flip), so the boundary's
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
    // Key::FIT's Ctrl-held branch - fits the viewport
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

namespace le::edit
{
    ViewState view_state(const LeHandle &handle)
    {
        const Point pan = handle.pan();
        const double scale = handle.scale();
        return ViewState{
            .abstract = handle.current_abstract(),
            .layout = handle.current_layout(),
            .visible_area = Rect{.ll = pan,
                                 .ur = Point{.x = pan.x + static_cast<int64_t>(handle.viewport_width_px() / scale),
                                             .y = pan.y + static_cast<int64_t>(handle.viewport_height_px() / scale)}},
            .scale = scale,
            .width_px = handle.viewport_width_px(),
            .height_px = handle.viewport_height_px(),
            .flip = handle.view_flip(),
        };
    }

    void fit_view(LeHandle &handle, int padding_px) { api_detail::fit_scene_unlocked(&handle, padding_px); }

    void fit_rect(LeHandle &handle, Rect area, int padding_px) { handle.fit_to_content(area, padding_px); }

    void zoom(LeHandle &handle, double factor, int x_px, int y_px) { api_detail::zoom_unlocked(&handle, factor, x_px, y_px); }

    void pan(LeHandle &handle, double x_factor, double y_factor) { api_detail::pan_unlocked(&handle, x_factor, y_factor); }

    void set_view_flip(LeHandle &handle, ViewFlip flip) { handle.set_view_flip(flip); }
}

namespace
{
    // The layer set as of the database now: the handle's, or - when an
    // edit has outdated it and the caller only holds the lock shared, so
    // can't store a rebuild - one built for this read.
    template <typename Read>
    auto read_current_view_layers(const LeHandle &handle, Read read)
    {
        if (view_layers_already_current(&handle))
            return read(handle.view_layers);
        le::ViewLayerSet fresh = le::ViewLayerSet::build_for_technology(handle.root, handle.current_technology_id);
        fresh.apply_color_overrides(handle.layer_color_overrides());
        return read(fresh);
    }

    std::vector<le::LayerRow> layer_rows_of(const le::ViewLayerSet &view_layers)
    {
        std::vector<le::LayerRow> rows;
        rows.reserve(view_layers.rows().size());
        for (const le::ViewLayerRow &row : view_layers.rows())
        {
            const le::ViewLayerData *first_column = row.columns.empty() ? nullptr : view_layers.get(row.columns.front().id);
            rows.push_back(le::LayerRow{
                .name = row.name,
                .color = first_column ? first_column->style.outline_color : le::Color{},
                .has_physical_layer = first_column && first_column->layer.valid(),
            });
        }
        return rows;
    }

    // The value list of `filter`; empty for an unknown one.
    std::span<const std::string> filter_values_of(const LeHandle &handle, le::ObjectFilter filter)
    {
        if (const std::vector<std::string> *values = object_filter_values(&handle, filter))
            return *values;
        return {};
    }
}

namespace le::edit
{
    int hierarchy_depth(const LeHandle &handle) { return handle.hierarchy_depth(); }

    void set_hierarchy_depth(LeHandle &handle, int depth) { handle.set_hierarchy_depth(depth); }

    std::vector<LayerRow> layer_rows(const LeHandle &handle) { return read_current_view_layers(handle, layer_rows_of); }

    std::vector<ViewLayerPurpose> purposes(const LeHandle &handle)
    {
        return read_current_view_layers(handle, [](const ViewLayerSet &view_layers)
                                        { return view_layers.purposes(); });
    }

    bool layer_visible(const LeHandle &handle, const std::string &layer) { return handle.is_layer_name_visible(layer); }

    void set_layer_visible(LeHandle &handle, const std::string &layer, bool visible) { handle.set_layer_name_visible(layer, visible); }

    bool layer_selectable(const LeHandle &handle, const std::string &layer) { return handle.is_layer_name_selectable(layer); }

    void set_layer_selectable(LeHandle &handle, const std::string &layer, bool selectable) { handle.set_layer_name_selectable(layer, selectable); }

    bool purpose_visible(const LeHandle &handle, ViewLayerPurpose purpose) { return handle.is_purpose_visible(purpose); }

    void set_purpose_visible(LeHandle &handle, ViewLayerPurpose purpose, bool visible) { handle.set_purpose_visible(purpose, visible); }

    bool purpose_selectable(const LeHandle &handle, ViewLayerPurpose purpose) { return handle.is_purpose_selectable(purpose); }

    void set_purpose_selectable(LeHandle &handle, ViewLayerPurpose purpose, bool selectable) { handle.set_purpose_selectable(purpose, selectable); }

    std::span<const std::string> object_filter_values(const LeHandle &handle, ObjectFilter filter) { return filter_values_of(handle, filter); }

    bool object_filter_visible(const LeHandle &handle, ObjectFilter filter, const std::string &value)
    {
        return handle.is_object_filter_visible(filter, value);
    }

    void set_object_filter_visible(LeHandle &handle, ObjectFilter filter, const std::string &value, bool visible)
    {
        handle.set_object_filter_visible(filter, value, visible);
    }

    bool object_filter_selectable(const LeHandle &handle, ObjectFilter filter, const std::string &value)
    {
        return handle.is_object_filter_selectable(filter, value);
    }

    void set_object_filter_selectable(LeHandle &handle, ObjectFilter filter, const std::string &value, bool selectable)
    {
        handle.set_object_filter_selectable(filter, value, selectable);
    }

    void set_viewport_size(LeHandle &handle, int width_px, int height_px) { handle.set_viewport_size(width_px, height_px); }

    std::array<double, 2> display_point(const LeHandle &handle, Point p) { return display_point_unlocked(&handle, p); }

    std::optional<Color> layer_color(const LeHandle &handle, const std::string &layer)
    {
        return read_current_view_layers(handle, [&](const ViewLayerSet &view_layers) -> std::optional<Color>
                                        {
                                            for (const ViewLayerRow &row : view_layers.rows())
                                                if (row.name == layer && !row.columns.empty())
                                                    if (const ViewLayerData *data = view_layers.get(row.columns.front().id))
                                                        return data->style.outline_color;
                                            return std::nullopt; });
    }
}
