#pragma once
#include <stdbool.h>
#include <stdint.h>

// Layout Engine's C API - the one surface every front end calls: the
// Dear ImGui GUI (src/gui/, via GuiProvider) and the Tcl shim
// (src/tcl/le_tcl_shim.cpp). Deliberately plain C (not C++) in every
// public declaration - no std:: types, no default arguments, no
// overloads - so LeHandle/LePixelBuffer have a stable,
// toolchain-independent ABI and any C FFI generator can parse this
// header.
//
// Thread safety: every function below is safe to call concurrently from
// multiple threads on the *same* LeHandle, except le_destroy() (see its
// own doc comment) - each internally locks a mutex owned by the handle.
// This is needed, not defensive: le_shell's GUI renders on a background
// thread (le_render_pixel_buffer()) while the GUI thread forwards input
// (le_set_mouse_position(), le_mouse_down()/up(), le_zoom(), ...) and the
// Tcl console thread runs commands - all on the same handle.

#ifdef __cplusplus
extern "C"
{
#endif

    /// @brief Opaque handle to one editor instance: a Root (database),
    /// ViewLayerSet, the pipelines module's own ViewRenderPipeline, and
    /// every piece of per-handle mutable view/interaction state (current
    /// Abstract/Layout, pan/zoom, layer visibility, selection, hover,
    /// rulers, Move-drag state, interaction mode), all
    /// reused across repeated calls rather than reconstructed per call -
    /// a fresh ViewRenderPipeline per call would defeat its own internal
    /// MemoizingStage caching entirely. Opaque so this header stays
    /// C-compatible; the real struct is defined only in api/le_handle.hpp
    /// (included by api.cpp, never by this header).
    typedef struct LeHandle LeHandle;

#include "generated/api/ids.inc"

    /// @brief Raw RGBA8888 pixel buffer, mirroring le::PixelBuffer
    /// (compose_stage.hpp) but using explicit fixed-width types (not
    /// `int`/`size_t`, whose width isn't guaranteed identical across
    /// toolchains) for a stable ABI. `data` points into memory owned by
    /// the LeHandle - valid
    /// only until the next le_render_pixel_buffer() call on the same
    /// handle (or le_destroy()), never owned by the caller and never to be
    /// freed by it. Premultiplied alpha, row-major, top-to-bottom; row_bytes
    /// may exceed width * 4 - always index by it, never assume a tight
    /// stride.
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
        /// LEF macro (le_read_lef() only) does not (a Design can have
        /// either, both, or neither of these two
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
        /// useful extra information -
        /// toggling either one already changes the exact same underlying
        /// visibility/selectability flag, since a pseudo-row has exactly
        /// one column.
        int32_t has_physical_layer;
    } LeLayerRow;

    /// @brief Result of le_snapped_mouse_position(): the current mouse
    /// position's coordinates, snapped to the minor grid (mirrors
    /// LeHandle::snapped_mouse_position) and converted
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

    /// @brief Number of extensions built into this binary (LE_EXTENSION_DIRS),
    /// registered at startup by le::ext::register_all().
    int32_t le_extension_count(void);

    /// @brief The name of extension `index` (0..le_extension_count()-1), in
    /// dependency order. Static storage; null if out of range.
    const char *le_extension_name(int32_t index);

    /// @brief The version of extension `index`. Static storage; null if out of range.
    const char *le_extension_version(int32_t index);

    /// @brief Read a LEF file into this handle's shared Root, its MACROs
    /// into the Library named `library_name` (required; created at the
    /// first MACRO if it doesn't exist yet). A
    /// MACRO whose Design (matched by name) already has an Abstract view
    /// is an error that fails the read. Safe to call multiple times on
    /// the same handle - e.g. a tech file (LAYER definitions, no macros)
    /// followed by one or more macro files that reference those layers by
    /// name, matching LEFReader::read_lef's own existing-Technology reuse
    /// (pass the tech file first when a macro file depends on it - see
    /// render_preview.cpp for the same convention already used there).
    /// Returns 0 on success, matching LEFReader::read_lef's own result
    /// code (nonzero otherwise, including if handle or path is null).
    int le_read_lef(LeHandle *handle, const char *path, const char *library_name);

    /// @brief Reads a DEF file into this handle's Root via DEFReader,
    /// same shape as le_read_lef (0 on success) - its DESIGN into the
    /// Library named `library_name` (required, created if missing); a
    /// Design that already has a Layout view is an error.
    /// Unlike le_read_lef, doesn't touch layer visibility defaults or
    /// rebuild ViewLayerSet - DEF doesn't introduce new physical Layers
    /// of its own (Step 2's own layer-generation work, not yet done, is
    /// what a Row/Track/Blockage/BOUNDARY-purpose rendering pass would
    /// need instead). Does still resolve/create the shared Technology
    /// (DEFReader::technology_id_) the same reuse-or-create way
    /// le_read_lef's own LEFReader does, since NONDEFAULTRULES needs one
    /// even when no LEF has been read into this handle yet.
    int le_read_def(LeHandle *handle, const char *path, const char *library_name);

    /// @brief Reads one or more SystemVerilog/Verilog files (filenames/
    /// filename_count - a plain C array, not std::vector, matching every
    /// other multi-value api.hpp entry point) into this handle's shared
    /// Root via SVReader, new Designs into the Library named
    /// `library_name` (required, created if missing); any module whose
    /// Design already has a Schematic view is an error that fails the
    /// read before anything is created. is_netlist nonzero selects the full-elaboration netlist
    /// flavor (SVReader::read_netlist - accurate parameter/generate
    /// resolution, for a real gate-level netlist); zero selects the
    /// syntax-only RTL flavor (SVReader::read_rtl - tolerates invalid/
    /// unsupported content by storing it as a logic-cloud Instance, see
    /// Instance.rtl_text). In netlist mode, automatically generates stub
    /// Verilog module declarations (verilog_stub_writer.hpp - a port
    /// list plus an empty body, LEF bus-bit pins combined into real
    /// `[msb:lsb]` ports) for every Design already in this handle's Root
    /// that has a LEF Abstract but no Schematic (any read_lef so far),
    /// writes them to a real temporary file, and includes that file in
    /// this same call's own elaboration - the only way slang can resolve
    /// a gate-level instantiation of a LEF-only leaf cell/macro for real
    /// instead of falling back to the raw-syntax/no-RTL path (they only
    /// share one slang::ast::Compilation within one call - see
    /// SVReader::read_netlist's own body). The temp file is removed
    /// again once this call returns; le_write_verilog_stubs below writes
    /// the same generated source to a caller-chosen path instead, for
    /// inspection or a manual multi-step workflow. Automatically
    /// re-resolves any newly-resolvable Instance.reference_design
    /// against Designs already in this session (SVReader::
    /// link_unresolved_instances - also directly callable via
    /// le_link_unresolved_instances for a later read, e.g. an LEF read
    /// after this one supplies a previously-missing leaf cell). Same
    /// 0/nonzero return + spdlog-logged-messages convention as le_read_lef/le_read_def.
    int le_read_verilog(LeHandle *handle, const char *const *filenames, int32_t filename_count, int32_t is_netlist, const char *library_name);

    /// @brief Writes stub Verilog module declarations (a port list plus
    /// an empty body - see verilog_stub_writer.hpp's own top-of-file
    /// comment) to `path`, one per Design in `library_id` that has an
    /// Abstract (a LEF-read physical view) but no Schematic - the usual
    /// standard-cell/macro situation for a gate-level netlist read
    /// against a LEF-only cell library. le_read_verilog(...,
    /// is_netlist=1) above already generates and uses this same content
    /// automatically for every read - call this directly only to inspect
    /// the generated stub source, or to drive a manual multi-step
    /// workflow of your own. `library_id` may be the invalid/default id
    /// to mean "the sole Library read so far" - fails with an ERROR
    /// message if zero or more than one Library exists and none was
    /// given explicitly (no "current Library" concept exists the way
    /// Abstract/Layout have - see le_write_lef/le_write_def's own
    /// current-view fallback for the two that do). Returns 0 on success;
    /// a nonzero code on failure, with the reason logged via spdlog.
    int le_write_verilog_stubs(LeHandle *handle, const char *path, LeLibraryId library_id);

    /// @brief Re-resolves Instance.reference_design for every Instance in
    /// this handle's Root whose reference_design is currently unset,
    /// matching reference_name against Design.name (SVReader::
    /// link_unresolved_instances) - e.g. after a later le_read_lef
    /// supplies a leaf cell a prior le_read_verilog call left unresolved.
    /// Returns the number of Instances newly resolved.
    int32_t le_link_unresolved_instances(LeHandle *handle);

    /// @brief Deletes a Net and clears every dangling reference to it
    /// left behind: deletes
    /// every linked Route in the Net's own sibling Layout (an orphaned
    /// Route has no meaning without its Net), clears (does not delete)
    /// every linked PhysicalPort's own `.net` in that same Layout, and
    /// clears every Pin.net/Port.net in the Net's own Schematic that
    /// pointed at it - all batched into one undo/redo transaction
    /// (matches move_click_unlocked's own pattern). Superset of the
    /// generated delete_net (which only deletes the Net itself and
    /// leaves every dangling reference above pointing at a stale id) -
    /// this is what the TCL `delete_net` command routes to (see
    /// le_tcl_procs.tcl). 0 on success, 1 (with a message pushed) if
    /// `id` doesn't resolve to a live Net.
    int le_delete_net_cascade(LeHandle *handle, LeNetId id);

    /// @brief Renames a Net and propagates the rename to its own linked
    /// Route/PhysicalPort, if any - a simple 1:1 follow-on rename, since
    /// a Net has no
    /// descendants of its own. Batched into one undo/redo transaction.
    /// Superset of the generated update_net (which only renames the Net
    /// itself); this is what the TCL `update_net` command routes to when
    /// its own `-name` flag is present (see le_tcl_procs.tcl). 0 on
    /// success, 1 (with a message pushed) on failure (unknown id, empty
    /// new_name, or a sibling Net/Route/PhysicalPort name collision).
    int le_rename_net_propagate(LeHandle *handle, LeNetId id, const char *new_name);

    /// @brief Renames an Instance and propagates the rename to every
    /// Placement/Route/PhysicalPort whose own DEF-style hierarchical
    /// name embeds its path segment - both the Instance's own linked
    /// Placement and every descendant Instance/Net's own linked
    /// Placement/Route/PhysicalPort - using real id-based graph traversal
    /// (never string-prefix matching, which would also hit siblings like
    /// "a/b2" when renaming "a/b"). Batched into one undo/redo transaction.
    /// Superset of the generated update_instance (which only renames the
    /// Instance itself); this is what the TCL `update_instance` command
    /// routes to when its own `-name` flag is present (see
    /// le_tcl_procs.tcl). 0 on success, 1 (with a message pushed) on
    /// failure (unknown id, empty new_name, or a sibling Instance name
    /// collision). See rename_propagation.hpp's own "known limitation"
    /// comment for the one case (the renamed Instance has no linked
    /// Placement yet) where descendant propagation is silently skipped.
    int le_rename_instance_propagate(LeHandle *handle, LeInstanceId id, const char *new_name);

    /// @brief What Technology layer content le_write_lef() also includes
    /// alongside (or instead of) the written Abstract's own MACRO -
    /// crosses the FFI boundary as a plain int32_t like every other small
    /// enum here. Mirrors LEFWriter::LayerWriteMode 1:1 - see that
    /// class's own doc comment (src/io/lef_writer.hpp) for full
    /// per-value semantics.
    typedef enum
    {
        LE_LEF_LAYER_WRITE_MODE_NONE = 0,
        LE_LEF_LAYER_WRITE_MODE_INCLUDE_WITH_ABSTRACT = 1,
        LE_LEF_LAYER_WRITE_MODE_TECHNOLOGY_ONLY = 2,
    } LeLefLayerWriteMode;

    /// @brief Writes a LEF file for one or more Abstracts via LEFWriter -
    /// see that class's own doc comment for exact scope. One call can
    /// write a whole Library's worth of MACROs into a single file, per this
    /// resolution order:
    ///   1. `abstract_id_count > 0` - write exactly those Abstracts, in
    ///      order (their own owning Library is irrelevant here - an
    ///      explicit list always wins outright).
    ///   2. Else, `library_id` valid - write every Abstract belonging to
    ///      every Design in that Library (LE_LEF_LAYER_WRITE_MODE_NONE/
    ///      _INCLUDE_WITH_ABSTRACT only - a Library with no Abstracts yet
    ///      writes zero MACROs, not an error).
    ///   3. Else, `le_current_abstract(handle)` is valid - write just that
    ///      one.
    ///   4. Else - fails with an ERROR message rather than silently writing
    ///      an empty MACRO-less file.
    /// All four steps are skipped entirely when `layer_write_mode` is
    /// LE_LEF_LAYER_WRITE_MODE_TECHNOLOGY_ONLY, which ignores every
    /// Abstract/Library argument (mirrors
    /// LEFWriter::LayerWriteMode::TechnologyOnly's own doc comment).
    /// `abstract_ids` may be null when `abstract_id_count` is 0.
    /// `library_id` may be the invalid/default id (a default-constructed
    /// LeLibraryId{}) to skip step 2 entirely. Returns 0 on success,
    /// matching le_read_lef's own convention (nonzero otherwise, including
    /// if handle or path is null); LEFWriter's own messages are logged via
    /// spdlog either way, same as le_read_lef.
    int le_write_lef(LeHandle *handle, const char *path,
                      const LeAbstractId *abstract_ids, int32_t abstract_id_count,
                      LeLibraryId library_id, int32_t layer_write_mode);

    /// @brief Writes a DEF file for `layout_id` via DEFWriter - see that
    /// class's own doc comment for exact scope. `layout_id` may be the
    /// invalid/default id to mean "use le_current_layout(handle)" instead -
    /// if that's also unset, fails with an ERROR message. Returns 0 on
    /// success, matching le_read_def's own convention; DEFWriter's own
    /// messages are logged via spdlog either way.
    int le_write_def(LeHandle *handle, const char *path, LeLayoutId layout_id);

    /// @brief Saves the whole database to a native Layout Engine database
    /// file (.led - see plans/NATIVE_FILE_FORMAT_RESEARCH.md). Written via a
    /// temporary file and a rename, so a failed save never damages an
    /// existing file. On success the database counts as saved (the exit
    /// dialog's unsaved-changes check). With `with_session` nonzero the
    /// file also keeps the session - the open view, viewport, current
    /// objects and layer/purpose/filter visibility - which le_read_db
    /// restores. Returns 0 on success; errors are logged via spdlog.
    int le_write_db(LeHandle *handle, const char *path, int32_t with_session);

    /// @brief Loads a native database file into this handle. Only into an
    /// empty database (nothing read or created yet) - fails otherwise,
    /// leaving everything as it was. A file written by an older schema
    /// loads too when its differences are only added, removed or
    /// reordered classes/fields (each dropped or defaulted field is logged
    /// as a warning); one needing a real migration fails with a message
    /// saying so. Clears undo/redo, and leaves the database saved. With
    /// `with_session` nonzero, restores the file's session if it has one.
    /// Returns 0 on success; errors are logged via spdlog.
    int le_read_db(LeHandle *handle, const char *path, int32_t with_session);

    /// @brief The native database file last read or written ("" until one
    /// is) - valid until the next call on this thread.
    const char *le_db_path(LeHandle *handle);

    /// @brief 1 if the database holds no objects (le_read_db can load
    /// into it), else 0.
    int32_t le_database_is_empty(LeHandle *handle);

    /// @brief A human-readable description of a native database file
    /// (schema version, whether this build's schema matches, its
    /// extensions, the migrations loading it runs, per-class object counts)
    /// without loading it - or an "error: ..." line. The
    /// returned string is valid until the next call on this thread.
    const char *le_db_info(const char *path);

    /// @brief Rewrites a native database file with this build's schema,
    /// running the migrations it needs and keeping its session, without
    /// touching any session's database. `out_path` may equal `in_path`.
    /// Returns a summary (the migrations applied, anything dropped), or
    /// text starting "error: " - valid until the next call on this thread.
    const char *le_migrate_db(const char *in_path, const char *out_path);

    /// @brief Number of Designs currently loaded across every LEF file
    /// read into this handle so far. 0 if handle is null.
    int32_t le_design_count(LeHandle *handle);

    /// @brief Name of the Design at `index` (0..le_design_count()-1).
    /// Returns null if handle is null or index is out of range. The
    /// returned pointer is owned by the handle's Root - valid until the
    /// handle is destroyed (no Design-removal API exists yet), never
    /// owned by the caller.
    const char *le_design_name(LeHandle *handle, int32_t index);

    /// @brief Whether the most recent le_X_property_path call on this
    /// handle (any class - one shared flag, not per-class) logged a
    /// parse/validation error via spdlog::error, as opposed to
    /// resolving cleanly (possibly to nothing, e.g. a structurally valid
    /// path whose last hop is an empty list - that case logs nothing and
    /// leaves this false). Both a genuine error and a legitimate "no
    /// data" resolution return the same all-null LeProperty, so this is
    /// the only way a caller can still tell them apart now that
    /// per-message detail only goes to spdlog, not a queryable queue.
    /// Reset to false at the top of every le_X_property_path call, so
    /// check it immediately after - not a "last error" that survives
    /// across other calls. False (not an error) if handle is null.
    int32_t le_property_path_failed(LeHandle *handle);

    /// @brief Select the Design at `index` as the one le_render_pixel_buffer()
    /// renders (its Abstract view). Returns 0 on success, nonzero if
    /// handle is null or index is out of range - the current selection is
    /// left unchanged on failure.
    int le_set_current_design_abstract(LeHandle *handle, int32_t index);

    /// @brief Number of Libraries currently loaded - one per distinct
    /// library name given to le_read_lef/le_read_def/le_read_verilog (or
    /// created directly). 0 if handle is null. The top level of a
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
    /// both). Clears current_abstract_id (and LeHandle::current_abstract(),
    /// so le_render_pixel_buffer() stops rendering the old Abstract) the
    /// same way le_set_current_design_abstract/_by_id clear
    /// current_layout_id (and LeHandle::current_layout()). Returns 0 on
    /// success, nonzero if handle is null or index is out of range - the
    /// current selection is left unchanged on failure. Once selected,
    /// le_render_pixel_buffer() renders this Layout's own content plus its
    /// placed instances, recursed le_hierarchy_depth() levels deep - see
    /// le_set_hierarchy_depth()'s own comment.
    int le_set_current_design_layout(LeHandle *handle, int32_t index);

    /// @brief Same as le_set_current_design_layout, but addressed by
    /// LeDesignId - same relationship to it as le_set_current_design_abstract_by_id
    /// has to le_set_current_design_abstract.
    int le_set_current_design_layout_by_id(LeHandle *handle, LeDesignId design_id);

    /// @brief How many further levels of Placement -> Design a Layout view
    /// recurses into before a placed instance falls back to its own
    /// Abstract, rather than recursing into its own nested Layout - 0
    /// (the default) means every placement
    /// falls back straight to its Abstract. 0 if handle is null.
    int32_t le_hierarchy_depth(LeHandle *handle);

    /// @brief Sets le_hierarchy_depth(). Negative values are rejected (the
    /// current depth is left unchanged) rather than clamped - same
    /// "reject, don't silently clamp" convention le_set_scale uses for a
    /// non-positive scale. Bumps LeHandle::hierarchy_version() (only)
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
    /// returned int is le::ViewLayerPurpose's raw ordinal (its order in
    /// schema.py's `purposes`, not necessarily this index); le_purpose_name()
    /// gives its label. Ordinals are only meaningful within one process.
    /// `index` itself walks ViewLayerSet::purposes()'s own
    /// first-encountered order instead (ROW, then BOUNDARY, then
    /// PLACEMENT, then TERMINAL/OBSTRUCTION/TRACK_PREFERRED/
    /// TRACK_NON_PREFERRED/ROUTING_BLOCKAGE/ROUTE from the first physical
    /// Layer row, then GCELLGRID/PLACEMENT_BLOCKAGE/REGION's own
    /// pseudo-rows) - a caller must
    /// always pass `le_purpose_at`'s own return value back into
    /// `le_is_purpose_visible`/`le_set_purpose_visible`, never assume
    /// index equals ordinal.
    ///
    /// Returns -1 if handle is null or index is out of range, rather than
    /// crashing.
    int32_t le_purpose_at(LeHandle *handle, int32_t index);

    /// @brief Number of purposes that exist (every le::ViewLayerPurpose,
    /// whether or not the current ViewLayerSet uses it); ordinals run
    /// 0..le_purpose_kind_count()-1. Needs no handle.
    int32_t le_purpose_kind_count(void);

    /// @brief The user-facing label of purpose ordinal `purpose` (e.g.
    /// "trackPreferred") - what the Tcl purpose commands take and the
    /// Layers panel shows. Static storage; null if out of range.
    const char *le_purpose_name(int32_t purpose);

    /// @brief Nonzero if purpose ordinal `purpose` is visible until the
    /// user hides it; zero if hidden by default or out of range.
    int32_t le_purpose_visible_by_default(int32_t purpose);

    /// @brief Current visibility of every ViewLayer whose LeLayerRow::name
    /// is `layer_name` (case-sensitive exact match) - i.e. a whole row
    /// (every purpose-column of it) together, not one column - a
    /// coarser-grained "layer visibility widget" model than one toggle per
    /// grid cell: see le_is_purpose_visible() for the other axis, and
    /// LeHandle::is_view_layer_visible for how a specific column's effective
    /// visibility combines both. Visible by default until toggled. Returns
    /// true if handle or layer_name is null, matching LeHandle's
    /// own "unknown name defaults to visible" default.
    bool le_is_layer_name_visible(LeHandle *handle, const char *layer_name);

    /// @brief Set the visibility of every ViewLayer whose LeLayerRow::name
    /// is `layer_name` - e.g. a layer-visibility widget's row-header
    /// checkbox. Mirrors LeHandle::set_layer_name_visible directly (affects
    /// rendering). A no-op if
    /// handle or layer_name is null.
    void le_set_layer_name_visible(LeHandle *handle, const char *layer_name, bool visible);

    /// @brief Max number of threads this handle's oneTBB-backed pipelines
    /// may use at once - a process-wide
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
    /// column-header checkbox. Mirrors LeHandle::set_purpose_visible directly
    /// (affects rendering). A no-op if handle is null.
    void le_set_purpose_visible(LeHandle *handle, int32_t purpose, int32_t visible);

    /// @brief The current interaction mode. Select is the only mode where
    /// le_mouse_up's mouse clicks/drags change the current selection -
    /// Edit mode restricts mouse interaction to editing whatever is
    /// already selected (Move, Resize, Delete). Ruler mode is where
    /// le_mouse_up's clicks place ruler points instead - see
    /// le_finish_ruler/le_clear_rulers. Switched either via
    /// LE_KEY_SELECT_MODE/LE_KEY_EDIT_MODE/LE_KEY_RULER_MODE (keyboard)
    /// or le_set_mode (a UI action) -
    /// both paths converge on the same LeHandle::Mode state. Switching to
    /// LE_MODE_RULER either way always finishes whatever ruler was
    /// already in progress first (LeHandle::reset_ruler_mode), including
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

    /// @brief One ruler point, in microns - see
    /// le_ruler_point_at.
    typedef struct LeRulerPoint
    {
        double x_um;
        double y_um;
    } LeRulerPoint;

    /// @brief Number of rulers - multiple can exist
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

    /// @brief Finishes the active ruler, if any (LeHandle::finish_active_ruler) -
    /// the next click in Ruler mode starts a new ruler instead of
    /// appending to this one, subject to LeHandle::add_ruler_point's own
    /// minimum-distance guard against restarting too close to the point
    /// this call just finished at. Called by the frontend on a
    /// double-click. A no-op if handle is null or there's no active ruler.
    void le_finish_ruler(LeHandle *handle);

    /// @brief Removes every ruler (finished or not). A no-op if handle
    /// is null.
    void le_clear_rulers(LeHandle *handle);

    // --- Editing / undo-redo ---
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
    /// recording is captured as one undo/redo step within it. Until
    /// le_end_command(), le_wait_for_render_needed() doesn't return (only
    /// le_cancel_render_wait() wakes it), so the command's mutations
    /// render once, when it ends. Calling this while a transaction is
    /// already recording is a no-op (no nesting supported this round) - a
    /// no-op if handle or label is null.
    void le_begin_command(LeHandle *handle, const char *label);

    /// @brief Ends the transaction started by le_begin_command(). If it
    /// recorded at least one step, pushes it onto the undo stack
    /// (clearing the redo stack) regardless of `succeeded`. `label` is
    /// always appended to the command-recall log
    /// (le_command_history_count/_at), `succeeded` or not - so a
    /// zero-step command (e.g. a pure read) is recallable even though
    /// there's nothing to undo, and a failed one can be recalled and
    /// fixed. Releases the render hold le_begin_command() set. A no-op if
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

    /// @brief Number of recorded command-recall entries - every command
    /// passed to le_end_command, successful or not, in submission order.
    /// Indexes le_command_history_at()'s
    /// own `index` parameter. Returns 0 if handle is null.
    int32_t le_command_history_count(LeHandle *handle);

    /// @brief The command text at `index` (0..le_command_history_count()-1).
    /// Owned by the handle - valid until the handle is destroyed (entries
    /// are never removed/reordered). Returns null if handle is null or
    /// index is out of range.
    const char *le_command_history_at(LeHandle *handle, int32_t index);

    /// @brief Clears the current selection (same underlying behavior as
    /// LE_KEY_DESELECT_ALL while LE_KEY_CTRL is held), callable directly
    /// for the Select-mode toolbar button, which has no "Ctrl held"
    /// precondition of its own. A no-op if handle is null.
    void le_deselect_all(LeHandle *handle);

    /// @brief Arms Move - equivalent to Ctrl-M or
    /// clicking the Move toolbox button. Only meaningful in Edit mode
    /// with a non-empty selection; a no-op otherwise (including if
    /// handle is null). The next two le_mouse_up() clicks in Edit mode
    /// set the move's anchor point, then commit the move - see
    /// le_mouse_up's own doc comment. Calling this again while already
    /// armed (including the re-arm a successful commit itself performs -
    /// see le_mouse_up) is harmless - it just re-snapshots the ghost
    /// preview from the current selection/geometry. Refused (a no-op) when
    /// the selection mixes Placements with any other kind of object.
    void le_arm_move(LeHandle *handle);

    /// @brief Deletes every selected shape piece - a rect, polygon, path,
    /// via or via array - from its Shape; a Shape left with no geometry is
    /// deleted too. Owners (a Route, a Terminal port, ...) and every other
    /// selected object stay (the Edit-mode toolbar's Delete button and
    /// the Del key). One undoable
    /// "delete" transaction (undo recreates a deleted Shape whole);
    /// the deleted pieces leave the selection. Cancels an armed Move or
    /// Resize first (their ghosts would name deleted pieces). Returns how
    /// many pieces were deleted (0 if none are selected, or handle is null).
    int32_t le_delete_selected_pieces(LeHandle *handle);

    /// @brief How many shape pieces (le_delete_selected_pieces' targets)
    /// are selected - 0 if handle is null.
    int32_t le_selected_shape_piece_count(LeHandle *handle);

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

    /// @brief True (nonzero) once an armed Move's first click has set its
    /// anchor - the move is under way and its ghost showing. Returns 0 if
    /// handle is null.
    int32_t le_is_move_anchored(LeHandle *handle);

    /// @brief What a moving Placement's location snaps to
    /// (le::PlacementSnapMode). SITE
    /// snaps a CORE-class Abstract's placement to the nearest row's site
    /// grid (rows of its own SITE, if it declares one) and forces an
    /// orientation that row allows; any other placement falls back to
    /// the manufacturing grid. FIN_GRID snaps to the FinFET grid (a
    /// LIBRARY LEF58_FINFET property, overridable via update_technology
    /// -fin_pitch/-fin_offset/-fin_direction) across the fins and to the
    /// manufacturing grid along them.
    typedef enum LePlacementSnapMode
    {
        LE_PLACEMENT_SNAP_NONE = 0,
        LE_PLACEMENT_SNAP_SITE = 1,
        LE_PLACEMENT_SNAP_FIN_GRID = 2,
        LE_PLACEMENT_SNAP_MANUFACTURING_GRID = 3,
    } LePlacementSnapMode;

    /// @brief Sets the placement snap mode (LePlacementSnapMode) - persists
    /// across moves, SITE by default. Ignores an out-of-range value. A
    /// no-op if handle is null.
    void le_set_placement_snap_mode(LeHandle *handle, int32_t mode);

    /// @brief The current placement snap mode. LE_PLACEMENT_SNAP_SITE if
    /// handle is null.
    int32_t le_get_placement_snap_mode(LeHandle *handle);

    /// @brief Nonzero if `mode` has anything to snap to in the current
    /// view: rows in the current Layout (SITE), a FinFET grid
    /// (FIN_GRID), a MANUFACTURINGGRID (MANUFACTURING_GRID); NONE always.
    /// 0 if handle is null.
    int32_t le_is_placement_snap_mode_available(LeHandle *handle, int32_t mode);

    /// @brief A rotate/flip of the selected placements (le::OrientationOp) -
    /// see le_apply_placement_orientation_op.
    typedef enum LeOrientationOp
    {
        LE_ORIENTATION_OP_ROTATE_CCW = 0,
        LE_ORIENTATION_OP_FLIP_HORIZONTAL = 1,
        LE_ORIENTATION_OP_FLIP_VERTICAL = 2,
    } LeOrientationOp;

    /// @brief Sets the flightline fanout limit: a net with more than
    /// `max_fanout` endpoints besides the
    /// selected pin draws no flightlines; 0 means no limit. 10 by default.
    /// Ignores a negative value, or a null handle.
    void le_set_flightline_max_fanout(LeHandle *handle, int32_t max_fanout);

    /// @brief The flightline fanout limit. 0 if handle is null.
    /// @brief Whether the GUI asks before a Save overwrites an existing
    /// design file - a setting (settings.json), 1 by default.
    void le_set_confirm_overwrite(LeHandle *handle, int32_t confirm);
    int32_t le_confirm_overwrite(LeHandle *handle);

    int32_t le_flightline_max_fanout(LeHandle *handle);

    /// @brief How many Placements are currently selected - nonzero (in Edit
    /// mode) is the GUI's cue to show the placement secondary toolbar. 0
    /// if handle is null.
    int32_t le_selected_placement_count(LeHandle *handle);

    /// @brief Which LeOrientationOps le_apply_placement_orientation_op
    /// would accept right now, as a bitmask (1 << op). 0 with no placement
    /// selected, while a Move is under way (anchored by its first click, the
    /// ghost showing - arming alone doesn't count), or outside a Layout view. Under
    /// SITE snapping, a CORE cell's op also needs its row's Site SYMMETRY
    /// to permit it (R90: rotate, Y: horizontal flip, X: vertical flip);
    /// every other snap mode leaves all three enabled. 0 if handle is null.
    int32_t le_placement_orientation_ops_enabled(LeHandle *handle);

    /// @brief Rotates (90 degrees counterclockwise, N -> W) or flips
    /// (horizontal: N -> FN, vertical: N -> FS) every selected placement
    /// about its own bbox center, committed immediately as one undoable
    /// edit. Returns 0 on success, 1 if no placement is selected, 2 if the
    /// op isn't enabled (le_placement_orientation_ops_enabled), -1 if
    /// handle is null or `op` is out of range.
    int32_t le_apply_placement_orientation_op(LeHandle *handle, int32_t op);

    /// @brief Which of a Shape's geometry lists a selected piece belongs
    /// to - le::PieceKind's own ordinal.
    typedef enum LePieceKind
    {
        LE_PIECE_KIND_RECT = 0,
        LE_PIECE_KIND_POLYGON = 1,
        LE_PIECE_KIND_PATH = 2,
        LE_PIECE_KIND_VIA = 3,         // a via instance - shares LE_PIECE_KIND_PATH's snap mode
        LE_PIECE_KIND_VIA_ITERATE = 4, // a via array - shares LE_PIECE_KIND_PATH's snap mode
    } LePieceKind;

    /// @brief What a resized edge/segment snaps to (le::ShapeSnapMode),
    /// chosen per LePieceKind. Rects and
    /// polygons take NONE/USER_GRID/MANUFACTURING_GRID/FIN_GRID; paths take
    /// NONE/USER_GRID/MANUFACTURING_GRID (the path's edges land on it)/
    /// TRACKS (its centerline lands on a routing track of its layer - the
    /// Layout's TRACKS, else the layer's own LEF PITCH/OFFSET grid).
    /// Move uses the same settings for paths, and vias/via arrays take
    /// NONE/USER_GRID/MANUFACTURING_GRID/TRACKS for their origin (item
    /// 13): a moved path or via snaps on its own; rects and polygons move
    /// by the user-grid-snapped mouse offset.
    typedef enum LeShapeSnapMode
    {
        LE_SHAPE_SNAP_NONE = 0,
        LE_SHAPE_SNAP_USER_GRID = 1,
        LE_SHAPE_SNAP_MANUFACTURING_GRID = 2,
        LE_SHAPE_SNAP_FIN_GRID = 3,
        LE_SHAPE_SNAP_TRACKS = 4,
    } LeShapeSnapMode;

    /// @brief Arms the Resize tool - Edit mode with at least one selected
    /// rect/polygon/path piece and no selected placement, a no-op
    /// otherwise. Disarms Move (and arming Move disarms Resize). Then, like
    /// Move, two clicks (le_mouse_up clicks): hovering a selected piece's
    /// edge (rect, polygon) or anywhere on a path segment highlights it
    /// (le_resize_hover_axis); the first click grabs it and a ghost follows
    /// the mouse; the second commits it as one undoable edit. A rect edge
    /// moves across its own axis; a polygon edge or path segment moves as
    /// a whole, stretching its neighbours (an axis-aligned one only across
    /// its own axis). Escape cancels a grab, or disarms with none; leaving
    /// Edit mode disarms. A no-op if handle is null.
    void le_arm_resize(LeHandle *handle);

    /// @brief Nonzero while Resize is armed. 0 if handle is null.
    int32_t le_is_resize_armed(LeHandle *handle);

    /// @brief Which way the Resize hover target (the selected piece's
    /// edge/segment under the mouse, armed with nothing grabbed) would move
    /// - for the GUI's resize cursor.
    typedef enum LeResizeAxis
    {
        LE_RESIZE_AXIS_NONE = -1, // nothing grabbable under the mouse
        LE_RESIZE_AXIS_X = 0,     // a vertical edge - moves left/right
        LE_RESIZE_AXIS_Y = 1,     // a horizontal edge - moves up/down
        LE_RESIZE_AXIS_BOTH = 2,  // a diagonal polygon edge/path segment
    } LeResizeAxis;

    /// @brief The current Resize hover target's LeResizeAxis.
    /// LE_RESIZE_AXIS_NONE if handle is null.
    int32_t le_resize_hover_axis(LeHandle *handle);

    /// @brief Sets `kind`'s (LePieceKind) resize snap mode (LeShapeSnapMode)
    /// - persists, USER_GRID by default. Paths, vias and via arrays share
    /// one mode, so setting any of them sets all three. Ignores a mode
    /// `kind` doesn't offer, an out-of-range value, or a null handle.
    void le_set_shape_snap_mode(LeHandle *handle, int32_t kind, int32_t mode);

    /// @brief `kind`'s resize snap mode. LE_SHAPE_SNAP_USER_GRID if handle
    /// is null or `kind` out of range.
    int32_t le_get_shape_snap_mode(LeHandle *handle, int32_t kind);

    /// @brief Nonzero if `kind` offers `mode` and it has something to snap
    /// to: always for NONE/USER_GRID; a MANUFACTURINGGRID; a FinFET grid;
    /// for TRACKS, tracks (or a LEF PITCH) for the layer of some selected
    /// path. 0 if handle is null.
    int32_t le_is_shape_snap_mode_available(LeHandle *handle, int32_t kind, int32_t mode);

    /// @brief Which LePieceKinds the current selection holds, as a bitmask
    /// (1 << kind) - the resize toolbar shows one snap group per kind
    /// present. Rects, polygons and paths only - vias have nothing to
    /// resize. 0 if handle is null.
    int32_t le_selected_piece_kinds(LeHandle *handle);

    /// @brief Which LePieceKinds in the current selection Move snaps one by
    /// one, as a bitmask: 1 <<
    /// LE_PIECE_KIND_PATH when any path, via or via array is selected -
    /// they share one routing snap mode, so the Move toolbar shows a
    /// single group for them. 0 if handle is null.
    int32_t le_selected_move_snap_piece_kinds(LeHandle *handle);

    /// @brief Current selectability of every ViewLayer whose LeLayerRow::name
    /// is `layer_name` - see le_is_layer_name_visible()'s comment for the
    /// general row/column model this mirrors. Selectable by default until
    /// toggled. Purely an interaction-layer concern (no hit-testing/
    /// click-to-select API exists yet to consult it) - doesn't affect
    /// rendering. Returns nonzero (selectable) if handle or layer_name is
    /// null.
    int32_t le_is_layer_name_selectable(LeHandle *handle, const char *layer_name);

    /// @brief Set the selectability of every ViewLayer whose LeLayerRow::name
    /// is `layer_name`. Mirrors LeHandle::set_layer_name_selectable directly.
    /// A no-op if handle or layer_name is null.
    void le_set_layer_name_selectable(LeHandle *handle, const char *layer_name, int32_t selectable);

    /// @brief Nonzero if anything drawn on `purpose` (a le_purpose_at
    /// ordinal) can ever be selected - zero means le_set_purpose_selectable
    /// has no effect on it, e.g. GCELLGRID/DEBUG. Needs no handle:
    /// a fixed property of the purpose itself.
    int32_t le_purpose_has_selectable_objects(int32_t purpose);

    /// @brief Current selectability of every ViewLayer whose purpose is
    /// `purpose`, across every layer. Selectable by default until toggled.
    /// Returns nonzero (selectable) if handle is null.
    int32_t le_is_purpose_selectable(LeHandle *handle, int32_t purpose);

    /// @brief Set the selectability of every ViewLayer whose purpose is
    /// `purpose`, across every layer. Mirrors LeHandle::set_purpose_selectable
    /// directly. A no-op if handle is null.
    void le_set_purpose_selectable(LeHandle *handle, int32_t purpose, int32_t selectable);

    /// @brief A per-value filter under a purpose in the Layers panel: by
    /// Placement.type (the reference design's Abstract.type, LEF MACRO
    /// CLASS, under PLACEMENT) or by Route.use (DEF USE, under ROUTE).
    /// Values are matched case-insensitively; a Placement whose cell has no
    /// type has the value "UNSET", and a Route with no use is "SIGNAL"
    /// (DEF's default).
    typedef enum LeObjectFilter
    {
        LE_OBJECT_FILTER_PLACEMENT_TYPE = 0,
        LE_OBJECT_FILTER_ROUTE_USE = 1,
    } LeObjectFilter;

    /// @brief How many values `filter` offers: for PLACEMENT_TYPE every
    /// distinct type a Design gives its placements (plus "UNSET" when any
    /// has none); for ROUTE_USE the DEF USE keywords. 0 for a
    /// null handle or unknown filter.
    int32_t le_object_filter_value_count(LeHandle *handle, int32_t filter);

    /// @brief The `index`th value of `filter`, upper-case, or null if out of
    /// range. Owned by `handle`; valid until the next database change.
    const char *le_object_filter_value_at(LeHandle *handle, int32_t filter, int32_t index);

    /// @brief Nonzero if objects with `value` are drawn and hit-tested
    /// (visible by default). Returns nonzero for a null handle or value.
    int32_t le_is_object_filter_value_visible(LeHandle *handle, int32_t filter, const char *value);

    /// @brief Shows or hides every Placement/Route with `value` - its
    /// outline, label and placed content for a Placement, its shapes and
    /// vias for a Route. A no-op for a null handle or value.
    void le_set_object_filter_value_visible(LeHandle *handle, int32_t filter, const char *value, int32_t visible);

    /// @brief Nonzero if objects with `value` can be selected (selectable by
    /// default). Returns nonzero for a null handle or value.
    int32_t le_is_object_filter_value_selectable(LeHandle *handle, int32_t filter, const char *value);

    /// @brief Sets whether every Placement/Route with `value` (and, for a
    /// Route, its shapes and vias) can be selected. A no-op for a null
    /// handle or value.
    void le_set_object_filter_value_selectable(LeHandle *handle, int32_t filter, const char *value, int32_t selectable);

    /// @brief Zoom the viewport, keeping the dbu point under screen pixel
    /// (x, y) fixed on screen. `factor` is a signed fractional step applied
    /// to the current scale (new_scale = scale * (1 + factor)) - positive
    /// zooms in, negative zooms out (e.g. 0.1 zooms in 10%, -0.1 zooms out
    /// 10%); a factor <= -1.0 (which would make new_scale non-positive) is
    /// ignored, same guard as LeHandle::set_scale. `x`/`y` are in the same
    /// pixel space as le_render_pixel_buffer()'s output image - top-left
    /// origin, y increasing downward (see api.hpp's LePixelBuffer) - not
    /// the rasterizer's pre-Y-flip pixel space, since this is meant to be fed
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

    /// @brief How the view is mirrored on screen. The same design area
    /// shows; HORIZONTAL swaps left and right about the view's centre,
    /// VERTICAL swaps top and bottom. Pan and zoom keep their meaning,
    /// mouse positions are read through the mirror, the arrow keys pan in
    /// screen directions, and le_snapped_mouse_position reports coordinates
    /// mirrored about the view's boundary centre. Every other coordinate
    /// (Tcl, properties, files) stays in database coordinates. Overlay text (rulers, labels drawn by
    /// overlays) stays readable; text in the design itself mirrors.
    typedef enum LeViewFlip
    {
        LE_VIEW_FLIP_NONE = 0,
        LE_VIEW_FLIP_HORIZONTAL = 1,
        LE_VIEW_FLIP_VERTICAL = 2,
    } LeViewFlip;

    /// @brief Mirrors the view (LeViewFlip). Returns 0, or 1 if `flip` is
    /// out of range or handle is null (nothing changes).
    int32_t le_set_view_flip(LeHandle *handle, int32_t flip);

    /// @brief The view's mirror (LeViewFlip). LE_VIEW_FLIP_NONE if handle is
    /// null.
    int32_t le_view_flip(LeHandle *handle);

    /// @brief Set the viewport size in pixels - also the size of the
    /// buffer le_render_pixel_buffer() produces. Mirrors
    /// LeHandle::set_viewport_size directly.
    void le_set_viewport_size(LeHandle *handle, int32_t width_px, int32_t height_px);

    /// @brief Fit the viewport's pan/scale to the currently selected
    /// Design's content bbox: uniform scale (no stretch) so the content
    /// fills the viewport set via le_set_viewport_size() with `padding_px`
    /// of margin on every side, pan centering it. Mirrors
    /// LeHandle::fit_to_content, using the view's declared bbox (a
    /// Layout's DIEAREA, or abstract_declared_bbox for an Abstract). A
    /// no-op if handle is null; degrades to scale 1.0 / pan (0, 0) if no
    /// Design is selected or it has no declared size, rather than crashing.
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
    /// pan centering it. Mirrors LeHandle::fit_to_content directly - the
    /// same "backend owns pan/scale entirely" fit-based approach
    /// le_fit_scene/le_zoom's own comments describe, just with a
    /// caller-supplied rect instead of a Design's own declared content
    /// bbox. A no-op if handle is null. A single-line (one axis
    /// zero-width) rect still fits correctly, using the other axis's own
    /// scale - see LeHandle::fit_to_content's own comment; a zero-area or
    /// inverted (ur_x_um < ll_x_um or ur_y_um < ll_y_um) rect, or a
    /// non-positive viewport size, degrades to scale 1.0 / pan (0, 0)
    /// instead, same as le_fit_scene's own empty-content fallback.
    void le_fit_rect(LeHandle *handle, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um, int32_t padding_px);

    /// @brief Spacing (dbu) between minor grid dots, drawn behind the
    /// design by le_render_pixel_buffer().
    /// Mirrors LeHandle::minor_grid_spacing directly. Defaults to 5 (dbu),
    /// matching a 5nm minor grid under the common "1 dbu = 1nm" Technology
    /// convention. Returns 0 if handle is null.
    int64_t le_minor_grid_spacing(LeHandle *handle);

    /// @brief Set the minor grid dot spacing (dbu). Mirrors
    /// LeHandle::set_minor_grid_spacing directly (affects rendering) - values
    /// <= 0 are ignored, same guard as le_set_scale. A no-op if handle is
    /// null.
    void le_set_minor_grid_spacing(LeHandle *handle, int64_t dbu);

    /// @brief Spacing (dbu) between major grid dots (drawn bolder than
    /// minor ones). Mirrors
    /// LeHandle::major_grid_spacing directly. Defaults to 50 (dbu), matching
    /// a 50nm major grid under the common "1 dbu = 1nm" Technology
    /// convention. Returns 0 if handle is null.
    int64_t le_major_grid_spacing(LeHandle *handle);

    /// @brief Set the major grid dot spacing (dbu). Mirrors
    /// LeHandle::set_major_grid_spacing directly (affects rendering) - values
    /// <= 0 are ignored. A no-op if handle is null.
    void le_set_major_grid_spacing(LeHandle *handle, int64_t dbu);

    /// @brief On-screen text size (px) for every ruler label - tick
    /// values, each segment's own point-to-point distance, and a
    /// ruler's running total. Mirrors
    /// LeHandle::ruler_label_size_px directly. Defaults to 11.0. Returns 0
    /// if handle is null.
    double le_ruler_label_size(LeHandle *handle);

    /// @brief Set the ruler label text size (px). Mirrors
    /// LeHandle::set_ruler_label_size_px directly (affects rendering) -
    /// values <= 0 are ignored, same guard as le_set_minor_grid_spacing.
    /// A no-op if handle is null.
    void le_set_ruler_label_size(LeHandle *handle, double px);

    /// @brief Smallest on-screen size (px) a shape or placement label is
    /// drawn at - the Settings panel's min label font size
    /// Labels scale with their geometry
    /// between this and le_label_max_size; a min above the max yields to
    /// the max. Defaults to 12. 0 if handle is null.
    double le_label_min_size(LeHandle *handle);

    /// @brief Sets le_label_min_size - values <= 0 are ignored. A no-op if
    /// handle is null.
    void le_set_label_min_size(LeHandle *handle, double px);

    /// @brief Largest on-screen size (px) a shape or placement label grows
    /// to - the Settings panel's max label font size. Defaults to 24. 0 if
    /// handle is null.
    double le_label_max_size(LeHandle *handle);

    /// @brief Sets le_label_max_size - values <= 0 are ignored. A no-op if
    /// handle is null.
    void le_set_label_max_size(LeHandle *handle, double px);

    /// @brief Sets the color (0-255 each) of every purpose of the Layers
    /// panel row `layer_name` - a Layer's name, or a pseudo-row's like
    /// BOUNDARY - replacing its default palette color
    /// Saved by le_save_settings. A
    /// name with no row yet is kept and applies once one exists (e.g. a
    /// settings file loaded before the LEF). Returns 0, or 1 (and changes
    /// nothing) for a null handle/name or a component outside 0-255.
    int32_t le_set_layer_color(LeHandle *handle, const char *layer_name, int32_t r, int32_t g, int32_t b);

    /// @brief Drops le_set_layer_color's color for `layer_name`, so the row
    /// goes back to its default palette color. A no-op if it had none, or
    /// handle/name is null.
    void le_reset_layer_color(LeHandle *handle, const char *layer_name);

    /// @brief The current color of the Layers panel row `layer_name`, as
    /// 0xRRGGBB - le_set_layer_color's if set, else the default. -1 if
    /// there's no such row, or handle/name is null.
    int32_t le_layer_color_rgb(LeHandle *handle, const char *layer_name);

    /// @brief The minor (`major` 0) or major (`major` nonzero) grid spacing
    /// in um - converted from le_minor_grid_spacing/le_major_grid_spacing
    /// through the Technology's dbu scale, or a value set/loaded before any
    /// Technology existed. -1 if neither is known yet, or handle is null.
    double le_grid_spacing_um(LeHandle *handle, int32_t major);

    /// @brief Sets the grid spacing in um (a value <= 0 leaves that one
    /// unchanged). Converted to dbu (at least 1) through the Technology's
    /// scale; with no Technology yet, held and applied by the first
    /// le_read_lef/le_read_def that establishes one. A no-op if handle is
    /// null.
    void le_set_grid_spacing_um(LeHandle *handle, double minor_um, double major_um);

    /// @brief The Technology's MANUFACTURINGGRID in um, as the LEF gives it
    /// - what the Settings panel's "set the grid to the manufacturing grid"
    /// button uses. 0 if there's no Technology, it has none, or handle is
    /// null.
    double le_manufacturing_grid_um(LeHandle *handle);

    /// @brief Where settings are saved/loaded when no path is given:
    /// $HOME/.layout_engine/settings.json ("" if HOME isn't set). Never null.
    const char *le_default_settings_path(void);

    /// @brief Writes the current settings as JSON to `path` (null
    /// or "" - le_default_settings_path), creating its directory if needed:
    /// grid spacing (um), ruler and label font sizes (px), hierarchy depth,
    /// flightline fanout limit, and the placement/shape snap modes. Top-level
    /// keys the last loaded file had that this version doesn't know are
    /// written back unchanged. Returns 0 on success, nonzero (logged) on
    /// failure or a null handle.
    int32_t le_save_settings(LeHandle *handle, const char *path);

    /// @brief Reads settings written by le_save_settings from `path` (null
    /// or "" - le_default_settings_path) and applies them. A file from an
    /// older version is migrated to the current format first; one from a
    /// newer version loads the keys this version knows, with a warning. A
    /// missing key keeps its current value; an invalid one is skipped with
    /// a warning.
    /// Returns 0 on success, nonzero (logged) if the file can't be read or
    /// isn't a JSON object, or handle is null.
    int32_t le_load_settings(LeHandle *handle, const char *path);

    /// @brief 1 if the design has changed since it was last written with
    /// write_db or read with read_db - the exit confirmation's "unsaved
    /// design". Reading LEF/DEF/Verilog is a change, and write_def/write_lef
    /// don't save: only a .led file holds the whole database. 0 otherwise
    /// (a new, empty session included), or if handle is null.
    int32_t le_has_unsaved_database_changes(LeHandle *handle);

    /// @brief 1 if any setting le_save_settings saves differs from what was
    /// last saved or loaded (or from the defaults, if neither has
    /// happened) - the exit confirmation's "unsaved settings". 0 otherwise,
    /// or if handle is null.
    int32_t le_has_unsaved_settings(LeHandle *handle);

    /// @brief Set the current mouse position, in the same pixel space as
    /// le_render_pixel_buffer()'s output image (top-left origin, y
    /// increasing downward) and le_zoom()'s x/y - meant to be fed straight
    /// from a pointer-move event. Drives the grid-snap indicator box drawn
    /// by le_render_pixel_buffer(), and - with Resize armed - the Resize
    /// hover indicator (le_arm_resize). A no-op if handle is null. Never
    /// invalidates the (potentially design-sized) rasterized images - only
    /// ComposeStage's cheap overlay pass redraws.
    void le_set_mouse_position(LeHandle *handle, int32_t x, int32_t y);

    /// @brief Clear the current mouse position (e.g. on a pointer-leave
    /// event) so the grid-snap indicator box stops showing at the last
    /// known position. A no-op if handle is null.
    void le_clear_mouse_position(LeHandle *handle);

    /// @brief The current mouse position's coordinates in microns, snapped
    /// to the minor grid - the same point the grid-snap indicator box
    /// drawn by le_render_pixel_buffer() is centered on (ComposeStage's
    /// draw_cursor_overlay), for a UI to display as coordinate text. In a
    /// mirrored view (le_set_view_flip) they're mirrored too, about the
    /// centre of the open Layout's diearea or Abstract's bounds, which is
    /// (0, 0): the boundary's on-screen bottom-left is (-w/2, -h/2) and its
    /// top-right (w/2, h/2). See
    /// LeSnappedMousePosition's own comment for the dbu-to-micron
    /// conversion and its degrade-gracefully cases (null handle, no mouse
    /// position set, no Technology read yet).
    LeSnappedMousePosition le_snapped_mouse_position(LeHandle *handle);

    /// @brief Named logical keys this API tracks the held/released state
    /// of via le_key_down()/le_key_up() - stable across platforms, so
    /// this API's meaning doesn't depend on OS/toolkit scan codes; the
    /// frontend maps its own key values to these before calling in.
    /// Extend as future commands need to know about more keys.
    ///
    /// LE_KEY_ZOOM/LE_KEY_FIT/LE_KEY_PAN_* are canvas-navigation
    /// commands, not modifiers - le_key_down() triggers the
    /// corresponding action immediately (see its own doc comment) rather
    /// than only recording held state. Frontends map a single physical
    /// key to each regardless of shift (e.g. both "z" and "Z" - the same
    /// physical key - map to LE_KEY_ZOOM); shift's
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
        /// LE_KEY_DESELECT_ALL, LE_KEY_FIT, and
        /// LE_KEY_1..LE_KEY_9.
        LE_KEY_CTRL = 8,
        // 9 is unassigned.
        /// Digit keys 1-9 toggle a ROUTING layer's visibility - which one
        /// depends on LE_KEY_CTRL's held state (the
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
        /// Deselect-all - an "action" code like
        /// LE_KEY_ZOOM/LE_KEY_FIT/LE_KEY_PAN_*, but only does anything
        /// while LE_KEY_CTRL is held (see le_key_down's own doc comment) -
        /// a bare "d" press with Ctrl not held is a deliberate no-op.
        LE_KEY_DESELECT_ALL = 19,
        /// Toggles the tenth ROUTING layer's visibility -
        /// not Ctrl-gated, unlike LE_KEY_1..LE_KEY_9's own Ctrl-held
        /// branch, since "0" has no bare-digit slot left below it to
        /// double up on (LE_KEY_1..LE_KEY_9 already cover the first
        /// nine). Same VIA-pairing behavior as LE_KEY_1..LE_KEY_9.
        LE_KEY_0 = 20,
        /// Switch to Select mode - an "action" code
        /// like LE_KEY_ZOOM/LE_KEY_FIT: le_key_down() calls
        /// LeHandle::set_mode(LeHandle::Mode::SELECT) immediately, every call
        /// (including key-repeat - idempotent, so no special one-shot
        /// handling is needed). Bare-only - fires only while neither
        /// LE_KEY_CTRL nor LE_KEY_SHIFT is currently held, so e.g. a
        /// Ctrl-S keystroke intended for something else doesn't also
        /// switch modes as a side effect. See le_get_mode/le_set_mode for
        /// the non-keyboard (UI action) path to the same state,
        /// which is not modifier-gated (there's no physical key to
        /// collide with there).
        LE_KEY_SELECT_MODE = 21,
        /// Switch to Edit mode - same shape as
        /// LE_KEY_SELECT_MODE, including the bare-only modifier gating,
        /// calling LeHandle::set_mode(LeHandle::Mode::EDIT). While in Edit
        /// mode, le_mouse_up no longer changes the current selection -
        /// see its own doc comment.
        LE_KEY_EDIT_MODE = 22,
        /// Switch to Ruler mode - same idempotent,
        /// bare-only action-code shape as LE_KEY_SELECT_MODE/
        /// LE_KEY_EDIT_MODE, but calls LeHandle::reset_ruler_mode() rather
        /// than a plain set_mode(): every call - including when already
        /// in Ruler mode, and including key-repeat - finishes whatever
        /// ruler was in progress, so re-pressing 'r' doubles as an
        /// explicit "abandon the current ruler" shortcut. While in Ruler
        /// mode, le_mouse_up's clicks place ruler points instead of
        /// changing the selection - see le_finish_ruler/le_clear_rulers.
        /// With Ctrl held (and not Shift) the same key arms Resize instead
        /// (Ctrl-R - see le_arm_resize) -
        /// branching inside this code, like Ctrl-Z inside LE_KEY_ZOOM,
        /// since the frontend sends every "r" press as this code.
        LE_KEY_RULER_MODE = 23,
        /// Finishes the active ruler, if any (see le_finish_ruler) - the
        /// Esc key. Idempotent/safe to fire on
        /// every call including key-repeat, same as every other action
        /// code here: LeHandle::finish_active_ruler() is already a no-op
        /// once there's nothing active to finish, and there's no active
        /// ruler at all outside Ruler mode (leaving it already finishes
        /// whatever was in progress - see LeHandle::set_mode), so this
        /// never needs mode-gating at the call site either. Also cancels
        /// an in-progress (not yet committed) Move, if any
        /// (le_cancel_move) - same "always safe to fire" reasoning,
        /// LeHandle::end_move() is a no-op once there's nothing to cancel.
        /// Deliberately *not* modifier-gated, unlike every bare-only key
        /// above - Escape is a pure cancel/finish gesture, and a real
        /// ruler-drawing sequence routinely ends with LE_KEY_SHIFT (the
        /// free-form toggle) still physically held right up to the Esc
        /// press, so suppressing it while a modifier happens to be down
        /// would make "finish the ruler" unreliable in exactly the
        /// workflow that uses Shift the most.
        LE_KEY_FINISH_RULER = 24,
        /// Arms Move (Ctrl-M) - see le_arm_move's own
        /// doc comment. Fires only while Ctrl is held and Shift is not,
        /// at the le_key_down call site, same shape as
        /// LE_KEY_DESELECT_ALL, even though the Move
        /// toolbox button calls le_arm_move() directly and bypasses this
        /// gate. No separate LE_KEY_UNDO/LE_KEY_REDO code exists -
        /// Ctrl-Z/Ctrl-Shift-Z are handled by branching inside
        /// LE_KEY_ZOOM's own handler instead (see le_key_down's doc
        /// comment) - the frontend's key-to-code map already sends every
        /// "z" press as LE_KEY_ZOOM regardless of modifiers, the same way
        /// every other canvas-navigation code already works.
        LE_KEY_MOVE = 25,
        /// Deletes the selected shape pieces (the Del key) - see
        /// le_delete_selected_pieces. Edit mode
        /// only, bare only (a no-op with LE_KEY_CTRL or LE_KEY_SHIFT held).
        LE_KEY_DELETE = 26,
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
    ///   most recent transaction instead of zooming (Ctrl-Z/Ctrl-Shift-Z)
    ///   - the frontend's key-to-code map already
    ///   sends every "z" press as LE_KEY_ZOOM regardless of modifiers, so
    ///   this branches here rather than needing its own key code.
    ///   Otherwise: le_zoom() by a fixed factor, anchored at the
    ///   current mouse position (LeHandle::mouse_x_px/mouse_y_px - i.e.
    ///   wherever le_set_mouse_position was last called for); zooms in,
    ///   or out if LE_KEY_SHIFT is currently held (le_is_key_held).
    /// - LE_KEY_FIT: le_fit_scene() with a fixed padding - or, if
    ///   LE_KEY_CTRL is currently held ("Ctrl-F fit selected"), fits the
    ///   viewport to the current selection's own
    ///   combined bbox instead (same fixed padding, one Shape's own bbox
    ///   per selection entry - see api.cpp's fit_selected_unlocked),
    ///   leaving the view unchanged if nothing is selected rather than
    ///   falling back to the whole-content fit. LE_KEY_SHIFT has no
    ///   meaning for Fit - held at all (with or without Ctrl) is a
    ///   no-op, not "same as unmodified".
    /// - LE_KEY_PAN_LEFT/RIGHT/UP/DOWN: le_pan() by a fixed
    ///   viewport-fraction step in the corresponding direction. Bare
    ///   only - a no-op while either LE_KEY_CTRL or LE_KEY_SHIFT is held.
    /// - LE_KEY_1..LE_KEY_9: toggles a ROUTING
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
    /// - LE_KEY_0: toggles the 10th ROUTING layer's
    ///   visibility - bare only (a no-op while either LE_KEY_CTRL or
    ///   LE_KEY_SHIFT is held; there's no digit slot left over for Ctrl
    ///   to double up on the way LE_KEY_1..LE_KEY_9 do) - same
    ///   VIA-pairing re-check as LE_KEY_1..LE_KEY_9 above.
    /// - LE_KEY_DESELECT_ALL: only while LE_KEY_CTRL is
    ///   currently held, LE_KEY_SHIFT is not, and the current mode is
    ///   LE_MODE_SELECT (Select-mode selection shortcuts are disabled in
    ///   Edit/Ruler mode) - clears the current selection. A no-op
    ///   (not an error) if the selection was already empty.
    /// - LE_KEY_MOVE: only while LE_KEY_CTRL is
    ///   currently held and LE_KEY_SHIFT is not - equivalent to
    ///   le_arm_move(); a no-op outside Edit mode or with an empty
    ///   selection, same as that function.
    /// - LE_KEY_DELETE: Edit mode and
    ///   bare only - le_delete_selected_pieces().
    /// - LE_KEY_SELECT_MODE/LE_KEY_EDIT_MODE/LE_KEY_RULER_MODE
    ///   bare only - a no-op while either
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
    /// widget/window receiving key events loses focus. A key's matching
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
    /// against a small threshold. A no-op if
    /// handle is null.
    void le_mouse_down(LeHandle *handle, int32_t x, int32_t y);

    /// @brief Like le_mouse_down(), but begins a rectangle-*zoom* gesture
    /// instead of a drag-*select* one (e.g. on a right-button
    /// pointer-down event) - le_mouse_up() is still the one
    /// call that ends either kind, deciding which behavior to run from
    /// which of le_mouse_down()/le_zoom_drag_down() started it (see its
    /// own doc comment). A no-op if handle is null.
    void le_zoom_drag_down(LeHandle *handle, int32_t x, int32_t y);

    /// @brief End a mouse gesture at the given pixel position, e.g. on a
    /// pointer-up event - the single shared endpoint for both
    /// le_mouse_down()'s drag-select gesture and
    /// le_zoom_drag_down()'s drag-zoom gesture; which
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
    /// the hit-tests' (hit_test.hpp) topmost-layer-first
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
    /// Selection is piece-granular: a Shape that
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
    /// **In Edit mode with Move armed** (see le_arm_move) - a click (not
    /// a drag; a drag's up-event is treated
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
    /// Select-mode-only - see LE_KEY_DESELECT_ALL in le_key_down).
    ///
    /// **Started by le_zoom_drag_down()**: a click-sized
    /// release (same threshold as above) is a no-op - fitting to a
    /// near-zero-size rectangle would produce an absurd scale; otherwise
    /// fits the viewport to the rectangle between the down and up points
    /// (LeHandle::fit_to_content, no padding), the same fitting math
    /// le_fit_scene() uses for a Design's own content bbox. Selection is
    /// untouched either way - this gesture is purely navigational.
    ///
    /// Either way, ends the in-progress drag gesture (LeHandle::end_drag) -
    /// see le_mouse_down's own doc comment for the live rubber-band
    /// rectangle this also stops showing.
    void le_mouse_up(LeHandle *handle, int32_t x, int32_t y);

    /// @brief Ends an in-progress le_mouse_down/le_zoom_drag_down gesture
    /// without acting on it - no selection, no zoom
    /// (the GUI calls this on Escape, or
    /// when the button's release went to another window). Escape
    /// (LE_KEY_FINISH_RULER) does the same while a drag is in progress. A
    /// no-op with no drag in progress, or a null handle.
    void le_cancel_drag(LeHandle *handle);

    /// @brief Instructional text describing which mouse gestures and
    /// keyboard shortcuts are currently available, for display in the
    /// GUI's Info panel - one fixed message per interaction mode
    /// (Select/Edit/Ruler), e.g. "Left click to select. Shift for
    /// multi-select. Left click and drag for rectangle multi-select." for
    /// Select. May contain line breaks (Edit mode separates its
    /// shortcuts with a blank line, "\n\n"). Unlike every other `const char*`-returning function in
    /// this header, the returned pointer refers to static, process-
    /// lifetime storage - not owned by `handle`, never invalidated, safe
    /// to hold indefinitely. Null only if `handle` is null.
    const char *le_tooltip_message(LeHandle *handle);

    /// @brief Number of currently selected objects (LeHandle::selection()).
    /// Indexes the `selection_index` parameter of le_selected_object_ref()
    /// below - 0..le_selection_count()-1, in LeHandle::selection()'s own
    /// (insertion) order. Returns 0 if handle is null.
    int32_t le_selection_count(LeHandle *handle);

    /// @brief Monotonic counter (LeHandle::selection_version()) bumped on
    /// every actual selection change (a select()/deselect()/
    /// clear_selection() call that isn't a no-op) - a cheap way for a
    /// caller to tell whether anything selection-related has changed
    /// since it last checked, without re-fetching le_selection_count()/
    /// le_selected_object_ref() for every selected object on every call
    /// (e.g. every mouse-move event - a real, measured cost that scales
    /// with selection size otherwise). Returns 0 if
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

    /// @brief Which database class an LeObjectRef names: one
    /// LE_OBJECT_KIND_<CLASS> per TCL-readable class, generated from
    /// schema.py, so every class the Property Viewer can reach has a kind.
