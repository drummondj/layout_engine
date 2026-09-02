// Phase 6 batch shell (UPDATES.md item 15 / TCL_EXPLORATION.md): "run a
// TCL shell from the terminal" - originally a small Tcl_AppInitProc
// handed to Tcl_Main(), which supplied both modes item 15 asked for out
// of the box: no script argument (and stdin is a terminal) dropped into
// an interactive REPL with a "% " prompt; a script argument ran it
// non-interactively then exited (batch mode).
//
// The AppInitProc's own job is still just bootstrapping: `load` the
// SWIG-wrapped le_tcl module (built by the le_tcl CMake target - a
// shared library, not linked into this binary, same as any other Tcl
// extension) and source le_tcl_procs.tcl, so every CRUD/search command
// (create_terminal, get_terminal_ports, ...) is ready to type the
// moment the shell starts, without the caller sourcing anything
// themselves.
//
// This binary no longer calls Tcl_Main() at all, though (see
// run_interactive() below for the full reasoning) - its own hardcoded
// interactive loop never routed a typed command through le_repl_eval
// (le_tcl_procs.tcl), the single bracket point that makes a command
// undoable, recorded into the recall log, and truncated for display
// (UPDATES.md item 21, BUGS_AND_ENHANCEMENTS.md E5/E6) - only
// flutter_plugin's own LeTclBridge ever exercised that, since the
// Flutter Terminal widget used to be the primary interactive surface.
// Now that `le_shell` is the *only* user-facing way to run Tcl commands
// interactively (the Dear ImGui prototype's own window has no console
// of its own - see src/gui/le_gui.hpp), it needed everything that
// widget's own hand-rolled Dart implementation provided: real line
// editing/recall history and Tab completion (GNU readline - see
// CMakeLists.txt's own comment for why not libedit), plus the same
// undo/recording/truncation behavior batch scripts and the Flutter
// bridge already got. Batch mode (a script path given) is unchanged in
// observable behavior - still Tcl_EvalFile, still sets up
// $argv0/$argv/$argc the same way Tcl_Main's own convention did
// (shell_test.tcl relies on this for its own $argv), and still exits
// nonzero on a script error.
//
// `show_gui` (see le_tcl_procs.tcl) opens a Dear ImGui window
// (src/gui/le_gui.hpp) sharing this same process's session state - see
// TCL_EXPLORATION.md's Phase 6 section for the earlier exploration of
// this and why it went a different direction for the Flutter plugin's
// own Tcl console first. Getting there means restructuring this
// binary's own thread ownership: a blocking stdin-reading interactive
// loop and a native GUI's own event loop (GLFW/Cocoa's NSApplication in
// particular) can't share one thread - the same conflict
// TCL_EXPLORATION.md already hit and steered around for the Flutter
// plugin (Tcl embedded as a library on its own worker thread there,
// instead of Tcl_Main). Here it's the mirror image: this process's own
// true main thread is reserved for le::gui::run_main_thread_loop()
// (GLFW requires window/context creation only there on macOS), and the
// interactive console runs on a spawned thread instead - injecting the
// LeHandle this main thread already created via set_session_handle
// (le_tcl_shim.hpp) right after `load`-ing le_tcl, so a `show_gui`
// window and this console mutate the exact same state. Both the GUI
// window and readline-based editing are mandatory, unconditional
// dependencies of this binary, not optional build features - `le_shell`
// is the only user-facing way to run Tcl commands or open a design
// window now, so a degraded build without either isn't a shape this
// binary supports; see CMakeLists.txt's own `le_shell` target comment.

#include <tcl.h>

#include "api.hpp"
#include "le_gui.hpp"
#include "generated/le_shell_version.hpp"

