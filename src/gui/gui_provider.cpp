#include "gui_provider.hpp"

#include "../core/themes.hpp"

#include <algorithm>
#include <cstdio>
#include <type_traits>
#include <variant>

namespace le::gui
{
    namespace
    {
        constexpr int kFitScenePaddingPx = 10;

        template <class T>
        size_t count_selected(std::span<const SelectedObject> selection)
        {
            return static_cast<size_t>(std::ranges::count_if(selection, [](const SelectedObject &selected)
                                                             { return std::holds_alternative<T>(selected); }));
        }

        void refresh_filter(const ReadView &view, ObjectFilter filter, std::vector<GuiProvider::FilterRow> &rows)
        {
            rows.clear();
            for (const std::string &value : view.object_filter_values(filter))
                rows.push_back(GuiProvider::FilterRow{value, view.object_filter_visible(filter, value), view.object_filter_selectable(filter, value)});
        }
    }

    void GuiProvider::refresh()
    {
        // Lock-free, so read before the view: a frame learns a render or
        // command is running even if the view has to wait.
        const Renderer renderer(handle_);
        state_.is_rendering = renderer.is_rendering();
        state_.is_command_running = renderer.renders_held();

        const ReadView view(handle_);
        state_.mode = view.mode();
        state_.is_move_armed = view.move_armed();
        state_.is_resize_armed = view.resize_armed();
        state_.resize.hover_axis = state_.is_resize_armed ? view.resize_hover_axis() : std::nullopt;
        state_.resize.selected_piece_kinds = state_.is_resize_armed ? view.selected_piece_kinds() : 0;
        state_.resize.move_snap_piece_kinds = state_.is_move_armed ? view.selected_move_snap_piece_kinds() : 0;
        if (state_.is_resize_armed || state_.resize.move_snap_piece_kinds != 0)
        {
            for (size_t kind = 0; kind < state_.resize.snap_modes.size(); ++kind)
            {
                const auto piece = static_cast<PieceKind>(kind);
                state_.resize.snap_modes[kind] = view.shape_snap_mode(piece);
                for (size_t mode = 0; mode < state_.resize.snap_available[kind].size(); ++mode)
                    state_.resize.snap_available[kind][mode] = view.shape_snap_mode_available(piece, static_cast<ShapeSnapMode>(mode));
            }
        }

        const std::span<const SelectedObject> selection = view.selection();
        const std::expected<Units, std::string> units = view.units();
        state_.status_bar.tooltip_message = view.tooltip();
        state_.status_bar.snapped_mouse_position.reset();
        if (const std::optional<Point> snapped = view.snapped_mouse_position(); snapped && units)
        {
            const auto [x, y] = view.display_point(*snapped);
            state_.status_bar.snapped_mouse_position = PointUm{x / units->dbu_per_um(), y / units->dbu_per_um()};
        }
        state_.status_bar.selection_count = selection.size();

        state_.settings.hierarchy_depth = view.hierarchy_depth();
        state_.settings.flightline_max_fanout = view.flightline_max_fanout();
        state_.settings.undo_limit = view.undo_limit();
        state_.settings.max_concurrency = view.max_concurrency();
        state_.settings.minor_grid_um = view.grid_spacing_um(false);
        state_.settings.major_grid_um = view.grid_spacing_um(true);
        const std::optional<int64_t> manufacturing_grid = view.manufacturing_grid();
        state_.settings.manufacturing_grid_um = manufacturing_grid && units ? units->to_um(*manufacturing_grid) : 0.0;
        state_.settings.ruler_label_size_px = view.ruler_label_size();
        state_.settings.confirm_overwrite = view.confirm_overwrite();
        state_.settings.theme = view.theme();
        state_.settings.label_min_size_px = view.label_min_size();
        state_.settings.label_max_size_px = view.label_max_size();
        state_.database_settings.view_flip = view.view().flip;

        const bool edit_mode = state_.mode == InteractionMode::EDIT;
        state_.placement_move.selected_count = edit_mode ? count_selected<PlacementId>(selection) : 0;
        state_.selected_shape_piece_count = edit_mode ? count_selected<ShapePiece>(selection) : 0;
        if (state_.placement_move.selected_count > 0)
        {
            state_.placement_move.snap_mode = view.placement_snap_mode();
            for (size_t mode = 0; mode < state_.placement_move.snap_available.size(); ++mode)
                state_.placement_move.snap_available[mode] = view.placement_snap_mode_available(static_cast<PlacementSnapMode>(mode));
            state_.placement_move.orientation_ops_enabled = view.placement_orientation_ops_enabled();
            state_.placement_move.is_move_anchored = view.move_anchored();
            state_.placement_move.all_fixed = view.selected_placements_all_fixed();
            state_.placement_move.locked_count = view.selected_locked_placement_count();
        }

        // Technology layers only: a pseudo-row with no physical Layer (ROW,
        // BOUNDARY, DEBUG, FLIGHTLINE, ...) has one purpose column, already
        // listed under purposes; listing it here too would be a second
        // checkbox for the same flag.
        state_.layer_manager.layers.clear();
        for (const le::LayerRow &row : view.layer_rows())
            if (row.has_physical_layer)
                state_.layer_manager.layers.push_back(LayerRow{.name = row.name,
                                                               .color = view.layer_color(row.name).value_or(row.color),
                                                               .visible = view.layer_visible(row.name),
                                                               .selectable = view.layer_selectable(row.name)});

        state_.layer_manager.purposes.clear();
        for (const ViewLayerPurpose purpose : view.purposes())
            state_.layer_manager.purposes.push_back(PurposeRow{.purpose = purpose,
                                                               .name = kViewLayerPurposes[static_cast<size_t>(purpose)].label.data(),
                                                               .visible = view.purpose_visible(purpose),
                                                               .selectable = view.purpose_selectable(purpose),
                                                               .has_selectable_objects = purpose_has_selectable_objects(purpose)});

        refresh_filter(view, ObjectFilter::PLACEMENT_TYPE, state_.layer_manager.placement_types);
        refresh_filter(view, ObjectFilter::PLACEMENT_STATUS, state_.layer_manager.placement_statuses);
        refresh_filter(view, ObjectFilter::ROUTE_USE, state_.layer_manager.route_uses);
        refresh_filter(view, ObjectFilter::ROUTING_STATUS, state_.layer_manager.routing_statuses);
        refresh_filter(view, ObjectFilter::ROUTE_SHAPE, state_.layer_manager.route_shapes);
    }

