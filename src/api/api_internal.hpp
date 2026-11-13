#pragma once

// Helpers the C API's translation units (api.cpp, input.cpp,
// selection.cpp, session.cpp, settings.cpp, view_options.cpp) share.
// Each is defined in the TU of its concern. An `_unlocked` helper expects
// the caller to hold the handle's lock already.

#include "api.hpp"
#include "le_handle.hpp"

#include "../database/database.hpp"
#include "../pipelines/pipeline_options.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace le::api_detail
{
    // --- api.cpp ---

    /// @brief Logs the exception being handled. The entry points that parse,
    /// read, write, link or render catch everything with it, so no exception
    /// crosses the C boundary.
    void log_exception(const char *function) noexcept;

    /// @brief Copies `value` into `function`'s string for this thread: valid
    /// until that function's next call on this thread.
    const char *hold_string(std::string_view function, std::string_view value);

    /// @brief The Technology's dbu per micron; nullopt if none has been read
    /// yet or it has no usable scale.
    std::optional<double> database_units_microns(const le::Root &root);

    /// @brief Microns to dbu, rounded to the nearest dbu.
    int64_t to_dbu(double value_um, double dbu_per_um);

    /// @brief database_units_microns, or 1.0 without a usable Technology.
    double display_dbu_per_um(const le::Root &root);

    /// @brief le_undo/le_redo's result: 1 if replayed, 0 if there was
    /// nothing to replay, -1 if some steps couldn't be applied (logged).
    int32_t report_replay(LeHandle *handle, const le::editing::ReplayResult &result, std::string_view verb);

    inline LeObjectRef invalid_object_ref()
    {
        return LeObjectRef{.kind = LE_OBJECT_KIND_LIBRARY, .index = UINT32_MAX, .generation = 0};
    }

    // A ref's `index`/`generation` pair is its class's LeXxxId fields, so
    // converting is a bare field copy.
    template <typename IdT>
    IdT id_from_ref(LeObjectRef ref)
    {
        return IdT{.index = ref.index, .generation = ref.generation};
    }

    template <typename IdT>
    LeObjectRef ref_from_id(LeObjectKind kind, IdT id)
    {
        return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation};
    }

    // --- view_options.cpp ---

    /// @brief Rebuilds handle->view_layers for `technology_id`.
    void rebuild_view_layers(LeHandle *handle, le::TechnologyId technology_id);

    /// @brief Rebuilds handle->view_layers if Root has changed since it was
    /// last built.
    void ensure_view_layers_current(LeHandle *handle);

    /// @brief Whether handle->view_layers is up to date, without rebuilding.
    bool view_layers_already_current(const LeHandle *handle);

    // The GUI's per-frame layer-list reads (le_layer_count/_at,
    // le_purpose_count/_at): current view_layers under a shared_lock when
    // they're up to date; otherwise rebuild them under the write lock - but
    // only if it's free right now. A render in progress holds a shared_lock
    // for its whole (possibly multi-second) run, and waiting for the write
    // lock behind it froze the GUI thread for all of it - no frames, so no
    // progress spinner, whenever a render followed any database change
    // (e.g. the first render after read_def). Then the current, slightly
    // stale layer list is read instead, and a later frame rebuilds it. No
    // notify_render_needed(): the render graph builds its own layer set.
    template <typename Read>
    auto read_view_layers(LeHandle *handle, Read read)
    {
        {
            std::shared_lock<std::shared_mutex> read_lock(handle->mutex_);
            if (view_layers_already_current(handle))
                return read(handle->view_layers);
        }
        std::unique_lock<std::shared_mutex> write_lock(handle->mutex_, std::try_to_lock);
        if (write_lock.owns_lock())
        {
            ensure_view_layers_current(handle);
            return read(handle->view_layers);
        }
        std::shared_lock<std::shared_mutex> read_lock(handle->mutex_);
        return read(handle->view_layers);
    }

    /// @brief The Technology's dbu per micron, 0.0 if unavailable.
    double technology_dbu_per_um(const le::Root &root);

    /// @brief A ViewRenderOptions snapshot of the handle's root and view
    /// state, for le_render_pixel_buffer.
    le::ViewRenderOptions view_render_options_for(const LeHandle *handle);

    void zoom_unlocked(LeHandle *handle, double factor, int32_t x, int32_t y);
    void pan_unlocked(LeHandle *handle, double x_factor, double y_factor);
    /// @brief -1 on an axis the view mirrors, else 1.
    double screen_x_sign(const LeHandle *handle);
    double screen_y_sign(const LeHandle *handle);
    void fit_scene_unlocked(LeHandle *handle, int32_t padding_px);
    /// @brief Fits the view to the current selection; a no-op with nothing selected.
    void fit_selected_unlocked(LeHandle *handle, int32_t padding_px);
    /// @brief The dbu point shown to the user as (0, 0).
    std::array<double, 2> display_origin_unlocked(const LeHandle *handle);
    /// @brief Where dbu point `p` is shown to the user, in dbu.
    std::array<double, 2> display_point_unlocked(const LeHandle *handle, le::Point p);

    // --- selection.cpp ---

    /// @brief A selected piece's drawable geometry: its own one-piece Shape,
    /// or for a via or via array, its expanded geometry.
    le::ShapeData drawable_piece(const le::Root &root, const le::ShapeData &data, le::PieceKind kind, size_t index, le::LayoutId layout_id);

    /// @brief One selected object's outline geometry in dbu, for the
    /// selection overlay.
    std::optional<le::ShapeData> resolve_selected_outline(const LeHandle *handle, const LeHandle::SelectedObject &selected, int remaining_depth);

    /// @brief Every PlacementId in the current selection, in selection order.
    std::vector<le::PlacementId> selected_placements_unlocked(const LeHandle *handle);

    /// @brief le_mouse_up's Select-mode click at dbu `p`.
    void click_select_unlocked(LeHandle *handle, le::Point p, const std::vector<LeHandle::SelectedObject> &previous, bool shift);
    /// @brief le_mouse_up's Select-mode drag-select, released at pixel (x, y).
    void select_in_abstract_view_unlocked(LeHandle *handle, int32_t x, int32_t y);
    void select_in_layout_view_unlocked(LeHandle *handle, int32_t x, int32_t y);

    // --- input.cpp ---

    /// @brief Where each of `placements` would land for a Placement Move by
    /// `raw_delta` right now.
    std::vector<le::PlacementMoveTarget> plan_moving_placements_unlocked(const LeHandle *handle, const std::vector<le::PlacementId> &placements, le::Point raw_delta);

    /// @brief Whether Move snaps a `kind` piece on its own rather than by the
    /// shared user-grid delta.
    bool snaps_individually_when_moved(le::PieceKind kind);

    /// @brief The delta each moving piece moves by right now; nullopt until
    /// anchored.
    std::optional<std::vector<le::Point>> moving_piece_deltas_unlocked(const LeHandle *handle);

    // One Shape or Wire a resize changes - the grabbed piece's own, plus
    // (for a path segment) any sibling whose runs follow its endpoints -
    // and the changed pieces' geometry, for the ghost.
    struct ResizeEdit
    {
        le::GeometryId owner;
        le::ShapeData before;
        le::ShapeData after;
        std::vector<le::ShapeData> ghost_pieces;
    };

    /// @brief Every Shape the current Resize grab would change if committed
    /// at dbu `current`.
    std::vector<ResizeEdit> plan_resize_unlocked(const LeHandle *handle, le::Point current);

    /// @brief Re-snapshots an armed Move's ghost geometry after undo/redo.
    void refresh_armed_move_geometry_unlocked(LeHandle *handle);

    // --- session.cpp ---

    /// @brief The session saved beside the database (a .led file's SESS
    /// chunk), as JSON.
    std::string session_to_json(const LeHandle *handle);

    /// @brief Applies a saved session; `path` is for messages.
    void apply_session_json(LeHandle *handle, const std::string &text, const std::string &path);

    // --- settings.cpp ---

    /// @brief Applies grid spacing (um) a settings file set before a
    /// Technology existed to convert it.
    void apply_pending_grid_um_unlocked(LeHandle *handle);

    // Unsaved-changes checks - see LeHandle::saved_mutation_version.
    std::string settings_snapshot(const LeHandle *handle);
    bool has_unsaved_database_unlocked(const LeHandle *handle);
    bool has_unsaved_settings_unlocked(const LeHandle *handle);

    // A read (LEF/DEF/Verilog) or link can fill in settings (grid spacing in
    // um) without the user changing any: settings clean before it stay
    // clean after it. Construct before; after a successful read commit(),
    // which leaves the design it read unsaved until write_db writes it;
    // after a failed one commit_failed(), which also keeps a clean design
    // clean - a failed read rolls back everything it added (ReadRollback).
    class CleanAcrossRead
    {
    public:
        explicit CleanAcrossRead(const LeHandle *handle)
            : database_clean_(!has_unsaved_database_unlocked(handle)), settings_clean_(!has_unsaved_settings_unlocked(handle)) {}

        void commit(LeHandle *handle) const
        {
            if (settings_clean_)
                handle->saved_settings_json = settings_snapshot(handle);
        }

        void commit_failed(LeHandle *handle) const
        {
            commit(handle);
            if (database_clean_)
                handle->saved_mutation_version = handle->root.mutation_version();
        }

    private:
        bool database_clean_;
        bool settings_clean_;
    };
}
