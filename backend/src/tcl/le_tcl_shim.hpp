#pragma once

#include <cstdint>

// Tcl-facing session shim (see TCL_EXPLORATION.md's "Tcl ergonomics
// layer" section for the full rationale). Owns one process-global
// LeHandle* (lazily created on first use - the single-process/
// shared-state model TCL_EXPLORATION.md's round-1 recommendation already
// settled on for GUI<->batch-mode connection), and exposes bare,
// handle-free functions named with UPDATES.md item 15's own domain
// vocabulary (read_lef, not le_read_lef) - these are what le_api.i wraps
// into Tcl commands, not api.hpp directly.
//
// Scalar/no-flag commands (read_lef, design_count, ...) are exposed
// under their final Tcl name directly. Anything item 15 shows called
// with `-flag value` syntax (e.g. `set_viewport_size -width W -height H`,
// `create_terminal -name IN0 -direction IN`) is exposed here under an
// internal `*_cmd` name taking plain positional arguments instead:
// SWIG-wrapped C++ functions are always positional, so Tcl's own
// `-flag value` calling convention has to be parsed in Tcl itself, not
// here - a matching thin proc in le_tcl_procs.tcl does that parsing and
// then calls the `*_cmd` form positionally. This split (C++ shim owns
// session state + command logic; a thin Tcl proc layer owns flag
// parsing) is the answer to "is smoke_test.tcl representative of the
// real interface?" - it wasn't; Phase 0 fixed that, and every Phase 4
// CRUD command below follows the same shape.
//
// --- IDs (Phase 5, revised for friendly ids - UPDATES.md items 18/19.1;
// generated create_<type> - see backend/CLAUDE.md's TCL codegen section -
// later moved every CRUD-flag parent id onto this same token convention
// too, so it's now the uniform rule, not an exception) ---
// AbstractId/DesignId's own session-selection uses (design_abstract_id,
// design_by_name, set_current_design_cmd - none of these are
// create_<type>/update_<type> CRUD flags) are still a plain {uint32_t
// index, generation} struct in api.hpp, packed
// into one int64_t (generation in the high 32 bits, index in the low 32
// bits, via pack<IdT>/unpack<IdT> in le_tcl_shim.cpp) - int64_t is a
// fundamental type SWIG's stdint.i already marshals to/from a plain Tcl
// integer with zero custom typemap code. kInvalidId (0xFFFFFFFF) marks an
// invalid id for these two types in that role, since a packed valid id
// (index != UINT32_MAX) can never equal it. Item 19.1's own new
// get_libraries/get_designs/get_abstracts commands (and every generated
// create_<type> parent flag, including `-abstract`) are additive - they
// return/accept friendly strings (below), not a retrofit of those
// already-shipped session-selection entry points (a deliberate, flagged
// choice, see UPDATES.md item 19.1's own Resolution).
//
// Every other class's ids (Terminal/TerminalPort/Obstruction/Shape from
// item 18, every other pool-backed class from item 19.1's own get_*
// commands and the generated create_<type> surface) cross this shim as
// type-prefixed strings instead - not user-friendly to hand a script a
// raw packed integer as its only handle on an object. `"terminal:<name>"`/
// `"library:<name>"`/`"design:<name>"` (each has a real, uniquely-
// indexed-or-uniqueness-enforced .name field - Terminal's uniqueness is
// enforced per-Abstract via the generated unique_per_parent index, not
// hand-written in api.hpp anymore; Library/Design's is a real global cmg
// index=True) and `"obstruction:<n>"`/`"terminal_port:<n>"`/`"shape:<n>"`/
// `"abstract:<n>"`/... (no name field on these - `<n>` is a packed
// integer, type-prefixed for self-description, resolved with
// resolve_numeric_friendly_id) are built/parsed entirely in
// le_tcl_shim.cpp (hand-written ones) or le_tcl_shim_generated.inc
// (everything else, including every generated create_<type>'s own
// return value and parent-token parameters) - api.hpp's own return types
// for these are unchanged (still typed LeTerminalId/etc structs),
// matching this shim's usual "ergonomics stay here, not in the shared C
// API" split. Empty string `""` is the uniform "not found / invalid"
// signal for all of them - a malformed string or a wrong-type prefix
// (e.g. passing an `"obstruction:..."` id where a `"terminal:..."` one is
// expected) resolves to the same invalid sentinel api.hpp's own not-found
// paths already produce, indistinguishable from "no such object" - no
// separate error path needed for it. An empty *token* string is exactly
// what a caller passes to mean "no -of given" (or "omit this optional
// create_<type> flag" - see api_declarations_inc_j2's own comment) too -
// it resolves the same way (an unresolvable/absent token both look like
// "invalid" to the resolver), which is exactly what "use the default" needs.
//
// --- get_<type> command pattern (UPDATES.md item 19.1) ---
// Every get_<type> command shares one shape at the api.hpp layer: zero or
// more `of_<parent>` ids (an invalid/default-constructed one means "no
// -of given, use the default scope" - see codegen/codegen/tcl_scope.py's
// own module docstring for how that default is derived), an optional
// `name_expression` (glob-matched against the type's own friendly-id
// field, Tcl `string match` semantics - only present for types that have
// one), and an optional `filter_expression` (backend/src/database/
// filter.hpp, field/hop names validated against a generated allowlist per
// type - an unrecognized name is a real error, not silent no-match). At
// the Tcl layer, `-of` accepts a *list* of same-type parent tokens (OR'd)
// and name expressions are likewise OR'd (see le_tcl_procs.tcl's
// parse_get_args) - the shim/api.hpp layer only ever sees one resolved
// id/expression per call; the Tcl proc loops over multiple tokens/
// expressions itself and unions the per-call results. Search-result
// accessors are uniformly count+by-index (get_<type>_cmd/get_<type>_at),
// same shape as every property table below - this whole surface is
// generated now (see backend/CLAUDE.md's TCL codegen section), including
// every class's own create_<type>/update_<type>/delete_<type>; the only
// hand-written CRUD-ish surface left is Shape's own remove_shape_rect/
// _polygon/_path (removing one geometry entry by index, further below -
// not per-class flag-driven CRUD in the same sense).
//
// --- Property tables and search results (Phase 5) ---
// Deliberately NOT built as Tcl lists/dicts in C++ here - that needs
// correct Tcl quoting (a value containing spaces/braces has to be
// list-escaped), which is Tcl's own job (`lappend`/`dict set` do it
// correctly by construction) and error-prone to hand-roll in C++. This
// shim only exposes count+by-index accessors, mirroring api.hpp's own
// shape exactly; le_tcl_procs.tcl loops over them to build the ergonomic
// dict/list a real Tcl caller wants (see e.g. `properties_for_token`,
// UPDATES.md item 19.2's shared helper behind `get_properties`/
// `report_properties`).
// Every property value is exposed pre-stringified (`*_property_value`),
// not int/double/string-tagged - Tcl is "everything is a string" by its
// own design (`expr {$v + 1}` works on a numeric string exactly the same
// as a native int), so preserving LeProperty's STRING/INT/DOUBLE tag
// across this boundary isn't worth the extra accessors it would take.

