#pragma once

#include <cstdint>

// The Tcl-facing session shim: bare, handle-free functions named with
// domain vocabulary that le_api.i wraps into Tcl commands. They call the
// C++ views (api/views.hpp) on one process-wide session, created on first
// use or injected (set_session_handle), so Tcl scripts and the GUI share
// it. Each opens one view for its whole operation.
//
// Scalar/no-flag commands (read_lef, design_count, ...) keep their Tcl
// name here. A command called with `-flag value` syntax is a proc in
// le_tcl_procs.tcl that parses the flags and calls a positional `*_cmd`
// function here: SWIG-wrapped functions are positional. The proc layer
// owns flag parsing; this shim owns the conversions (friendly ids,
// microns to dbu, enum names) and command logic.
//
// A failing command returns a nonzero/negative status or "", keeping the
// operation's own reason for last_error_cmd, which the procs raise.
//
// --- IDs ---
// Ids cross as type-prefixed "friendly" strings: `terminal:<name>`,
// `library:<name>`, `design:<name>` (or `design:<lib>/<name>` when several
// Libraries have one) for named classes, `shape:<n>`/`abstract:<n>`/...
// (a packed index and generation) for the rest. A per-parent name
// resolves within the current Abstract (Terminal), Layout (Row,
// Placement, ...) or Schematic (Net, Instance, ... - below it, by Instance
// path, `instance:u_mid/u_leaf`). A malformed token, a wrong prefix or a
// name naming nothing resolves to an invalid id, which every operation
// treats as "no such object"; an empty token means "not given" (no -of,
// an omitted optional flag).
//
// The open_design path's DesignId/AbstractId cross packed into one
// int64_t instead (generation high, index low): SWIG marshals it as a
// plain Tcl integer. kInvalidId is an invalid one.
//
// --- get_<type> ---
// Each search takes zero or more `of_<parent>` tokens (none given means
// the default scope, codegen/codegen/tcl_scope.py), a name glob where the
// class has a name, and a -filter expression (src/database/filter.hpp). At
// the Tcl layer -of and name expressions take lists, OR'd: the proc loops
// over them and unions the results. A search returns its count (-1 for a
// bad -filter); get_<type>_at reads each result back. All of it is
// generated (CLAUDE.md's TCL codegen section), as are every class's
// create_/update_/delete_<type>; the only hand-written CRUD is Shape's
// remove_shape_rect/_polygon/_path.
//
// --- Property tables ---
// Not built as Tcl lists/dicts here: a value with spaces or braces needs
// Tcl's own quoting, so the shim exposes count and by-index accessors and
// le_tcl_procs.tcl builds the dict (properties_for_token). Every value
// crosses as a string - Tcl is "everything is a string".

int read_lef(const char *path, const char *library_name);
int read_def(const char *path, const char *library_name);
/// @brief read_def -placements_only: `layout_token` empty means the current Layout.
int read_def_placements_cmd(const char *path, const char *layout_token);
/// @brief Reads every file in `paths` (space-separated) in one read, so a
/// netlist elaborates together with the stub files its cells come from
/// (verilog_stub_writer.hpp).
int read_verilog_cmd(const char *paths, int is_netlist, const char *library_name);
/// @brief How many Instances `link` resolved, or -1 on failure.
int link_unresolved_instances_cmd();

/// @brief Writes stub Verilog modules for `library_token`'s Abstract-only
/// Designs; an empty token means the only Library.
int write_verilog_stubs_cmd(const char *path, const char *library_token);

/// @brief Hierarchical-path searches, called by get_instances/get_nets/
/// get_ports when a name expression contains "/"; their results are read
/// back with get_instances_at/get_nets_at/get_ports_at.
int get_instances_by_path_cmd(const char *of_schematic, const char *path, const char *filter_expression);
int get_nets_by_path_cmd(const char *of_schematic, const char *path, const char *filter_expression);
int get_ports_by_path_cmd(const char *of_schematic, const char *path, const char *filter_expression);