    std::optional<ObjectId> GuiProvider::selected_object(size_t index) const
    {
        const ReadView view(handle_);
        const std::span<const SelectedObject> selection = view.selection();
        if (index >= selection.size())
            return std::nullopt;
        // A piece is its Shape, or its Wire for routed wiring; the Property
        // Viewer walks up from there to the owning Route, Terminal, ...
        return std::visit([](const auto &selected) -> ObjectId
                          {
                              using T = std::decay_t<decltype(selected)>;
                              if constexpr (std::is_same_v<T, ShapePiece>)
                              {
                                  if (selected.wire_id.valid())
                                      return selected.wire_id;
                                  return selected.shape_id;
                              }
                              else
                                  return selected; },
                          selection[index]);
    }

    std::vector<ObjectId> GuiProvider::ancestry(const ObjectId &id) const
    {
        const ReadView view(handle_);
        if (view.properties(id).empty())
            return {};
        std::vector<ObjectId> chain{id};
        for (std::optional<ObjectId> parent = view.parent(id); parent; parent = view.parent(*parent))
            chain.push_back(*parent);
        std::ranges::reverse(chain);
        return chain;
    }

    std::vector<PropertyValue> GuiProvider::properties(const ObjectId &id) const
    {
        return ReadView(handle_).properties(id);
    }