#include <readline/history.h>
#include <readline/readline.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    std::string g_module_path;
    std::string g_procs_path;

    LeHandle *g_injected_handle = nullptr;

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

    // Bootstrapping, same job this used to do as Tcl_Main's own
    // Tcl_AppInitProc callback: `load` le_tcl and source
    // le_tcl_procs.tcl, so both the interactive and batch-script paths
    // get the full command surface identically - a batch script never
    // needs its own `load`/`source` preamble. Called directly now
    // (run_shell below), not handed to Tcl_Main.
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

        if (Tcl_EvalFile(interp, g_procs_path.c_str()) != TCL_OK)
        {
            return TCL_ERROR;
        }

        return TCL_OK;
    }

    // Runs one already-assembled command string through le_repl_eval and
    // prints whatever it returns - passed through Tcl_SetVar/a variable
    // reference, not substituted directly into the eval'd string, to
    // avoid re-escaping arbitrary user-typed text (same pattern
    // flutter_plugin/src/le_tcl_bridge.cpp's own worker thread already
    // uses for the identical reason). Tcl_Eval's own return code isn't
    // checked - le_repl_eval already catches the wrapped command's own
    // error internally and always returns TCL_OK itself, the
    // interpreter's own string result holding the (successful or error)
    // text either way, exactly what a real interactive Tcl shell prints.
    void eval_and_print(Tcl_Interp *interp, const std::string &command)
    {
        Tcl_SetVar(interp, "le_pending_command", command.c_str(), TCL_GLOBAL_ONLY);
        Tcl_Eval(interp, "le_repl_eval $le_pending_command");
        const char *result = Tcl_GetStringResult(interp);
        if (result != nullptr && result[0] != '\0')
        {
            std::fputs(result, stdout);
            std::fputc('\n', stdout);
        }
    }

    // rl_attempted_completion_function has no userdata slot to carry the
    // interpreter through, unlike every other callback in this file.
    Tcl_Interp *g_completion_interp = nullptr;

    // Classic readline "generator" idiom (called repeatedly with
    // state=0,1,2,... until it returns nullptr) - state==0 computes and
    // caches the whole candidate list via complete_command once
    // (le_tcl_procs.tcl), the same command Tab completion already went
    // through for the Flutter Terminal widget
    // (frontend/lib/components/terminal.dart's own _completeCommand) -
    // complete_command does its own, richer whole-line analysis (command
    // name/flag/dot-path context, bracket nesting) rather than
    // readline's default "just the last word in isolation" model, so
    // `text` itself (readline's own idea of the word being completed) is
    // unused here; readline still needs it for rl_completion_matches'
    // own bookkeeping, and for its [start,end) replacement span to
    // actually line up with what complete_command's candidates expect,
    // rl_completer_word_break_characters is narrowed to whitespace-only
    // below (run_interactive) to match complete_command's own \S+
    // tokenization exactly.
    char *completion_generator(const char *text, int state)
    {
        static std::vector<std::string> candidates;
        static std::size_t index = 0;
        (void)text;
        if (state == 0)
        {
            candidates.clear();
            index = 0;
            const std::string line(rl_line_buffer, static_cast<std::size_t>(rl_point));
            Tcl_SetVar(g_completion_interp, "le_pending_completion_line", line.c_str(), TCL_GLOBAL_ONLY);
            if (Tcl_Eval(g_completion_interp, "complete_command $le_pending_completion_line") == TCL_OK)
            {
                const char *result = Tcl_GetStringResult(g_completion_interp);
                std::istringstream stream(result != nullptr ? result : "");
                std::string token;
                while (stream >> token)
                {
                    candidates.push_back(token);
                }
            }

            // Readline only ever auto-appends its completion character
            // (a space, by default) for an *unambiguous* match - one
            // candidate, no list shown - the case relevant here.
            // _filename_candidates (le_tcl_procs.tcl) already appends a
            // trailing "/" itself for a directory match, matching real
            // shells' own convention so a caller can keep tabbing
            // deeper without retyping the separator; without this,
            // readline (which has no idea this candidate is a
            // filename - rl_filename_completion_desired is never set,
            // since most completions here aren't file paths at all)
            // still appends its own space after that "/" too, leaving
            // "somedir/ " instead of "somedir/". rl_completion_suppress_append
            // isn't reset by readline itself between completion
            // attempts, so it's set explicitly every time, not just
            // when suppressing.
            rl_completion_suppress_append =
                (candidates.size() == 1 && !candidates[0].empty() && candidates[0].back() == '/') ? 1 : 0;
        }
        if (index >= candidates.size())
        {
            return nullptr;
        }
        return strdup(candidates[index++].c_str());
    }

    char **attempted_completion(const char *text, int start, int end)
    {
        (void)start;
        (void)end;
        rl_attempted_completion_over = 1; // no filename-completion fallback
        return rl_completion_matches(text, completion_generator);
    }

    // Cross-thread bridge into readline's own event loop - rl_event_hook
    // has no userdata slot either, same constraint as
    // rl_attempted_completion_function above, so this reaches
    // g_injected_handle/g_completion_interp the same way completion_generator
    // does. See le_enqueue_tcl_command's own doc comment (api.hpp) for
    // why a GUI component (src/gui/components/ - no Tcl interpreter of
    // its own) needs this at all: some of its own actions (layer/purpose
    // visibility, hierarchy depth - the same subset the Flutter
    // frontend's own LeProvider already routes through a Tcl command
    // instead of a direct FFI call) should leave the same command-
    // history trail a typed command would, but this thread's own
    // readline() call is the only place that can actually evaluate one.
    // Readline calls this periodically (its own ~0.1s select() timeout)
    // while blocked waiting for terminal input - the standard readline
    // idiom for draining another thread's own work queue without a real
    // Tcl event loop of this thread's own. Each drained command goes
    // through the exact same eval_and_print (le_repl_eval) a typed line
    // does - same command_history/undo recording - just not added to
    // readline's own separate up-arrow *editing* history (add_history),
    // since a GUI-originated action isn't something a user would expect
    // to recall by pressing Up at the prompt the way a line they
    // actually typed is.
    int drain_pending_gui_commands()
    {
        for (;;)
        {
            const char *command = le_take_next_pending_tcl_command(g_injected_handle);
            if (command == nullptr)
            {
                break;
            }
            // le_take_next_pending_tcl_command's own return is only
            // valid until the *next* call to it (api.hpp's own doc
            // comment) - this loop's own next iteration is exactly
            // that, so copy out first.
            const std::string command_copy = command;

            // Readline owns the terminal's current line/cursor while
            // this hook runs (the user may be mid-edit) - printing
            // eval_and_print's own output directly here, without this
            // save/clear/restore dance, would visually corrupt whatever
            // they've typed so far. The standard readline idiom for
            // asynchronous output during an active readline() call.
            const int saved_point = rl_point;
            char *saved_line = rl_copy_text(0, rl_end);
            rl_save_prompt();
            rl_replace_line("", 0);
            rl_redisplay();

            eval_and_print(g_completion_interp, command_copy);

            rl_restore_prompt();
            rl_replace_line(saved_line, 0);
            rl_point = saved_point;
            rl_redisplay();
            std::free(saved_line);
        }
        return 0;
    }

    // BUGS_AND_ENHANCEMENTS.md E26 - printed once, only in interactive
    // mode (run_shell's own script-argument branch never calls
    // run_interactive at all - a batch script's stdout shouldn't gain
    // unexpected banner noise). LE_SHELL_VERSION/LE_SHELL_BUILD_DATE come
    // from generated/le_shell_version.hpp, regenerated fresh on every
    // build by cmake/generate_le_shell_version.cmake (CMakeLists.txt's
    // own le_shell target) - see that script's own header comment for
    // why a build-time custom target, not a configure-time
    // configure_file().
    void print_banner()
    {
        std::fputs(
            "\n"
            "┌┐    ┌┬──┐ ┌┐ ┌┐ ┌┬──┐ ┌┐  ┐ ┌─┬┬─┐      ┌┬──┐ ┌┬─┐ ┐ ┌┬──  ┌┐ ┌┬─┐ ┐ ┌┬──┐\n"
            "├┤    ├┼──┤ └┴─┼┤ ├┤  │ ├┤  │   ├┤        ├┼─   ├┤ │ │ ├┤ ┬┐ ├┤ ├┤ │ │ ├┼─  \n"
            "└┴──┘ └┘  ┘ └──┴┘ └┴──┘ └┴──┘   └┘        └┴──┘ └┘ └─┘ └┴─┴┘ └┘ └┘ └─┘ └┴──┘\n"
            "\n",
            stdout);
        std::printf("Version  : %s\n", LE_SHELL_VERSION);
        std::printf("Built on : %s\n", LE_SHELL_BUILD_DATE);
        std::fputs(
            "\n"
            "HINT: Use help [wildcard] for help on the various TCL commands. Use man <command> for details.\n"
            "\n",
            stdout);
    }

    // Replaces Tcl_Main's own hardcoded interactive loop - see this
    // file's own header comment for why. Multi-line commands (an
    // unbalanced brace/quote/bracket) keep reading further lines -
    // Tcl_CommandComplete is the same check Tcl_Main's own loop used
    // internally for this, so a script pasted across several lines (or a
    // deliberately multi-line `if`/`foreach`) still works exactly the
    // same way.
    void run_interactive(Tcl_Interp *interp)
    {
        print_banner();

        g_completion_interp = interp;
        rl_attempted_completion_function = attempted_completion;
        rl_completer_word_break_characters = const_cast<char *>(" \t\n");
        rl_event_hook = drain_pending_gui_commands;

        std::string buffer;
        for (;;)
        {
            // BUGS_AND_ENHANCEMENTS.md E27 - was "% ", Tcl_Main's own
            // default prompt.
            const char *prompt = buffer.empty() ? "le_shell > " : "";
            char *raw = readline(prompt);
            if (raw == nullptr)
            {
                std::fputc('\n', stdout);
                return;
            }
            std::string line = raw;
            std::free(raw);

            if (!buffer.empty())
            {
                buffer += '\n';
            }
            buffer += line;

            if (!Tcl_CommandComplete(buffer.c_str()))
            {
                continue;
            }

            // A blank line (or one that only ever had whitespace across
            // however many continuation lines it took) is a well-formed
            // empty command - evaluating it is a harmless no-op, but
            // routing it through le_repl_eval would still record a
            // pointless empty entry into command_history
            // (BUGS_AND_ENHANCEMENTS.md E5's own recall log), unlike a
            // real interactive tclsh, which does nothing at all for one.
            const bool blank = buffer.find_first_not_of(" \t\n\r") == std::string::npos;
            if (!blank)
            {
                add_history(buffer.c_str());
                eval_and_print(interp, buffer);
            }
            buffer.clear();
        }
    }

    // Replaces Tcl_Main(argc, argv, app_init) - same $argv0/$argv/$argc
    // convention for a batch script's own extra arguments
    // (shell_test.tcl's own `lassign $argv lef_path` relies on this),
    // same nonzero exit on a script error, same "no script argument"
    // dispatch to the interactive loop instead. `args` is
    // [executable_path, script_path?, script_args...] - exactly the argv
    // shape main() already trims -module/-procs out of.
    void run_shell(std::vector<char *> &args)
    {
        Tcl_FindExecutable(args[0]);
        Tcl_Interp *interp = Tcl_CreateInterp();

        if (app_init(interp) != TCL_OK)
        {
            std::fprintf(stderr, "le_shell: initialization failed: %s\n", Tcl_GetStringResult(interp));
            std::exit(1);
        }

        if (args.size() > 1)
        {
            Tcl_SetVar(interp, "argv0", args[1], TCL_GLOBAL_ONLY);
            Tcl_Obj *script_args = Tcl_NewListObj(0, nullptr);
            for (std::size_t i = 2; i < args.size(); ++i)
            {
                Tcl_ListObjAppendElement(interp, script_args, Tcl_NewStringObj(args[i], -1));
            }
            Tcl_SetVar2Ex(interp, "argv", nullptr, script_args, TCL_GLOBAL_ONLY);
            Tcl_SetVar(interp, "argc", std::to_string(args.size() - 2).c_str(), TCL_GLOBAL_ONLY);

            if (Tcl_EvalFile(interp, args[1]) != TCL_OK)
            {
                std::fprintf(stderr, "%s\n", Tcl_GetStringResult(interp));
                std::exit(1);
            }
            std::exit(0);
        }

        Tcl_SetVar(interp, "argv0", args[0], TCL_GLOBAL_ONLY);
        Tcl_SetVar2Ex(interp, "argv", nullptr, Tcl_NewListObj(0, nullptr), TCL_GLOBAL_ONLY);
        Tcl_SetVar(interp, "argc", "0", TCL_GLOBAL_ONLY);

        run_interactive(interp);
        std::exit(0);
    }
}