/// @brief Netlist edits that keep the Layout linked (WriteView's
/// delete_net_cascade/rename_net/rename_instance): delete_net, and
/// update_net/update_instance when they rename.
int delete_net_cascade_cmd(const char *id);
const char *rename_net_cmd(const char *id, const char *new_name);
const char *rename_instance_cmd(const char *id, const char *new_name);

int design_count();
const char *design_name(int index);

/// @brief 1 if the last *_property_path call had a path that didn't parse
/// or named no field (its reason in last_error_cmd), not just one that led
/// to no value.
int property_path_failed();

/// @brief Positional form behind `set_viewport_size -width W -height H`.
void set_viewport_size_cmd(int width_px, int height_px);

/// @brief The viewport size set by set_viewport_size_cmd(), in pixels.
int viewport_width();
int viewport_height();

/// @brief The packed AbstractId of the Design at `design_index` in the
/// first Library (0..design_count()-1); kInvalidId out of range. Doesn't
/// change the open view.
long long design_abstract_id(int design_index);

/// @brief The packed DesignId named `name` (as `design:` tokens name it),
/// for open_design; kInvalidId if none.
long long design_by_name(const char *name);

/// @brief The Technology's friendly id (`technology_layers [technology_id]`);
/// "" before one is read.
const char *technology_id();

/// @brief Opens Design `design_id`'s Abstract, scoping later Abstract
/// searches to it, and fits the view: 0, or 1 with the reason kept.
int set_current_design_abstract_cmd(long long design_id);

/// @brief Opens Design `design_id`'s Layout instead (one view is open at a
/// time), scoping later Layout searches to it.
int set_current_design_layout_cmd(long long design_id);

/// @brief `zoom -factor <f>`: scales by (1 + factor), positive in, about
/// the viewport's center (a script has no mouse to zoom about).
void zoom_cmd(double factor);

/// @brief `zoom_area`: fits the viewport to the micron rect, centered,
/// with `padding_px` margin.
void zoom_area_cmd(double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um, int padding_px);

/// @brief `dump_png <path>`: renders the open view (no GUI needed) and
/// writes it as an RGBA PNG. 0, or nonzero if nothing rendered (e.g. no
/// viewport size set) or the file couldn't be written.
int dump_png_cmd(const char *path);

/// @brief `write_lef`: `abstract_tokens` is a space-separated list of
/// Abstract tokens; with none and no `library_token`, the current Abstract.
/// `layer_write_mode` is an LeLefLayerWriteMode value.
int write_lef_cmd(const char *path, const char *abstract_tokens, const char *library_token, int32_t layer_write_mode);

/// @brief `write_def`: `layout_token`'s Layout, or the current one if empty.
int write_def_cmd(const char *path, const char *layout_token);

/// @brief The native database file commands write_db/read_db/db_info/migrate_db.
int write_db_cmd(const char *path, int with_session);
int read_db_cmd(const char *path, int with_session);
const char *db_info_cmd(const char *path);
const char *migrate_db_cmd(const char *in_path, const char *out_path);

/// @brief `get_selection`: how many objects are selected.
int selection_count_cmd();

/// @brief `get_selection`: the token (shape:/wire:/row:/placement:/region:)
/// of selected object `index`; "" out of range.
const char *get_selection_at_cmd(int index);

/// @brief `select`: selects the object `token` names. 0, 1 if it can't be
/// selected (the reason kept), or 2 for an unrecognized prefix (select
/// names the token in its own error).
int select_cmd(const char *token);

/// @brief Why the last shim command that failed (a nonzero or negative
/// status, or "") failed: the operation's own message.
const char *last_error_cmd();

