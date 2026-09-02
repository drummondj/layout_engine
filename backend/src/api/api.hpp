#pragma once
#include <stdbool.h>
#include <stdint.h>

// The C API surface a Flutter plugin's Dart FFI binds to (see README's
// `api` module entry and "Recommended build order" step 5 - the minimal
// slice: init/destroy, load-file, render_pixel_buffer only, before
// zoom/pan/selection grow into a real events module). Deliberately plain
// C (not C++) in every public declaration - no std:: types, no default
// arguments, no overloads - so this header parses cleanly for Dart's
// ffigen (or any other C FFI generator) and so LeHandle/LePixelBuffer have
// a stable, toolchain-independent ABI between this project's macOS dev
// machine and its Linux deployment target.
//
// Thread safety: every function below is safe to call concurrently from
// multiple threads on the *same* LeHandle, except le_destroy() (see its
// own doc comment) - each internally locks a mutex owned by the handle.
// This isn't defensive-but-unneeded caution: Flutter's own external-
// texture API calls le_render_pixel_buffer() from a dedicated raster
// thread once per frame, while ordinary pointer/FFI calls
// (le_set_mouse_position(), le_mouse_down()/up(), le_zoom(), ...) run on
// the platform thread - both reach the same handle. A real crash
// (concurrent, unsynchronized std::map mutation inside Pipeline) shipped
// before this was added.

