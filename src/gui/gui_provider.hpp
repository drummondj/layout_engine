#pragma once

#include "views.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace le::gui
{
    /// @brief `text` as one Tcl word: double-quoted, with every character
    /// Tcl substitutes inside quotes escaped. For any name or path put into
    /// a queued Tcl command.
    std::string tcl_quote(const std::string &text);

    // The GUI's one contact with the session: no other file in src/gui/
    // opens a view or touches the LeHandle (le_gui.cpp's render thread and
    // its show/close polling aside, which use Renderer and SessionRequests).
    // Fits Dear ImGui's redraw-every-frame model: refresh() reads what every
    // frame needs into State under one ReadView, so a frame sees one
    // snapshot; the on-demand reads below each open their own. Never copied
    // or moved: one per open window, passed around as GuiProvider&.
    //
    // Holding a view on this thread while opening another deadlocks, so
    // nothing here hands one out: every member opens and closes its own.
    class GuiProvider
    {
    public:
        explicit GuiProvider(LeHandle *handle) noexcept : handle_(handle) {}

        GuiProvider(const GuiProvider &) = delete;
        GuiProvider &operator=(const GuiProvider &) = delete;
        GuiProvider(GuiProvider &&) = delete;
        GuiProvider &operator=(GuiProvider &&) = delete;

        // One row of State::LayerManager, built by refresh(). Copies: the
        // console thread can rebuild the layer rows while a frame uses them.
        struct LayerRow
        {
            std::string name;
            Color color;
            bool visible;
            bool selectable;
        };
        struct PurposeRow
        {
            ViewLayerPurpose purpose;
            const char *name; // the generated label: static storage, null-terminated
            bool visible;
            bool selectable;
            bool has_selectable_objects; // false: no selectable checkbox
        };
        // One Placement.type, Route.use, ... value under its purpose row.
        struct FilterRow
        {
            std::string value;
            bool visible;
            bool selectable;
        };

        // What components read every frame, whatever is expanded or open.
        // Grouped by the component that reads it; values more than one
        // reads stay top-level.
        struct State
        {
            InteractionMode mode = InteractionMode::SELECT;
            bool is_rendering = false;
            bool is_command_running = false; // a Tcl command holds renders
            bool is_move_armed = false;
            bool is_resize_armed = false;
            // The Edit-mode Delete button's targets; refreshed in Edit mode only.
            size_t selected_shape_piece_count = 0;

            struct StatusBar
            {
                std::string tooltip_message;
                // The snapped mouse position in microns, as the user sees
                // it (relative to the view's origin, mirrored as it is).
                std::optional<PointUm> snapped_mouse_position;
                size_t selection_count = 0;
            } status_bar;

            struct LayerManager
            {
                std::vector<LayerRow> layers; // Technology layers only
                std::vector<PurposeRow> purposes;
                std::vector<FilterRow> placement_types;    // under PLACEMENT
                std::vector<FilterRow> placement_statuses; // under PLACEMENT
                std::vector<FilterRow> route_uses;         // under ROUTE
                std::vector<FilterRow> routing_statuses;   // under ROUTE
                std::vector<FilterRow> route_shapes;       // under ROUTE
            } layer_manager;

            // settings_panel.cpp. Grid spacing is in um, <= 0 while unknown
            // (no Technology yet).
            struct Settings
            {
                double minor_grid_um = -1.0;
                double major_grid_um = -1.0;
                double manufacturing_grid_um = 0.0; // 0: none
                double ruler_label_size_px = 0.0;
                double label_min_size_px = 0.0;
                double label_max_size_px = 0.0;
                int hierarchy_depth = 0;
                int flightline_max_fanout = 0;
                size_t undo_limit = 0;
                int max_concurrency = 0; // "CPUs"
                bool confirm_overwrite = true; // ask before a Save overwrites a design file
                size_t theme = 0;              // index into le::kThemes
            } settings;

            // The Settings panel's Database Settings: view state saved with
            // the design's session (write_db), not in settings.json.
            struct DatabaseSettings
            {
                ViewFlip view_flip = ViewFlip::NONE;
            } database_settings;

            // secondary_toolbar.cpp's placement toolbar: shown in Edit mode
            // while selected_count > 0; the rest is only refreshed then.
            // snap_available is indexed by PlacementSnapMode,
            // orientation_ops_enabled a (1 << OrientationOp) bitmask.
            struct PlacementMove
            {
                size_t selected_count = 0;
                PlacementSnapMode snap_mode = PlacementSnapMode::SITE;
                std::array<bool, 4> snap_available = {true, false, false, false};
                uint32_t orientation_ops_enabled = 0;
                bool is_move_anchored = false;
                bool all_fixed = false;   // every selected placement is FIXED
                size_t locked_count = 0; // selected FIXED/COVER placements, which Move refuses
            } placement_move;

            // secondary_toolbar.cpp's resize toolbar (while Resize is armed)
            // and Move's path/via snap toolbar (while Move is armed with
            // paths or vias selected); the snap settings are only refreshed
            // then. Arrays are indexed by PieceKind, RECT..VIA, then by
            // ShapeSnapMode.
            struct Resize
            {
                std::optional<ResizeAxis> hover_axis; // le_gui.cpp's resize cursor
                uint32_t selected_piece_kinds = 0;    // (1 << PieceKind), while Resize is armed
                uint32_t move_snap_piece_kinds = 0;   // (1 << PieceKind), while Move is armed
                std::array<ShapeSnapMode, 4> snap_modes = {ShapeSnapMode::USER_GRID, ShapeSnapMode::USER_GRID, ShapeSnapMode::USER_GRID,
                                                           ShapeSnapMode::USER_GRID};
                std::array<std::array<bool, 5>, 4> snap_available = {};
            } resize;
        };

        /// @brief Re-reads state() under one ReadView. Call once per frame,
        /// before any component reads state().
        void refresh();
        const State &state() const { return state_; }

        // On-demand reads, for what a component needs only while something
        // is expanded or open. Each takes one ReadView.

        /// @brief The `index`th selected object: a piece as its Shape or
        /// Wire, or the Row, Placement or Region.
        std::optional<ObjectId> selected_object(size_t index) const;
        /// @brief `id` and its owners, the outermost first; empty if `id`
        /// doesn't exist.
        std::vector<ObjectId> ancestry(const ObjectId &id) const;
        /// @brief `id`'s property table, as Tcl shows it (lengths in microns).
        std::vector<PropertyValue> properties(const ObjectId &id) const;
        /// @brief The children the Property Viewer links to: a Library's
        /// Designs, a Design's Abstract, an Abstract's Terminals,
        /// Obstructions and free Shapes, ... (never a Layout's placements or
        /// routes, which can number millions).
        std::vector<ObjectId> children(const ObjectId &id) const;
        /// @brief Each id's Tcl-style friendly id: `kind:name` for a class
        /// named by its name, else `kind:<generation << 32 | index>`. For
        /// display only.
        std::vector<std::string> tokens(const std::vector<ObjectId> &ids) const;

        // The Browser's tree.
        struct DesignEntry
        {
            DesignId id;
            std::string name;
            bool has_abstract = false;
            bool has_layout = false;
        };
        struct LibraryEntry
        {
            std::string name;
            std::vector<DesignEntry> designs;
        };
        /// @brief Every Library and its Designs, in database order; re-read
        /// only when the database has changed.
        const std::vector<LibraryEntry> &libraries();

        // --- Actions ---

        // Opens `design`'s Abstract/Layout and fits the view to it: direct
        // calls, not queued Tcl commands.
        void open_design_abstract(DesignId design);
        void open_design_layout(DesignId design);

        void set_mode(InteractionMode mode);
        void deselect_all();
        void arm_move();
        void arm_resize();
        void delete_selected_pieces();
        // Lock: makes the selected placements FIXED; unlock: PLACED.
        void set_selected_placements_fixed(bool fixed);
        void set_shape_snap_mode(PieceKind kind, ShapeSnapMode mode);
        void set_placement_snap_mode(PlacementSnapMode mode);
        void rotate_placement();
        void flip_placement_horizontal();
        void flip_placement_vertical();
        void undo();
        void redo();
        void clear_rulers();
        void set_hierarchy_depth(int depth);
        void set_view_flip(ViewFlip flip);
        void set_flightline_max_fanout(int max_fanout);
        void set_undo_limit(int limit);
        // Settings panel: a spacing <= 0 is left unchanged; an empty path
        // means the default settings file.
        void set_grid_spacing_um(double minor_um, double major_um);
        void set_ruler_label_size(double px);
        void set_max_concurrency(int threads);
        void set_label_min_size(double px);
        void set_label_max_size(double px);
        void set_confirm_overwrite(bool confirm);
        // `theme` indexes le::kThemes.
        void set_theme(size_t theme);
        void save_settings(const std::string &path);
        // The close dialog. Direct calls, not queued Tcl commands: the
        // dialog needs the answer this frame, and a batch-mode le_shell
        // never drains that queue.
        bool has_unsaved_design() const;
        bool has_unsaved_settings() const;
        // Either of the above: File > Exit asks first only if so.
        bool has_unsaved_changes() const;
        bool save_settings_now();
        // Writes the design to `path` now, with its session (the close
        // dialog's saves); false if the save failed.
        bool write_db_now(const std::string &path);
        // The File menu: read_db/write_db, queued as Tcl commands so they
        // land in the console's history; db_path() is the .led file last
        // read or written ("" if none), and read_db only loads into an
        // empty database.
        void read_db(const std::string &path);
        void write_db(const std::string &path);
        std::string db_path() const;
        bool database_is_empty() const;
        // The Settings panel's "Reset window layout" asks le_gui.cpp (a
        // GUI-only concern, no session state) to rebuild the default dock
        // layout next frame.
        void request_window_layout_reset() { window_layout_reset_requested_ = true; }
        bool take_window_layout_reset_request() { return std::exchange(window_layout_reset_requested_, false); }
        void load_settings(const std::string &path);
        void set_layer_visible(const std::string &layer_name, bool value);
        void set_layer_selectable(const std::string &layer_name, bool value);
        // The Layers panel's color picker.
        void set_layer_color(const std::string &layer_name, Color color);
        void reset_layer_color(const std::string &layer_name);
        void set_purpose_visible(std::string_view purpose_name, bool value);
        void set_purpose_selectable(std::string_view purpose_name, bool value);
        void set_object_filter_visible(ObjectFilter filter, const std::string &value, bool visible);
        void set_object_filter_selectable(ObjectFilter filter, const std::string &value, bool selectable);

        /// @brief Queues `script` for the console thread, so it lands in
        /// the command history and undo as if typed. The actions above that
        /// go through Tcl build their command and call this; the Layers
        /// panel's "All ..." toggles join one script per click so a bulk
        /// toggle is one history entry.
        void run_tcl_command(const std::string &script);

        // Input forwarding and the viewport size, from le_gui.cpp's frame
        // loop. Each takes the session's lock exclusively, so le_gui.cpp
        // skips them while a render runs.
        void set_mouse_position(int x, int y);
        void clear_mouse_position();
        void mouse_down(int x, int y);
        void mouse_up(int x, int y);
        void cancel_drag(); // Escape during a drag
        void zoom(double factor, int x, int y);
        void zoom_drag_down(int x, int y);
        void key_down(Key key);
        void key_up(Key key);
        void clear_all_keys();
        void set_viewport_size(int width_px, int height_px);

    private:
        LeHandle *handle_;
        bool window_layout_reset_requested_ = false;
        State state_;
        std::vector<LibraryEntry> libraries_;
        std::optional<uint64_t> libraries_version_; // the mutation version libraries_ was read at
    };
}