    std::vector<ObjectId> GuiProvider::children(const ObjectId &id) const
    {
        const ReadView view(handle_);
        const Root &root = view.root();
        std::vector<ObjectId> children;
        const auto add = [&](const auto &ids)
        {
            for (const auto child : ids)
                children.emplace_back(child);
        };
        std::visit([&](const auto typed)
                   {
                       using T = std::decay_t<decltype(typed)>;
                       if constexpr (std::is_same_v<T, LibraryId>)
                           add(root.get_library_designs(typed));
                       else if constexpr (std::is_same_v<T, DesignId>)
                       {
                           if (const AbstractId abstract = root.get_design_abstract(typed); root.get_abstract(abstract))
                               children.emplace_back(abstract);
                       }
                       else if constexpr (std::is_same_v<T, AbstractId>)
                       {
                           add(root.get_abstract_terminals(typed));
                           add(root.get_abstract_obstructions(typed));
                           add(root.get_abstract_free_shapes(typed));
                       }
                       else if constexpr (std::is_same_v<T, LayoutId>)
                           add(root.get_layout_free_shapes(typed));
                       else if constexpr (std::is_same_v<T, TerminalId>)
                           add(root.get_terminal_ports(typed));
                       else if constexpr (std::is_same_v<T, TerminalPortId>)
                           add(root.get_terminal_port_shapes(typed));
                       else if constexpr (std::is_same_v<T, ObstructionId>)
                           add(root.get_obstruction_shapes(typed));
                       else if constexpr (std::is_same_v<T, BlockageId>)
                           add(root.get_blockage_shapes(typed));
                       else if constexpr (std::is_same_v<T, RouteId>)
                       {
                           add(root.get_route_wires(typed));
                           add(root.get_route_shapes(typed));
                       }
                       else if constexpr (std::is_same_v<T, PhysicalPortSegmentId>)
                           add(root.get_physical_port_segment_shapes(typed));
                       else if constexpr (std::is_same_v<T, PhysicalPortId>)
                           add(root.get_physical_port_segments(typed)); },
                   id);
        return children;
    }

    std::vector<std::string> GuiProvider::tokens(const std::vector<ObjectId> &ids) const
    {
        const ReadView view(handle_);
        std::vector<std::string> tokens;
        tokens.reserve(ids.size());
        for (const ObjectId &id : ids)
        {
            std::string token(object_kind_name(id));
            token += ':';
            if (object_kind_is_named(id))
            {
                for (const PropertyValue &property : view.properties(id))
                    if (property.name == "name")
                    {
                        token += property.string_value;
                        break;
                    }
            }
            else
            {
                const auto [index, generation] = std::visit([](const auto typed)
                                                            { return std::pair<uint64_t, uint64_t>{typed.index, typed.generation}; },
                                                            id);
                token += std::to_string(generation << 32 | index);
            }
            tokens.push_back(std::move(token));
        }
        return tokens;
    }

    const std::vector<GuiProvider::LibraryEntry> &GuiProvider::libraries()
    {
        const ReadView view(handle_);
        const Root &root = view.root();
        if (libraries_version_ == root.mutation_version())
            return libraries_;
        libraries_.clear();
        for (const LibraryId library_id : root.get_library_ids())
        {
            const LibraryData *library = root.get_library(library_id);
            if (!library)
                continue;
            LibraryEntry &entry = libraries_.emplace_back(LibraryEntry{.name = library->name, .designs = {}});
            for (const DesignId design_id : root.get_library_designs(library_id))
                if (const DesignData *design = root.get_design(design_id))
                    entry.designs.push_back(DesignEntry{.id = design_id,
                                                        .name = design->name,
                                                        .has_abstract = root.get_design_abstract(design_id).valid(),
                                                        .has_layout = root.get_design_layout(design_id).valid()});
        }
        libraries_version_ = root.mutation_version();
        return libraries_;
    }

    void GuiProvider::open_design_abstract(DesignId design)
    {
        WriteView view(handle_);
        if (view.open_abstract(design))
            view.fit_view(kFitScenePaddingPx);
    }

    void GuiProvider::open_design_layout(DesignId design)
    {
        WriteView view(handle_);
        if (view.open_layout(design))
            view.fit_view(kFitScenePaddingPx);
    }

    void GuiProvider::set_mode(InteractionMode mode)
    {
        switch (mode)
        {
        case InteractionMode::EDIT:
            run_tcl_command("set_mode edit");
            break;
        case InteractionMode::RULER:
            run_tcl_command("set_mode ruler");
            break;
        case InteractionMode::SELECT:
            run_tcl_command("set_mode select");
            break;
        }
    }

