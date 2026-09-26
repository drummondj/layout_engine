#pragma once

struct LeHandle;

namespace le::gui
{
    /// @brief Dear ImGui prototype (replacing Flutter for the CPU-only-
    /// Linux-VM deploy target - Flutter's own GPU-oriented rendering
    /// assumptions perform poorly there, ImGui's tiny per-frame draw-call
    /// count should not).
    ///
    /// Blocks the calling thread forever, running this process's own
    /// GUI-owning loop: idles (polling le_take_show_gui_request()) until
    /// the Tcl console's `show_gui` command requests a window, then opens
    /// a GLFW + Dear ImGui window rendering `handle`'s own pixel buffer
    /// (le_render_pixel_buffer) and driving its mouse/keyboard input
    /// through the existing le_* API, closing back down to the idle
    /// state (ready for `show_gui` to reopen it) when the window is
    /// closed. This module has no dependency on Tcl/SWIG at all and
    /// doesn't know le_shell exists - its only coupling to the console is
    /// through `handle` itself and the show-gui-request flag both sides
    /// already share via LeHandle.
    ///
    /// Must be called from the process's own true main thread - GLFW
    /// requires window/context creation to happen only there on macOS
    /// (a hard Cocoa constraint; harmless to also do this on Linux, which
    /// has no such restriction). This is why the caller (le_shell.cpp)
    /// runs its own interactive Tcl console on a *different*, spawned
    /// thread instead of the process's main one - Tcl_Main's own
    /// blocking stdin/event loop and this loop can't share a thread
    /// either way (see le_shell.cpp's own comment for the full
    /// threading story). Never returns in normal operation - process
    /// exit is driven entirely by the Tcl console thread's own
    /// Tcl_Main() reaching `exit`/EOF, which terminates the whole
    /// process immediately (Tcl_Exit() calls the platform exit()), with
    /// no coordinated shutdown needed here.
    void run_main_thread_loop(LeHandle *handle);

    /// @brief What "Exit le_shell" in the window's close dialog does
    /// (NEW_FEATURES_SEPT_2026.md item 18: closing the window asks whether
    /// to close just the window or exit the tool) - called on the GUI
    /// thread once the window is torn down. The default flushes stdio and
    /// ends the process at once (std::_Exit - no static destructors racing
    /// the Tcl thread), right for a batch script; le_shell's interactive
    /// mode instead asks its Tcl thread to exit, so readline can restore
    /// the terminal first. Set before run_main_thread_loop.
    using ExitHandler = void (*)();
    void set_exit_handler(ExitHandler handler);
}
