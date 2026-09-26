#include "gui_provider.hpp"

#include <cstdio>

namespace le::gui
{
    namespace
    {
        // Same helpers property_viewer.cpp's own object_children free
        // function used before it moved here - see that function's own
        // history for why each exists (LeObjectRef <-> a concrete LeXxxId
        // pair share the same {index, generation} shape, so these are
        // trivial field copies, not real conversions).
        template <typename IdT>
        IdT ref_to_id(const LeObjectRef &ref)
        {
            IdT id{};
            id.index = ref.index;
            id.generation = ref.generation;
            return id;
        }

        LeObjectRef make_ref(int32_t kind, uint32_t index, uint32_t generation)
        {
            LeObjectRef ref;
            ref.kind = kind;
            ref.index = index;
            ref.generation = generation;
            return ref;
        }
    }

    void GuiProvider::refresh()
    {
        state_.mode = le_get_mode(handle_);
        state_.is_rendering = le_is_rendering(handle_) != 0;
        state_.is_move_armed = le_is_move_armed(handle_) != 0;
        state_.is_resize_armed = le_is_resize_armed(handle_) != 0;
        state_.resize.hover_axis = state_.is_resize_armed ? le_resize_hover_axis(handle_) : LE_RESIZE_AXIS_NONE;
        state_.resize.selected_piece_kinds = state_.is_resize_armed ? le_selected_piece_kinds(handle_) : 0;
        state_.resize.move_snap_piece_kinds = state_.is_move_armed ? le_selected_move_snap_piece_kinds(handle_) : 0;
        if (state_.is_resize_armed || state_.resize.move_snap_piece_kinds != 0)
        {
            for (int32_t kind = LE_PIECE_KIND_RECT; kind <= LE_PIECE_KIND_VIA; ++kind)
            {
                state_.resize.snap_modes[kind] = le_get_shape_snap_mode(handle_, kind);
                for (int32_t mode = LE_SHAPE_SNAP_NONE; mode <= LE_SHAPE_SNAP_TRACKS; ++mode)
                    state_.resize.snap_available[kind][mode] = le_is_shape_snap_mode_available(handle_, kind, mode) != 0;
            }
        }

        state_.status_bar.tooltip_message = le_tooltip_message(handle_);
        state_.status_bar.snapped_mouse_position = le_snapped_mouse_position(handle_);
        state_.status_bar.selection_count = le_selection_count(handle_);

        state_.settings.hierarchy_depth = le_hierarchy_depth(handle_);
        state_.settings.flightline_max_fanout = le_flightline_max_fanout(handle_);
        state_.settings.minor_grid_um = le_grid_spacing_um(handle_, 0);
        state_.settings.major_grid_um = le_grid_spacing_um(handle_, 1);
        state_.settings.manufacturing_grid_um = le_manufacturing_grid_um(handle_);
        state_.settings.ruler_label_size_px = le_ruler_label_size(handle_);
        state_.settings.label_min_size_px = le_label_min_size(handle_);
        state_.settings.label_max_size_px = le_label_max_size(handle_);

        state_.placement_move.selected_count = state_.mode == LE_MODE_EDIT ? le_selected_placement_count(handle_) : 0;
        if (state_.placement_move.selected_count > 0)
        {
            state_.placement_move.snap_mode = le_get_placement_snap_mode(handle_);
            for (int32_t mode = LE_PLACEMENT_SNAP_NONE; mode <= LE_PLACEMENT_SNAP_MANUFACTURING_GRID; ++mode)
                state_.placement_move.snap_available[mode] = le_is_placement_snap_mode_available(handle_, mode) != 0;
            state_.placement_move.orientation_ops_enabled = le_placement_orientation_ops_enabled(handle_);
            state_.placement_move.is_move_anchored = le_is_move_anchored(handle_) != 0;
        }

        // Same build-every-frame shape layer_manager.cpp's own local
        // layers/purposes vectors used before this moved here - see
        // State's own doc comment (gui_provider.hpp) for why this is safe
        // to do unconditionally every frame (layer_manager.cpp already
        // walked every layer/purpose row every frame regardless of any
        // expand/collapse state, so nothing gets more eager here).
        state_.layer_manager.layers.clear();
        const int32_t layer_count = le_layer_count(handle_);
        state_.layer_manager.layers.reserve(static_cast<size_t>(layer_count));
        for (int32_t i = 0; i < layer_count; ++i)
        {
            const LeLayerRow row = le_layer_at(handle_, i);
            // Technology layers only (BUGS_AND_ENHANCEMENTS.md E12): a
            // pseudo-row with no physical Layer (ROW, BOUNDARY, DEBUG,
            // FLIGHTLINE, ...) has exactly one purpose column, already
            // listed in `purposes` below - listing it here too would be a
            // duplicate checkbox for the same flag. Dropped once before,
            // when this loop moved here from layer_manager.cpp - see
            // gui_provider_test.cpp's regression test.
            if (row.name == nullptr || !row.has_physical_layer)
                continue;
            const bool visible = le_is_layer_name_visible(handle_, row.name);
            const bool selectable = le_is_layer_name_selectable(handle_, row.name) != 0;
            state_.layer_manager.layers.push_back(LayerRow{row, visible, selectable});
        }

        state_.layer_manager.purposes.clear();
        const int32_t purpose_count = le_purpose_count(handle_);
        state_.layer_manager.purposes.reserve(static_cast<size_t>(purpose_count));
        for (int32_t i = 0; i < purpose_count; ++i)
        {
            const int32_t ordinal = le_purpose_at(handle_, i);
            const bool visible = le_is_purpose_visible(handle_, ordinal) != 0;
            const bool selectable = le_is_purpose_selectable(handle_, ordinal) != 0;
            state_.layer_manager.purposes.push_back(PurposeRow{ordinal, visible, selectable, le_purpose_has_selectable_objects(ordinal) != 0});
        }
    }