/// @brief Layer visibility and selectability by Layer name, every row of
/// that Layer at once; false for a name that isn't a Layer.
void set_layer_visible_cmd(const char *layer_name, bool visible);
bool get_layer_visible_cmd(const char *layer_name);
void set_layer_selectable_cmd(const char *layer_name, bool selectable);
bool get_layer_selectable_cmd(const char *layer_name);

/// @brief Purpose visibility and selectability. `purpose` is the raw
/// ordinal le_tcl_procs.tcl's ::purpose_names maps a label to.
void set_purpose_visible_cmd(int32_t purpose, bool visible);
bool get_purpose_visible_cmd(int32_t purpose);
void set_purpose_selectable_cmd(int32_t purpose, bool selectable);
bool get_purpose_selectable_cmd(int32_t purpose);

/// @brief The purpose registry, for ::purpose_names and help text.
/// purpose_name_cmd returns "" out of range.
int purpose_kind_count_cmd();
const char *purpose_name_cmd(int32_t purpose);
bool purpose_visible_by_default_cmd(int32_t purpose);

/// @brief The Layers panel's value filters (set_placement_type_visible,
/// set_route_use_selectable, ...). `filter` is an LeObjectFilter value.
void set_object_filter_visible_cmd(int32_t filter, const char *value, bool visible);
bool get_object_filter_visible_cmd(int32_t filter, const char *value);
void set_object_filter_selectable_cmd(int32_t filter, const char *value, bool selectable);
bool get_object_filter_selectable_cmd(int32_t filter, const char *value);

/// @brief `set_mode`/`get_mode`: an LeMode value (0 select, 1 edit, 2
/// ruler); the proc maps the keywords.
void set_mode_cmd(int32_t mode);
int32_t get_mode_cmd();

void clear_rulers_cmd();
void deselect_all_cmd();
void arm_move_cmd();

/// @brief How many selected placements are FIXED or COVER.
int selected_locked_placement_count_cmd();

/// @brief Whether a Move is under way (its first click made).
bool is_move_anchored_cmd();

/// @brief `delete_selected_pieces`: how many were deleted.
int delete_selected_pieces_cmd();

/// @brief Placement snapping: LePlacementSnapMode values; the procs map
/// the keywords.
void set_placement_snap_mode_cmd(int mode);
int get_placement_snap_mode_cmd();
bool is_placement_snap_mode_available_cmd(int mode);

/// @brief `set_view_flip`/`get_view_flip`: LeViewFlip values.
int set_view_flip_cmd(int flip);
int get_view_flip_cmd();

/// @brief `rotate_placement`/`flip_placement`: an le::OrientationOp value.
/// 0, 1 nothing selected, 2 not available, 3 locked, -1 an unknown op.
int apply_placement_orientation_op_cmd(int op);

/// @brief Resize and shape snapping: le::PieceKind/le::ShapeSnapMode values.
void arm_resize_cmd();
void set_shape_snap_mode_cmd(int kind, int mode);
int get_shape_snap_mode_cmd(int kind);
bool is_shape_snap_mode_available_cmd(int kind, int mode);

/// @brief `show_gui`/`close_gui`: requests the GUI thread
/// (src/gui/le_gui.hpp) picks up; Tcl's blocking event loop can't share a
/// thread with the GUI's.
void request_show_gui_cmd();
void request_close_gui_cmd();
int has_unsaved_database_changes_cmd();
int has_unsaved_settings_cmd();

/// @brief A packed invalid DesignId/AbstractId (index UINT32_MAX,
/// generation 0) - see the "IDs" comment. Every other id's invalid form
/// is "".
constexpr long long kInvalidId = 0xFFFFFFFFLL;

/// @brief Points every later shim call at an externally-owned session
/// (le_shell's, which its GUI renders) instead of creating one. The shim
/// never destroys it. Call it before anything else touches the session:
/// the first use latches whichever session is in place.
void set_session_handle(long long handle_address);

// --- Shape geometry (create_/update_/delete_shape are generated) ---

const char *shape_layer_name(const char *id);