#ifdef __cplusplus
extern "C"
{
#endif

    /// @brief Opaque handle to one editor instance: a Root (database),
    /// ViewLayerSet, Scene (view state), Pipeline, and Renderer, all with
    /// the "one instance per Scene-equivalent lifetime, reused across
    /// repeated calls" lifetime Pipeline/Renderer are designed around (see
    /// pipeline.hpp/render.hpp) - a fresh Pipeline/Renderer per call would
    /// defeat their internal caching entirely. Opaque so this header stays
    /// C-compatible; the real struct is defined only in api.cpp.
    typedef struct LeHandle LeHandle;

#include "generated_tcl/ids.inc"

    /// @brief Raw RGBA8888 pixel buffer, mirroring render::PixelBuffer but
    /// using explicit fixed-width types (not `int`/`size_t`, whose width
    /// isn't guaranteed identical across toolchains) for a stable FFI ABI.
    /// `data` points into memory owned by the LeHandle's Renderer - valid
    /// only until the next le_render_pixel_buffer() call on the same
    /// handle (or le_destroy()), never owned by the caller and never to be
    /// freed by it. Premultiplied alpha, row-major, top-to-bottom; row_bytes
    /// may exceed width * 4 - always index by it, never assume a tight
    /// stride (see render.hpp's PixelBuffer for the full format contract).
    typedef struct LePixelBuffer
    {
        const uint8_t *data;
        int32_t width;
        int32_t height;
        int64_t row_bytes;
    } LePixelBuffer;

    /// @brief One row of le_library_at(): a Library's identity and name.
    typedef struct LeLibraryInfo
    {
        LeLibraryId id;
        /// Owned by the handle's Root - valid until the handle is
        /// destroyed, never owned by the caller. Null if this row is
        /// invalid (out-of-range index or null handle).
        const char *name;
    } LeLibraryInfo;

    /// @brief One row of le_library_design_at(): a Design's identity
    /// (plus its parent Library's) and name.
    typedef struct LeDesignInfo
    {
        LeLibraryId library_id;
        LeDesignId id;
        /// Invalid (index == UINT32_MAX) if this Design has no Abstract
        /// view - every Design read via le_read_lef() has one, but the
        /// field degrades gracefully rather than assume that.
        LeAbstractId abstract_id;
        /// Invalid (index == UINT32_MAX) if this Design has no Layout
        /// view - a DEF-defined Design (le_read_def()) has one; a plain
        /// LEF macro (le_read_lef() only) does not (BUGS_AND_ENHANCEMENTS.md
        /// E15 - a Design can have either, both, or neither of these two
        /// independent views, same as Root::get_design_abstract/
        /// get_design_layout's own "may return an invalid id" contract).
        LeLayoutId layout_id;
        /// Owned by the handle's Root - valid until the handle is
        /// destroyed, never owned by the caller. Null if this row is
        /// invalid (out-of-range index or null handle).
        const char *name;
    } LeDesignInfo;

    /// @brief One row of le_layer_at(): a layer-visibility/selectability
    /// widget's row-header - a name plus a swatch color, *not necessarily*
    /// tied to a physical Layer existing - BOUNDARY is a row like any
    /// other, and any future non-Technology-derived ("extra") ViewLayer
    /// becomes a row the same way (see has_physical_layer below).
    /// Visibility/selectability are set by this name directly
    /// (le_set_layer_name_visible()/le_set_layer_name_selectable()), not
    /// by any id here - there's no per-row column list to address (see
    /// le_purpose_count()/le_purpose_at() for the other, row-independent
    /// "columns" axis).
    typedef struct LeLayerRow
    {
        /// Owned by the handle's Root - valid until the handle is
        /// destroyed, never owned by the caller. Null if this row is
        /// invalid (out-of-range index or null handle).
        const char *name;
        /// This row's own outline color, for a swatch next to its name -
        /// not the fill color or FillPattern, which draw_group already
        /// resolves per shape.
        uint8_t color_r;
        uint8_t color_g;
        uint8_t color_b;
        /// 1 if this row corresponds to a real Technology Layer (M1, V1,
        /// ...), 0 for a pseudo-row with no physical Layer of its own
        /// (ROW/BOUNDARY/GCELLGRID/PLACEMENT_BLOCKAGE/REGION) - each of
        /// those already has its own single-purpose entry in
        /// le_purpose_count()/le_purpose_at()'s own listing, so showing
        /// it *again* here as if it were a whole extra layer is a
        /// redundant, confusing duplicate for a layer-widget UI, not
        /// useful extra information (BUGS_AND_ENHANCEMENTS.md E12) -
        /// toggling either one already changes the exact same underlying
        /// visibility/selectability flag, since a pseudo-row has exactly
        /// one column.
        int32_t has_physical_layer;
    } LeLayerRow;

    /// @brief Result of le_snapped_mouse_position(): the current mouse
    /// position's coordinates, snapped to the minor grid (mirrors
    /// Scene::snapped_mouse_position - see UPDATES.md 5.3) and converted
    /// from dbu to microns via the Root's Technology::database_units_microns
    /// (e.g. 1000 dbu/um -> 3 decimal digits of representable precision;
    /// dividing the already-integral snapped dbu value by this is exact to
    /// the finest resolution the database itself supports - no additional
    /// rounding is meaningful beyond it). `has_position` is 0 (with
    /// x_um/y_um both 0) if no mouse position has been set yet (see
    /// le_set_mouse_position), the handle is null, or no Technology has
    /// been read yet (le_read_lef) to convert with - checked explicitly
    /// rather than a sentinel x_um/y_um value, since any coordinate
    /// including 0 is otherwise legitimate. Mirrors this project's single
    /// shared/global Technology assumption (see ViewLayerSet::
    /// build_for_technology's own caller in le_read_lef).
    typedef struct LeSnappedMousePosition
    {
        double x_um;
        double y_um;
        int32_t has_position;
    } LeSnappedMousePosition;

    /// @brief Allocate a new, empty editor instance (no LEF loaded, no
    /// Design selected, 0x0 viewport). Never returns null.
    LeHandle *le_create(void);

    /// @brief Destroy an instance created by le_create(). Safe to call
    /// with a null handle (no-op), matching free()'s convention. The one
    /// function this header's own thread-safety guarantee doesn't cover -
    /// the handle's mutex is destroyed along with everything else, so it
    /// can't protect this call itself. The caller must ensure no other
    /// thread is still calling into this handle when le_destroy() runs
    /// (the same standard contract as destroying any C++ object with
    /// live references elsewhere - not a new constraint this introduces).
    void le_destroy(LeHandle *handle);

    /// @brief Read a LEF file into this handle's shared Root, deriving a
    /// library name from the file's stem. Safe to call multiple times on
    /// the same handle - e.g. a tech file (LAYER definitions, no macros)
    /// followed by one or more macro files that reference those layers by
    /// name, matching LEFReader::read_lef's own existing-Technology reuse
    /// (pass the tech file first when a macro file depends on it - see
    /// render_preview.cpp for the same convention already used there).
    /// Returns 0 on success, matching LEFReader::read_lef's own result
    /// code (nonzero otherwise, including if handle or path is null).
    int le_read_lef(LeHandle *handle, const char *path);

    /// @brief Reads a DEF file into this handle's Root via DEFReader,
    /// same shape as le_read_lef (messages appended, 0 on success).
    /// Unlike le_read_lef, doesn't touch layer visibility defaults or
    /// rebuild ViewLayerSet - DEF doesn't introduce new physical Layers
    /// of its own (Step 2's own layer-generation work, not yet done, is
    /// what a Row/Track/Blockage/BOUNDARY-purpose rendering pass would
    /// need instead). Does still resolve/create the shared Technology
    /// (DEFReader::technology_id_) the same reuse-or-create way
    /// le_read_lef's own LEFReader does, since NONDEFAULTRULES needs one
    /// even when no LEF has been read into this handle yet.
    int le_read_def(LeHandle *handle, const char *path);

    /// @brief Total number of error/warning/info messages produced by
    /// this handle's backend operations so far (currently just
    /// le_read_lef() - file-open/parse errors, parser warnings, parser
    /// info notes, and a success summary, each already formatted with
    /// its own "ERROR "/"WARNING "/"INFO " prefix). Monotonically
    /// increasing - entries are never removed, cleared, or reordered -
    /// so a caller can poll this like le_selection_version() and only
    /// fetch le_message_at() for indices at or past what it last saw,
    /// rather than re-reading everything on every check. 0 if handle is
    /// null.
    int32_t le_message_count(LeHandle *handle);

    /// @brief The message at `index` (0..le_message_count()-1). Returns
    /// null if handle is null or index is out of range. The returned
    /// pointer is owned by the handle - valid until the handle is
    /// destroyed (messages are never removed/reordered, so unlike a
    /// "last error" design this pointer is never invalidated by a later
    /// le_read_lef() call).
    const char *le_message_at(LeHandle *handle, int32_t index);

    /// @brief Number of Designs currently loaded across every LEF file
    /// read into this handle so far. 0 if handle is null.
    int32_t le_design_count(LeHandle *handle);

    /// @brief Name of the Design at `index` (0..le_design_count()-1).
    /// Returns null if handle is null or index is out of range. The
    /// returned pointer is owned by the handle's Root - valid until the
    /// handle is destroyed (no Design-removal API exists yet), never
    /// owned by the caller.
    const char *le_design_name(LeHandle *handle, int32_t index);

    /// @brief Select the Design at `index` as the one le_render_pixel_buffer()
    /// renders (its Abstract view). Returns 0 on success, nonzero if
    /// handle is null or index is out of range - the current selection is
    /// left unchanged on failure.
    int le_set_current_design_abstract(LeHandle *handle, int32_t index);

    /// @brief Number of Libraries currently loaded - one per le_read_lef()
    /// call so far (see its own comment: each derives a fresh Library from
    /// its file's stem). 0 if handle is null. The top level of a
    /// Library -> Design -> Abstract browser widget; see
    /// le_library_design_count()/le_library_design_at() for the next level.
    int32_t le_library_count(LeHandle *handle);

    /// @brief The Library at `index` (0..le_library_count()-1). An
    /// all-invalid/null row (id.index == UINT32_MAX, name == null) if
    /// handle is null or index is out of range, rather than crashing.
    LeLibraryInfo le_library_at(LeHandle *handle, int32_t index);

    /// @brief Number of Designs belonging to the Library at
    /// `library_index` (into the same enumeration le_library_at() uses).
    /// 0 if handle is null or library_index is out of range.
    int32_t le_library_design_count(LeHandle *handle, int32_t library_index);

    /// @brief The Design at `design_index` within the Library at
    /// `library_index` (0..le_library_design_count(library_index)-1). An
    /// all-invalid/null row if handle is null or either index is out of
    /// range, rather than crashing.
    LeDesignInfo le_library_design_at(LeHandle *handle, int32_t library_index, int32_t design_index);

    /// @brief Select a Design by its stable LeDesignId (e.g. one read from
    /// le_library_design_at()'s LeDesignInfo::id) as the one
    /// le_render_pixel_buffer() renders, same effect as
    /// le_set_current_design_abstract() but addressed by identity instead of a
    /// position in the flat le_design_count() list - the natural fit for a
    /// browser widget's row click, which already has the Design's
    /// LeDesignId on hand and shouldn't need to re-derive a flat index for
    /// it. Returns 0 on success, nonzero if handle is null or design_id
    /// doesn't name a Design currently loaded on this handle - the current
    /// selection is left unchanged on failure.
    int le_set_current_design_abstract_by_id(LeHandle *handle, LeDesignId design_id);

    /// @brief Select the Design at `index`'s Layout view instead of its
    /// Abstract view - the two are mutually exclusive, only one view is
    /// "open" at a time (matches a real GUI showing one editor, not
    /// both). Clears current_abstract_id (and Scene's own
    /// current_abstract, so le_render_pixel_buffer() stops rendering the
    /// old Abstract) the same way le_set_current_design_abstract/_by_id clear
    /// current_layout_id (and Scene's own current_layout). Returns 0 on
    /// success, nonzero if handle is null or index is out of range - the
    /// current selection is left unchanged on failure. Once selected,
    /// le_render_pixel_buffer() renders this Layout's own content plus its
    /// placed instances, recursed le_hierarchy_depth() levels deep
    /// (Migration Step 3) - see le_set_hierarchy_depth()'s own comment.
    int le_set_current_design_layout(LeHandle *handle, int32_t index);

    /// @brief Same as le_set_current_design_layout, but addressed by
    /// LeDesignId - same relationship to it as le_set_current_design_abstract_by_id
    /// has to le_set_current_design_abstract.
    int le_set_current_design_layout_by_id(LeHandle *handle, LeDesignId design_id);

    /// @brief How many further levels of Placement -> Design a Layout view
    /// recurses into before a placed instance falls back to its own
    /// Abstract, rather than recursing into its own nested Layout
    /// (Migration Step 3 Phase C) - 0 (the default) means every placement
    /// falls back straight to its Abstract. 0 if handle is null.
    int32_t le_hierarchy_depth(LeHandle *handle);

    /// @brief Sets le_hierarchy_depth(). Negative values are rejected (the
    /// current depth is left unchanged) rather than clamped - same
    /// "reject, don't silently clamp" convention le_set_scale uses for a
    /// non-positive scale. Bumps Scene's own hierarchy_version() (only)
    /// on an actual change, cheap for a caller to compare instead of
    /// snapshotting the depth by value. A no-op return value isn't
    /// distinguished from a successful no-op (same value set again) -
    /// query le_hierarchy_depth() afterward if the distinction matters.
    void le_set_hierarchy_depth(LeHandle *handle, int32_t depth);

    /// @brief Number of layer-widget rows currently available - mirrors
    /// ViewLayerSet::rows() directly (see LeLayerRow's own comment: this
    /// includes BOUNDARY and any future non-Technology-derived "extra"
    /// row, not just physical Layers), so this doesn't care whether a
    /// Technology has even been declared yet - it's simply however many
    /// rows the handle's current ViewLayerSet happens to have (0 if
    /// handle is null or none has been built yet, e.g. before the first
    /// le_read_lef() call). See le_layer_at() for each row's contents.
    int32_t le_layer_count(LeHandle *handle);

    /// @brief The row at `row_index` (0..le_layer_count()-1). An
    /// all-invalid/null row (name == null) if handle is null or row_index
    /// is out of range, rather than crashing.
    LeLayerRow le_layer_at(LeHandle *handle, int32_t row_index);

    /// @brief Number of distinct purposes across the handle's current
    /// ViewLayerSet - mirrors ViewLayerSet::purposes() directly. The
    /// "columns" axis of a layer visibility/selectability widget,
    /// independent of any row/layer (see le_purpose_at()). 0 if handle is
    /// null or no ViewLayerSet has been built yet.
    int32_t le_purpose_count(LeHandle *handle);

    /// @brief The purpose at `index` (0..le_purpose_count()-1) - the
    /// returned int is le::ViewLayerPurpose's own raw ordinal (its
    /// declaration order, not necessarily this index): 0 = TERMINAL,
    /// 1 = OBSTRUCTION, 2 = BOUNDARY, 3 = TRACK_PREFERRED,
    /// 4 = TRACK_NON_PREFERRED, 5 = ROUTING_BLOCKAGE, 6 = ROW,
    /// 7 = GCELLGRID, 8 = PLACEMENT_BLOCKAGE, 9 = ROUTE, 10 = REGION,
    /// 11 = PLACEMENT_NAME.
    /// `index` itself walks ViewLayerSet::purposes()'s own
    /// first-encountered order instead (ROW, then BOUNDARY, then
    /// PLACEMENT_NAME, then TERMINAL/OBSTRUCTION/TRACK_PREFERRED/
    /// TRACK_NON_PREFERRED/ROUTING_BLOCKAGE/ROUTE from the first physical
    /// Layer row, then GCELLGRID/PLACEMENT_BLOCKAGE/REGION's own
    /// pseudo-rows - BUGS_AND_ENHANCEMENTS.md E8/E13) - a caller must
    /// always pass `le_purpose_at`'s own return value back into
    /// `le_is_purpose_visible`/`le_set_purpose_visible`, never assume
    /// index equals ordinal.
    ///
    /// NOTE: this ordinal list crosses into flutter_plugin/lib/
    /// layout_engine_plugin.dart's own hand-synced `LeLayerPurpose` enum/
    /// `fromValue` switch (no C-side named enum exists for it) - update
    /// both together if le::ViewLayerPurpose's declaration order ever
    /// changes again.
    ///
    /// Returns -1 if handle is null or index is out of range, rather than
    /// crashing.
    int32_t le_purpose_at(LeHandle *handle, int32_t index);

    /// @brief Current visibility of every ViewLayer whose LeLayerRow::name
    /// is `layer_name` (case-sensitive exact match) - i.e. a whole row
    /// (every purpose-column of it) together, not one column - a
    /// coarser-grained "layer visibility widget" model than one toggle per
    /// grid cell: see le_is_purpose_visible() for the other axis, and
    /// Scene::is_view_layer_visible for how a specific column's effective
    /// visibility combines both. Visible by default until toggled. Returns
    /// true if handle or layer_name is null, matching Scene's
    /// own "unknown name defaults to visible" default.
    bool le_is_layer_name_visible(LeHandle *handle, const char *layer_name);

    /// @brief Set the visibility of every ViewLayer whose LeLayerRow::name
    /// is `layer_name` - e.g. a layer-visibility widget's row-header
    /// checkbox. Mirrors Scene::set_layer_name_visible directly (affects
    /// rendering - see Pipeline::filter_by_layer_visibility). A no-op if
    /// handle or layer_name is null.
    void le_set_layer_name_visible(LeHandle *handle, const char *layer_name, bool visible);

    /// @brief Whether fill/stroke geometry paints antialias their own
    /// edges (Scene::antialiasing_enabled()) - this also covers
    /// per-shape/per-placement design-content text (terminal/route labels,
    /// placement name labels, and their own small anchor-point cross
    /// markers - draw_group/draw_placement_labels, BUGS_AND_ENHANCEMENTS.md
    /// E19) and their own hairline-mode paints, since those can appear
    /// as often as real geometry in a dense design. Fixed, small-count
    /// interactive chrome (the background grid, cursor/hover/selection
    /// overlays, ruler labels, the Abstract origin marker) is unaffected,
    /// always antialiased - drawn once or a handful of times per frame
    /// regardless of design size, not a meaningful cost either way. Off
    /// by default, matching most commercial EDA tools' own default.
    /// Returns false if handle is null.
    bool le_is_antialiasing_enabled(LeHandle *handle);

    /// @brief Toggle antialiasing - e.g. a view-options checkbox. Mirrors
    /// Scene::set_antialiasing_enabled directly (affects rendering). A
    /// no-op if handle is null.
    void le_set_antialiasing_enabled(LeHandle *handle, bool enabled);

    /// @brief Max number of threads this handle's oneTBB-backed pipelines
    /// (BUGS_AND_ENHANCEMENTS.md E10) may use at once - a process-wide
    /// oneapi::tbb::global_control cap, not a per-pipeline setting, since
    /// every pipeline/HierarchyResolver graph shares the same implicit
    /// default TBB arena. Defaults to 8. Returns 0 if handle is null.
    int32_t le_max_concurrency(LeHandle *handle);

    /// @brief Sets le_max_concurrency(). Clamped to a minimum of 2 (not
    /// rejected below it, unlike le_set_hierarchy_depth's own negative-
    /// value convention) - a machine with 128+ CPUs is the whole reason
    /// this exists, and 1 would starve the process's own background
    /// render thread against whatever else is using TBB concurrently,
    /// leaving nothing for the interactive Tcl console/GUI threads this
    /// runs alongside. A no-op if handle is null.
    void le_set_max_concurrency(LeHandle *handle, int32_t max_concurrency);

    /// @brief Current visibility of every ViewLayer whose purpose is
    /// `purpose` (le::ViewLayerPurpose's own raw ordinal - see
    /// le_purpose_at's own doc comment for the full list and why a caller
    /// should always pass that return value here rather than a hand-picked
    /// index), across every layer - i.e. a whole column, not one row.
    /// Visible by default until toggled. Returns nonzero (visible) if
    /// handle is null.
    int32_t le_is_purpose_visible(LeHandle *handle, int32_t purpose);

    /// @brief Set the visibility of every ViewLayer whose purpose is
    /// `purpose`, across every layer - e.g. a layer-visibility widget's
    /// column-header checkbox. Mirrors Scene::set_purpose_visible directly
    /// (affects rendering). A no-op if handle is null.
    void le_set_purpose_visible(LeHandle *handle, int32_t purpose, int32_t visible);

    /// @brief The current interaction mode (UPDATES.md item 11). Select
    /// is the only mode where le_mouse_up's mouse clicks/drags change the
    /// current selection - Edit mode restricts mouse interaction to
    /// editing whatever is already selected (behavior TBD, a later item).
    /// Ruler mode (UPDATES.md item 13) is where le_mouse_up's clicks
    /// place ruler points instead - see le_finish_ruler/le_clear_rulers.
    /// Switched either via LE_KEY_SELECT_MODE/LE_KEY_EDIT_MODE/
    /// LE_KEY_RULER_MODE (keyboard) or le_set_mode (a Flutter UI event) -
    /// both paths converge on the same Scene::Mode state. Switching to
    /// LE_MODE_RULER either way always finishes whatever ruler was
    /// already in progress first (Scene::reset_ruler_mode), including
    /// when the mode is already Ruler - so re-selecting Ruler mode is
    /// itself a way to abandon an in-progress ruler.
    typedef enum LeMode
    {
        LE_MODE_SELECT = 0,
        LE_MODE_EDIT = 1,
        LE_MODE_RULER = 2,
    } LeMode;

    /// @brief The current interaction mode (see LeMode). Returns
    /// LE_MODE_SELECT if handle is null.
    int32_t le_get_mode(LeHandle *handle);

    /// @brief Switch the current interaction mode (see LeMode). A no-op
    /// if handle is null.
    void le_set_mode(LeHandle *handle, int32_t mode);

    /// @brief One ruler point, in microns (UPDATES.md item 13) - see
    /// le_ruler_point_at.
    typedef struct LeRulerPoint
    {
        double x_um;
        double y_um;
    } LeRulerPoint;

    /// @brief Number of rulers (UPDATES.md item 13) - multiple can exist
    /// at once, since starting a new one never clears an existing one.
    /// Indexes le_ruler_point_count()/le_ruler_point_at()'s own
    /// `ruler_index` parameter, 0..this-1, in the order each ruler was
    /// started. Returns 0 if handle is null.
    int32_t le_ruler_count(LeHandle *handle);

    /// @brief Number of committed points on the ruler at `ruler_index`
    /// (0..le_ruler_count()-1) - indexes le_ruler_point_at()'s own
    /// `point_index` parameter. Returns 0 if handle is null or
    /// ruler_index is out of range.
    int32_t le_ruler_point_count(LeHandle *handle, int32_t ruler_index);

    /// @brief The point at `point_index` (0..le_ruler_point_count(ruler_index)-1)
    /// on the ruler at `ruler_index`, converted to microns. Returns
    /// {0.0, 0.0} if handle is null, either index is out of range, or no
    /// Technology has been read yet to convert with.
    LeRulerPoint le_ruler_point_at(LeHandle *handle, int32_t ruler_index, int32_t point_index);

    /// @brief Finishes the active ruler, if any (Scene::finish_active_ruler) -
    /// the next click in Ruler mode starts a new ruler instead of
    /// appending to this one, subject to Scene::add_ruler_point's own
    /// minimum-distance guard against restarting too close to the point
    /// this call just finished at. Called by the frontend on a
    /// double-click (UPDATES.md item 13) - see flutter_plugin's
    /// LeEditor.registerClickAndCheckDoubleClick. A no-op if handle is
    /// null or there's no active ruler.
    void le_finish_ruler(LeHandle *handle);

    /// @brief Removes every ruler (finished or not). A no-op if handle
    /// is null.
    void le_clear_rulers(LeHandle *handle);

    // --- Editing / undo-redo (UPDATES.md item 21) ---
    // A command-pattern undo/redo stack (le::editing::CommandHistory, one
    // per LeHandle) that every generated le_create_X/le_update_X/le_delete_X
    // function records itself into whenever a transaction is currently
    // recording - see le_begin_command/le_end_command below, and
    // le_repl_eval (le_tcl_procs.tcl), the single bracket point that wraps
    // every typed Tcl console command with them so a typed command is
    // exactly as undoable as a GUI edit like Move (le_arm_move below).

    /// @brief Begins recording a new undo/redo transaction labeled
    /// `label` (e.g. the raw text of a command a user just typed, or a
    /// short synthesized label like "move"). Every le_create_X/le_update_X/
    /// le_delete_X call made on this handle while a transaction is
    /// recording is captured as one undo/redo step within it. Calling
    /// this while a transaction is already recording is a no-op (no
    /// nesting supported this round) - a no-op if handle or label is
    /// null.
    void le_begin_command(LeHandle *handle, const char *label);

    /// @brief Ends the transaction started by le_begin_command(). If it
    /// recorded at least one step, pushes it onto the undo stack
    /// (clearing the redo stack) regardless of `succeeded`; if
    /// `succeeded` is nonzero, `label` is also appended to the
    /// command-recall log (le_command_history_count/_at) - so a
    /// zero-step command (e.g. a pure read) that still ran successfully
    /// is recallable even though there's nothing to undo. A no-op if
    /// handle is null or no transaction is currently recording.
    void le_end_command(LeHandle *handle, int32_t succeeded);

    /// @brief Undoes the most recently recorded transaction, if any
    /// (Ctrl-Z). Returns nonzero if something was undone. A no-op
    /// (returns 0) if handle is null or the undo stack is empty.
    int32_t le_undo(LeHandle *handle);

    /// @brief Redoes the most recently undone transaction, if any
    /// (Ctrl-Shift-Z). Returns nonzero if something was redone. A no-op
    /// (returns 0) if handle is null or the redo stack is empty.
    int32_t le_redo(LeHandle *handle);

    /// @brief True (nonzero) if le_undo() would currently do something.
    /// Returns 0 if handle is null.
    int32_t le_can_undo(LeHandle *handle);

    /// @brief True (nonzero) if le_redo() would currently do something.
    /// Returns 0 if handle is null.
    int32_t le_can_redo(LeHandle *handle);

    /// @brief Number of recorded command-recall entries (migrated from
    /// the Flutter Terminal's own local `_commandHistory` list) - only
    /// successfully (le_end_command(..., 1)) executed commands are
    /// recorded, in submission order. Indexes le_command_history_at()'s
    /// own `index` parameter. Returns 0 if handle is null.
    int32_t le_command_history_count(LeHandle *handle);

    /// @brief The command text at `index` (0..le_command_history_count()-1).
    /// Owned by the handle - valid until the handle is destroyed (entries
    /// are never removed/reordered). Returns null if handle is null or
    /// index is out of range.
    const char *le_command_history_at(LeHandle *handle, int32_t index);

    /// @brief Selects every currently selectable shape in the current
    /// Abstract (same underlying behavior as LE_KEY_SELECT_ALL while
    /// LE_KEY_CTRL is held - see its own doc comment for the 10,000-object
    /// cap and capped-selection warning message), but callable directly -
    /// for the Select-mode toolbox button (UPDATES.md item 21), which has
    /// no natural "Ctrl held" precondition of its own the way the keyboard
    /// shortcut does. A no-op if handle is null.
    void le_select_all(LeHandle *handle);

    /// @brief Clears the current selection (same underlying behavior as
    /// LE_KEY_DESELECT_ALL while LE_KEY_CTRL is held), callable directly -
    /// see le_select_all's own comment. A no-op if handle is null.
    void le_deselect_all(LeHandle *handle);

    /// @brief Arms Move (UPDATES.md item 21) - equivalent to Ctrl-M or
    /// clicking the Move toolbox button. Only meaningful in Edit mode
    /// with a non-empty selection; a no-op otherwise (including if
    /// handle is null). The next two le_mouse_up() clicks in Edit mode
    /// set the move's anchor point, then commit the move - see
    /// le_mouse_up's own doc comment. Calling this again while already
    /// armed (including the re-arm a successful commit itself performs -
    /// see le_mouse_up) is harmless - it just re-snapshots the ghost
    /// preview from the current selection/geometry.
    void le_arm_move(LeHandle *handle);

    /// @brief Cancels an in-progress move (armed or anchored, not yet
    /// committed) without applying it - e.g. the Escape key, which
    /// already reaches this via LE_KEY_FINISH_RULER's handler (safe to
    /// call regardless of mode, same as that key's own ruler-finishing
    /// behavior). A no-op if handle is null or no move is in progress.
    void le_cancel_move(LeHandle *handle);

    /// @brief True (nonzero) if Move is currently armed (whether or not
    /// its anchor has been set yet) - for the Move toolbox button's own
    /// pressed/armed visual state. Returns 0 if handle is null.
    int32_t le_is_move_armed(LeHandle *handle);

    /// @brief Current selectability of every ViewLayer whose LeLayerRow::name
    /// is `layer_name` - see le_is_layer_name_visible()'s comment for the
    /// general row/column model this mirrors. Selectable by default until
    /// toggled. Purely an interaction-layer concern (no hit-testing/
    /// click-to-select API exists yet to consult it) - doesn't affect
    /// rendering. Returns nonzero (selectable) if handle or layer_name is
    /// null.
    int32_t le_is_layer_name_selectable(LeHandle *handle, const char *layer_name);

    /// @brief Set the selectability of every ViewLayer whose LeLayerRow::name
    /// is `layer_name`. Mirrors Scene::set_layer_name_selectable directly.
    /// A no-op if handle or layer_name is null.
    void le_set_layer_name_selectable(LeHandle *handle, const char *layer_name, int32_t selectable);

    /// @brief Current selectability of every ViewLayer whose purpose is
    /// `purpose`, across every layer. Selectable by default until toggled.
    /// Returns nonzero (selectable) if handle is null.
    int32_t le_is_purpose_selectable(LeHandle *handle, int32_t purpose);

    /// @brief Set the selectability of every ViewLayer whose purpose is
    /// `purpose`, across every layer. Mirrors Scene::set_purpose_selectable
    /// directly. A no-op if handle is null.
    void le_set_purpose_selectable(LeHandle *handle, int32_t purpose, int32_t selectable);

    /// @brief Zoom the viewport, keeping the dbu point under screen pixel
    /// (x, y) fixed on screen. `factor` is a signed fractional step applied
    /// to the current scale (new_scale = scale * (1 + factor)) - positive
    /// zooms in, negative zooms out (e.g. 0.1 zooms in 10%, -0.1 zooms out
    /// 10%); a factor <= -1.0 (which would make new_scale non-positive) is
    /// ignored, same guard as Scene::set_scale. `x`/`y` are in the same
    /// pixel space as le_render_pixel_buffer()'s output image - top-left
    /// origin, y increasing downward (see api.hpp's LePixelBuffer) - not
    /// Renderer's own pre-Y-flip pixel space, since this is meant to be fed
    /// straight from a pointer/tap event on the rendered image. A no-op if
    /// handle is null. Backend now owns pan/scale entirely - there is no
    /// direct pan/scale setter; use le_fit_scene() to reset to a known view.
    void le_zoom(LeHandle *handle, double factor, int32_t x, int32_t y);

    /// @brief Pan the viewport by a fraction of its own size, in dbu-space
    /// directions (positive x_factor/y_factor move the view toward
    /// increasing dbu x/y - the same "up is positive" convention as the
    /// database itself, not screen space): pan += (x_factor, y_factor) *
    /// viewport_size / scale. E.g. x_factor = 1.0 pans right by exactly one
    /// full viewport width of content. A no-op if handle is null.
    void le_pan(LeHandle *handle, double x_factor, double y_factor);

    /// @brief Set the viewport size in pixels - also the size of the
    /// buffer le_render_pixel_buffer() produces. Mirrors
    /// Scene::set_viewport_size directly.
    void le_set_viewport_size(LeHandle *handle, int32_t width_px, int32_t height_px);

    /// @brief Fit the viewport's pan/scale to the currently selected
    /// Design's content bbox: uniform scale (no stretch) so the content
    /// fills the viewport set via le_set_viewport_size() with `padding_px`
    /// of margin on every side, pan centering it. Mirrors
    /// Scene::fit_to_content, using Pipeline::generate_shapes' output for
    /// the bbox (same shapes le_render_pixel_buffer() would draw). A no-op
    /// if handle is null; degrades to scale 1.0 / pan (0, 0) if no Design
    /// is selected or its Abstract has no shapes, rather than crashing.
    void le_fit_scene(LeHandle *handle, int32_t padding_px);

    /// @brief Fit the viewport's pan/scale to an arbitrary caller-supplied
    /// rectangle, in microns (ll_x_um, ll_y_um)-(ur_x_um, ur_y_um) -
    /// converted to dbu via the shared Technology's own
    /// database_units_microns (falling back to 1.0, i.e. treated as
    /// already-dbu magnitudes, if no Technology has been read yet - same
    /// "degrade gracefully" fallback display_dbu_per_um() uses elsewhere
    /// in this file), matching every other `_um`-suffixed dbu field this
    /// API takes rather than requiring the caller to pre-convert. Uniform
    /// scale (no stretch) so it fills the viewport set via
    /// le_set_viewport_size() with `padding_px` of margin on every side,
    /// pan centering it. Mirrors Scene::fit_to_content directly - the
    /// same "backend owns pan/scale entirely" fit-based approach
    /// le_fit_scene/le_zoom's own comments describe, just with a
    /// caller-supplied rect instead of a Design's own declared content
    /// bbox. A no-op if handle is null. A single-line (one axis
    /// zero-width) rect still fits correctly, using the other axis's own
    /// scale - see Scene::fit_to_content's own comment; a zero-area or
    /// inverted (ur_x_um < ll_x_um or ur_y_um < ll_y_um) rect, or a
    /// non-positive viewport size, degrades to scale 1.0 / pan (0, 0)
    /// instead, same as le_fit_scene's own empty-content fallback.
    void le_fit_rect(LeHandle *handle, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um, int32_t padding_px);

    /// @brief Spacing (dbu) between minor grid dots, drawn behind the
    /// design by le_render_pixel_buffer() - see Renderer::draw_grid.
    /// Mirrors Scene::minor_grid_spacing directly. Defaults to 5 (dbu),
    /// matching a 5nm minor grid under the common "1 dbu = 1nm" Technology
    /// convention. Returns 0 if handle is null.
    int64_t le_minor_grid_spacing(LeHandle *handle);

    /// @brief Set the minor grid dot spacing (dbu). Mirrors
    /// Scene::set_minor_grid_spacing directly (affects rendering) - values
    /// <= 0 are ignored, same guard as le_set_scale. A no-op if handle is
    /// null.
    void le_set_minor_grid_spacing(LeHandle *handle, int64_t dbu);

    /// @brief Spacing (dbu) between major grid dots (drawn bolder than
    /// minor ones - see Renderer::draw_grid). Mirrors
    /// Scene::major_grid_spacing directly. Defaults to 50 (dbu), matching
    /// a 50nm major grid under the common "1 dbu = 1nm" Technology
    /// convention. Returns 0 if handle is null.
    int64_t le_major_grid_spacing(LeHandle *handle);

    /// @brief Set the major grid dot spacing (dbu). Mirrors
    /// Scene::set_major_grid_spacing directly (affects rendering) - values
    /// <= 0 are ignored. A no-op if handle is null.
    void le_set_major_grid_spacing(LeHandle *handle, int64_t dbu);

    /// @brief On-screen text size (px) for every ruler label - tick
    /// values, each segment's own point-to-point distance, and a
    /// ruler's running total (UPDATES.md item 13). Mirrors
    /// Scene::ruler_label_size_px directly. Defaults to 11.0. Returns 0
    /// if handle is null.
    double le_ruler_label_size(LeHandle *handle);

    /// @brief Set the ruler label text size (px). Mirrors
    /// Scene::set_ruler_label_size_px directly (affects rendering) -
    /// values <= 0 are ignored, same guard as le_set_minor_grid_spacing.
    /// A no-op if handle is null.
    void le_set_ruler_label_size(LeHandle *handle, double px);

    /// @brief Set the current mouse position, in the same pixel space as
    /// le_render_pixel_buffer()'s output image (top-left origin, y
    /// increasing downward) and le_zoom()'s x/y - meant to be fed straight
    /// from a pointer-move event. Drives the grid-snap indicator box drawn
    /// by le_render_pixel_buffer() (see Renderer::draw_cursor), and
    /// updates which selectable shape (if any) is hovered (UPDATES.md
    /// 7.1 item 1 - Renderer::draw_hover_outline draws its yellow
    /// outline) via a hit-test against the shapes currently on screen. A
    /// no-op if handle is null. Never invalidates the (potentially
    /// design-sized) rasterized design cache - only the small overlay
    /// picture, see Renderer::compose_with_overlays - but unlike before
    /// hover was added, this is no longer O(1): the hit-test is bounded
    /// by the number of shapes currently visible (already viewport-culled
    /// by Pipeline), not the whole design. See BENCHMARKS.md for measured
    /// cost on a 1M-shape design.
    void le_set_mouse_position(LeHandle *handle, int32_t x, int32_t y);

    /// @brief Clear the current mouse position (e.g. on a pointer-leave
    /// event) so the grid-snap indicator box and any hover outline stop
    /// showing at/for the last known position. A no-op if handle is null.
    void le_clear_mouse_position(LeHandle *handle);

    /// @brief The current mouse position's coordinates in microns, snapped
    /// to the minor grid (UPDATES.md 5.3) - the same point the grid-snap
    /// indicator box drawn by le_render_pixel_buffer() is centered on (see
    /// Renderer::draw_cursor), for a Flutter UI to display as coordinate
    /// text. See LeSnappedMousePosition's own comment for the dbu-to-micron
    /// conversion and its degrade-gracefully cases (null handle, no mouse
    /// position set, no Technology read yet).
    LeSnappedMousePosition le_snapped_mouse_position(LeHandle *handle);

    /// @brief Named logical keys this API tracks the held/released state
    /// of via le_key_down()/le_key_up() - stable across platforms, so
    /// this API's meaning doesn't depend on OS/Flutter scan codes; the
    /// frontend maps its own key values to these before calling in.
    /// Extend as future commands need to know about more keys (UPDATES.md
    /// 7's other interaction modes will need their own shortcuts).
    ///
    /// LE_KEY_ZOOM/LE_KEY_FIT/LE_KEY_PAN_* are canvas-navigation
    /// commands, not modifiers - le_key_down() triggers the
    /// corresponding action immediately (see its own doc comment) rather
    /// than only recording held state. Frontends map a single physical
    /// key to each regardless of shift (e.g. both "z" and "Z" - the same
    /// LogicalKeyboardKey.keyZ in Flutter - map to LE_KEY_ZOOM); shift's
    /// own held state (LE_KEY_SHIFT, tracked exactly like any other key)
    /// decides zoom in vs. out, the same "backend reads modifier state
    /// rather than the frontend pre-deciding" split as le_mouse_up's
    /// shift-click/shift-drag.
    enum LeKeyCode
    {
        LE_KEY_SHIFT = 1,
        LE_KEY_ZOOM = 2,
        LE_KEY_FIT = 3,
        LE_KEY_PAN_LEFT = 4,
        LE_KEY_PAN_RIGHT = 5,
        LE_KEY_PAN_UP = 6,
        LE_KEY_PAN_DOWN = 7,
        /// A pure modifier, tracked exactly like LE_KEY_SHIFT - held
        /// state only, no immediate action of its own. Read by
        /// LE_KEY_SELECT_ALL (UPDATES.md 9.1), LE_KEY_DESELECT_ALL
        /// (UPDATES.md 9.5), LE_KEY_FIT (UPDATES.md 9.6), and
        /// LE_KEY_1..LE_KEY_9 (UPDATES.md 9.7).
        LE_KEY_CTRL = 8,
        /// Select-all (UPDATES.md 9.1) - an "action" code like
        /// LE_KEY_ZOOM/LE_KEY_FIT/LE_KEY_PAN_*, but only actually does
        /// anything while LE_KEY_CTRL is currently held (see le_key_down's
        /// own doc comment) - a bare "a" press with Ctrl not held is a
        /// deliberate no-op, not "select nothing."
        LE_KEY_SELECT_ALL = 9,
        /// Digit keys 1-9 toggle a ROUTING layer's visibility (UPDATES.md
        /// 9.4/9.7) - which one depends on LE_KEY_CTRL's held state (the
        /// same "backend reads modifier state" split LE_KEY_ZOOM/
        /// LE_KEY_FIT already use): with Ctrl not held, LE_KEY_1 is the
        /// first ROUTING layer in the current Technology's own
        /// declaration order, LE_KEY_2 the second, etc. through
        /// LE_KEY_9's ninth; with Ctrl held, the same keys address the
        /// *eleventh* through *nineteenth* ROUTING layer instead (see
        /// LE_KEY_0 for the tenth) - a no-op if there's no ROUTING layer
        /// at the addressed position. See le_key_down's own doc comment
        /// for the VIA-pairing behavior this triggers that a direct
        /// le_set_layer_name_visible() call deliberately does not.
        LE_KEY_1 = 10,
        LE_KEY_2 = 11,
        LE_KEY_3 = 12,
        LE_KEY_4 = 13,
        LE_KEY_5 = 14,
        LE_KEY_6 = 15,
        LE_KEY_7 = 16,
        LE_KEY_8 = 17,
        LE_KEY_9 = 18,
        /// Deselect-all (UPDATES.md 9.5) - same "action code, gated on
        /// LE_KEY_CTRL" shape as LE_KEY_SELECT_ALL, but simply clears the
        /// current selection rather than building a new one; a bare "d"
        /// press with Ctrl not held is a deliberate no-op, same reasoning
        /// as LE_KEY_SELECT_ALL's own comment.
        LE_KEY_DESELECT_ALL = 19,
        /// Toggles the tenth ROUTING layer's visibility (UPDATES.md 9.7) -
        /// not Ctrl-gated, unlike LE_KEY_1..LE_KEY_9's own Ctrl-held
        /// branch, since "0" has no bare-digit slot left below it to
        /// double up on (LE_KEY_1..LE_KEY_9 already cover the first
        /// nine). Same VIA-pairing behavior as LE_KEY_1..LE_KEY_9.
        LE_KEY_0 = 20,
        /// Switch to Select mode (UPDATES.md item 11) - an "action" code
        /// like LE_KEY_ZOOM/LE_KEY_FIT: le_key_down() calls
        /// Scene::set_mode(Scene::Mode::SELECT) immediately, every call
        /// (including key-repeat - idempotent, so no special one-shot
        /// handling is needed). Bare-only - fires only while neither
        /// LE_KEY_CTRL nor LE_KEY_SHIFT is currently held, so e.g. a
        /// Ctrl-S keystroke intended for something else doesn't also
        /// switch modes as a side effect. See le_get_mode/le_set_mode for
        /// the non-keyboard (Flutter UI event) path to the same state,
        /// which is not modifier-gated (there's no physical key to
        /// collide with there).
        LE_KEY_SELECT_MODE = 21,
        /// Switch to Edit mode (UPDATES.md item 11) - same shape as
        /// LE_KEY_SELECT_MODE, including the bare-only modifier gating,
        /// calling Scene::set_mode(Scene::Mode::EDIT). While in Edit
        /// mode, le_mouse_up no longer changes the current selection -
        /// see its own doc comment.
        LE_KEY_EDIT_MODE = 22,
        /// Switch to Ruler mode (UPDATES.md item 13) - same idempotent,
        /// bare-only action-code shape as LE_KEY_SELECT_MODE/
        /// LE_KEY_EDIT_MODE, but calls Scene::reset_ruler_mode() rather
        /// than a plain set_mode(): every call - including when already
        /// in Ruler mode, and including key-repeat - finishes whatever
        /// ruler was in progress, so re-pressing 'r' doubles as an
        /// explicit "abandon the current ruler" shortcut. While in Ruler
        /// mode, le_mouse_up's clicks place ruler points instead of
        /// changing the selection - see le_finish_ruler/le_clear_rulers.
        LE_KEY_RULER_MODE = 23,
        /// Finishes the active ruler, if any (UPDATES.md item 13, see
        /// le_finish_ruler) - the Esc key. Idempotent/safe to fire on
        /// every call including key-repeat, same as every other action
        /// code here: Scene::finish_active_ruler() is already a no-op
        /// once there's nothing active to finish, and there's no active
        /// ruler at all outside Ruler mode (leaving it already finishes
        /// whatever was in progress - see Scene::set_mode), so this
        /// never needs mode-gating at the call site either. Also cancels
        /// an in-progress (not yet committed) Move, if any (UPDATES.md
        /// item 21, le_cancel_move) - same "always safe to fire" reasoning,
        /// Scene::end_move() is a no-op once there's nothing to cancel.
        /// Deliberately *not* modifier-gated, unlike every bare-only key
        /// above - Escape is a pure cancel/finish gesture, and a real
        /// ruler-drawing sequence routinely ends with LE_KEY_SHIFT (the
        /// free-form toggle) still physically held right up to the Esc
        /// press, so suppressing it while a modifier happens to be down
        /// would make "finish the ruler" unreliable in exactly the
        /// workflow that uses Shift the most.
        LE_KEY_FINISH_RULER = 24,
        /// Arms Move (UPDATES.md item 21, Ctrl-M) - see le_arm_move's own
        /// doc comment. Fires only while Ctrl is held and Shift is not,
        /// at the le_key_down call site, same shape as
        /// LE_KEY_SELECT_ALL/LE_KEY_DESELECT_ALL, even though the Move
        /// toolbox button calls le_arm_move() directly and bypasses this
        /// gate. No separate LE_KEY_UNDO/LE_KEY_REDO code exists -
        /// Ctrl-Z/Ctrl-Shift-Z are handled by branching inside
        /// LE_KEY_ZOOM's own handler instead (see le_key_down's doc
        /// comment) - the frontend's key-to-code map already sends every
        /// "z" press as LE_KEY_ZOOM regardless of modifiers, the same way
        /// every other canvas-navigation code already works.
        LE_KEY_MOVE = 25,
    };

    /// @brief Mark `key_code` (an LeKeyCode value) as currently held,
    /// e.g. on a key-down *or* key-repeat event - queried internally by
    /// commands that care (e.g. le_mouse_up's shift-click/shift-drag
    /// behavior). For the canvas-navigation codes (LE_KEY_ZOOM/
    /// LE_KEY_FIT/LE_KEY_PAN_*), also triggers that action immediately,
    /// once per call - so a held key that keeps re-delivering key-repeat
    /// events (e.g. an arrow key auto-repeating) keeps panning/zooming
    /// once per repeat, matching a real keyboard's own repeat behavior,
    /// not a one-shot trigger on first press only:
    ///
    /// - LE_KEY_ZOOM: while LE_KEY_CTRL is currently held, undoes
    ///   (le_undo) or, if LE_KEY_SHIFT is also held, redoes (le_redo) the
    ///   most recent transaction instead of zooming (UPDATES.md item 21,
    ///   Ctrl-Z/Ctrl-Shift-Z) - the frontend's key-to-code map already
    ///   sends every "z" press as LE_KEY_ZOOM regardless of modifiers, so
    ///   this branches here rather than needing its own key code.
    ///   Otherwise: le_zoom() by a fixed factor, anchored at the
    ///   current mouse position (Scene::mouse_x_px/mouse_y_px - i.e.
    ///   wherever le_set_mouse_position was last called for); zooms in,
    ///   or out if LE_KEY_SHIFT is currently held (le_is_key_held).
    /// - LE_KEY_FIT: le_fit_scene() with a fixed padding - or, if
    ///   LE_KEY_CTRL is currently held (UPDATES.md 9.6, "Ctrl-F fit
    ///   selected"), fits the viewport to the current selection's own
    ///   combined bbox instead (same fixed padding, one Shape's own bbox
    ///   per selection entry - see api.cpp's fit_selected_unlocked),
    ///   leaving the view unchanged if nothing is selected rather than
    ///   falling back to the whole-content fit. LE_KEY_SHIFT has no
    ///   meaning for Fit - held at all (with or without Ctrl) is a
    ///   no-op, not "same as unmodified".
    /// - LE_KEY_PAN_LEFT/RIGHT/UP/DOWN: le_pan() by a fixed
    ///   viewport-fraction step in the corresponding direction. Bare
    ///   only - a no-op while either LE_KEY_CTRL or LE_KEY_SHIFT is held.
    /// - LE_KEY_SELECT_ALL (UPDATES.md 9.1): only while LE_KEY_CTRL is
    ///   currently held, LE_KEY_SHIFT is *not*, and the current mode is
    ///   LE_MODE_SELECT (UPDATES.md item 21 - Select-mode selection
    ///   shortcuts are disabled in Edit/Ruler mode; switch back to
    ///   Select mode to change the selection there) - clears the current
    ///   selection, then selects every piece of every currently
    ///   selectable shape in the current Abstract regardless of viewport
    ///   (not just what's on screen), up to a fixed cap of 10,000
    ///   objects (pieces, not whole shapes - UPDATES.md item 21). If the
    ///   design has more selectable pieces than that, the selection
    ///   stops at the cap and a "WARNING: Selection capped..." entry is
    ///   appended to the le_message_count()/le_message_at() queue
    ///   (UPDATES.md item 3) - there's no separate "was it capped"
    ///   return value, this is the same mechanism any other
    ///   backend-originated message uses.
    /// - LE_KEY_1..LE_KEY_9 (UPDATES.md 9.4/9.7): toggles a ROUTING
    ///   layer's visibility - the 1st..9th if LE_KEY_CTRL is not
    ///   currently held, the 11th..19th if it is (a no-op if there's no
    ///   ROUTING layer at that position); LE_KEY_SHIFT held at all (with
    ///   or without Ctrl) is a no-op instead - then, either way, - only
    ///   via this keyboard path, never from a direct
    ///   le_set_layer_name_visible() call - re-checks every pair of
    ///   adjacent ROUTING layers: if both are now visible, every CUT
    ///   layer between them (LEF has no distinct "VIA" layer type - vias
    ///   are TYPE CUT layers - see this header's own LeKeyCode comment)
    ///   becomes visible too; if not, those CUT layers become invisible.
    /// - LE_KEY_0 (UPDATES.md 9.7): toggles the 10th ROUTING layer's
    ///   visibility - bare only (a no-op while either LE_KEY_CTRL or
    ///   LE_KEY_SHIFT is held; there's no digit slot left over for Ctrl
    ///   to double up on the way LE_KEY_1..LE_KEY_9 do) - same
    ///   VIA-pairing re-check as LE_KEY_1..LE_KEY_9 above.
    /// - LE_KEY_DESELECT_ALL (UPDATES.md 9.5): only while LE_KEY_CTRL is
    ///   currently held, LE_KEY_SHIFT is not, and the current mode is
    ///   LE_MODE_SELECT (same mode-gating as LE_KEY_SELECT_ALL above,
    ///   UPDATES.md item 21) - clears the current selection. A no-op
    ///   (not an error) if the selection was already empty.
    /// - LE_KEY_MOVE (UPDATES.md item 21): only while LE_KEY_CTRL is
    ///   currently held and LE_KEY_SHIFT is not - equivalent to
    ///   le_arm_move(); a no-op outside Edit mode or with an empty
    ///   selection, same as that function.
    /// - LE_KEY_SELECT_MODE/LE_KEY_EDIT_MODE/LE_KEY_RULER_MODE
    ///   (UPDATES.md item 11/13): bare only - a no-op while either
    ///   LE_KEY_CTRL or LE_KEY_SHIFT is held, so e.g. a Ctrl-S keystroke
    ///   meant for something else doesn't also switch modes.
    /// - LE_KEY_FINISH_RULER: unconditional regardless of any modifier -
    ///   see its own LeKeyCode doc comment for why Escape is the one
    ///   bare-ish key that's deliberately exempt from modifier-gating.
    ///
    /// A no-op if handle is null.
    void le_key_down(LeHandle *handle, int32_t key_code);

    /// @brief Mark `key_code` as no longer held, e.g. on a key-up event.
    /// A no-op if handle is null.
    void le_key_up(LeHandle *handle, int32_t key_code);

    /// @brief True (nonzero) if `key_code` is currently held (see
    /// le_key_down). Returns 0 if handle is null.
    int32_t le_is_key_held(LeHandle *handle, int32_t key_code);

    /// @brief Clear every currently-held key at once - call when the
    /// widget/window receiving key events loses focus (e.g. a Flutter
    /// `Focus` widget's `onFocusChange(false)`). A key's matching
    /// key-up event is not guaranteed to still reach a widget that no
    /// longer has focus by the time the physical key is released, so
    /// without calling this on a focus loss, a modifier (e.g. shift)
    /// held at that moment stays "held" from this API's point of view
    /// indefinitely - silently changing the behavior of every later
    /// gesture that consults it (le_mouse_up's shift-click/shift-drag
    /// decision) until that same key happens to be pressed and released
    /// again while focused. A no-op if handle is null.
    void le_clear_all_keys(LeHandle *handle);

    /// @brief Begin a mouse gesture at the given pixel position (top-left
    /// origin, y down - same convention as le_set_mouse_position/le_zoom),
    /// e.g. on a pointer-down event. Records the gesture's anchor point;
    /// le_mouse_up() later decides whether the gesture was a click or a
    /// rubber-band drag-select by comparing the down/up pixel distance
    /// against a small threshold (UPDATES.md 7.1 items 2-6). A no-op if
    /// handle is null.
    void le_mouse_down(LeHandle *handle, int32_t x, int32_t y);

    /// @brief Like le_mouse_down(), but begins a rectangle-*zoom* gesture
    /// instead of a drag-*select* one (UPDATES.md 9.3 - e.g. on a
    /// right-button pointer-down event) - le_mouse_up() is still the one
    /// call that ends either kind, deciding which behavior to run from
    /// which of le_mouse_down()/le_zoom_drag_down() started it (see its
    /// own doc comment). A no-op if handle is null.
    void le_zoom_drag_down(LeHandle *handle, int32_t x, int32_t y);

    /// @brief End a mouse gesture at the given pixel position, e.g. on a
    /// pointer-up event - the single shared endpoint for both
    /// le_mouse_down()'s drag-select gesture and
    /// le_zoom_drag_down()'s drag-zoom gesture (UPDATES.md 9.3); which
    /// one runs depends entirely on which of those two started the
    /// in-progress gesture, not on which mouse button this call itself
    /// corresponds to (a real up-event's own "which button" state isn't
    /// reliably available at release time on every platform, so the
    /// frontend doesn't need to track or pass it here - only the down
    /// side needs to pick correctly). A no-op if handle is null or there
    /// was no matching le_mouse_down()/le_zoom_drag_down() call first (a
    /// stray/duplicate mouse-up).
    ///
    /// **Started by le_mouse_down()** - two outcomes, both subject to
    /// Pipeline::hit_test_point/hit_test_rect's own topmost-layer-first
    /// (click) / all-layers (drag) and layer-selectability rules, and
    /// both consulting LE_KEY_SHIFT's current held state (see
    /// le_key_down) rather than taking it as a parameter here:
    ///
    /// - **Click** (down/up pixel distance below a small threshold): hit-
    ///   tests the single point. Without shift held, replaces the
    ///   current selection with the hit piece, or clears it if nothing
    ///   was hit; with shift held, adds the hit to the current selection
    ///   (a no-op if nothing was hit).
    /// - **Drag-select** (distance above the threshold): hit-tests every
    ///   selectable shape, on every layer, fully enclosed by the
    ///   rectangle between the down and up points. Without shift held,
    ///   replaces the current selection with the results; with shift
    ///   held, adds them to it.
    ///
    /// Selection is piece-granular (UPDATES.md item 21): a Shape that
    /// bundles several rects/polygons/paths together (e.g. several RECT
    /// statements under one LEF PORT/OBS LAYER line) selects only the
    /// one piece actually clicked, or the individual pieces actually
    /// enclosed by a drag - not every piece the Shape happens to own.
    /// Two different pieces of the same Shape can be independently
    /// selected (e.g. shift-clicking each in turn) as two separate
    /// entries. The Property Viewer still resolves to the *owning Shape*
    /// regardless of which piece is selected (le_selected_object_ref) -
    /// only the selection outline and Move (le_arm_move) act on the
    /// specific piece.
    ///
    /// **In Edit mode with Move armed** (UPDATES.md item 21, see
    /// le_arm_move) - a click (not a drag; a drag's up-event is treated
    /// the same as a click here, Move has no rubber-band behavior of its
    /// own) does one of two things depending on whether the move already
    /// has an anchor: with no anchor yet, this click sets it (the move's
    /// start point); with an anchor already set, this click computes the
    /// offset from the anchor to this click's own (grid-snapped)
    /// position - orthogonally constrained to whichever axis moved
    /// further unless LE_KEY_SHIFT is held (free-form) - applies it to
    /// every moving shape's geometry and records the whole set as one
    /// undoable transaction (le_undo/le_redo). Move then stays armed,
    /// re-snapshotted from the shapes' new positions and with the
    /// anchor cleared, ready for an immediate follow-up move on the same
    /// selection - only le_cancel_move (the Escape key) or leaving Edit
    /// mode actually disarms it, so moving several shapes one after
    /// another doesn't need Ctrl-M/the Move button re-pressed between
    /// each one. Selection is untouched either way - Move never changes
    /// *which* shapes are selected, only their geometry. In Edit mode
    /// with Move *not* armed, this is a no-op (selection changes are
    /// Select-mode-only - see LE_KEY_SELECT_ALL's own comment).
    ///
    /// **Started by le_zoom_drag_down()** (UPDATES.md 9.3): a click-sized
    /// release (same threshold as above) is a no-op - fitting to a
    /// near-zero-size rectangle would produce an absurd scale; otherwise
    /// fits the viewport to the rectangle between the down and up points
    /// (Scene::fit_to_content, no padding), the same fitting math
    /// le_fit_scene() uses for a Design's own content bbox. Selection is
    /// untouched either way - this gesture is purely navigational.
    ///
    /// Either way, ends the in-progress drag gesture (Scene::end_drag) -
    /// see le_mouse_down's own doc comment for the live rubber-band
    /// rectangle this also stops showing.
    void le_mouse_up(LeHandle *handle, int32_t x, int32_t y);

    /// @brief Instructional text describing which mouse gestures and
    /// keyboard modifiers are currently available, for display in the
    /// GUI's status bar below the texture (UPDATES.md item 7.3) - e.g.
    /// "Left click to select. Shift for multi-select. Left click and
    /// drag for rectangle multi-select." for the current (and, for now,
    /// only) interaction mode, Select - see UPDATES.md item 7's own
    /// intro for the other modes this will eventually distinguish
    /// between. Unlike every other `const char*`-returning function in
    /// this header, the returned pointer refers to static, process-
    /// lifetime storage - not owned by `handle`, never invalidated, safe
    /// to hold indefinitely. Null only if `handle` is null.
    const char *le_tooltip_message(LeHandle *handle);

    /// @brief Number of currently selected objects (Scene::selection()).
    /// Indexes the `selection_index` parameter of le_selected_object_ref()
    /// below - 0..le_selection_count()-1, in Scene::selection()'s own
    /// (insertion) order. Returns 0 if handle is null.
    int32_t le_selection_count(LeHandle *handle);

    /// @brief Monotonic counter (Scene::selection_version()) bumped on
    /// every actual selection change (a select()/deselect()/
    /// clear_selection() call that isn't a no-op) - a cheap way for a
    /// caller to tell whether anything selection-related has changed
    /// since it last checked, without re-fetching le_selection_count()/
    /// le_selected_object_ref() for every selected object on every call
    /// (e.g. every mouse-move event - a real, measured cost that scales
    /// with selection size otherwise, see BENCHMARKS.md). Returns 0 if
    /// handle is null - a real handle's version is never observably 0
    /// forever (any interaction eventually changes it), so a caller
    /// comparing against a sentinel it initialized to a negative value on
    /// its own side won't be confused by a null-handle 0 looking like
    /// "unchanged".
    int64_t le_selection_version(LeHandle *handle);

    /// @brief Which field of LeProperty is meaningful for a given row -
    /// see LeProperty's own comment.
    typedef enum LePropertyType
    {
        LE_PROPERTY_TYPE_STRING = 0,
        LE_PROPERTY_TYPE_INT = 1,
        LE_PROPERTY_TYPE_DOUBLE = 2,
    } LePropertyType;

    /// @brief One name/value row of an object's property table - a caller
    /// reads `type` to know which of string_value/int_value/double_value
    /// to use; the other two are unspecified. `name`/`string_value` point
    /// into memory owned by the LeHandle - valid only until the next
    /// le_object_property_at()/le_object_property_count() call *for the
    /// same LeObjectRef* (a different ref may invalidate them sooner) -
    /// same "valid until the next call" convention as LePixelBuffer,
    /// never owned by the caller and never to be freed by it. `name` is
    /// null (and every other field 0/unspecified) if this row is invalid
    /// (out-of-range index or null handle).
    typedef struct LeProperty
    {
        const char *name;
        int32_t type; // LePropertyType
        const char *string_value;
        int64_t int_value;
        double double_value;
    } LeProperty;

    /// @brief Which database class an LeObjectRef names - originally the
    /// seven classes get_properties/the friendly-id convention already
    /// distinguish (le_tcl_shim.cpp's library:/design:/abstract:/
    /// terminal:/terminal_port:/obstruction:/shape: prefixes), just typed
    /// instead of stringly-typed, for the GUI's Property Viewer (UPDATES.md
    /// 7.2's later database-hierarchy redesign). Extended (E1,
    /// BUGS_AND_ENHANCEMENTS.md) with the six top-level Layout-view kinds
    /// Scene::SelectedObject's own variant grew - le_selected_object_ref()
    /// dispatches each selected object to its own kind here so the
    /// Property Viewer shows real properties for a selected Row/
    /// Placement/Blockage/Route/PhysicalPort/Region, not just a Shape.
    typedef enum LeObjectKind
    {
        LE_OBJECT_KIND_LIBRARY = 0,
        LE_OBJECT_KIND_DESIGN = 1,
        LE_OBJECT_KIND_ABSTRACT = 2,
        LE_OBJECT_KIND_TERMINAL = 3,
        LE_OBJECT_KIND_TERMINAL_PORT = 4,
        LE_OBJECT_KIND_OBSTRUCTION = 5,
        LE_OBJECT_KIND_SHAPE = 6,
        LE_OBJECT_KIND_ROW = 7,
        LE_OBJECT_KIND_PLACEMENT = 8,
        LE_OBJECT_KIND_BLOCKAGE = 9,
        LE_OBJECT_KIND_ROUTE = 10,
        LE_OBJECT_KIND_PHYSICAL_PORT = 11,
        LE_OBJECT_KIND_REGION = 12,
        // Intermediate parent-hop node only (like LE_OBJECT_KIND_TERMINAL_PORT
        // - never itself a mouse-click selection result, only reached via
        // le_object_parent() from a Shape belonging to a PhysicalPort).
        LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT = 13,
        // Another intermediate parent-hop node - originally deferred
        // ("no current consumer needs one", see object_ref_parent's own
        // history) until the Property Viewer's own breadcrumb tree
        // (walking le_object_parent() all the way to LE_OBJECT_KIND_LIBRARY)
        // turned out to be exactly that consumer: without this, every
        // Layout-view selection's own parent chain dead-ended at
        // LE_OBJECT_KIND_ROW/_PLACEMENT/etc. instead of continuing
        // Layout -> Design -> Library the way an Abstract-view
        // selection's own Terminal -> Abstract -> Design -> Library
        // chain already does.
        LE_OBJECT_KIND_LAYOUT = 14,
    } LeObjectKind;

    /// @brief A generic, typed reference to one database object of any
    /// LeObjectKind class - `index`/`generation` are that class's own id
    /// fields (e.g. for LE_OBJECT_KIND_SHAPE, the same pair LeShapeId
    /// carries). Used by le_object_property_count()/_at()/
    /// le_object_parent()/le_selected_object_ref() below, so a Property
    /// Viewer can walk the database hierarchy (Shape -> TerminalPort ->
    /// Terminal -> Abstract -> Design -> Library) generically instead of
    /// needing a different function per class. An invalid ref (see
    /// le_object_invalid_ref) has index == UINT32_MAX, mirroring every
    /// other LeXxxId's own invalid-index convention in this header.
    typedef struct LeObjectRef
    {
        int32_t kind; // LeObjectKind
        uint32_t index;
        uint32_t generation;
    } LeObjectRef;

    /// @brief The sentinel LeObjectRef meaning "no such object" - returned
    /// by le_object_parent() when a ref has no parent (LE_OBJECT_KIND_LIBRARY)
    /// or the input ref itself doesn't resolve, and by le_selected_object_ref()
    /// for an out-of-range selection_index or a null handle.
    LeObjectRef le_object_invalid_ref(void);

    /// @brief Number of property rows `ref` has (see LeProperty) - indexes
    /// le_object_property_at()'s own `index` parameter, 0..this-1. Read-
    /// only: never mutates Scene::selection() or bumps selection_version(),
    /// so a Property Viewer can call this while navigating parent/child
    /// links without affecting canvas selection. Dispatches to the same
    /// by-id property builder each class's own le_X_property_count/_at
    /// already uses (e.g. LE_OBJECT_KIND_SHAPE reads exactly what
    /// get_properties shape:<id> shows). Returns 0 if handle is null or
    /// ref doesn't resolve to a real object.
    int32_t le_object_property_count(LeHandle *handle, LeObjectRef ref);

    /// @brief The property row at `index` (0..le_object_property_count(ref)-1)
    /// for `ref`. Read-only, same contract as le_object_property_count().
    /// Returns an all-null/zero row (LeProperty::name == nullptr) if
    /// handle is null, ref doesn't resolve, or index is out of range.
    LeProperty le_object_property_at(LeHandle *handle, LeObjectRef ref, int32_t index);

    /// @brief `ref`'s immediate parent in the database hierarchy (Shape's
    /// own terminal_port/obstruction field, TerminalPort's terminal,
    /// Terminal/Obstruction's abstract, Abstract's design, Design's
    /// library) - the same parent-hop graph filter_field_tables() already
    /// declares for -filter validation, exposed here for GUI navigation
    /// links. Read-only: never mutates Scene::selection(). Returns
    /// le_object_invalid_ref() for LE_OBJECT_KIND_LIBRARY (no parent), a
    /// ref that doesn't resolve, or a null handle.
    LeObjectRef le_object_parent(LeHandle *handle, LeObjectRef ref);

    /// @brief The selected object at `selection_index` (0..le_selection_count()-1),
    /// as a generic ref usable with le_object_property_count()/_at()/
    /// le_object_parent() - LE_OBJECT_KIND_SHAPE for a piece-granular
    /// ShapePiece selection (Terminal/Obstruction/Blockage/Route/
    /// PhysicalPort - see Scene::SelectedObject's own comment), or the
    /// matching whole-object kind (LE_OBJECT_KIND_ROW/_PLACEMENT/_REGION)
    /// for E1's bare-id alternatives. Read-only: never mutates
    /// Scene::selection() or bumps selection_version(). Returns
    /// le_object_invalid_ref() if handle is null or selection_index is
    /// out of range.
    LeObjectRef le_selected_object_ref(LeHandle *handle, int32_t selection_index);

    /// @brief Run the full pipeline+render chain (generate -> filter ->
    /// filter -> transform -> picture -> rasterize) for the currently
    /// selected Design and viewport, returning the resulting pixel
    /// buffer - including the grid-snap indicator box at the current
    /// mouse position, if one has been set (see le_set_mouse_position).
    /// Each stage is cached internally (see Pipeline/Renderer) - calling
    /// this again with nothing changed since the last call is close to
    /// free; only viewport/selection/mouse-position changes actually
    /// recompute, and a mouse-position-only change is itself cheap (see
    /// Renderer::compose_with_overlays), not proportional to design size.
    /// Returns an all-zero/null LePixelBuffer if handle is null. No
    /// Design selected (le_set_current_design_abstract was never called) degrades
    /// gracefully to an empty (but correctly-sized, non-null) buffer
    /// rather than crashing - Root/Pipeline's own lookups already degrade
    /// gracefully for an unset AbstractId (see pipeline.hpp).
    LePixelBuffer le_render_pixel_buffer(LeHandle *handle);

    /// @brief Whether le_render_pixel_buffer() is doing real work on this
    /// handle right now, on whatever thread called it (BUGS_AND_ENHANCEMENTS.md
    /// E17 - a status-bar spinner also driven by interactive zoom/pan, not
    /// just a running Tcl command like the existing one). Meant to be
    /// polled (e.g. every ~50ms) from a different thread than the one
    /// calling le_render_pixel_buffer() itself - deliberately does NOT
    /// take the same lock le_render_pixel_buffer() holds for its own
    /// entire duration, so this never blocks behind the very render it's
    /// reporting on. Most renders are well under a millisecond (see
    /// le_render_pixel_buffer's own comment) and will never be observed
    /// as "rendering" by a poll at that interval - this only matters for
    /// the rare case a real, dense design takes long enough to notice.
    /// Returns 0 (not rendering) if handle is null.
    int32_t le_is_rendering(LeHandle *handle);

    /// @brief Signals that a window showing this handle's own rendered
    /// content should be opened - the backing for the Tcl `show_gui`
    /// command. Fire-and-forget: sets a flag and returns immediately, so
    /// the Tcl console thread that called it keeps working while a
    /// separate, dedicated GUI thread (src/gui/le_gui.hpp - not this
    /// process's Tcl console thread) notices the request (via
    /// le_take_show_gui_request()) and actually creates the window. A
    /// no-op if handle is null.
    void le_request_show_gui(LeHandle *handle);

    /// @brief Atomically reads and clears the pending-show-gui flag
    /// le_request_show_gui() sets - "was a request made since the last
    /// time this was called", not "is one currently pending" (there is
    /// no currently, it's a one-shot edge, not a level - see this
    /// function's own gui_show_requested_ backing field in api.cpp).
    /// Meant to be polled from the one thread that owns opening/showing
    /// the GUI window. Returns 0 if handle is null.
    int32_t le_take_show_gui_request(LeHandle *handle);

    /// @brief Queues `command` (a plain Tcl command string, e.g.
    /// "set_layer_visible {M1} 1") to be evaluated by whichever caller
    /// drains this queue via le_take_next_pending_tcl_command() below -
    /// le_shell.cpp's own console thread, through le_repl_eval, the same
    /// bracket point a typed command goes through (recording it into
    /// command_history/undo). Meant for a caller that has no Tcl
    /// interpreter of its own to evaluate a command directly (src/gui/'s
    /// components - see le_gui.hpp's own doc comment for why that
    /// module has no Tcl/SWIG dependency at all) but still wants an
    /// action to leave the same command-history trail a typed command
    /// would, for the specific subset of actions the Flutter frontend's
    /// own LeProvider already routes through a Tcl command instead of a
    /// direct FFI call (see le_provider.dart's own runTclCommand call
    /// sites) - mouse/keyboard interaction stays a direct le_* call
    /// either way, same as LeEditorInput's own shape. Thread-safe,
    /// fire-and-forget: queues and returns immediately, not evaluated
    /// synchronously by this call. A no-op if handle or command is null.
    void le_enqueue_tcl_command(LeHandle *handle, const char *command);

    /// @brief Pops and returns the oldest command queued via
    /// le_enqueue_tcl_command(), or null if none is pending. The
    /// returned pointer refers to storage owned by the handle - valid
    /// only until the *next* le_take_next_pending_tcl_command() call on
    /// the same handle (same "valid until the next call" convention as
    /// le_message_at()/LeProperty's string fields) - copy it out (e.g.
    /// into a Tcl_Eval call) before then. Thread-safe: meant to be
    /// polled from whichever thread owns evaluating these (le_shell.cpp's
    /// own console thread).
    const char *le_take_next_pending_tcl_command(LeHandle *handle);

    // --- CRUD + filter-search (UPDATES.md item 15 / TCL_EXPLORATION.md
    // Phase 4) - Terminal only so far; TerminalPort/Obstruction/Abstract-
    // boundary follow the same shape in a later pass. Layered on top of
    // Root's Phase 1/2 primitives (create_x/delete_x/set_x_<field>,
    // get_field()/match_hop()/search_x, backend/src/database/filter.hpp's
    // parser+evaluator) - see TCL_EXPLORATION.md for the full design.
    // Every id here is addressed directly, not through the current GUI
    // selection (though le_object_property_count/_at, added later, can
    // address these same ids generically too - see LeObjectRef) - the
    // natural fit for a TCL caller that isn't driving the GUI at all. ---

    /// @brief Mirrors le::SignalDirection (generated/signal_direction.hpp)
    /// field-for-field - kept as an explicit enum (not a bare int) so a
    /// future reordering of either fails to compile instead of silently
    /// mismatching, same reasoning as LePropertyType/LeObjectKind.
    typedef enum LeSignalDirection
    {
        LE_SIGNAL_DIRECTION_INPUT = 0,
        LE_SIGNAL_DIRECTION_OUTPUT = 1,
        LE_SIGNAL_DIRECTION_INOUT = 2,
        LE_SIGNAL_DIRECTION_NONE = 3,
        LE_SIGNAL_DIRECTION_OUTPUT_TRISTATE = 4,
        LE_SIGNAL_DIRECTION_FEEDTHRU = 5,
    } LeSignalDirection;

    /// @brief Find the Terminal named `name` (exact match) within the
    /// currently selected Abstract (le_set_current_abstract/
    /// le_current_abstract's own handle->current_abstract_id - same
    /// current-view scoping le_get_terminals' own default scope already
    /// uses; deliberately *not* handle->scene.current_abstract(), a
    /// separate GUI-rendering "current view" only ever moved as a side
    /// effect of selecting a Design, e.g. le_set_current_design_abstract_by_id -
    /// a script that builds an Abstract from scratch and calls
    /// le_set_current_abstract directly, with no Design to select, needs
    /// this to still work). A linear scan over
    /// Root::get_abstract_terminals, not a cmg index=True lookup - Terminal
    /// name uniqueness is per-Abstract (unique_per_parent, see
    /// backend/src/database/schema.py's own Terminal.name comment), not
    /// global, so a flat index=True lookup would be the wrong shape here.
    /// Returns an invalid LeTerminalId (index == UINT32_MAX) if handle or
    /// name is null, no Abstract is currently selected, or no Terminal in
    /// the current Abstract has that name.
    LeTerminalId le_terminal_by_name(LeHandle *handle, const char *name);

    /// @brief The Terminal at `id`'s own name - a direct field accessor
    /// (unlike le_terminal_property_at's stringly-typed property table),
    /// for a caller that just wants the name itself. Returned pointer is
    /// owned by the handle's Root - valid until the next call that
    /// mutates this handle's Terminal pool (same convention as
    /// le_design_name) - copy out immediately, don't hold across another
    /// call. Returns nullptr if handle is null or id doesn't name a
    /// Terminal on this handle.
    const char *le_terminal_name(LeHandle *handle, LeTerminalId id);

    /// @brief Row/Placement/PhysicalPort/Route/Region/LayoutVia friendly-id lookup
    /// pair, one per class - same reasoning as le_terminal_by_name/
    /// le_terminal_name above: each of these classes' own `name` field is
    /// unique_per_parent (scoped to its own Layout, not global - e.g. two
    /// different sub-block Layouts can each have a Placement named "U1"),
    /// so the generated by-name lookup pair is skipped for them the same
    /// way it is for Terminal (see Field.unique_per_parent's own
    /// docstring) - each `le_X_by_name` here does a linear scan over its
    /// class's own `Root::get_layout_X`, scoped to
    /// `handle->current_layout_id` (the Layout-view analog of
    /// `handle->current_abstract_id`), and each `le_X_name` is a direct
    /// field accessor for the resolve/format shim pair in
    /// le_tcl_shim.cpp. Returns an invalid id (by_name) or nullptr (name)
    /// under the same null/not-found conditions as le_terminal_by_name/
    /// le_terminal_name.
    LeRowId le_row_by_name(LeHandle *handle, const char *name);
    const char *le_row_name(LeHandle *handle, LeRowId id);
    LePlacementId le_placement_by_name(LeHandle *handle, const char *name);
    const char *le_placement_name(LeHandle *handle, LePlacementId id);
    LePhysicalPortId le_physical_port_by_name(LeHandle *handle, const char *name);
    const char *le_physical_port_name(LeHandle *handle, LePhysicalPortId id);
    LeRouteId le_route_by_name(LeHandle *handle, const char *name);
    const char *le_route_name(LeHandle *handle, LeRouteId id);
    LeRegionId le_region_by_name(LeHandle *handle, const char *name);
    const char *le_region_name(LeHandle *handle, LeRegionId id);
    LeLayoutViaId le_layout_via_by_name(LeHandle *handle, const char *name);
    const char *le_layout_via_name(LeHandle *handle, LeLayoutViaId id);

    /// @brief Number of property rows for the Terminal at `id` - same
    /// name/value table shape (LeProperty) le_object_property_count/_at
    /// use for LE_OBJECT_KIND_TERMINAL, but addressed directly by id, for
    /// a TCL `get_terminal`-style caller that isn't working off the
    /// current GUI selection. Rows: every plain Terminal field from cmg's
    /// generated to_properties() ("name", "direction", ...), plus
    /// "port_count" (int, from Root's own port index - "ports" itself
    /// isn't a stored field). No "rects"/"polygons"/"paths"/"layer_name"
    /// rows (those are Shape-level geometry - see le_shape_property_at
    /// directly, or le_object_property_at with LE_OBJECT_KIND_SHAPE, for
    /// one Shape's own geometry). Returns 0 if handle is null or id
    /// doesn't name a Terminal on this handle.
    int32_t le_terminal_property_count(LeHandle *handle, LeTerminalId id);

    /// @brief The property row at `index`
    /// (0..le_terminal_property_count(id)-1) for the Terminal at `id`.
    /// Returns an all-null/zero row (LeProperty::name == nullptr) if
    /// handle is null, id doesn't name a Terminal on this handle, or
    /// index is out of range.
    LeProperty le_terminal_property_at(LeHandle *handle, LeTerminalId id, int32_t index);

    /// @brief Resolve a dotted property path against the Terminal at
    /// `id` (UPDATES.md item 19.2's `get_properties`/`report_properties`
    /// dot-notation, e.g. `.name`, or chained through a hop like
    /// `.ports.port_class`) - see `backend/src/database/filter.hpp`'s
    /// `parse_property_path`/`resolve_property_path` for the grammar and
    /// resolution semantics (same `match_hop`/`get_field` machinery
    /// `-filter` expressions already use, just resolving a value instead
    /// of evaluating a comparison). Every segment but the last must be a
    /// hop, the last must be a leaf field - both checked against the
    /// same allowlist `-filter` validation uses (`validate_filter_path`
    /// in api.cpp) before resolving, so an unrecognized field/hop name
    /// is a real error (pushed via `le_message_count`/`le_message_at`),
    /// not silent. Returns an all-null/zero row (LeProperty::name ==
    /// nullptr) if handle/path is null, `path` fails to parse or
    /// validate, id doesn't name a Terminal on this handle, or the path
    /// is structurally valid but resolves to nothing for this specific
    /// object (e.g. a list hop with zero elements) - the latter case
    /// pushes no message, unlike a parse/validation failure.
    LeProperty le_terminal_property_path(LeHandle *handle, LeTerminalId id, const char *path);

    // --- Library/Design/Abstract/Shape property rows (UPDATES.md item
    // 19.2's get_properties/report_properties) - same by-id shape as
    // le_terminal_property_count/_at above, one pair per type. Unlike
    // Terminal's own "port_count" (or TerminalPort/Obstruction's
    // "shapes_count", further below), none of these four add a derived
    // child-pool "_count" row - just cmg's generated to_properties() as-is
    // for each type; see le_get_designs/le_get_terminals/
    // le_get_obstructions if you need those counts instead. ---

    /// @brief Number of property rows for the Library at `id`. Rows:
    /// every plain Library field from cmg's generated to_properties()
    /// ("name"). Returns 0 if handle is null or id doesn't name a Library
    /// on this handle.
    int32_t le_library_property_count(LeHandle *handle, LeLibraryId id);

    /// @brief The property row at `index`
    /// (0..le_library_property_count(id)-1) for the Library at `id`.
    /// Returns an all-null/zero row (LeProperty::name == nullptr) if
    /// handle is null, id doesn't name a Library on this handle, or index
    /// is out of range.
    LeProperty le_library_property_at(LeHandle *handle, LeLibraryId id, int32_t index);

    /// @brief Resolve a dotted property path against the Library at
    /// `id` (e.g. `.name`, or chained through a hop like
    /// `.designs.name`) - see le_terminal_property_path's own comment
    /// for the full grammar/validation/error contract.
    LeProperty le_library_property_path(LeHandle *handle, LeLibraryId id, const char *path);

    /// @brief Number of property rows for the Design at `id`. Rows: every
    /// plain Design field from cmg's generated to_properties() ("name").
    /// Returns 0 if handle is null or id doesn't name a Design on this
    /// handle.
    int32_t le_design_property_count(LeHandle *handle, LeDesignId id);

    /// @brief The property row at `index`
    /// (0..le_design_property_count(id)-1) for the Design at `id`.
    /// Returns an all-null/zero row (LeProperty::name == nullptr) if
    /// handle is null, id doesn't name a Design on this handle, or index
    /// is out of range.
    LeProperty le_design_property_at(LeHandle *handle, LeDesignId id, int32_t index);

    /// @brief Resolve a dotted property path against the Design at
    /// `id` (e.g. `.name`, or chained through a hop like
    /// `.library.name`) - see le_terminal_property_path's own comment
    /// for the full grammar/validation/error contract.
    LeProperty le_design_property_path(LeHandle *handle, LeDesignId id, const char *path);

    /// @brief Number of property rows for the Abstract at `id`. Rows:
    /// every plain Abstract field from cmg's generated to_properties()
    /// ("type", "size", "origin", ..., plus its own list-field "_count"
    /// rows like "boundary_count"). Returns 0 if handle is null or id
    /// doesn't name an Abstract on this handle.
    int32_t le_abstract_property_count(LeHandle *handle, LeAbstractId id);

    /// @brief The property row at `index`
    /// (0..le_abstract_property_count(id)-1) for the Abstract at `id`.
    /// Returns an all-null/zero row (LeProperty::name == nullptr) if
    /// handle is null, id doesn't name an Abstract on this handle, or
    /// index is out of range.
    LeProperty le_abstract_property_at(LeHandle *handle, LeAbstractId id, int32_t index);

    /// @brief Resolve a dotted property path against the Abstract at
    /// `id` (e.g. `.type`, or chained through a hop like
    /// `.design.name`) - see le_terminal_property_path's own comment
    /// for the full grammar/validation/error contract.
    LeProperty le_abstract_property_path(LeHandle *handle, LeAbstractId id, const char *path);

    /// @brief Search every Terminal on this handle for
    /// `filter_expression` (UPDATES.md item 15's `-filter {...}`, e.g.
    /// ".name =~ IN*" - see backend/src/database/filter.hpp for the full
    /// grammar). Returns the number of matches (0 if handle or
    /// filter_expression is null, or if nothing matched), or -1 if
    /// filter_expression fails to parse (see le_message_count/
    /// le_message_at for the parse error, pushed the same way
    /// le_read_lef's own errors are). Results are cached on the handle
    /// until the next le_search_terminal call - read them via
    /// le_search_result_terminal_at, same "valid until the next call"
    /// convention as this API's other cached-result accessors
    /// (le_object_property_at et al).
    int32_t le_search_terminal(LeHandle *handle, const char *filter_expression);

    /// @brief The LeTerminalId at `index` (0..le_search_terminal's last
    /// return value - 1) from the most recent le_search_terminal call on
    /// this handle. Returns an invalid id (index == UINT32_MAX) if
    /// handle is null or index is out of range.
    LeTerminalId le_search_result_terminal_at(LeHandle *handle, int32_t index);

    /// @brief Search Terminals (UPDATES.md item 19.1 - supersedes item
    /// 17's own single-filter-string `le_get_terminals`, this is the
    /// general contract every other le_get_* function in this API
    /// follows too). Three independent, each-optional axes:
    ///   - `of_abstract` scopes to one Abstract's own Terminals (see
    ///     le_get_abstracts) - pass an invalid id (e.g. a
    ///     default-constructed LeAbstractId) to use the default scope
    ///     instead: the currently selected Design's Abstract (item 17's
    ///     "current view" - le_set_current_design_abstract/
    ///     le_set_current_design_abstract_by_id), or none if no Design is
    ///     selected.
    ///   - `name_expression` glob-matches Terminal::name (Tcl `string
    ///     match` semantics, e.g. "IN*") - pass null or "" to skip this
    ///     axis (match every name).
    ///   - `filter_expression` (see backend/src/database/filter.hpp) -
    ///     pass null or "" to skip this axis. Only field/hop names this
    ///     API recognizes as valid for Terminal are accepted - an
    ///     unrecognized one is a validation error (item 19.1's own
    ///     error-checking requirement), not silent no-match.
    /// A Terminal matches iff it satisfies every given axis. Shares
    /// le_search_terminal's result buffer - read results back via
    /// le_search_result_terminal_at. Returns the match count (0 if handle
    /// is null or nothing matched), or -1 if filter_expression fails to
    /// parse or references an unknown field/hop (see le_message_count/
    /// le_message_at for either error).
    int32_t le_get_terminals(LeHandle *handle, LeAbstractId of_abstract, const char *name_expression, const char *filter_expression);

    // --- TerminalPort/Obstruction filter-search (Phase 4, continued) ---
    //
    // le_create_terminal_port/le_create_obstruction (an empty parent
    // only - no layer, no geometry) and le_create_shape/le_update_shape
    // (including their own -rects/-polygons/-paths - see
    // Klass.list_compound_kind() in codegen/codegen/schema.py) are all
    // generated (generated_tcl/declarations.inc, below), not hand-written
    // here. Only per-index rect/polygon/path removal
    // (le_remove_shape_rect/_polygon/_path, further below) stays
    // hand-written - adding/replacing geometry goes through
    // le_create_shape/le_update_shape's own generated flat-array
    // parameters instead.

    /// @brief Number of property rows for the TerminalPort at `id` - same
    /// by-id (not selection-scoped) shape as le_terminal_property_count.
    /// Rows: "shapes_count" (int), "port_class" (string) - cmg's
    /// generated to_properties() output for TerminalPortData, unchanged.
    /// Returns 0 if handle is null or id doesn't name a TerminalPort on
    /// this handle.
    int32_t le_terminal_port_property_count(LeHandle *handle, LeTerminalPortId id);

    /// @brief The property row at `index`
    /// (0..le_terminal_port_property_count(id)-1) for the TerminalPort at
    /// `id`. Returns an all-null/zero row (LeProperty::name == nullptr)
    /// if handle is null, id doesn't name a TerminalPort on this handle,
    /// or index is out of range.
    LeProperty le_terminal_port_property_at(LeHandle *handle, LeTerminalPortId id, int32_t index);

    /// @brief Resolve a dotted property path against the TerminalPort at
    /// `id` (e.g. `.port_class`, or chained through a hop like
    /// `.terminal.name`) - see le_terminal_property_path's own comment
    /// for the full grammar/validation/error contract.
    LeProperty le_terminal_port_property_path(LeHandle *handle, LeTerminalPortId id, const char *path);

    /// @brief Search every TerminalPort on this handle for
    /// `filter_expression` - see le_search_terminal's own comment for the
    /// full contract (grammar, error/caching behavior); identical here,
    /// just scoped to TerminalPort (e.g. ".terminal.name =~ IN*" or
    /// ".shapes.layer_name == M4", both straight from UPDATES.md item
    /// 15's own example). Returns the match count, or -1 on a parse
    /// error.
    int32_t le_search_terminal_port(LeHandle *handle, const char *filter_expression);

    /// @brief The LeTerminalPortId at `index` from the most recent
    /// le_search_terminal_port call - see le_search_result_terminal_at's
    /// own comment for the general contract.
    LeTerminalPortId le_search_result_terminal_port_at(LeHandle *handle, int32_t index);

    /// @brief Search TerminalPorts - see le_get_terminals' own comment
    /// for the general contract. `of_terminal` scopes to one Terminal's
    /// own Ports (see le_terminal_by_name) - pass an invalid id to use
    /// the default scope: every TerminalPort under every Terminal of the
    /// currently selected Abstract (a 2-hop current-view default).
    /// TerminalPort has no name field, so there is no `name_expression`
    /// parameter, only `filter_expression`. Returns the match count, or
    /// -1 on a filter parse/validation error.
    int32_t le_get_terminal_ports(LeHandle *handle, LeTerminalId of_terminal, const char *filter_expression);

    /// @brief Number of property rows for the Obstruction at `id` - same
    /// by-id shape as le_terminal_property_count. Rows: "shapes_count"
    /// (int) - cmg's generated to_properties() output for
    /// ObstructionData, unchanged. Returns 0 if handle is null or id
    /// doesn't name an Obstruction on this handle.
    int32_t le_obstruction_property_count(LeHandle *handle, LeObstructionId id);

    /// @brief The property row at `index`
    /// (0..le_obstruction_property_count(id)-1) for the Obstruction at
    /// `id`. Returns an all-null/zero row (LeProperty::name == nullptr)
    /// if handle is null, id doesn't name an Obstruction on this handle,
    /// or index is out of range.
    LeProperty le_obstruction_property_at(LeHandle *handle, LeObstructionId id, int32_t index);

    /// @brief Resolve a dotted property path against the Obstruction at
    /// `id` - Obstruction itself has no leaf scalar fields (see
    /// api.cpp's filter_field_tables), so every valid path here is
    /// chained through a hop, e.g. `.shapes.layer_name`. See
    /// le_terminal_property_path's own comment for the full grammar/
    /// validation/error contract.
    LeProperty le_obstruction_property_path(LeHandle *handle, LeObstructionId id, const char *path);

    /// @brief Search every Obstruction on this handle for
    /// `filter_expression` - see le_search_terminal's own comment for the
    /// full contract. Returns the match count, or -1 on a parse error.
    int32_t le_search_obstruction(LeHandle *handle, const char *filter_expression);

    /// @brief The LeObstructionId at `index` from the most recent
    /// le_search_obstruction call - see le_search_result_terminal_at's
    /// own comment for the general contract.
    LeObstructionId le_search_result_obstruction_at(LeHandle *handle, int32_t index);

    /// @brief Search Obstructions - see le_get_terminals' own comment
    /// for the general contract. `of_abstract` scopes to one Abstract's
    /// own Obstructions - pass an invalid id to use the default scope:
    /// the currently selected Abstract. Obstruction has no name field,
    /// so there is no `name_expression` parameter, only
    /// `filter_expression`. Returns the match count, or -1 on a filter
    /// parse/validation error.
    int32_t le_get_obstructions(LeHandle *handle, LeAbstractId of_abstract, const char *filter_expression);

    // --- Shape CRUD, addressed by a stable id (TCL_EXPLORATION.md Phase 3:
    // `Shape` was pooled specifically so a single existing shape - attached
    // to either a TerminalPort or an Obstruction - could be read/updated/
    // deleted independently of its parent). A shape's own id doesn't say
    // which kind of parent it belongs to - le_terminal_port_shape_at/
    // le_obstruction_shape_at are how a caller discovers a LeShapeId in
    // the first place (enumerating a specific parent's shapes) or the
    // generated le_create_shape (generated_tcl/declarations.inc, below -
    // takes both a LeTerminalPortId and a LeObstructionId, exactly one of
    // which must resolve, replacing this API's own former
    // le_create_terminal_port_shape/le_create_obstruction_shape split)
    // returns one directly; after that, every le_shape_*/le_remove_shape_*
    // call below, and the generated le_delete_shape (generated_tcl/
    // declarations.inc), only needs the LeShapeId itself.
    //
    // Rects/polygons/paths are all treated the same way: none are
    // privileged, each has its own count/read/add/remove functions
    // following the same shape (a plain 0-based position within that
    // shape's own list of that member - not a further stable id; unlike
    // a Shape itself, a rect/polygon/path never needs to be addressed
    // independently of the Shape it's part of). Coordinates are always a
    // flat microns array (converted to/from dbu via the same shared/
    // global Technology::database_units_microns every other coordinate
    // in this API uses) - a real coordinate-list typemap, so a caller
    // doesn't have to pre-flatten into a raw double*, is Phase 5's job
    // (see TCL_EXPLORATION.md); this is the plain-C building block it
    // wraps. Texts aren't included - see TCL_EXPLORATION.md for why
    // (they're a Pipeline-computed render-time label, never LEF-authored
    // data, so there's nothing for a caller to create/read here). ---


    /// @brief One point, in microns - `le_shape_polygon_point_at`'s and
    /// `le_shape_path_point_at`'s return type.
    typedef struct LePointUm
    {
        double x_um;
        double y_um;
    } LePointUm;

    /// @brief One rect's corners, in microns - `le_shape_rect_at`'s
    /// return type.
    typedef struct LeRectUm
    {
        double ll_x_um;
        double ll_y_um;
        double ur_x_um;
        double ur_y_um;
    } LeRectUm;


    /// @brief The LeShapeId at `index` from the most recent le_get_shapes
    /// call - see le_search_result_library_at's own contract.
    LeShapeId le_search_result_shape_at(LeHandle *handle, int32_t index);

    /// @brief Number of property rows for the Shape at `id` (UPDATES.md
    /// item 19.2 - same by-id shape as le_terminal_property_count/_at and
    /// its Library/Design/Abstract siblings above). Rows: every plain
    /// Shape field from cmg's generated to_properties() ("layer_name",
    /// "spacing", ..., plus its own list-field "_count" rows like
    /// "rects_count"). Shape has no children (the leaf of the
    /// Library->Design->Abstract->{Terminal->TerminalPort,Obstruction}->
    /// Shape hierarchy), so unlike TerminalPort/Obstruction there's no
    /// derived child-pool "_count" row this function could even add.
    /// Returns 0 if handle is null or id doesn't name a Shape on this
    /// handle.
    int32_t le_shape_property_count(LeHandle *handle, LeShapeId id);

    /// @brief The property row at `index`
    /// (0..le_shape_property_count(id)-1) for the Shape at `id`. Returns
    /// an all-null/zero row (LeProperty::name == nullptr) if handle is
    /// null, id doesn't name a Shape on this handle, or index is out of
    /// range.
    LeProperty le_shape_property_at(LeHandle *handle, LeShapeId id, int32_t index);

    /// @brief Resolve a dotted property path against the Shape at `id`
    /// (e.g. `.layer_name`, or chained through a hop like
    /// `.terminal_port.terminal.name`) - see le_terminal_property_path's
    /// own comment for the full grammar/validation/error contract.
    LeProperty le_shape_property_path(LeHandle *handle, LeShapeId id, const char *path);

    /// @brief Number of shapes owned by the TerminalPort at `id` -
    /// indexes `le_terminal_port_shape_at`'s own `index` parameter,
    /// 0..this-1. Returns 0 if handle is null or id doesn't name a
    /// TerminalPort on this handle.
    int32_t le_terminal_port_shape_count(LeHandle *handle, LeTerminalPortId id);

    /// @brief The LeShapeId at `index` (0..le_terminal_port_shape_count(id)-1)
    /// owned by the TerminalPort at `id`. Returns an invalid LeShapeId
    /// (index == UINT32_MAX) if handle is null, id doesn't name a
    /// TerminalPort on this handle, or index is out of range.
    LeShapeId le_terminal_port_shape_at(LeHandle *handle, LeTerminalPortId id, int32_t index);

    /// @brief Number of shapes owned by the Obstruction at `id` - see
    /// le_terminal_port_shape_count's own comment for the general
    /// contract. Returns 0 if handle is null or id doesn't name an
    /// Obstruction on this handle.
    int32_t le_obstruction_shape_count(LeHandle *handle, LeObstructionId id);

    /// @brief The LeShapeId at `index` owned by the Obstruction at `id` -
    /// see le_terminal_port_shape_at's own comment for the general
    /// contract. Returns an invalid LeShapeId if handle is null, id
    /// doesn't name an Obstruction on this handle, or index is out of
    /// range.
    LeShapeId le_obstruction_shape_at(LeHandle *handle, LeObstructionId id, int32_t index);

    /// @brief The layer name of the Shape at `id`. Owned by the handle's
    /// Root - valid until the handle is destroyed, this shape is
    /// deleted, or le_update_shape changes it, never owned by
    /// the caller. Returns null if handle is null or id doesn't name a
    /// Shape on this handle.
    const char *le_shape_layer_name(LeHandle *handle, LeShapeId id);

    /// @brief Number of rects on the Shape at `id` - indexes
    /// `le_shape_rect_at`'s own `index` parameter, 0..this-1. Returns 0
    /// if handle is null or id doesn't name a Shape on this handle.
    int32_t le_shape_rect_count(LeHandle *handle, LeShapeId id);

    /// @brief The rect at `index` (0..le_shape_rect_count(id)-1) on the
    /// Shape at `id`, in microns. Returns an all-zero LeRectUm if handle
    /// is null, id doesn't name a Shape on this handle, index is out of
    /// range, or no Technology has been read yet (needed for the
    /// dbu-to-micron conversion) - the same "can't distinguish a real
    /// zero-sized rect from a degrade-gracefully zero" tradeoff
    /// le_snapped_mouse_position's own comment already accepts for this
    /// API, not a new one.
    LeRectUm le_shape_rect_at(LeHandle *handle, LeShapeId id, int32_t index);

    /// @brief Remove the rect at `index` (0..le_shape_rect_count(id)-1)
    /// from the Shape at `id`, shifting every later rect's index down by
    /// one (matching le_shape_rect_at's own 0-based-position addressing
    /// - not a stable id, see this section's own comment for why that's
    /// fine here). Returns 0 on success, nonzero if handle is null, id
    /// doesn't name a Shape on this handle, or index is out of range.
    int le_remove_shape_rect(LeHandle *handle, LeShapeId id, int32_t index);

    /// @brief Number of polygons on the Shape at `id` - indexes
    /// `le_shape_polygon_point_count`/`le_shape_polygon_point_at`'s own
    /// `polygon_index` parameter, 0..this-1. Returns 0 if handle is null
    /// or id doesn't name a Shape on this handle.
    int32_t le_shape_polygon_count(LeHandle *handle, LeShapeId id);

    /// @brief Number of points in the polygon at `polygon_index`
    /// (0..le_shape_polygon_count(id)-1) on the Shape at `id` - indexes
    /// `le_shape_polygon_point_at`'s own `point_index` parameter,
    /// 0..this-1. Returns 0 if handle is null, id doesn't name a Shape
    /// on this handle, or polygon_index is out of range.
    int32_t le_shape_polygon_point_count(LeHandle *handle, LeShapeId id, int32_t polygon_index);

    /// @brief The point at `point_index` in the polygon at
    /// `polygon_index` on the Shape at `id`, in microns. Returns an
    /// all-zero LePointUm if handle is null, id doesn't name a Shape on
    /// this handle, either index is out of range, or no Technology has
    /// been read yet.
    LePointUm le_shape_polygon_point_at(LeHandle *handle, LeShapeId id, int32_t polygon_index, int32_t point_index);

    /// @brief Remove the polygon at `polygon_index`
    /// (0..le_shape_polygon_count(id)-1) from the Shape at `id` - same
    /// position-shifts-down semantics as le_remove_shape_rect. Returns 0
    /// on success, nonzero if handle is null, id doesn't name a Shape on
    /// this handle, or polygon_index is out of range.
    int le_remove_shape_polygon(LeHandle *handle, LeShapeId id, int32_t polygon_index);

    /// @brief Number of paths on the Shape at `id` - indexes
    /// `le_shape_path_width_um`/`le_shape_path_point_count`/
    /// `le_shape_path_point_at`'s own `path_index` parameter, 0..this-1.
    /// Returns 0 if handle is null or id doesn't name a Shape on this
    /// handle.
    int32_t le_shape_path_count(LeHandle *handle, LeShapeId id);

    /// @brief The width, in microns, of the path at `path_index`
    /// (0..le_shape_path_count(id)-1) on the Shape at `id`. Returns 0 if
    /// handle is null, id doesn't name a Shape on this handle,
    /// path_index is out of range, or no Technology has been read yet.
    double le_shape_path_width_um(LeHandle *handle, LeShapeId id, int32_t path_index);

    /// @brief Number of points in the centerline of the path at
    /// `path_index` on the Shape at `id` - indexes
    /// `le_shape_path_point_at`'s own `point_index` parameter,
    /// 0..this-1. Returns 0 if handle is null, id doesn't name a Shape
    /// on this handle, or path_index is out of range.
    int32_t le_shape_path_point_count(LeHandle *handle, LeShapeId id, int32_t path_index);

    /// @brief The point at `point_index` on the centerline of the path
    /// at `path_index` on the Shape at `id`, in microns. Returns an
    /// all-zero LePointUm if handle is null, id doesn't name a Shape on
    /// this handle, either index is out of range, or no Technology has
    /// been read yet.
    LePointUm le_shape_path_point_at(LeHandle *handle, LeShapeId id, int32_t path_index, int32_t point_index);

    /// @brief Remove the path at `path_index`
    /// (0..le_shape_path_count(id)-1) from the Shape at `id` - same
    /// position-shifts-down semantics as le_remove_shape_rect. Returns 0
    /// on success, nonzero if handle is null, id doesn't name a Shape on
    /// this handle, or path_index is out of range.
    int le_remove_shape_path(LeHandle *handle, LeShapeId id, int32_t path_index);

    // --- Generated TCL property-reading, create_<type>, update_<type>,
    // and delete_<type> surface (see backend/CLAUDE.md's TCL section) -
    // one Id typedef, friendly-id-by-name lookup, property table
    // accessors, is_child-field enumeration pairs, get_<type> search,
    // create_<type> (le_create_terminal/le_create_terminal_port/
    // le_create_obstruction/le_create_shape included - the last unifies
    // the former hand-written le_create_terminal_port_shape/
    // le_create_obstruction_shape split into one function taking both
    // parent ids, exactly one of which must resolve), update_<type>, and
    // delete_<type> (le_delete_terminal/le_delete_terminal_port/
    // le_delete_obstruction/le_delete_shape included - these four used to
    // be hand-written here; see Klass.delete_api_body(),
    // codegen/codegen/schema.py, for the cascade-to-owned-children
    // mechanism their generated replacements use), for every TCL-readable
    // class. Never edit generated_tcl/declarations.inc directly -
    // regenerate via the regen-tcl skill instead. ---
#include "generated_tcl/declarations.inc"

    /// @brief The singleton Technology's friendly id (see
    /// le_tcl_shim.hpp's own "IDs" comment for the friendly-id
    /// convention this feeds) - Technology is a single shared per-
    /// session instance, same assumption database_units_microns() (api.cpp)
    /// already makes. Returns an invalid id (index == UINT32_MAX) if no
    /// Technology has been read yet.
    LeTechnologyId le_technology_id(LeHandle *handle);

#ifdef __cplusplus
}
#endif