#include "generated/api/object_kinds.inc"

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

    /// @brief The snake_case class name of LeObjectKind `kind` (e.g.
    /// "terminal_port") - the prefix of its TCL friendly id. Static
    /// storage; null if `kind` is out of range.
    const char *le_object_kind_name(int32_t kind);

    /// @brief Nonzero if `kind`'s friendly id is its name
    /// ("library:lib1"), zero if it's a packed numeric id
    /// ("shape:4294967296") or `kind` is out of range.
    int32_t le_object_kind_is_named(int32_t kind);

    /// @brief Number of property rows `ref` has (see LeProperty) - indexes
    /// le_object_property_at()'s own `index` parameter, 0..this-1. Read-
    /// only: never mutates LeHandle::selection() or bumps selection_version(),
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

    /// @brief Whether le_object_property_count/_at would hit their own
    /// fast, shared-lock-only path for `ref` right now (le_handle.hpp's
    /// own cached_object_property_ref already matches it), rather than
    /// needing to escalate to a write lock to rebuild - always
    /// shared_lock-only itself, so safe to call unconditionally even
    /// while a render is in progress (never blocks behind one - same
    /// reasoning as le_render_pixel_buffer itself, le_handle.hpp's own
    /// mutex_ doc comment). Meant for a caller (property_viewer.cpp) that
    /// wants to know, before actually calling le_object_property_count/_at,
    /// whether doing so right now risks blocking behind an in-progress
    /// render - so it only needs to degrade (skip for a frame) in the one
    /// case that actually needs it (the cache genuinely going stale, e.g.
    /// a fresh selection), not every frame a render merely happens to be
    /// in flight regardless of whether `ref` is already cached. Returns 0
    /// if handle is null.
    int32_t le_object_property_cache_current(LeHandle *handle, LeObjectRef ref);

    /// @brief `ref`'s immediate parent in the database hierarchy (Shape's
    /// own terminal_port/obstruction field, TerminalPort's terminal,
    /// Terminal/Obstruction's abstract, Abstract's design, Design's
    /// library) - the same parent-hop graph filter_field_tables() already
    /// declares for -filter validation, exposed here for GUI navigation
    /// links. Read-only: never mutates LeHandle::selection(). Returns
    /// le_object_invalid_ref() for LE_OBJECT_KIND_LIBRARY (no parent), a
    /// ref that doesn't resolve, or a null handle.
    LeObjectRef le_object_parent(LeHandle *handle, LeObjectRef ref);

    /// @brief The selected object at `selection_index` (0..le_selection_count()-1),
    /// as a generic ref usable with le_object_property_count()/_at()/
    /// le_object_parent() - LE_OBJECT_KIND_SHAPE for a piece-granular
    /// ShapePiece selection (Terminal/Obstruction/Blockage/Route/
    /// PhysicalPort - see LeHandle::SelectedObject's own comment), or the
    /// matching whole-object kind (LE_OBJECT_KIND_ROW/_PLACEMENT/_REGION)
    /// for the bare-id alternatives. Read-only: never mutates
    /// LeHandle::selection() or bumps selection_version(). Returns
    /// le_object_invalid_ref() if handle is null or selection_index is
    /// out of range.
    LeObjectRef le_selected_object_ref(LeHandle *handle, int32_t selection_index);

    /// @brief Adds the object `ref` refers to to the current selection
    /// - the script-driven counterpart to
    /// le_mouse_up's own hit-test-driven selection.
    /// LE_OBJECT_KIND_SHAPE/_ROUTE/_PHYSICAL_PORT/_ROW/_PLACEMENT/_REGION
    /// are supported - any other kind, or a ref that doesn't resolve, is
    /// a no-op logged via spdlog::error rather than crashing or silently
    /// doing nothing. For LE_OBJECT_KIND_SHAPE/_ROUTE/_PHYSICAL_PORT,
    /// this selects every piece of every underlying Shape (every rect/
    /// polygon/path entry, walking Route's own get_route_shapes or
    /// PhysicalPort's own get_physical_port_segments ->
    /// get_physical_port_segment_shapes for the latter two), not just
    /// one - piece-level granularity is reachable only via a real mouse
    /// hit-test (le_mouse_up), there's no "just this one piece" concept
    /// a bare id can express on its own. LeHandle::SelectedObject's own
    /// variant needs no case for ROUTE/PHYSICAL_PORT specifically - a
    /// Route/PhysicalPort piece rides the same ShapePiece alternative
    /// Terminal/Obstruction/Shape already use (see that variant's own
    /// doc comment, le_handle.hpp). Does not clear the existing
    /// selection first (mirrors LeHandle::select()'s own additive behavior,
    /// same as ctrl/shift-clicking) - call le_deselect_all() first for a
    /// script that wants to replace the selection outright. Returns 0 if
    /// this added at least one selection entry, nonzero otherwise
    /// (including a null handle).
    int32_t le_select_object_ref(LeHandle *handle, LeObjectRef ref);

    /// @brief Run the full pipeline+render chain (generate -> filter ->
    /// filter -> transform -> picture -> rasterize) for the currently
    /// selected Design and viewport, returning the resulting pixel
    /// buffer - including the grid-snap indicator box at the current
    /// mouse position, if one has been set (see le_set_mouse_position).
    /// Each stage is cached internally (ViewRenderPipeline) - calling
    /// this again with nothing changed since the last call is close to
    /// free; only viewport/selection/mouse-position changes actually
    /// recompute, and a mouse-position-only change is itself cheap (only
    /// ComposeStage's overlay pass reruns), not proportional to design size.
    /// Returns an all-zero/null LePixelBuffer if handle is null. No
    /// Design selected (le_set_current_design_abstract was never called) degrades
    /// gracefully to an empty (but correctly-sized, non-null) buffer
    /// rather than crashing - Root/Pipeline's own lookups already degrade
    /// gracefully for an unset AbstractId (see pipeline.hpp).
    LePixelBuffer le_render_pixel_buffer(LeHandle *handle);

    /// @brief Whether le_render_pixel_buffer() is doing real work on this
    /// handle right now, on whatever thread called it (drives the GUI's
    /// "rendering..." indicator). Meant to be
    /// polled from a different thread than the one calling
    /// le_render_pixel_buffer() itself - deliberately does NOT take the
    /// same lock le_render_pixel_buffer() holds for its own entire
    /// duration, so this never blocks behind the very render it's
    /// reporting on. Set true only around the pipeline's own actual
    /// recompute (ViewRenderPipeline::would_recompute() returning true),
    /// not for the whole le_render_pixel_buffer() call - a call that
    /// finds nothing changed (the common case) never sets this at all,
    /// so a poller sees an accurate, precisely-timed signal rather than
    /// one that fires for cheap no-op calls too. Returns 0 (not
    /// rendering) if handle is null.
    int32_t le_is_rendering(LeHandle *handle);

    /// @brief 1 between le_begin_command() and le_end_command(): a Tcl
    /// command is running and the view won't re-render until it ends.
    /// Lock-free like le_is_rendering(), so the GUI can poll it every
    /// frame. Returns 0 if handle is null.
    int32_t le_is_command_running(LeHandle *handle);

    /// @brief Blocks the calling thread until a mutation has been made to
    /// this handle (any call that takes HandleWriteLock, le_handle.hpp -
    /// every le_create_X/le_update_X/le_delete_X/le_set_*/le_mouse_*/
    /// le_key_*/le_read_*/... call) since the last time this returned, or
    /// until le_cancel_render_wait() is called - whichever comes first.
    /// Between le_begin_command() and le_end_command() only
    /// le_cancel_render_wait() wakes it.
    /// The intended caller is a single dedicated render thread
    /// (le_gui.cpp's render_thread_loop): call this, then call
    /// le_render_pixel_buffer() once and publish whatever it returns,
    /// then call this again - an event-driven replacement for a
    /// fixed-interval sleep/poll loop, so the thread costs nothing while
    /// idle and never depends on tuning a sleep interval to some
    /// machine's own load. Several mutations that land while this thread
    /// is still busy on a previous render coalesce into exactly one more
    /// wait/render cycle afterward - see LeHandle::render_needed_'s own
    /// doc comment for why nothing is lost or duplicated. A no-op that
    /// returns immediately if handle is null.
    void le_wait_for_render_needed(LeHandle *handle);

    /// @brief Wakes any thread currently blocked in
    /// le_wait_for_render_needed() on this handle, with no real mutation
    /// having happened - purely so that thread can re-check its own
    /// "should I stop" condition and exit cleanly (le_gui.cpp's own
    /// window-teardown path: set its stop flag, then call this, then
    /// join the thread) rather than staying blocked forever waiting for
    /// a mutation that may never come. Safe to call even if no thread is
    /// currently waiting (the next le_wait_for_render_needed() call would
    /// simply return immediately instead of blocking - the cancel is a
    /// level, not a one-shot edge). Wakes the waiter even while a command
    /// holds renders (le_begin_command). A no-op if handle is null.
    void le_cancel_render_wait(LeHandle *handle);

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

    /// @brief Asks the GUI window (if one is open) to close, leaving
    /// le_shell running - the `close_gui` Tcl command
    /// No confirmation: closing the
    /// window loses nothing, show_gui reopens it. Lock-free, same shape as
    /// le_request_show_gui. A no-op if handle is null.
    void le_request_close_gui(LeHandle *handle);

    /// @brief Test-and-clear of le_request_close_gui's request - polled by
    /// the GUI thread each frame (and cleared when a window opens, so a
    /// request made with no window open doesn't close the next one). 0 if
    /// handle is null.
    int32_t le_take_close_gui_request(LeHandle *handle);

    /// @brief Queues `command` (a plain Tcl command string, e.g.
    /// "set_layer_visible {M1} 1") to be evaluated by whichever caller
    /// drains this queue via le_take_next_pending_tcl_command() below -
    /// le_shell.cpp's own console thread, through le_repl_eval, the same
    /// bracket point a typed command goes through (recording it into
    /// command_history/undo). Meant for a caller that has no Tcl
    /// interpreter of its own to evaluate a command directly (src/gui/'s
    /// components - see le_gui.hpp's own doc comment for why that
    /// module has no Tcl/SWIG dependency at all) but still wants an
    /// action (e.g. a layer-visibility toggle) to leave the same
    /// command-history trail a typed command would - mouse/keyboard
    /// interaction stays a direct le_* call. Thread-safe,
    /// fire-and-forget: queues and returns immediately, not evaluated
    /// synchronously by this call. A no-op if handle or command is null.
    void le_enqueue_tcl_command(LeHandle *handle, const char *command);

    /// @brief Pops and returns the oldest command queued via
    /// le_enqueue_tcl_command(), or null if none is pending. The
    /// returned pointer refers to storage owned by the handle - valid
    /// only until the *next* le_take_next_pending_tcl_command() call on
    /// the same handle (same "valid until the next call" convention as
    /// LeProperty's string fields) - copy it out (e.g.
    /// into a Tcl_Eval call) before then. Thread-safe: meant to be
    /// polled from whichever thread owns evaluating these (le_shell.cpp's
    /// own console thread).
    const char *le_take_next_pending_tcl_command(LeHandle *handle);

    // --- CRUD + filter-search - layered on top of Root's primitives
    // (create_x/delete_x/set_x_<field>, get_field()/match_hop()/search_x,
    // src/database/filter.hpp's parser+evaluator); most per-class CRUD is
    // generated (generated/api/declarations.inc). Every id here is
    // addressed directly, not through the current GUI selection
    // (le_object_property_count/_at can address these same ids
    // generically too - see LeObjectRef) - the
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
    /// uses; deliberately *not* handle->current_abstract(), a
    /// separate GUI-rendering "current view" only ever moved as a side
    /// effect of selecting a Design, e.g. le_set_current_design_abstract_by_id -
    /// a script that builds an Abstract from scratch and calls
    /// le_set_current_abstract directly, with no Design to select, needs
    /// this to still work). A linear scan over
    /// Root::get_abstract_terminals, not a codegen index=True lookup - Terminal
    /// name uniqueness is per-Abstract (unique_per_parent, see
    /// src/database/schema.py's own Terminal.name comment), not
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

    /// @brief The Design `name` refers to: `NAME` when only one Library
    /// has a Design of that name, else `LIB/NAME` (a Design's name is
    /// unique only within its Library). Returns an invalid id
    /// (index == UINT32_MAX) if handle/name is null or nothing matches.
    LeDesignId le_design_by_name(LeHandle *handle, const char *name);

    /// @brief The name of the Design at `id`. Owned by the handle's Root -
    /// valid until the handle is destroyed. Returns nullptr if handle is
    /// null or id doesn't name a Design on this handle.
    const char *le_design_name_by_id(LeHandle *handle, LeDesignId id);

    /// @brief The Library that holds the Design at `id`. Invalid if handle
    /// is null or id doesn't name a Design on this handle.
    LeLibraryId le_design_library_by_id(LeHandle *handle, LeDesignId id);

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

    /// @brief Port/Net/Instance/PortBus/NetBus friendly-id lookup pair -
    /// same shape and reasoning as le_row_by_name/le_row_name above
    /// (Port.name/Net.name/Instance.name/PortBus.name/NetBus.name are
    /// each unique_per_parent, scoped to their own Schematic, not
    /// global), but scoped to `handle->current_schematic_id` (the
    /// Schematic-view analog of `handle->current_layout_id`) instead.
    LePortId le_port_by_name(LeHandle *handle, const char *name);
    const char *le_port_name(LeHandle *handle, LePortId id);
    LeNetId le_net_by_name(LeHandle *handle, const char *name);
    const char *le_net_name(LeHandle *handle, LeNetId id);
    LeInstanceId le_instance_by_name(LeHandle *handle, const char *name);
    const char *le_instance_name(LeHandle *handle, LeInstanceId id);
    LePortBusId le_port_bus_by_name(LeHandle *handle, const char *name);
    const char *le_port_bus_name(LeHandle *handle, LePortBusId id);
    LeNetBusId le_net_bus_by_name(LeHandle *handle, const char *name);
    const char *le_net_bus_name(LeHandle *handle, LeNetBusId id);

    /// @brief Hierarchical-path variants of le_get_instances/le_get_nets/
    /// le_get_ports - `path`
    /// is a "/"-delimited path down the Instance hierarchy (each segment
    /// may be a plain literal, a single-level glob, or "**" recursive
    /// descent - see hierarchical_resolver.hpp), anchored at `of_schematic`
    /// (or `handle->current_schematic_id` when `of_schematic` doesn't
    /// resolve, same convention as the plain le_get_<type> functions).
    /// `filter_expression` applies on top of the resolved results via the
    /// same generic evaluator the flat search already uses - a path and a
    /// `-filter` aren't mutually exclusive. Populates the same
    /// instance_search_results/net_search_results/port_search_results
    /// fields the flat search does, so le_search_result_instance_at/etc.
    /// read the results back identically either way. Returns the result
    /// count, or -1 if `filter_expression` failed to parse/validate (an
    /// error message is logged via spdlog::error either way).
    int32_t le_get_instances_by_path(LeHandle *handle, LeSchematicId of_schematic, const char *path,
                                      const char *filter_expression);
    int32_t le_get_nets_by_path(LeHandle *handle, LeSchematicId of_schematic, const char *path,
                                 const char *filter_expression);
    int32_t le_get_ports_by_path(LeHandle *handle, LeSchematicId of_schematic, const char *path,
                                  const char *filter_expression);

    /// @brief Number of property rows for the Terminal at `id` - same
    /// name/value table shape (LeProperty) le_object_property_count/_at
    /// use for LE_OBJECT_KIND_TERMINAL, but addressed directly by id, for
    /// a TCL `get_terminal`-style caller that isn't working off the
    /// current GUI selection. Rows: every plain Terminal field from codegen's
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
    /// `id` (`get_properties`/`report_properties` dot-notation, e.g.
    /// `.name`, or chained through a hop like
    /// `.ports.port_class`) - see `src/database/filter.hpp`'s
    /// `parse_property_path`/`resolve_property_path` for the grammar and
    /// resolution semantics (same `match_hop`/`get_field` machinery
    /// `-filter` expressions already use, just resolving a value instead
    /// of evaluating a comparison). Every segment but the last must be a
    /// hop, the last must be a leaf field - both checked against the
    /// same allowlist `-filter` validation uses (`validate_filter_path`
    /// in api.cpp) before resolving, so an unrecognized field/hop name
    /// is a real error (logged via spdlog::error), not silent. Returns
    /// an all-null/zero row (LeProperty::name ==
    /// nullptr) if handle/path is null, `path` fails to parse or
    /// validate, id doesn't name a Terminal on this handle, or the path
    /// is structurally valid but resolves to nothing for this specific
    /// object (e.g. a list hop with zero elements) - the latter case
    /// pushes no message, unlike a parse/validation failure.
    LeProperty le_terminal_property_path(LeHandle *handle, LeTerminalId id, const char *path);

    // --- Library/Design/Abstract/Shape property rows (for
    // get_properties/report_properties) - same by-id shape as
    // le_terminal_property_count/_at above, one pair per type. Unlike
    // Terminal's own "port_count" (or TerminalPort/Obstruction's
    // "shapes_count", further below), none of these four add a derived
    // child-pool "_count" row - just codegen's generated to_properties() as-is
    // for each type; see le_get_designs/le_get_terminals/
    // le_get_obstructions if you need those counts instead. ---

    /// @brief Number of property rows for the Library at `id`. Rows:
    /// every plain Library field from codegen's generated to_properties()
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
    /// plain Design field from codegen's generated to_properties() ("name").
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
    /// every plain Abstract field from codegen's generated to_properties()
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
    /// `filter_expression` (`-filter {...}`, e.g.
    /// ".name =~ IN*" - see src/database/filter.hpp for the full
    /// grammar). Returns the number of matches (0 if handle or
    /// filter_expression is null, or if nothing matched), or -1 if
    /// filter_expression fails to parse (the parse error is logged via
    /// spdlog::error, the same way le_read_lef's own errors are).
    /// Results are cached on the handle
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

    /// @brief Search Terminals - the general contract every other
    /// le_get_* function in this API follows too. Three independent,
    /// each-optional axes:
    ///   - `of_abstract` scopes to one Abstract's own Terminals (see
    ///     le_get_abstracts) - pass an invalid id (e.g. a
    ///     default-constructed LeAbstractId) to use the default scope
    ///     instead: the currently selected Design's Abstract (the
    ///     "current view" - le_set_current_design_abstract/
    ///     le_set_current_design_abstract_by_id), or none if no Design is
    ///     selected.
    ///   - `name_expression` glob-matches Terminal::name (Tcl `string
    ///     match` semantics, e.g. "IN*") - pass null or "" to skip this
    ///     axis (match every name).
    ///   - `filter_expression` (see src/database/filter.hpp) -
    ///     pass null or "" to skip this axis. Only field/hop names this
    ///     API recognizes as valid for Terminal are accepted - an
    ///     unrecognized one is a validation error, not silent no-match.
    /// A Terminal matches iff it satisfies every given axis. Shares
    /// le_search_terminal's result buffer - read results back via
    /// le_search_result_terminal_at. Returns the match count (0 if handle
    /// is null or nothing matched), or -1 if filter_expression fails to
    /// parse or references an unknown field/hop (either error is logged
    /// via spdlog::error).
    int32_t le_get_terminals(LeHandle *handle, LeAbstractId of_abstract, const char *name_expression, const char *filter_expression);

    // --- TerminalPort/Obstruction filter-search ---
    //
    // le_create_terminal_port/le_create_obstruction (an empty parent
    // only - no layer, no geometry) and le_create_shape/le_update_shape
    // (including their own -rects/-polygons/-paths - see
    // Klass.list_compound_kind() in codegen/codegen/schema.py) are all
    // generated (generated/api/declarations.inc, below), not hand-written
    // here. Only per-index rect/polygon/path removal
    // (le_remove_shape_rect/_polygon/_path, further below) stays
    // hand-written - adding/replacing geometry goes through
    // le_create_shape/le_update_shape's own generated flat-array
    // parameters instead.

    /// @brief Number of property rows for the TerminalPort at `id` - same
    /// by-id (not selection-scoped) shape as le_terminal_property_count.
    /// Rows: "shapes_count" (int), "port_class" (string) - codegen's
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
    /// ".shapes.layer_name == M4"). Returns the match count, or -1 on a parse
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
    /// (int) - codegen's generated to_properties() output for
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

    // --- Shape CRUD, addressed by a stable id (`Shape` is pooled so a
    // single existing shape - attached to either a TerminalPort or an
    // Obstruction - can be read/updated/deleted independently of its
    // parent). A shape's own id doesn't say
    // which kind of parent it belongs to - le_terminal_port_shape_at/
    // le_obstruction_shape_at are how a caller discovers a LeShapeId in
    // the first place (enumerating a specific parent's shapes) or the
    // generated le_create_shape (generated/api/declarations.inc, below -
    // takes both a LeTerminalPortId and a LeObstructionId, exactly one of
    // which must resolve) returns one directly; after that, every
    // le_shape_*/le_remove_shape_*
    // call below, and the generated le_delete_shape (generated/api/
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
    // in this API uses) - le_api.i's coordinate-list typemap wraps these
    // so a Tcl caller doesn't have to pre-flatten. Texts aren't included
    // (they're a computed render-time label, never LEF-authored data, so
    // there's nothing for a caller to create/read here). ---


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

    /// @brief Number of property rows for the Shape at `id` (same by-id
    /// shape as le_terminal_property_count/_at and its
    /// Library/Design/Abstract siblings above). Rows: every plain
    /// Shape field from codegen's generated to_properties() ("layer_name",
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

    /// @brief le_remove_shape_path for a Wire: removes its `path_index`th
    /// path (a run of segments, as le_wire_* reads them). Returns 0 on
    /// success, nonzero if `id` isn't a Wire or the index is out of range.
    int le_remove_wire_path(LeHandle *handle, LeWireId id, int32_t path_index);

    // --- shape_* operations ---
    //
    // Each creates new Shapes from existing Shapes or Wires - `shapes` are
    // LE_OBJECT_KIND_SHAPE or LE_OBJECT_KIND_WIRE refs, a Wire read as the
    // Shape it converts to (src/geometry/
    // shape_ops.hpp) - and returns how many it created (>= 0, possibly 0 for
    // e.g. an empty AND), or -1 on failure with the reason logged via
    // spdlog. The new ids are then readable via le_shape_op_result_at
    // until the next shape_* call. Every op works on each input Shape's
    // own merged area (rects + polygons + stroked paths).
    //
    // `layer` may be the invalid id (index == UINT32_MAX) and `purpose`
    // null/empty to keep each result on its input's own layer
    // (le_shape_copy/le_shape_change_layer require one or the other); `purpose` is
    // a ShapePurpose spelling such as "DEBUG" (`-layer debug`), for a
    // layer-less result drawn on the DEBUG pseudo-row, and is only used
    // when `layer` is invalid.
    // `parent` may be the invalid ref (le_object_invalid_ref) to put the
    // results in the current Abstract/Layout's free-standing shapes
    // (Abstract/Layout.free_shapes - never written by write_lef/
    // write_def); else it names an abstract/layout (same, but that one),
    // or an obstruction/terminal_port/route/blockage/physical_port_segment
    // whose own shapes list the results join. Each created Shape is
    // recorded into the currently-recording transaction, if any, so undo
    // removes it.

    typedef enum LeShapeBooleanOp
    {
        LE_SHAPE_BOOLEAN_OR = 0,
        LE_SHAPE_BOOLEAN_AND = 1,
        LE_SHAPE_BOOLEAN_NOT = 2, // a minus b
    } LeShapeBooleanOp;

    /// @brief One new Shape per input, same geometry, on `layer` (required).
    int32_t le_shape_copy(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, LeLayerId layer, const char *purpose, LeObjectRef parent);

    /// @brief One new Shape holding `op` (LeShapeBooleanOp) of every shape
    /// in `shapes_a` against every shape in `shapes_b` (both non-empty);
    /// on shapes_a[0]'s own layer unless `layer` is given. A region with
    /// holes comes back as exact rects (a Polygon can't hold a hole).
    int32_t le_shape_boolean(LeHandle *handle, const LeObjectRef *shapes_a, int32_t shape_a_count, const LeObjectRef *shapes_b,
                             int32_t shape_b_count, int32_t op, LeLayerId layer, const char *purpose, LeObjectRef parent);

    /// @brief One new polygon-only Shape per input.
    int32_t le_shape_to_polygon(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, LeLayerId layer, const char *purpose, LeObjectRef parent);

    /// @brief One new rect-only Shape per input, non-overlapping - `vertical`
    /// nonzero cuts with vertical lines (vertical strips), else horizontal.
    int32_t le_shape_to_rects(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, int32_t vertical, LeLayerId layer, const char *purpose,
                              LeObjectRef parent);

    /// @brief One new Shape per input, grown (positive) or shrunk
    /// (negative) by dx_um/dy_um. Different X/Y amounts need rectilinear
    /// input. A shape shrunk away entirely creates nothing.
    int32_t le_shape_size(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, double dx_um, double dy_um, LeLayerId layer,
                          const char *purpose, LeObjectRef parent);

    /// @brief One new path-only Shape per input: a closed path of width_um
    /// along its outline (and any holes), plus its own input paths
    /// re-stroked at width_um.
    int32_t le_shape_path(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, double width_um, LeLayerId layer, const char *purpose,
                          LeObjectRef parent);

    /// @brief Puts each shape in place onto `layer` (or the layer-less
    /// `purpose`, not for a Wire), clearing whichever of the two it doesn't
    /// set; geometry, position and owner are unchanged. Returns how many changed, or -1
    /// (nothing changed) on failure. Records an exact undo per shape.
    int32_t le_shape_change_layer(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, LeLayerId layer, const char *purpose);

    /// @brief The `index`th Shape created by the most recent le_shape_* call
    /// (invalid id if out of range).
    LeShapeId le_shape_op_result_at(LeHandle *handle, int32_t index);

    typedef struct LeShapeBbox
    {
        int32_t valid; // 0 if any id was unknown, none were given, or they have no geometry
        double ll_x_um;
        double ll_y_um;
        double ur_x_um;
        double ur_y_um;
    } LeShapeBbox;

    /// @brief The bbox of every given shape together, in microns. Creates nothing.
    LeShapeBbox le_shape_bbox(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count);

    // --- Generated TCL property-reading, create_<type>, update_<type>,
    // and delete_<type> surface (see CLAUDE.md's TCL section) -
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
    // class. Never edit generated/api/declarations.inc directly -
    // the build regenerates it. ---
#include "generated/api/declarations.inc"

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
