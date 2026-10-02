#pragma once

struct LeHandle;

namespace le::gui
{
    /// @brief The Dear ImGui GUI - chosen for the CPU-only Linux-VM
    /// deploy target, where its tiny per-frame draw-call count stays
    /// cheap without a GPU.
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
    /// threading story). Never returns: the process ends on this thread
    /// once request_exit() is called, or through the exit handler.
    void run_main_thread_loop(LeHandle *handle);

    /// @brief Asks run_main_thread_loop to close any open window (without
    /// the close dialog), terminate GLFW and std::exit(status) on the main
    /// thread. Callable from any thread; the caller must not touch shared
    /// state afterwards and should block until the process ends. Exiting
    /// from another thread instead would run static destructors while the
    /// window and its render thread are still live.
    void request_exit(int status);

    /// @brief What "Exit le_shell" in the window's close dialog does
    /// (closing the window asks whether to close just the window or exit
    /// the tool) - called on the GUI
    /// thread once the window is torn down. The default flushes stdio and
    /// ends the process at once (std::_Exit - no static destructors racing
    /// the Tcl thread), right for a batch script; le_shell's interactive
    /// mode instead asks its Tcl thread to exit, so readline can restore
    /// the terminal first. Set before run_main_thread_loop.
    using ExitHandler = void (*)();
    void set_exit_handler(ExitHandler handler);
}
