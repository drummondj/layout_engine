#pragma once

#include "api.hpp"

#include <string>
#include <vector>

namespace le::gui
{
    // The single point of contact between this whole module (le_gui.cpp's
    // own per-frame loop and every src/gui/components/*.cpp file) and the
    // C API (api.hpp)/LeHandle - no other file in src/gui/ may call a
    // le_* function or touch a LeHandle directly once a component has
    // been migrated to take a GuiProvider& instead (see each component's
    // own git history for the "before" shape). Mirrors the pre-existing
    // Flutter frontend's own LeProvider (frontend/lib/providers/le_provider.dart,
    // recoverable via `git show afbafa1:frontend/lib/providers/le_provider.dart`
    // - afbafa1 is the last commit before that frontend's removal in
    // 9f1fd76), adapted for Dear ImGui's immediate-mode redraw-every-frame
    // model rather than Flutter's retained-mode notifyListeners() one -
    // see State's own doc comment for exactly what that adaptation means
    // and why only *some* of what LeProvider cached belongs in State here.
    //
    // Wraps a single non-owning LeHandle* - never copyable or movable
    // (constructed once per open_and_run_window() call, in le_gui.cpp,
    // and passed everywhere else as GuiProvider&; there's no case in this
    // module where a second instance aliasing the same handle, or moving
    // this one, means anything sensible). Deliberately has no raw
    // handle()/LeHandle* accessor - that would be a standing loophole
    // around the entire point of this class existing.
    class GuiProvider
    {
    public:
        explicit GuiProvider(LeHandle *handle) noexcept : handle_(handle) {}

        GuiProvider(const GuiProvider &) = delete;
        GuiProvider &operator=(const GuiProvider &) = delete;
        GuiProvider(GuiProvider &&) = delete;
        GuiProvider &operator=(GuiProvider &&) = delete;

        // One row of State::LayerManager::layers/purposes - the same
        // shape layer_manager.cpp's own local LayerEntry/PurposeEntry
        // structs held before this class existed, just relocated here so
        // refresh() can build them once per frame instead of that
        // component rebuilding them itself every frame.
        struct LayerRow
        {
            LeLayerRow row;
            bool visible;
            bool selectable;
        };
        struct PurposeRow
        {
            int32_t ordinal;
            bool visible;
            bool selectable;
        };

        // Ambient state - read in full, unconditionally, by one or more
        // components every frame today regardless of any expand/collapse
        // UI state (unlike e.g. library_browser.cpp's library->design
        // tree, only walked for an *expanded* node, or property_viewer.cpp's
        // per-ref queries, which can address several different refs in
        // one frame - neither belongs here, see the parameterized
        // passthrough methods below instead). refresh() populates this
        // fresh from the live API every time it's called; every component
        // then reads state() for the remainder of that frame. Grouped
        // into nested structs per the component(s) that primarily read
        // each group - shared scalars (mode/is_rendering/is_move_armed)
        // stay top-level since more than one component reads each.
        struct State
        {
            int32_t mode = LE_MODE_SELECT;
            bool is_rendering = false;
            bool is_move_armed = false;
            bool is_resize_armed = false;

            struct StatusBar
            {
                std::string tooltip_message;
                LeSnappedMousePosition snapped_mouse_position{};
                int32_t selection_count = 0;
            } status_bar;

            struct LayerManager
            {
                int32_t hierarchy_depth = 0;
                std::vector<LayerRow> layers;
                std::vector<PurposeRow> purposes;
            } layer_manager;

            // secondary_toolbar.cpp's placement toolbar
            // (NEW_FEATURES_SEPT_2026.md item 2) - shown in Edit mode
            // while selected_count > 0; the rest is only refreshed then.
            // snap_available is LePlacementSnapMode-indexed,
            // orientation_ops_enabled a (1 << LeOrientationOp) bitmask.
            struct PlacementMove
            {
                int32_t selected_count = 0;
                int32_t snap_mode = LE_PLACEMENT_SNAP_SITE;
                bool snap_available[4] = {true, false, false, false};
                int32_t orientation_ops_enabled = 0;
                bool is_move_anchored = false;
            } placement_move;

            // secondary_toolbar.cpp's resize toolbar (NEW_FEATURES_SEPT_2026.md
            // item 3) - shown while Resize is armed; only refreshed then.
            // Arrays are LePieceKind-indexed (then LeShapeSnapMode-indexed).
            struct Resize
            {
                int32_t hover_axis = LE_RESIZE_AXIS_NONE; // le_gui.cpp's resize cursor
                int32_t selected_piece_kinds = 0;
                int32_t snap_modes[3] = {LE_SHAPE_SNAP_USER_GRID, LE_SHAPE_SNAP_USER_GRID, LE_SHAPE_SNAP_USER_GRID};
                bool snap_available[3][5] = {};
            } resize;
        };

        // Re-populates state() from the live API - call exactly once per
        // frame (le_gui.cpp's main loop, right where is_rendering used to
        // be read directly), before any component reads state() that
        // frame.
        void refresh();
        const State &state() const { return state_; }

        // --- Parameterized/on-demand passthroughs - deliberately never
        // folded into State (see State's own doc comment above for why:
        // no bounded set to precompute, and this is exactly where a real,
        // reproduced GUI-thread freeze bug lived before - see
        // would_block_property_lookup's own comment). ---

