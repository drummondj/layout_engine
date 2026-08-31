#include "le_tcl_bridge.hpp"

#include <tcl.h>

#include <cstdio>

namespace {

// Pushes one `puts` call's worth of text into the owning TclBridge's
// mutex-guarded output buffer (see TclBridge::poll()) - a real C command
// (not a Tcl-level accumulate-into-a-variable, which was this file's own
// pre-async-eval design) so it's safe to drain from a different thread
// than the one running Tcl_Eval, and so a partial script's own output is
// observable before the command it belongs to finishes.
int PushConsoleOutputCmd(ClientData client_data, Tcl_Interp* interp, int objc,
                          Tcl_Obj* const objv[]) {
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "text");
        return TCL_ERROR;
    }
    auto* bridge = reinterpret_cast<le::TclBridge*>(client_data);
    bridge->PushOutput(Tcl_GetString(objv[1]));
    return TCL_OK;
}

// Redefines `puts` to route its text through le_push_console_output
// (registered by TclBridge's constructor, see below) instead of writing to
// the real stdout/stderr channels - deliberately injected here as a
// bootstrap Tcl_Eval, not added to le_tcl_procs.tcl itself: that file is
// also sourced by le_shell.cpp (a real interactive terminal program,
// backend/src/tcl/le_shell.cpp), where `puts` must keep printing to the
// real terminal. Scoping the override to this one embedded Tcl_Interp is
// what makes it safe to redefine a built-in this way at all.
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
    rename ::puts ::le_tcl_real_puts
    proc ::puts {args} {
        set noNewline [expr {[lindex $args 0] eq "-nonewline"}]
        set text [lindex $args end]
        if {!$noNewline} {
            append text "\n"
        }
        le_push_console_output $text
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

    Tcl_CreateObjCommand(interp_, "le_push_console_output", PushConsoleOutputCmd, this, nullptr);
    if (Tcl_Eval(interp_, kCapturePutsBootstrap) != TCL_OK) {
        std::fprintf(stderr, "TclBridge: failed to install puts capture: %s\n",
                      Tcl_GetStringResult(interp_));
    }

    worker_ = std::thread(&TclBridge::workerLoop, this);
}

TclBridge::~TclBridge() {
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        stop_ = true;
    }
    cmd_cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (interp_ != nullptr) {
        Tcl_DeleteInterp(interp_);
        interp_ = nullptr;
    }
}

void TclBridge::startEval(const std::string& command) {
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        pending_command_ = command;
        has_pending_command_ = true;
        running_ = true;
    }
    cmd_cv_.notify_one();
}

TclBridge::PollResult TclBridge::poll() {
    PollResult r;

    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        r.output = std::move(output_buffer_);
        output_buffer_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        if (has_pending_result_) {
            r.has_result = true;
            r.result = std::move(pending_result_);
            pending_result_.clear();
            has_pending_result_ = false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        r.running = running_;
    }

    return r;
}

void TclBridge::PushOutput(const std::string& text) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    output_buffer_ += text;
}

void TclBridge::workerLoop() {
    while (true) {
        std::string command;
        {
            std::unique_lock<std::mutex> lock(cmd_mutex_);
            cmd_cv_.wait(lock, [this] { return has_pending_command_ || stop_; });
            if (stop_ && !has_pending_command_) {
                return;
            }
            command = pending_command_;
            has_pending_command_ = false;
        }

        std::string result;
        if (interp_ == nullptr) {
            result = "error: Tcl interpreter unavailable";
        } else {
            // Routed through le_repl_eval (le_tcl_procs.tcl), not a raw
            // Tcl_Eval of `command` itself (UPDATES.md item 21) - the
            // single bracket point that makes a typed console command
            // exactly as undoable (Ctrl-Z/Ctrl-Shift-Z) as a GUI edit, and
            // the source of the command-recall log itself (every typed
            // command, successful or not, except complete_command -
            // BUGS_AND_ENHANCEMENTS.md E5). `command` is passed through
            // Tcl_SetVar/a
            // variable reference, not substituted directly into the
            // eval'd string, to avoid re-escaping arbitrary user-typed
            // text.
            //
            // Tcl_Eval's return code (TCL_OK vs TCL_ERROR) isn't surfaced
            // separately here - le_repl_eval already catches the wrapped
            // command's own error internally and always returns TCL_OK
            // itself, and the interpreter's own string result holds the
            // (successful or error) result either way, exactly what a
            // real interactive Tcl shell would print.
            Tcl_SetVar(interp_, "le_pending_command", command.c_str(), TCL_GLOBAL_ONLY);
            Tcl_Eval(interp_, "le_repl_eval $le_pending_command");
            result = Tcl_GetStringResult(interp_);
        }

        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            pending_result_ = std::move(result);
            has_pending_result_ = true;
        }
        {
            std::lock_guard<std::mutex> lock(cmd_mutex_);
            running_ = false;
        }
    }
}

}  // namespace le
