#pragma once

#include <cstdint>
#include <string>

struct Tcl_Interp;

namespace le {

// Plain C++ core of the embedded-Tcl-console logic every platform's own
// method-channel handler wraps (macOS: LeTclBridge.h/.mm, now a thin
// Objective-C++ shim around this class; Linux: layout_engine_plugin.cc's
// createTclConsole/evalTclCommand/disposeTclConsole cases construct one of
// these directly) - see TCL_EXPLORATION.md's show_gui section for the full
// design rationale. Nothing platform-specific here (no NSString/GLib/etc) -
// std::string in and out, so it compiles and links identically into either
// platform's plugin target.
//
// One Tcl_Interp per instance, evaluated synchronously one command at a
// time via evalTcl(), called from whatever thread the owning method
// channel handler already runs on (the platform thread) - there is no
// second event loop here to reconcile with the platform's own run loop.
// Unlike backend/src/tcl/le_shell.cpp (a Tcl_Main-based standalone binary
// owning the whole process and its own blocking event loop), this embeds
// Tcl as a plain library.
class TclBridge {
public:
    // Creates a fresh Tcl_Interp, loads the SWIG-built le_tcl module from
    // tcl_module_path, points its session at handle_address via
    // set_session_handle (see backend/src/tcl/le_tcl_shim.hpp) so every
    // CRUD/search command mutates the exact same database the GUI is
    // already rendering, then sources tcl_procs_path (le_tcl_procs.tcl)
    // for the -flag value ergonomic command layer. Never destroys
    // handle_address - ownership stays with whoever created it (the
    // Dart-owned LeEditor), same as le_tcl_shim.cpp's own injected-handle
    // contract. Logs to stderr (not exceptions/return codes) on any setup
    // failure, matching the platform wrappers' own pre-refactor behavior -
    // a partially-initialized bridge still responds to evalTcl() rather
    // than crashing the caller, just with Tcl's own "invalid command name"
    // style errors instead of this project's domain commands.
    TclBridge(int64_t handle_address, const std::string& tcl_module_path,
              const std::string& tcl_procs_path);
    ~TclBridge();

    TclBridge(const TclBridge&) = delete;
    TclBridge& operator=(const TclBridge&) = delete;

    // Evaluates one Tcl command synchronously against this bridge's
    // interpreter, returning the interpreter's string result whether the
    // command succeeded or failed (Tcl already puts the error message in
    // the same place on TCL_ERROR - a caller doesn't need a different path
    // for errors, matching what a real interactive shell prints either
    // way). Also includes anything the command wrote via `puts` (stdout or
    // stderr), prepended ahead of the interpreter's own result - `puts`
    // inside this bridge's interpreter never reaches the process's real
    // stdout/stderr, so a script's own output only ever reaches the caller
    // through here.
    std::string evalTcl(const std::string& command);

private:
    Tcl_Interp* interp_ = nullptr;
};

}  // namespace le
