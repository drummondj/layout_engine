// Phase 0 spike proved SWIG can wrap a Tcl-facing shim into a loadable
// Tcl extension. Phase 5 (TCL_EXPLORATION.md) extends that to the full
// Phase 4 CRUD/search surface and adds the one genuinely new SWIG
// mechanism this phase needs: a custom typemap converting a Tcl list of
// doubles into the flat `(const double*, int32_t count)` pairs
// create_shape_cmd/update_shape_cmd's own generated -rects/-polygons/
// -paths parameters take (le_api_generated_i_j2.py's own %apply lines,
// see Klass.list_compound_swig_applies() in codegen/codegen/schema.py),
// so a Tcl caller passes a real (possibly nested) list rather than
// pre-flattening into a raw C array by hand.
//
// Wraps le_tcl_shim.hpp, not api.hpp directly - see this file's own
// Phase 0 header comment (still accurate) for why: api.hpp's
// handle-per-call design is for Dart FFI; the Tcl-facing surface item 15
// wants has no visible handle and uses domain verb names, not
// le_<verb>_<noun> ones. le_tcl_shim.hpp/.cpp supplies both by calling
// into api.hpp underneath a hidden process-global LeHandle*.
//
// AbstractId/DesignId cross this boundary packed into a plain `long
// long`, not as a wrapped C struct - see le_tcl_shim.hpp's own "IDs"
// comment for why (avoids custom struct typemaps; `long long`/`int32_t`
// are fundamental/portable types SWIG already knows how to marshal
// to/from a Tcl integer). TerminalId/TerminalPortId/ObstructionId/
// ShapeId cross as type-prefixed `const char*` friendly strings instead
// (`"terminal:NAME"`/`"terminal_port:N"`/`"obstruction:N"`/`"shape:N"` - see
// le_tcl_shim.hpp's own "IDs" comment) - SWIG's built-in string typemap
// already handles plain `const char*` params/returns, so this needs no
// typemap of its own either - only the coordinate-list one below.
//
// `-flag value` commands (create_terminal, update_terminal,
// create_shape, ...) are declared here under their internal `*_cmd`
// name, taking plain positional arguments - SWIG-wrapped C++ functions
// are always positional. le_tcl_procs.tcl supplies the real, flag-
// parsing command name on top of each (see its own header comment for
// why that split exists).
%module le_tcl

%{
#include "le_tcl_shim.hpp"
#include <vector>
%}

// Gives SWIG a portable definition of int32_t/int64_t (and their Tcl
// marshalling) independent of whatever <cstdint> resolves to on this
// toolchain - needed so the typemap pattern below (and the plain
// int32_t point_coord_count/coord_count parameters elsewhere) actually
// matches.
%include <stdint.i>

// --- Coordinate-list typemap (Phase 5's own reason to exist) ---
//
// Converts a Tcl list of doubles ($input) into the temp buffer's backing
// storage, then hands the CRUD shim function a plain (const double*,
// int32_t count) pair pointing into it. `temp` is declared as a typemap
// local variable (the third parens block) so it lives for the whole
// wrapper function body, not just this typemap's own code block -
// otherwise $1 would point into a vector already destroyed by the time
// the wrapped function runs.
%typemap(in) (const double *POINTS_ARRAY_UM, int32_t POINTS_COORD_COUNT) (std::vector<double> temp) {
    int listc;
    Tcl_Obj **listv;
    if (Tcl_ListObjGetElements(interp, $input, &listc, &listv) != TCL_OK) {
        SWIG_fail;
    }
    temp.resize(static_cast<size_t>(listc));
    for (int i = 0; i < listc; i++) {
        double val;
        if (Tcl_GetDoubleFromObj(interp, listv[i], &val) != TCL_OK) {
            SWIG_fail;
        }
        temp[static_cast<size_t>(i)] = val;
    }
    $1 = temp.empty() ? nullptr : temp.data();
    $2 = static_cast<int32_t>(listc);
}


int read_lef(const char *path);
int design_count();
const char *design_name(int index);
int message_count();
const char *message_at(int index);
void set_viewport_size_cmd(int width_px, int height_px);
int viewport_width();
int viewport_height();

long long design_abstract_id(int design_index);
long long design_by_name(const char *name);
const char *technology_id();
int set_current_design_cmd(long long design_id);
void set_session_handle(long long handle_address);

// --- Terminal/TerminalPort/Obstruction CRUD is fully generated now
// (create_X_cmd/update_X_cmd/delete_X_cmd - le_api_generated.i) ---

// --- Shape (create_shape_cmd/update_shape_cmd/delete_shape_cmd are
// generated - create_shape_cmd unifies the former
// create_terminal_port_shape_cmd/create_obstruction_shape_cmd split) ---
const char *shape_layer_name(const char *id);

int shape_rect_count(const char *id);
const char *shape_rect_at(const char *id, int index);
int remove_shape_rect(const char *id, int index);

int shape_polygon_count(const char *id);
int shape_polygon_point_count(const char *id, int polygon_index);
const char *shape_polygon_point_at(const char *id, int polygon_index, int point_index);
int remove_shape_polygon(const char *id, int polygon_index);

int shape_path_count(const char *id);
double shape_path_width_um(const char *id, int path_index);
int shape_path_point_count(const char *id, int path_index);
const char *shape_path_point_at(const char *id, int path_index, int point_index);
int remove_shape_path(const char *id, int path_index);

// --- Editing / undo-redo (UPDATES.md item 21) ---
void begin_command(const char *label);
int end_command(int succeeded);
int undo_command();
int redo_command();
int command_history_count();
const char *command_history_at(int index);

// --- Generated TCL property-reading surface (see backend/CLAUDE.md's
// TCL section) - never edit generated/le_api_generated.i directly,
// regenerate via the regen-tcl skill instead. ---
%include "generated/le_api_generated.i"
