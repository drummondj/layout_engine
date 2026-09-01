// Phase 6 batch shell (UPDATES.md item 15 / TCL_EXPLORATION.md): "run a
// TCL shell from the terminal" - not a modified tclsh, but built exactly
// the way tclsh/wish/OpenROAD's own shell are: a small
// Tcl_AppInitProc handed to Tcl_Main(). Tcl_Main already supplies both
// modes item 15 asks for, for free: no script argument (and stdin is a
// terminal) drops into an interactive REPL with a "% " prompt; a script
// argument runs it non-interactively then exits (batch mode) - this
// binary doesn't implement either mode itself.
//
// The AppInitProc's own job is just bootstrapping: `load` the SWIG-
// wrapped le_tcl module (built by the le_tcl CMake target - a shared
// library, not linked into this binary, same as any other Tcl
// extension) and source le_tcl_procs.tcl, so every CRUD/search command
// (create_terminal, get_terminal_ports, ...) is ready to type the
// moment the shell starts, without the caller sourcing anything
// themselves.
//
// `show_gui` (see le_tcl_procs.tcl) opens a Dear ImGui window (LE_BUILD_GUI_SHELL,
// src/gui/le_gui.hpp) sharing this same process's session state -
// replacing the earlier "deliberate stub" this comment used to describe
// (see git history/TCL_EXPLORATION.md's Phase 6 section for that earlier
// exploration and why it went a different direction for the Flutter
// plugin's own Tcl console first). Getting there means restructuring
// this binary's own thread ownership: Tcl_Main's own blocking stdin/
// event loop and a native GUI's own event loop (GLFW/Cocoa's
// NSApplication in particular) can't share one thread - the same
// conflict TCL_EXPLORATION.md already hit and steered around for the
// Flutter plugin (Tcl embedded as a library on its own worker thread
// there, instead of Tcl_Main). Here it's the mirror image: with
// LE_SHELL_HAS_GUI, this process's own true main thread is reserved for
// le::gui::run_main_thread_loop() (GLFW requires window/context creation
// only there on macOS), and Tcl_Main runs on a spawned thread instead -
// unchanged in every other respect, injecting the LeHandle this main
// thread already created via set_session_handle (le_tcl_shim.hpp) right
// after `load`-ing le_tcl, so a `show_gui` window and this console
// mutate the exact same state. Without LE_BUILD_GUI_SHELL, this binary
// keeps today's original single-threaded shape entirely (Tcl_Main
// directly on this process's own main thread, no handle of its own to
// inject - le_tcl_shim.cpp lazily self-creates one as it always has).

#include <tcl.h>

#include "api.hpp"

#ifdef LE_SHELL_HAS_GUI
#include "le_gui.hpp"

#include <thread>
#endif

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    std::string g_module_path;
    std::string g_procs_path;

#ifdef LE_SHELL_HAS_GUI
    LeHandle *g_injected_handle = nullptr;
#endif

    std::string resolve_path(const char *cli_value, const char *env_var, const char *what)
    {
        if (cli_value != nullptr)
        {
            return cli_value;
        }
        if (const char *from_env = std::getenv(env_var))
        {
            return from_env;
        }
        std::fprintf(stderr, "le_shell: no %s given - pass it as an argument or set %s\n", what, env_var);
        std::exit(2);
    }

    // Handed to Tcl_Main as its Tcl_AppInitProc - called once, on the
    // interpreter Tcl_Main itself creates, before it decides whether to
    // run a script or start the interactive loop. Loading le_tcl and
    // sourcing le_tcl_procs.tcl here (rather than via a startup script
    // argument) means both the interactive and batch-script paths get
    // the full command surface identically - a batch script never needs
    // its own `load`/`source` preamble.
    int app_init(Tcl_Interp *interp)
    {
        if (Tcl_Init(interp) == TCL_ERROR)
        {
            return TCL_ERROR;
        }

        const std::string load_command = "load {" + g_module_path + "} le_tcl";
        if (Tcl_Eval(interp, load_command.c_str()) != TCL_OK)
        {
            return TCL_ERROR;
        }

#ifdef LE_SHELL_HAS_GUI
        // Must happen before le_tcl_procs.tcl is sourced below - see
        // set_session_handle's own doc comment (le_tcl_shim.hpp) for why:
        // it only redirects future session()-touching calls, not ones
        // that already ran. Shares the exact LeHandle main() already
        // created (and le::gui::run_main_thread_loop() is about to drive
        // on the other thread), so a `show_gui` window and this console
        // mutate the same state.
        const std::string inject_command = "set_session_handle " +
            std::to_string(reinterpret_cast<int64_t>(g_injected_handle));
        if (Tcl_Eval(interp, inject_command.c_str()) != TCL_OK)
        {
            return TCL_ERROR;
        }
#endif

        if (Tcl_EvalFile(interp, g_procs_path.c_str()) != TCL_OK)
        {
            return TCL_ERROR;
        }

        return TCL_OK;
    }
}

int main(int argc, char **argv)
{
    const char *module_arg = nullptr;
    const char *procs_arg = nullptr;

    // -module/-procs are this shell's own bootstrap flags, consumed here
    // (only as a fixed leading run, before anything Tcl_Main itself
    // needs to see) rather than passed through - what's left (a script
    // path, or nothing at all for interactive mode) is exactly the argv
    // shape Tcl_Main already knows how to handle unchanged.
    std::vector<char *> remaining;
    remaining.push_back(argv[0]);

    int i = 1;
    while (i < argc)
    {
        const std::string arg = argv[i];
        if (arg == "-module" && i + 1 < argc)
        {
            module_arg = argv[i + 1];
            i += 2;
        }
        else if (arg == "-procs" && i + 1 < argc)
        {
            procs_arg = argv[i + 1];
            i += 2;
        }
        else
        {
            break;
        }
    }
    for (; i < argc; ++i)
    {
        remaining.push_back(argv[i]);
    }

    g_module_path = resolve_path(module_arg, "LE_TCL_MODULE", "the le_tcl module path (-module)");
    g_procs_path = resolve_path(procs_arg, "LE_TCL_PROCS_PATH", "the le_tcl_procs.tcl path (-procs)");

#ifdef LE_SHELL_HAS_GUI
    g_injected_handle = le_create();

    // Tcl_Main "never returns" (calls Tcl_Exit()/exit() itself once the
    // script (batch mode) or the interactive loop (EOF/`exit`) ends) -
    // runs on its own thread here instead of this process's main one,
    // which le::gui::run_main_thread_loop() below needs for itself (see
    // this file's own header comment). Detached, not joined: the whole
    // process exits from inside this thread's own Tcl_Main() call, so
    // there's no normal path where joining it would ever return either -
    // `remaining` is captured by value (a cheap copy of a vector of
    // pointers into argv's own persistent strings) so this thread's own
    // copy stays valid regardless of main()'s local variables, even
    // though main() also never returns before process exit here.
    std::thread tcl_thread([remaining]() mutable
                            { Tcl_Main(static_cast<int>(remaining.size()), remaining.data(), app_init); });
    tcl_thread.detach();

    le::gui::run_main_thread_loop(g_injected_handle);
    return 0;
#else
    // Never returns - Tcl_Main calls Tcl_Exit()/exit() itself once the
    // script (batch mode) or the interactive loop (EOF/`exit`) ends.
    Tcl_Main(static_cast<int>(remaining.size()), remaining.data(), app_init);
    return 0;
#endif
}