int read_lef(const char *path);
int design_count();
const char *design_name(int index);
int message_count();
const char *message_at(int index);

/// @brief Positional form behind `set_viewport_size -width W -height H`
/// (see le_tcl_procs.tcl). Demonstrates the -flag-parsing split
/// end-to-end using an existing api.hpp call (le_set_viewport_size).
void set_viewport_size_cmd(int width_px, int height_px);

/// @brief Round-trips through le_render_pixel_buffer() to prove
/// set_viewport_size_cmd() actually took effect, without wrapping the
/// whole LePixelBuffer struct (its `data` pointer would need its own
/// typemap - out of scope for this demonstration).
int viewport_width();
int viewport_height();

/// @brief The AbstractId of the design at `design_index` (0..design_count()-1,
/// same flat indexing as design_name) - a raw packed id, unlike
/// create_terminal/create_obstruction/update_abstract, which all take an
/// `"abstract:N"` friendly-id token instead (see get_abstracts/
/// get_abstracts_at, or just format "abstract:" + this call's own
/// result). Scoped to the first Library only
/// (le_library_design_at's library_index=0) - correct as long as only
/// one LEF file/library is in play, same assumption this project's
/// single-shared-Technology convention already makes elsewhere. Superseded
/// for the "select a view to work in" use case by open_design below
/// (UPDATES.md item 17), and for general enumeration by get_abstracts
/// (UPDATES.md item 19.1) - kept as-is since scripts may still want a raw
/// AbstractId without changing the session's current-view state.
/// Returns kInvalidId if index is out of range.
long long design_abstract_id(int design_index);

/// @brief Positional form behind `open_design NAME -view abstract` (see
/// le_tcl_procs.tcl) - resolves NAME to a LeDesignId. Returns kInvalidId
/// if no Design named `name` is loaded on this session.
long long design_by_name(const char *name);

/// @brief The singleton Technology's friendly `"technology:N"` id (see
/// this header's own "IDs" comment) - the way a script reaches Layer/
/// Via/Site/NonDefaultRule, e.g. `technology_layers [technology_id]`.
/// Returns "" if no Technology has been read yet.
const char *technology_id();