int shape_rect_count(const char *id);
/// @brief Rect `index` in microns, "{ll_x ll_y} {ur_x ur_y}".
const char *shape_rect_at(const char *id, int index);
int remove_shape_rect(const char *id, int index);

int shape_polygon_count(const char *id);
int shape_polygon_point_count(const char *id, int polygon_index);
/// @brief A polygon's point in microns, "x y".
const char *shape_polygon_point_at(const char *id, int polygon_index, int point_index);
int remove_shape_polygon(const char *id, int polygon_index);

int shape_path_count(const char *id);
double shape_path_width_um(const char *id, int path_index);
int shape_path_point_count(const char *id, int path_index);
const char *shape_path_point_at(const char *id, int path_index, int point_index);
int remove_shape_path(const char *id, int path_index);

// --- shape_* operations: each takes a space-separated shape token list;
// an empty -layer/-parent token means "omitted". Creating ones return how
// many Shapes they made (read back with shape_op_results_cmd), -1 on
// failure (the reason kept), -2 for an unknown -layer token, -3 for an
// unknown or unsupported -parent token. ---
int shape_copy_cmd(const char *shape_tokens, const char *layer_token, const char *parent_token);
int shape_boolean_cmd(const char *shape_tokens_a, const char *shape_tokens_b, int op, const char *layer_token, const char *parent_token);
int shape_to_polygon_cmd(const char *shape_tokens, const char *layer_token, const char *parent_token);
int shape_to_rects_cmd(const char *shape_tokens, int vertical, const char *layer_token, const char *parent_token);
int shape_size_cmd(const char *shape_tokens, double dx_um, double dy_um, const char *layer_token, const char *parent_token);
int shape_path_cmd(const char *shape_tokens, double width_um, const char *layer_token, const char *parent_token);
int shape_change_layer_cmd(const char *shape_tokens, const char *layer_token);
const char *shape_op_results_cmd(int count);
const char *shape_bbox_cmd(const char *shape_tokens);

// --- Undo: begin_command/end_command bracket one undo step across a
// whole typed command (le_repl_eval in le_tcl_procs.tcl is the caller),
// so it undoes as one, like a GUI edit. ---

void begin_command(const char *label);
int end_command(int succeeded);
int undo_command();
int redo_command();
int command_history_count();
/// @brief Command `index` (0..command_history_count()-1); "" out of range.
const char *command_history_at(int index);

int get_hierarchy_depth_command();
void set_hierarchy_depth_command(int depth);

int get_flightline_max_fanout_command();
void set_flightline_max_fanout_command(int max_fanout);

int get_undo_limit_command();
void set_undo_limit_command(int limit);

// Settings. The commands returning int give 0 on success, nonzero (the
// reason kept) on failure.
double get_grid_spacing_um_command(int major);
void set_grid_spacing_um_command(double minor_um, double major_um);
int get_confirm_overwrite_command();
void set_confirm_overwrite_command(int confirm);
const char *get_theme_command();
int set_theme_command(const char *name);
int theme_count_command();
const char *theme_name_command(int index);
double get_ruler_label_size_command();
void set_ruler_label_size_command(double px);
double get_label_min_size_command();
void set_label_min_size_command(double px);
double get_label_max_size_command();
void set_label_max_size_command(double px);
int set_layer_color_command(const char *layer, const char *color);
void reset_layer_color_command(const char *layer);
const char *get_layer_color_command(const char *layer);
int save_settings_command(const char *path);
int load_settings_command(const char *path);
const char *default_settings_path_command();
/// @brief The build tree's generated procs file, or "" when this module
/// wasn't built with one (le_tcl_procs.tcl then looks beside itself).
const char *generated_procs_default_path_command();

int get_max_concurrency_command();
void set_max_concurrency_command(int max_concurrency);

// The generated commands for every readable class (never edit the
// generated header; rebuild).
#include "generated/tcl/le_tcl_shim_generated.hpp"