int main(int argc, char **argv)
{
    const char *module_arg = nullptr;
    const char *procs_arg = nullptr;

    // -module/-procs are this shell's own bootstrap flags, consumed here
    // (only as a fixed leading run, before anything run_shell itself
    // needs to see) rather than passed through - what's left (a script
    // path plus its own arguments, or nothing at all for interactive
    // mode) is exactly the argv shape run_shell expects, matching
    // Tcl_Main's own original convention.
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

    g_injected_handle = le_create();

    // run_shell() "never returns" (calls std::exit() itself once the
    // script (batch mode) or the interactive loop (EOF/`exit`) ends) -
    // runs on its own thread here instead of this process's main one,
    // which le::gui::run_main_thread_loop() below needs for itself (see
    // this file's own header comment). Detached, not joined: the whole
    // process exits from inside this thread's own call, so there's no
    // normal path where joining it would ever return either - `remaining`
    // is captured by value (a cheap copy of a vector of pointers into
    // argv's own persistent strings) so this thread's own copy stays
    // valid regardless of main()'s local variables, even though main()
    // also never returns before process exit here.
    //
    // Joining instead of detaching was tried as a fix for the real
    // glfwInit()-failure race below (see run_main_thread_loop's own
    // comment) and reverted - it broke the normal interactive/GUI path
    // outright (show_gui stopped opening a window at all, confirmed live)
    // for reasons that don't reduce to anything in this file's own code -
    // std::thread's own join/detach state is pure userspace bookkeeping,
    // it has no business affecting GLFW/window-server behavior, but it
    // empirically did. Left detached, matching the original design; the
    // race is fixed on the other side instead (run_main_thread_loop
    // itself never returns now, even on glfwInit failure), which doesn't
    // touch this file at all.
    std::thread tcl_thread([remaining]() mutable
                            { run_shell(remaining); });
    tcl_thread.detach();

    le::gui::run_main_thread_loop(g_injected_handle);
    return 0;
}
