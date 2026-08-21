#include "le_tcl_bridge.hpp"

#include <tcl.h>

#include <cstdio>

namespace {

// Redefines `puts` to capture its text into `::le_console_output` instead
// of writing to the real stdout/stderr channels - deliberately injected
// here as a bootstrap Tcl_Eval, not added to le_tcl_procs.tcl itself: that
// file is also sourced by le_shell.cpp (a real interactive terminal
// program, backend/src/tcl/le_shell.cpp), where `puts` must keep printing
// to the real terminal. Scoping the override to this one embedded
// Tcl_Interp is what makes it safe to redefine a built-in this way at all.
//
// A pure-Tcl `rename`+`proc` override rather than a custom C
// Tcl_ChannelType (the more "complete" way to intercept stdout/stderr,
// registered via Tcl_RegisterChannel/Tcl_SetStdChannel) - every realistic
// source of console output in this project's own Tcl surface (user-typed
// commands, le_tcl_procs.tcl, Tcl core's own default `bgerror`) goes
// through the `puts` command, channel argument (stdout/stderr) and all
// (`puts stderr $msg` is still a call to `puts`), so this catches both
// without needing a lower-level channel driver.
const char* const kCapturePutsBootstrap = R"tcl(
    set ::le_console_output {}
    rename ::puts ::le_tcl_real_puts
    proc ::puts {args} {
        set noNewline [expr {[lindex $args 0] eq "-nonewline"}]
        append ::le_console_output [lindex $args end]
        if {!$noNewline} {
            append ::le_console_output "\n"
        }
        return {}
    }
)tcl";

}  // namespace

namespace le {

TclBridge::TclBridge(int64_t handle_address, const std::string& tcl_module_path,
                      const std::string& tcl_procs_path) {
    Tcl_FindExecutable(nullptr);
    interp_ = Tcl_CreateInterp();
    if (Tcl_Init(interp_) != TCL_OK) {
        std::fprintf(stderr, "TclBridge: Tcl_Init failed: %s\n", Tcl_GetStringResult(interp_));
    }

    const std::string load_command = "load {" + tcl_module_path + "} le_tcl";
    if (Tcl_Eval(interp_, load_command.c_str()) != TCL_OK) {
        std::fprintf(stderr, "TclBridge: failed to load le_tcl module: %s\n",
                      Tcl_GetStringResult(interp_));
    }

    const std::string inject_command = "set_session_handle " + std::to_string(handle_address);
    if (Tcl_Eval(interp_, inject_command.c_str()) != TCL_OK) {
        std::fprintf(stderr, "TclBridge: set_session_handle failed: %s\n",
                      Tcl_GetStringResult(interp_));
    }

    if (Tcl_EvalFile(interp_, tcl_procs_path.c_str()) != TCL_OK) {
        std::fprintf(stderr, "TclBridge: failed to source le_tcl_procs.tcl: %s\n",
                      Tcl_GetStringResult(interp_));
    }

    if (Tcl_Eval(interp_, kCapturePutsBootstrap) != TCL_OK) {
        std::fprintf(stderr, "TclBridge: failed to install puts capture: %s\n",
                      Tcl_GetStringResult(interp_));
    }
}

TclBridge::~TclBridge() {
    if (interp_ != nullptr) {
        Tcl_DeleteInterp(interp_);
        interp_ = nullptr;
    }
}

std::string TclBridge::evalTcl(const std::string& command) {
    if (interp_ == nullptr) {
        return "error: Tcl interpreter unavailable";
    }
    // Routed through le_repl_eval (le_tcl_procs.tcl), not a raw Tcl_Eval of
    // `command` itself (UPDATES.md item 21) - the single bracket point
    // that makes a typed console command exactly as undoable (Ctrl-Z/
    // Ctrl-Shift-Z) as a GUI edit, and the source of the command-recall
    // log's success-only filtering. `command` is passed through
    // Tcl_SetVar/a variable reference, not substituted directly into the
    // eval'd string, to avoid re-escaping arbitrary user-typed text - same
    // pattern this file already uses for `le_console_output` below.
    //
    // Tcl_Eval's return code (TCL_OK vs TCL_ERROR) isn't surfaced
    // separately here - le_repl_eval already catches the wrapped command's
    // own error internally and always returns TCL_OK itself, and the
    // interpreter's own string result holds the (successful or error)
    // result either way, exactly what a real interactive Tcl shell would
    // print.
    Tcl_SetVar(interp_, "le_pending_command", command.c_str(), TCL_GLOBAL_ONLY);
    Tcl_Eval(interp_, "le_repl_eval $le_pending_command");
    std::string result = Tcl_GetStringResult(interp_);

    // Drain whatever `puts` captured during this command (see
    // kCapturePutsBootstrap above) and reset it for the next call - shown
    // ahead of the interpreter's own result, matching the order a real
    // terminal would print them in (output as the command runs, its
    // return value logged last).
    const char* captured = Tcl_GetVar(interp_, "le_console_output", TCL_GLOBAL_ONLY);
    std::string captured_output = captured != nullptr ? captured : "";
    Tcl_SetVar(interp_, "le_console_output", "", TCL_GLOBAL_ONLY);

    if (captured_output.empty()) {
        return result;
    }
    if (result.empty()) {
        return captured_output;
    }
    return captured_output + result;
}

}  // namespace le
