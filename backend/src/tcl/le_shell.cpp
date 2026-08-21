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
// `show_gui` (see le_tcl_procs.tcl) is a deliberate stub for now, not a
// missing feature quietly deferred - see TCL_EXPLORATION.md's Phase 6
// section for why real GUI integration is out of scope for this pass:
// this project's only GUI is the Flutter app, a separate Dart/Flutter
// runtime consuming api/render/io via FFI, not something this C++/Tcl
// process can "just start rendering" in-process without embedding a
// full FlutterEngine (a substantial, separate integration effort - see
// OpenROAD's own gui_start for what the real version of this looks
// like, and why it needs its own pass rather than being folded into
// this one).

#include <tcl.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    std::string g_module_path;
    std::string g_procs_path;

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

    // Never returns - Tcl_Main calls Tcl_Exit()/exit() itself once the
    // script (batch mode) or the interactive loop (EOF/`exit`) ends.
    Tcl_Main(static_cast<int>(remaining.size()), remaining.data(), app_init);
    return 0;
}