/// @brief Select the Design `design_id` (as returned by design_by_name)
/// as this session's current view (UPDATES.md item 17) - every
/// subsequent get_terminals/get_obstructions/get_terminal_ports call is
/// scoped to its Abstract. Returns 0 on success, nonzero if design_id
/// doesn't name a Design on this session.
int set_current_design_cmd(long long design_id);

/// @brief Sentinel for "no such id" for AbstractId/DesignId in their
/// CRUD-flag/session-selection role - see this header's own "IDs"
/// comment. Every api.hpp failure path for these two types in that role
/// returns an id struct with index == UINT32_MAX and generation == 0
/// (never left uninitialized - see TCL_EXPLORATION.md's "zero-init bug"
/// note), which pack() (in le_tcl_shim.cpp) always turns into exactly
/// this value - not -1 (0xFFFFFFFFFFFFFFFF), which would require
/// generation == UINT32_MAX too. Every other id type (including Library/
/// Design/Abstract wherever get_* commands return/accept them) uses an
/// empty string ("") as their own invalid sentinel instead - see the
/// "IDs" comment above.
constexpr long long kInvalidId = 0xFFFFFFFFLL;

/// @brief Point every subsequent shim call at an externally-owned
/// LeHandle* (packed the same way every other id crosses this shim -
/// see the "IDs" comment above) instead of the shim's own lazily-self-
/// created one - e.g. the Dart-owned handle a Flutter-embedded Tcl
/// console shares (see TCL_EXPLORATION.md's show_gui section), so a Tcl
/// command mutates the exact same database the GUI is already
/// rendering, not an unrelated standalone one. The shim never destroys
/// an injected handle - ownership (le_destroy()) stays with whoever
/// created it, same as le_shell's own self-created handle is never
/// explicitly destroyed either (process-lifetime). Must be called (if
/// at all) before any other shim function that touches session() - the
/// self-created handle is lazily latched on session()'s first call and
/// is never revisited afterward, injected or not.
void set_session_handle(long long handle_address);

// --- Terminal/TerminalPort/Obstruction CRUD - create_X_cmd/update_X_cmd/
// delete_X_cmd are all generated (see le_tcl_shim_generated.hpp), nothing
// hand-written here for these three classes anymore. ---

// --- Shape CRUD (rects/polygons/paths - texts deliberately excluded,
// see TCL_EXPLORATION.md's round-7 finding: they're a Pipeline-computed
// render-time label, never LEF-authored data; create_shape_cmd/
// update_shape_cmd/delete_shape_cmd are generated - see
// le_tcl_shim_generated.hpp - create_shape_cmd takes both a
// terminal_port_id and an obstruction_id token, exactly one of which must
// resolve, replacing this header's former create_terminal_port_shape_cmd/
// create_obstruction_shape_cmd split) ---

const char *shape_layer_name(const char *id);

int shape_rect_count(const char *id);
/// @brief The rect at `index`, as a 4-element "ll_x ll_y ur_x ur_y"
/// microns string (already a well-formed Tcl list of 4 numbers).
const char *shape_rect_at(const char *id, int index);
int remove_shape_rect(const char *id, int index);

int shape_polygon_count(const char *id);
int shape_polygon_point_count(const char *id, int polygon_index);
/// @brief The point at `point_index` in the polygon at `polygon_index`,
/// as a 2-element "x y" microns string.
const char *shape_polygon_point_at(const char *id, int polygon_index, int point_index);
int remove_shape_polygon(const char *id, int polygon_index);

int shape_path_count(const char *id);
double shape_path_width_um(const char *id, int path_index);
int shape_path_point_count(const char *id, int path_index);
const char *shape_path_point_at(const char *id, int path_index, int point_index);
int remove_shape_path(const char *id, int path_index);

// --- Editing / undo-redo (UPDATES.md item 21) - begin_command/end_command
// bracket one recording transaction; le_repl_eval (le_tcl_procs.tcl) is
// the only caller, wrapping every top-level typed console command with
// them so it's exactly as undoable as a GUI edit like Move. undo/redo/
// command_history back the `undo`/`redo`/`command_history` Tcl commands
// directly (le_tcl_procs.tcl). ---

void begin_command(const char *label);
int end_command(int succeeded);
int undo_command();
int redo_command();
int command_history_count();
/// @brief The command text at `index` (0..command_history_count()-1).
/// Empty string if index is out of range.
const char *command_history_at(int index);

// --- Generated TCL property-reading surface (see backend/CLAUDE.md's
// TCL section) - bare property-table accessors and is_child-field
// enumeration for every TCL-readable class not already covered above.
// Never edit generated/le_tcl_shim_generated.hpp directly - regenerate
// via the regen-tcl skill instead. ---
#include "generated/le_tcl_shim_generated.hpp"