        LeObjectRef object_parent(LeObjectRef ref) const;
        LeObjectRef selected_object_ref(int32_t selection_index) const;
        int32_t object_property_count(LeObjectRef ref) const;
        LeProperty object_property_at(LeObjectRef ref, int32_t index) const;
        // Every child of `ref`, one call covering whatever LeObjectKind
        // switch used to live in property_viewer.cpp's own object_children
        // free function - moved here verbatim, including its one
        // remaining exception (LE_OBJECT_KIND_DESIGN, still on the older
        // le_get_abstracts/le_search_result_abstract_at search surface
        // pending a generated is_child accessor, gated on state().is_rendering
        // directly rather than would_block_property_lookup() since it
        // isn't going through the per-ref property cache at all).
        std::vector<LeObjectRef> object_children(LeObjectRef ref) const;
        // Whether calling object_property_count/_at(ref) right now risks
        // blocking the whole GUI thread behind an in-progress render -
        // true only when a render is actually in flight AND ref isn't
        // already what the single-slot property cache holds (a stable,
        // already-cached ref stays safe regardless of how long a
        // viewport-only render takes). A real, reproduced freeze bug
        // (property_viewer.cpp's breadcrumb trail and child-link rows
        // each query a *different* ref through that one-slot cache every
        // frame; a thrash-induced rebuild landing mid-render blocked this
        // whole thread, not just one panel) - callers show a placeholder
        // instead of calling object_property_count/_at when this is true.
        bool would_block_property_lookup(LeObjectRef ref) const;
        LeObjectRef invalid_ref() const;

        int32_t library_count() const;
        LeLibraryInfo library_at(int32_t index) const;
        int32_t library_design_count(int32_t library_index) const;
        LeDesignInfo library_design_at(int32_t library_index, int32_t design_index) const;

        // --- Actions ---

        // Opens design_id's Abstract/Layout view - each a direct,
        // low-latency call (not routed through Tcl, unlike most actions
        // below), matching the Flutter frontend's own LeProvider.openDesign/
        // openDesignLayout. Each bundles a set-current-design call with an
        // immediate le_fit_scene(10) so the newly opened view starts
        // framed on its own content.
        void open_design_abstract(LeDesignId design_id);
        void open_design_layout(LeDesignId design_id);

        void set_mode(int32_t mode);
        void select_all();
        void deselect_all();
        void arm_move();
        void arm_resize();
        void set_shape_snap_mode(int32_t kind, int32_t mode);
        void set_placement_snap_mode(int32_t mode);
        void rotate_placement();
        void flip_placement_horizontal();
        void flip_placement_vertical();
        void undo();
        void redo();
        void clear_rulers();
        void set_hierarchy_depth(int32_t depth);
        void set_layer_visible(const std::string &layer_name, bool value);
        void set_layer_selectable(const std::string &layer_name, bool value);
        void set_purpose_visible(const std::string &purpose_name, bool value);
        void set_purpose_selectable(const std::string &purpose_name, bool value);

        // The single call site for le_enqueue_tcl_command anywhere in
        // this module - every named action method above that goes
        // through Tcl builds its own command string and calls this
        // internally. Also exposed publicly as a deliberate, narrow
        // escape hatch for layer_manager.cpp's own "All ..." aggregate-
        // row toggles, which build one semicolon-joined multi-statement
        // script covering every row client-side (unchanged) so a bulk
        // toggle still lands as one command-history entry, not one per
        // row - mirrors the Flutter frontend's own LeProvider.setAllLayersVisible/
        // etc., the only LeProvider action methods that called
        // runTclCommand directly instead of a narrower wrapper.
        void run_tcl_command(const std::string &script);

        // --- App-shell / input forwarding - one-shot commands, not
        // state, called every frame from le_gui.cpp's own main loop
        // (mouse/keyboard forwarding, viewport resize) - included here
        // (not left as direct le_* calls) to match the Flutter frontend's
        // own LeProvider.handlePointerEvent/handleKeyEvent/resize, which
        // wrapped the exact same calls on the Provider itself, not just
        // widget-level state. le_wait_for_render_needed/le_render_pixel_buffer/
        // le_cancel_render_wait (le_gui.cpp's own background render
        // thread) and le_take_show_gui_request (run_main_thread_loop's
        // idle poll, before a GuiProvider even exists) are deliberately
        // NOT here - neither has a per-frame "component" shape, and
        // le_wait_for_render_needed/le_render_pixel_buffer specifically
        // run on a different thread than every other method on this
        // class is ever called from. ---
        void set_mouse_position(int32_t x, int32_t y);
        void clear_mouse_position();
        void mouse_down(int32_t x, int32_t y);
        void mouse_up(int32_t x, int32_t y);
        void zoom(double factor, int32_t x, int32_t y);
        void zoom_drag_down(int32_t x, int32_t y);
        void key_down(int32_t key_code);
        void key_up(int32_t key_code);
        void clear_all_keys();
        void set_viewport_size(int32_t width_px, int32_t height_px);

    private:
        // le_object_property_cache_current's own doc comment (api.hpp) -
        // shared_lock-only, never blocks. Kept private: nothing outside
        // would_block_property_lookup needs it directly.
        bool property_cache_current(LeObjectRef ref) const;

        LeHandle *handle_;
        State state_;
    };
}