    LeObjectRef GuiProvider::object_parent(LeObjectRef ref) const
    {
        return le_object_parent(handle_, ref);
    }

    LeObjectRef GuiProvider::selected_object_ref(int32_t selection_index) const
    {
        return le_selected_object_ref(handle_, selection_index);
    }

    int32_t GuiProvider::object_property_count(LeObjectRef ref) const
    {
        return le_object_property_count(handle_, ref);
    }

    LeProperty GuiProvider::object_property_at(LeObjectRef ref, int32_t index) const
    {
        return le_object_property_at(handle_, ref, index);
    }

    // Moved verbatim from property_viewer.cpp's own object_children free
    // function - see gui_provider.hpp's own doc comment on this method
    // for why the LE_OBJECT_KIND_DESIGN case stays the one exception
    // still on the older search surface.
    std::vector<LeObjectRef> GuiProvider::object_children(LeObjectRef ref) const
    {
        std::vector<LeObjectRef> children;
        switch (ref.kind)
        {
        case LE_OBJECT_KIND_LIBRARY:
        {
            const LeLibraryId library_id = ref_to_id<LeLibraryId>(ref);
            const int32_t count = le_library_designs_count(handle_, library_id);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeDesignId id = le_library_designs_at(handle_, library_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_DESIGN, id.index, id.generation));
            }
            break;
        }
        case LE_OBJECT_KIND_DESIGN:
        {
            if (state_.is_rendering)
                break;
            const LeDesignId design_id = ref_to_id<LeDesignId>(ref);
            const int32_t count = le_get_abstracts(handle_, design_id, nullptr);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeAbstractId id = le_search_result_abstract_at(handle_, i);
                children.push_back(make_ref(LE_OBJECT_KIND_ABSTRACT, id.index, id.generation));
            }
            break;
        }
        case LE_OBJECT_KIND_ABSTRACT:
        {
            const LeAbstractId abstract_id = ref_to_id<LeAbstractId>(ref);
            const int32_t terminal_count = le_abstract_terminals_count(handle_, abstract_id);
            for (int32_t i = 0; i < terminal_count; ++i)
            {
                const LeTerminalId id = le_abstract_terminals_at(handle_, abstract_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_TERMINAL, id.index, id.generation));
            }
            const int32_t obstruction_count = le_abstract_obstructions_count(handle_, abstract_id);
            for (int32_t i = 0; i < obstruction_count; ++i)
            {
                const LeObstructionId id = le_abstract_obstructions_at(handle_, abstract_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_OBSTRUCTION, id.index, id.generation));
            }
            const int32_t free_shape_count = le_abstract_free_shapes_count(handle_, abstract_id);
            for (int32_t i = 0; i < free_shape_count; ++i)
            {
                const LeShapeId id = le_abstract_free_shapes_at(handle_, abstract_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
            }
            break;
        }
        case LE_OBJECT_KIND_LAYOUT:
        {
            const LeLayoutId layout_id = ref_to_id<LeLayoutId>(ref);
            const int32_t free_shape_count = le_layout_free_shapes_count(handle_, layout_id);
            for (int32_t i = 0; i < free_shape_count; ++i)
            {
                const LeShapeId id = le_layout_free_shapes_at(handle_, layout_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
            }
            break;
        }
        case LE_OBJECT_KIND_TERMINAL:
        {
            const LeTerminalId terminal_id = ref_to_id<LeTerminalId>(ref);
            const int32_t count = le_terminal_ports_count(handle_, terminal_id);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeTerminalPortId id = le_terminal_ports_at(handle_, terminal_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_TERMINAL_PORT, id.index, id.generation));
            }
            break;
        }
        case LE_OBJECT_KIND_TERMINAL_PORT:
        {
            const LeTerminalPortId port_id = ref_to_id<LeTerminalPortId>(ref);
            const int32_t count = le_terminal_port_shapes_count(handle_, port_id);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeShapeId id = le_terminal_port_shapes_at(handle_, port_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
            }
            break;
        }
        case LE_OBJECT_KIND_OBSTRUCTION:
        {
            const LeObstructionId obstruction_id = ref_to_id<LeObstructionId>(ref);
            const int32_t count = le_obstruction_shapes_count(handle_, obstruction_id);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeShapeId id = le_obstruction_shapes_at(handle_, obstruction_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
            }
            break;
        }
        case LE_OBJECT_KIND_BLOCKAGE:
        {
            const LeBlockageId blockage_id = ref_to_id<LeBlockageId>(ref);
            const int32_t count = le_blockage_shapes_count(handle_, blockage_id);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeShapeId id = le_blockage_shapes_at(handle_, blockage_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
            }
            break;
        }
        case LE_OBJECT_KIND_ROUTE:
        {
            const LeRouteId route_id = ref_to_id<LeRouteId>(ref);
            const int32_t count = le_route_shapes_count(handle_, route_id);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeShapeId id = le_route_shapes_at(handle_, route_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
            }
            break;
        }
        case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
        {
            const LePhysicalPortSegmentId segment_id = ref_to_id<LePhysicalPortSegmentId>(ref);
            const int32_t count = le_physical_port_segment_shapes_count(handle_, segment_id);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeShapeId id = le_physical_port_segment_shapes_at(handle_, segment_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
            }
            break;
        }
        case LE_OBJECT_KIND_PHYSICAL_PORT:
        {
            const LePhysicalPortId port_id = ref_to_id<LePhysicalPortId>(ref);
            const int32_t count = le_physical_port_segments_count(handle_, port_id);
            for (int32_t i = 0; i < count; ++i)
            {
                const LePhysicalPortSegmentId id = le_physical_port_segments_at(handle_, port_id, i);
                children.push_back(make_ref(LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT, id.index, id.generation));
            }
            break;
        }
        default:
            break;
        }
        return children;
    }

    bool GuiProvider::property_cache_current(LeObjectRef ref) const
    {
        return le_object_property_cache_current(handle_, ref) != 0;
    }

    bool GuiProvider::would_block_property_lookup(LeObjectRef ref) const
    {
        return state_.is_rendering && !property_cache_current(ref);
    }

    LeObjectRef GuiProvider::invalid_ref() const
    {
        return le_object_invalid_ref();
    }

    int32_t GuiProvider::library_count() const
    {
        return le_library_count(handle_);
    }

    LeLibraryInfo GuiProvider::library_at(int32_t index) const
    {
        return le_library_at(handle_, index);
    }

    int32_t GuiProvider::library_design_count(int32_t library_index) const
    {
        return le_library_design_count(handle_, library_index);
    }

    LeDesignInfo GuiProvider::library_design_at(int32_t library_index, int32_t design_index) const
    {
        return le_library_design_at(handle_, library_index, design_index);
    }

    namespace
    {
        constexpr int32_t kFitScenePaddingPx = 10;
    }

    void GuiProvider::open_design_abstract(LeDesignId design_id)
    {
        le_set_current_design_abstract_by_id(handle_, design_id);
        le_fit_scene(handle_, kFitScenePaddingPx);
    }

    void GuiProvider::open_design_layout(LeDesignId design_id)
    {
        le_set_current_design_layout_by_id(handle_, design_id);
        le_fit_scene(handle_, kFitScenePaddingPx);
    }

    void GuiProvider::set_mode(int32_t mode)
    {
        switch (mode)
        {
        case LE_MODE_EDIT:
            run_tcl_command("set_mode edit");
            break;
        case LE_MODE_RULER:
            run_tcl_command("set_mode ruler");
            break;
        case LE_MODE_SELECT:
        default:
            run_tcl_command("set_mode select");
            break;
        }
    }

    void GuiProvider::select_all() { run_tcl_command("select_all"); }
    void GuiProvider::deselect_all() { run_tcl_command("deselect_all"); }
    void GuiProvider::arm_move() { run_tcl_command("arm_move"); }
    void GuiProvider::arm_resize() { run_tcl_command("arm_resize"); }

    void GuiProvider::set_shape_snap_mode(int32_t kind, int32_t mode)
    {
        static const char *const kKinds[] = {"rect", "polygon", "path", "via"};
        static const char *const kModes[] = {"none", "user", "manufacturing", "fin", "tracks"};
        if (kind < LE_PIECE_KIND_RECT || kind > LE_PIECE_KIND_VIA || mode < LE_SHAPE_SNAP_NONE || mode > LE_SHAPE_SNAP_TRACKS)
            return;
        run_tcl_command(std::string("set_shape_snap_mode ") + kKinds[kind] + " " + kModes[mode]);
    }

    void GuiProvider::set_placement_snap_mode(int32_t mode)
    {
        switch (mode)
        {
        case LE_PLACEMENT_SNAP_NONE:
            run_tcl_command("set_placement_snap_mode none");
            break;
        case LE_PLACEMENT_SNAP_FIN_GRID:
            run_tcl_command("set_placement_snap_mode fin");
            break;
        case LE_PLACEMENT_SNAP_MANUFACTURING_GRID:
            run_tcl_command("set_placement_snap_mode manufacturing");
            break;
        case LE_PLACEMENT_SNAP_SITE:
        default:
            run_tcl_command("set_placement_snap_mode site");
            break;
        }
    }

    void GuiProvider::rotate_placement() { run_tcl_command("rotate_placement"); }
    void GuiProvider::flip_placement_horizontal() { run_tcl_command("flip_placement horizontal"); }
    void GuiProvider::flip_placement_vertical() { run_tcl_command("flip_placement vertical"); }
    void GuiProvider::undo() { run_tcl_command("undo"); }
    void GuiProvider::redo() { run_tcl_command("redo"); }
    void GuiProvider::clear_rulers() { run_tcl_command("clear_rulers"); }

    void GuiProvider::set_hierarchy_depth(int32_t depth)
    {
        run_tcl_command("set_hierarchy_depth " + std::to_string(depth));
    }

    void GuiProvider::set_flightline_max_fanout(int32_t max_fanout)
    {
        run_tcl_command("set_flightline_max_fanout " + std::to_string(max_fanout));
    }

    namespace
    {
        // A Tcl word for `text` - double-quoted, with every character Tcl
        // would substitute inside quotes escaped (a file path can hold any
        // of them).
        std::string tcl_quote(const std::string &text)
        {
            std::string out = "\"";
            for (const char c : text)
            {
                if (c == '\\' || c == '"' || c == '$' || c == '[' || c == ']')
                    out += '\\';
                out += c;
            }
            return out + "\"";
        }

        // Enough digits for any spacing a user types, without float noise
        // (0.1 stays "0.1", not "0.10000000000000001").
        std::string tcl_number(double value)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.10g", value);
            return buffer;
        }
    }

    void GuiProvider::set_grid_spacing_um(double minor_um, double major_um)
    {
        std::string command = "set_grid_spacing";
        if (minor_um > 0.0)
            command += " -minor " + tcl_number(minor_um);
        if (major_um > 0.0)
            command += " -major " + tcl_number(major_um);
        if (minor_um > 0.0 || major_um > 0.0)
            run_tcl_command(command);
    }

    void GuiProvider::set_ruler_label_size(double px) { run_tcl_command("set_ruler_label_size " + tcl_number(px)); }
    void GuiProvider::set_label_min_size(double px) { run_tcl_command("set_label_min_size " + tcl_number(px)); }
    void GuiProvider::set_label_max_size(double px) { run_tcl_command("set_label_max_size " + tcl_number(px)); }
    void GuiProvider::save_settings(const std::string &path) { run_tcl_command(path.empty() ? "save_settings" : "save_settings " + tcl_quote(path)); }
    void GuiProvider::load_settings(const std::string &path) { run_tcl_command(path.empty() ? "load_settings" : "load_settings " + tcl_quote(path)); }

    void GuiProvider::set_layer_color(const std::string &layer_name, uint8_t r, uint8_t g, uint8_t b)
    {
        char color[8];
        std::snprintf(color, sizeof(color), "#%02x%02x%02x", r, g, b);
        run_tcl_command("set_layer_color {" + layer_name + "} " + color);
    }

    void GuiProvider::reset_layer_color(const std::string &layer_name)
    {
        run_tcl_command("reset_layer_color {" + layer_name + "}");
    }

    void GuiProvider::set_layer_visible(const std::string &layer_name, bool value)
    {
        run_tcl_command("set_layer_visible {" + layer_name + "} " + (value ? "1" : "0"));
    }

    void GuiProvider::set_layer_selectable(const std::string &layer_name, bool value)
    {
        run_tcl_command("set_layer_selectable {" + layer_name + "} " + (value ? "1" : "0"));
    }

    void GuiProvider::set_purpose_visible(const std::string &purpose_name, bool value)
    {
        run_tcl_command("set_purpose_visible " + purpose_name + " " + (value ? "1" : "0"));
    }

    void GuiProvider::set_purpose_selectable(const std::string &purpose_name, bool value)
    {
        run_tcl_command("set_purpose_selectable " + purpose_name + " " + (value ? "1" : "0"));
    }

    void GuiProvider::run_tcl_command(const std::string &script)
    {
        le_enqueue_tcl_command(handle_, script.c_str());
    }

    void GuiProvider::set_mouse_position(int32_t x, int32_t y) { le_set_mouse_position(handle_, x, y); }
    void GuiProvider::clear_mouse_position() { le_clear_mouse_position(handle_); }
    void GuiProvider::mouse_down(int32_t x, int32_t y) { le_mouse_down(handle_, x, y); }
    void GuiProvider::mouse_up(int32_t x, int32_t y) { le_mouse_up(handle_, x, y); }
    void GuiProvider::zoom(double factor, int32_t x, int32_t y) { le_zoom(handle_, factor, x, y); }
    void GuiProvider::zoom_drag_down(int32_t x, int32_t y) { le_zoom_drag_down(handle_, x, y); }
    void GuiProvider::key_down(int32_t key_code) { le_key_down(handle_, key_code); }
    void GuiProvider::key_up(int32_t key_code) { le_key_up(handle_, key_code); }
    void GuiProvider::clear_all_keys() { le_clear_all_keys(handle_); }
    void GuiProvider::set_viewport_size(int32_t width_px, int32_t height_px)
    {
        le_set_viewport_size(handle_, width_px, height_px);
    }
}