    void GuiProvider::deselect_all() { run_tcl_command("deselect_all"); }
    void GuiProvider::arm_move() { run_tcl_command("arm_move"); }
    void GuiProvider::arm_resize() { run_tcl_command("arm_resize"); }
    void GuiProvider::delete_selected_pieces() { run_tcl_command("delete_selected_pieces"); }

    void GuiProvider::set_selected_placements_fixed(bool fixed) { run_tcl_command(fixed ? "set_placement_status FIXED" : "set_placement_status PLACED"); }

    void GuiProvider::set_shape_snap_mode(PieceKind kind, ShapeSnapMode mode)
    {
        static constexpr const char *kKinds[] = {"rect", "polygon", "path", "via"};
        static constexpr const char *kModes[] = {"none", "user", "manufacturing", "fin", "tracks"};
        const auto kind_index = static_cast<size_t>(kind);
        const auto mode_index = static_cast<size_t>(mode);
        if (kind_index >= std::size(kKinds) || mode_index >= std::size(kModes))
            return;
        run_tcl_command(std::string("set_shape_snap_mode ") + kKinds[kind_index] + " " + kModes[mode_index]);
    }

    void GuiProvider::set_placement_snap_mode(PlacementSnapMode mode)
    {
        switch (mode)
        {
        case PlacementSnapMode::NONE:
            run_tcl_command("set_placement_snap_mode none");
            break;
        case PlacementSnapMode::FIN_GRID:
            run_tcl_command("set_placement_snap_mode fin");
            break;
        case PlacementSnapMode::MANUFACTURING_GRID:
            run_tcl_command("set_placement_snap_mode manufacturing");
            break;
        case PlacementSnapMode::SITE:
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

    void GuiProvider::set_hierarchy_depth(int depth)
    {
        run_tcl_command("set_hierarchy_depth " + std::to_string(depth));
    }

    void GuiProvider::set_view_flip(ViewFlip flip)
    {
        switch (flip)
        {
        case ViewFlip::NONE:
            run_tcl_command("set_view_flip none");
            break;
        case ViewFlip::HORIZONTAL:
            run_tcl_command("set_view_flip horizontal");
            break;
        case ViewFlip::VERTICAL:
            run_tcl_command("set_view_flip vertical");
            break;
        }
    }

    void GuiProvider::set_flightline_max_fanout(int max_fanout)
    {
        run_tcl_command("set_flightline_max_fanout " + std::to_string(max_fanout));
    }

    void GuiProvider::set_undo_limit(int limit)
    {
        if (limit >= 0)
            run_tcl_command("set_undo_limit " + std::to_string(limit));
    }

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

    namespace
    {
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
    void GuiProvider::set_confirm_overwrite(bool confirm) { run_tcl_command(confirm ? "set_confirm_overwrite true" : "set_confirm_overwrite false"); }

    void GuiProvider::set_theme(size_t theme)
    {
        if (theme < le::kThemes.size())
            run_tcl_command("set_theme " + std::string(le::kThemes[theme].name));
    }
    void GuiProvider::set_max_concurrency(int threads) { run_tcl_command("set_max_concurrency " + std::to_string(threads)); }
    void GuiProvider::set_label_min_size(double px) { run_tcl_command("set_label_min_size " + tcl_number(px)); }
    void GuiProvider::set_label_max_size(double px) { run_tcl_command("set_label_max_size " + tcl_number(px)); }
    void GuiProvider::save_settings(const std::string &path) { run_tcl_command(path.empty() ? "save_settings" : "save_settings " + tcl_quote(path)); }
    void GuiProvider::load_settings(const std::string &path) { run_tcl_command(path.empty() ? "load_settings" : "load_settings " + tcl_quote(path)); }

    bool GuiProvider::has_unsaved_design() const { return ReadView(handle_).has_unsaved_database(); }
    bool GuiProvider::has_unsaved_settings() const { return ReadView(handle_).has_unsaved_settings(); }
    bool GuiProvider::has_unsaved_changes() const
    {
        const ReadView view(handle_);
        return view.has_unsaved_database() || view.has_unsaved_settings();
    }
    bool GuiProvider::save_settings_now() { return WriteView(handle_).save_settings().has_value(); }

    bool GuiProvider::write_db_now(const std::string &path) { return !path.empty() && WriteView(handle_).write_db(path, true).has_value(); }

    void GuiProvider::read_db(const std::string &path) { run_tcl_command("read_db " + tcl_quote(path)); }
    void GuiProvider::write_db(const std::string &path) { run_tcl_command("write_db " + tcl_quote(path)); }
    std::string GuiProvider::db_path() const { return ReadView(handle_).db_path(); }
    bool GuiProvider::database_is_empty() const { return ReadView(handle_).database_is_empty(); }

    void GuiProvider::set_layer_color(const std::string &layer_name, Color color)
    {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", color.r, color.g, color.b);
        run_tcl_command("set_layer_color " + tcl_quote(layer_name) + " " + hex);
    }

    void GuiProvider::reset_layer_color(const std::string &layer_name)
    {
        run_tcl_command("reset_layer_color " + tcl_quote(layer_name));
    }

    void GuiProvider::set_layer_visible(const std::string &layer_name, bool value)
    {
        run_tcl_command("set_layer_visible " + tcl_quote(layer_name) + " " + (value ? "true" : "false"));
    }

    void GuiProvider::set_layer_selectable(const std::string &layer_name, bool value)
    {
        run_tcl_command("set_layer_selectable " + tcl_quote(layer_name) + " " + (value ? "true" : "false"));
    }

    void GuiProvider::set_purpose_visible(std::string_view purpose_name, bool value)
    {
        run_tcl_command("set_purpose_visible " + tcl_quote(std::string(purpose_name)) + " " + (value ? "true" : "false"));
    }

    void GuiProvider::set_purpose_selectable(std::string_view purpose_name, bool value)
    {
        run_tcl_command("set_purpose_selectable " + tcl_quote(std::string(purpose_name)) + " " + (value ? "true" : "false"));
    }

    namespace
    {
        // The <noun> of a filter's set_<noun>_visible/_selectable.
        const char *object_filter_command_noun(ObjectFilter filter)
        {
            switch (filter)
            {
            case ObjectFilter::PLACEMENT_TYPE:
                return "placement_type";
            case ObjectFilter::PLACEMENT_STATUS:
                return "placement_status";
            case ObjectFilter::ROUTING_STATUS:
                return "routing_status";
            case ObjectFilter::ROUTE_SHAPE:
                return "route_shape";
            case ObjectFilter::ROUTE_USE:
                break;
            }
            return "route_use";
        }
    }

    void GuiProvider::set_object_filter_visible(ObjectFilter filter, const std::string &value, bool visible)
    {
        run_tcl_command(std::string("set_") + object_filter_command_noun(filter) + "_visible " + tcl_quote(value) + " " + (visible ? "true" : "false"));
    }

    void GuiProvider::set_object_filter_selectable(ObjectFilter filter, const std::string &value, bool selectable)
    {
        run_tcl_command(std::string("set_") + object_filter_command_noun(filter) + "_selectable " + tcl_quote(value) + " " + (selectable ? "true" : "false"));
    }

    void GuiProvider::run_tcl_command(const std::string &script)
    {
        SessionRequests(handle_).enqueue_tcl_command(script);
    }

    void GuiProvider::set_mouse_position(int x, int y) { WriteView(handle_).set_mouse_position(x, y); }
    void GuiProvider::clear_mouse_position() { WriteView(handle_).clear_mouse_position(); }
    void GuiProvider::mouse_down(int x, int y) { WriteView(handle_).mouse_down(x, y); }
    void GuiProvider::mouse_up(int x, int y) { WriteView(handle_).mouse_up(x, y); }
    void GuiProvider::cancel_drag() { WriteView(handle_).cancel_drag(); }
    void GuiProvider::zoom(double factor, int x, int y) { WriteView(handle_).zoom(factor, x, y); }
    void GuiProvider::zoom_drag_down(int x, int y) { WriteView(handle_).zoom_drag_down(x, y); }
    void GuiProvider::key_down(Key key) { WriteView(handle_).key_down(key); }
    void GuiProvider::key_up(Key key) { WriteView(handle_).key_up(key); }
    void GuiProvider::clear_all_keys() { WriteView(handle_).clear_keys(); }
    void GuiProvider::set_viewport_size(int width_px, int height_px) { WriteView(handle_).set_viewport_size(width_px, height_px); }
}
