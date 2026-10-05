#pragma once
// The extension SDK's Tcl side, for an extension's TCL_SOURCES (linked into
// the le_tcl module only - an extension's core library has no Tcl
// dependency). Commands follow the core pattern: a C++ function here, a
// SWIG declaration in the extension's TCL_SWIG .i file, and a proc in its
// tcl_procs file that parses flags and calls register_command_help.

#include "le/extension.hpp"

#include <tcl.h>

namespace le::ext
{
    /// @brief The session the Tcl commands act on - the handle le_shell
    /// injected (the one its GUI renders), or the module's own.
    LeHandle *tcl_session();

    /// @brief Calls each extension's `le_ext_<name>_init_tcl(Tcl_Interp *)`
    /// (those declared with le_add_extension(... TCL_INIT)), in dependency
    /// order - generated; the le_tcl module calls it once when Tcl loads it.
    /// The escape hatch for commands SWIG doesn't fit (Tcl_CreateObjCommand).
    void init_tcl(Tcl_Interp *interp);
}
