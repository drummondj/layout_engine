#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct Tcl_Interp;

namespace le {

// Plain C++ core of the embedded-Tcl-console logic every platform's own
// method-channel handler wraps (macOS: LeTclBridge.h/.mm, now a thin
// Objective-C++ shim around this class; Linux: layout_engine_plugin.cc's
// createTclConsole/startTclEval/pollTclEval/disposeTclConsole cases
// construct one of these directly) - see TCL_EXPLORATION.md's show_gui
// section for the full design rationale. Nothing platform-specific here
// (no NSString/GLib/etc) - std::string in and out, so it compiles and
// links identically into either platform's plugin target.
//
// One Tcl_Interp per instance, owned exclusively by a single persistent
// worker thread this class starts in its constructor and stops in its
// destructor - startEval()/poll() are the only members meant to be called
// from the platform thread (the one the owning method channel handler runs
// on). This split exists so a long-running command (e.g. a `for` loop with
// `after 1000` in it) doesn't block that thread - see BUGS_AND_ENHANCEMENTS.md
// item E3. It's safe to add this third caller into what api.cpp's LeHandle
// already treats as a multi-threaded surface (see LeHandle::mutex_'s own
// doc comment - the raster thread and the platform thread already both
// call into the same handle) - every command this bridge evaluates reaches
// backend state through the SWIG shim, which itself goes through api.hpp's
// le_* functions, each already locking that same mutex_ per call.
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
    // a partially-initialized bridge still responds to startEval()/poll()
    // rather than crashing the caller, just with Tcl's own "invalid
    // command name" style errors instead of this project's domain
    // commands.
    TclBridge(int64_t handle_address, const std::string& tcl_module_path,
              const std::string& tcl_procs_path);

    // Signals the worker thread to stop and joins it - if a command is
    // still in flight (e.g. mid `after`), this blocks until it finishes,
    // narrowing the same wait a synchronous evalTcl() used to impose on
    // every caller down to just this bridge's own teardown.
    ~TclBridge();

    TclBridge(const TclBridge&) = delete;
    TclBridge& operator=(const TclBridge&) = delete;

    // Hands `command` to the worker thread and returns immediately -
    // called from the platform thread. Not reentrant while an eval is
    // already in flight (callers are expected to gate on poll()'s own
    // `running` flag, same "one command at a time" contract a real
    // interactive shell already has).
    void startEval(const std::string& command);

    struct PollResult {
        bool running = false;
        // Every `puts` line captured since the last poll() call, in
        // order, concatenated exactly as `puts` produced them (including
        // or omitting the trailing newline per `-nonewline`) - empty if
        // nothing new.
        std::string output;
        // True on exactly one poll() call per startEval() - the first one
        // observed after the worker finishes. `result` is only meaningful
        // when this is true.
        bool has_result = false;
        std::string result;
    };

    // Drains whatever `puts` has captured since the last call and reports
    // whether the in-flight eval (if any) has finished - safe to call from
    // a different thread than the worker (this never touches interp_
    // itself, only the mutex-guarded output/result buffers below).
    PollResult poll();

    // Appends one `puts` call's worth of text to the output buffer poll()
    // drains. Public (not private) because it's called from
    // PushConsoleOutputCmd (le_tcl_bridge.cpp), a plain C Tcl_ObjCmdProc
    // registered against this instance via ClientData, not a member
    // function - not meant to be called from outside this file otherwise.
    void PushOutput(const std::string& text);

private:
    void workerLoop();

    Tcl_Interp* interp_ = nullptr;

    std::thread worker_;
    std::mutex cmd_mutex_;
    std::condition_variable cmd_cv_;
    std::string pending_command_;
    bool has_pending_command_ = false;
    bool stop_ = false;

    // True from startEval() until the worker has stored a result -
    // written with cmd_mutex_ held so a poll() racing a just-issued
    // startEval() can't observe a stale "not running" state before the
    // worker has even woken up.
    bool running_ = false;

    std::mutex output_mutex_;
    std::string output_buffer_;

    std::mutex result_mutex_;
    bool has_pending_result_ = false;
    std::string pending_result_;
};

}  // namespace le
