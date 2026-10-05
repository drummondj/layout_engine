// le_shell: Layout Engine's Tcl shell. With no script argument it runs an
// interactive readline loop; with a script path it evaluates the script
// (Tcl_EvalFile, with $argv0/$argv/$argc set the way Tcl_Main sets them -
// shell_test.tcl relies on this) and exits nonzero on a script error.
//
// Both modes bootstrap identically: `load` the SWIG-wrapped le_tcl module
// (a shared library built by the le_tcl CMake target, not linked into this
// binary) and source le_tcl_procs.tcl, so every CRUD/search command is
// ready without the caller sourcing anything.
//
// The interactive loop is hand-rolled rather than Tcl_Main(), so every
// typed command goes through le_repl_eval (le_tcl_procs.tcl) - the single
// bracket point that makes a command undoable, recorded into the recall
// log, and truncated for display - with GNU readline for line editing,
// history and Tab completion (see CMakeLists.txt for why not libedit).
//
// In interactive mode, `show_gui` (le_tcl_procs.tcl) opens a Dear ImGui
// window (src/gui/le_gui.hpp) sharing this process's session state. A
// batch script can't open one and runs on the main thread. A blocking
// stdin-reading loop and a native GUI's event loop can't share one
// thread, and GLFW requires window/context creation on the true main
// thread on macOS, so the main thread runs le::gui::run_main_thread_loop()
// and the console runs on a spawned thread - injecting the LeHandle the
// main thread created via set_session_handle (le_tcl_shim.hpp) right
// after `load`-ing le_tcl, so the window and the console mutate the exact
// same state. The GUI window and readline are mandatory dependencies of
// this binary, not optional build features; see CMakeLists.txt's own
// `le_shell` target comment.

#include <filesystem>
#include <tcl.h>

#include "api.hpp"
#include "le_gui.hpp"
#include "generated/le_shell_version.hpp"
#include "../core/resource_path.hpp"
#include "le/register_all.hpp"

#include <readline/history.h>
#include <readline/readline.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    std::string g_module_path;
    std::string g_procs_path;

    LeHandle *g_injected_handle = nullptr;

    // Set (from the GUI thread) when the
    // window's close dialog chose "Exit": drain_pending_gui_commands, on
    // this Tcl thread, then exits the way a typed `exit` would, after
    // letting readline restore the terminal.
    std::atomic<bool> g_gui_exit_requested{false};

    // Set by drain_pending_gui_commands when stdin reaches end of input.
    // With rl_event_hook installed, readline() never returns NULL at EOF on
    // its own: it sees stdin readable with nothing to read and calls the
    // hook again, forever.
    bool g_stdin_eof = false;

    // Interactive mode only: the main thread runs the GUI loop and Tcl
    // runs on a spawned thread. A batch script runs on the main thread
    // and can't open a window.
    bool g_gui_loop_running = false;

    // Ends the process. With the GUI loop running, the main thread closes
    // any `show_gui` window and calls std::exit, so static destructors
    // never run under a live window; this thread parks until then. Tcl's
    // own finalization is skipped, so its buffered std channels are
    // flushed here.
    [[noreturn]] void exit_process(int status)
    {
        for (const int type : {TCL_STDOUT, TCL_STDERR})
            if (Tcl_Channel channel = Tcl_GetStdChannel(type))
                Tcl_Flush(channel);
        if (!g_gui_loop_running)
            std::exit(status);
        le::gui::request_exit(status);
        for (;;)
            std::this_thread::sleep_for(std::chrono::hours(1));
    }

    // Tcl_SetExitProc's hook: a script's or a typed `exit` lands here
    // instead of the platform exit().
    [[noreturn]] void tcl_exit_proc(ClientData status)
    {
        exit_process(static_cast<int>(reinterpret_cast<intptr_t>(status)));
    }

    // Before an interactive `exit`/Ctrl-D: with nothing unsaved, true at
    // once; otherwise says what's unsaved and asks. The GUI's own exit
    // doesn't come through here - its close dialog already asked.
    bool confirm_exit_if_unsaved()
    {
        const bool design = le_has_unsaved_database_changes(g_injected_handle) != 0;
        const bool settings = le_has_unsaved_settings(g_injected_handle) != 0;
        if (!design && !settings)
            return true;
        std::printf("Unsaved changes:\n");
        if (design)
            std::printf("  - the design has edits that haven't been written out (write_db / write_def / write_lef)\n");
        if (settings)
            std::printf("  - settings have changed since they were last saved (save_settings)\n");
        char *answer = readline("Exit anyway? [y/N] ");
        const bool yes = answer && (answer[0] == 'y' || answer[0] == 'Y');
        std::free(answer);
        return yes;
    }

    // `le_shell_confirm_exit` - the interactive `exit` wrapper's check (see
    // run_interactive).
    int confirm_exit_cmd(ClientData, Tcl_Interp *interp, int, Tcl_Obj *const[])
    {
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(confirm_exit_if_unsaved()));
        return TCL_OK;
    }

    // -module/-procs beat LE_TCL_MODULE/LE_TCL_PROCS_PATH beat the
    // default: a same-named file beside the executable (find_resource),
    // where an installed bundle (`cmake --install`) puts le_tcl.so and
    // le_tcl_procs.tcl, else the compile-time build-tree path baked in by
    // CMakeLists.txt's le_shell target (LE_TCL_MODULE_DEFAULT_PATH/
    // LE_TCL_PROCS_DEFAULT_PATH), so a plain `./le_shell` works against its
    // own build tree. An explicit override is trusted as-is. Checked up
    // front so a missing file is reported
    // here rather than by Tcl's opaque `load` error.
    std::string resolve_path(const char *cli_value, const char *env_var, const char *default_value, const char *what)
    {
        if (cli_value != nullptr)
        {
            return cli_value;
        }
        if (const char *from_env = std::getenv(env_var))
        {
            return from_env;
        }
        if (default_value == nullptr)
        {
            std::fprintf(stderr, "le_shell: no %s given - pass it as an argument or set %s\n", what, env_var);
            std::exit(2);
        }
        auto found = le::find_resource(default_value, std::filesystem::path(default_value).filename().string());
        if (!found)
        {
            std::fprintf(stderr, "le_shell: %s not found (tried %s) - pass it as an argument or set %s\n", what,
                         le::quoted_paths(found.error()).c_str(), env_var);
            std::exit(2);
        }
        return std::move(*found);
    }

    // Bootstrapping: `load` le_tcl and source le_tcl_procs.tcl, so both
    // the interactive and batch-script paths get the full command surface
    // identically - a batch script never needs its own `load`/`source`
    // preamble. Called directly by run_shell below.
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
    // avoid re-escaping arbitrary user-typed text. Tcl_Eval's own return code isn't
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
    // (le_tcl_procs.tcl) - complete_command does its own, richer
    // whole-line analysis (command
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
    // visibility, hierarchy depth) should leave the same command-
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
        if (g_gui_exit_requested.load(std::memory_order_relaxed))
        {
            rl_deprep_terminal();
            std::fputc('\n', stdout);
            exit_process(0);
        }
        // Readline only calls this once its own input buffer is empty, so
        // stdin readable with no bytes pending is end of input. A stuffed
        // newline accepts whatever partial line was read, as Enter would;
        // run_interactive then sees g_stdin_eof.
        pollfd stdin_poll{STDIN_FILENO, POLLIN, 0};
        int pending = 0;
        if (!g_stdin_eof && poll(&stdin_poll, 1, 0) == 1 && ioctl(STDIN_FILENO, FIONREAD, &pending) == 0 &&
            pending == 0)
        {
            g_stdin_eof = true;
            rl_stuff_char('\n');
            return 0;
        }
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

    // Printed once, only in interactive
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

        // `exit` asks first when
        // something is unsaved (confirm_exit_if_unsaved); the real one
        // stays reachable as ::le_shell_builtin_exit.
        Tcl_CreateObjCommand(interp, "le_shell_confirm_exit", confirm_exit_cmd, nullptr, nullptr);
        Tcl_Eval(interp, "rename exit ::le_shell_builtin_exit\n"
                         "proc exit {{code 0}} {if {[le_shell_confirm_exit]} {::le_shell_builtin_exit $code}}");

        // Ctrl-D at a terminal asks about unsaved changes like `exit`
        // does; end of piped or redirected input has nobody to answer.
        const bool stdin_is_terminal = isatty(STDIN_FILENO) != 0;

        std::string buffer;
        for (;;)
        {
            if (g_stdin_eof)
                return;
            const char *prompt = buffer.empty() ? "le_shell > " : "";
            char *raw = readline(prompt);
            if (raw == nullptr)
            {
                std::fputc('\n', stdout);
                if (stdin_is_terminal && !confirm_exit_if_unsaved())
                    continue;
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
            // pointless empty entry into command_history's recall log,
            // unlike a
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
        Tcl_SetExitProc(tcl_exit_proc);
        Tcl_Interp *interp = Tcl_CreateInterp();

        if (app_init(interp) != TCL_OK)
        {
            std::fprintf(stderr, "le_shell: initialization failed: %s\n", Tcl_GetStringResult(interp));
            exit_process(1);
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
            // show_gui/close_gui refuse to run (le_tcl_procs.tcl).
            Tcl_SetVar(interp, "le_shell_batch", "1", TCL_GLOBAL_ONLY);

            if (Tcl_EvalFile(interp, args[1]) != TCL_OK)
            {
                std::fprintf(stderr, "%s\n", Tcl_GetStringResult(interp));
                exit_process(1);
            }
            exit_process(0);
        }

        Tcl_SetVar(interp, "argv0", args[0], TCL_GLOBAL_ONLY);
        Tcl_SetVar2Ex(interp, "argv", nullptr, Tcl_NewListObj(0, nullptr), TCL_GLOBAL_ONLY);
        Tcl_SetVar(interp, "argc", "0", TCL_GLOBAL_ONLY);

        run_interactive(interp);
        exit_process(0);
    }
}

int main(int argc, char **argv)
{
    // Before anything can use the extensions this binary was built with.
    le::ext::register_all();

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

#ifdef LE_TCL_MODULE_DEFAULT_PATH
    const char *module_default = LE_TCL_MODULE_DEFAULT_PATH;
#else
    const char *module_default = nullptr;
#endif
#ifdef LE_TCL_PROCS_DEFAULT_PATH
    const char *procs_default = LE_TCL_PROCS_DEFAULT_PATH;
#else
    const char *procs_default = nullptr;
#endif
    g_module_path = resolve_path(module_arg, "LE_TCL_MODULE", module_default, "the le_tcl module (-module)");
    g_procs_path = resolve_path(procs_arg, "LE_TCL_PROCS_PATH", procs_default, "le_tcl_procs.tcl (-procs)");

    g_injected_handle = le_create();

    // The user's saved settings - in
    // interactive mode only: a batch script (including every ctest run of
    // le_shell) stays reproducible regardless of what a developer has
    // saved, and can call load_settings itself if it wants them.
    if (remaining.size() == 1)
    {
        std::error_code ec;
        if (const char *settings_path = le_default_settings_path(); settings_path[0] && std::filesystem::exists(settings_path, ec))
            le_load_settings(g_injected_handle, settings_path);
    }

    // A batch script runs right here and never opens a window.
    // run_shell() never returns.
    if (remaining.size() > 1)
        run_shell(remaining);

    // Interactively, run_shell() runs on its own thread because
    // le::gui::run_main_thread_loop() below needs this one (see this
    // file's header comment). It never returns: when the interactive
    // loop (EOF/`exit`) ends, it asks this main thread to end the process
    // (exit_process) and parks. `remaining` is captured by value; its
    // pointers are into argv, which outlives the process.
    //
    // Detached, not joined: joining stops show_gui from opening a window
    // at all (for reasons not understood - std::thread's join/detach state
    // shouldn't affect GLFW, but it does).
    g_gui_loop_running = true;
    std::thread tcl_thread([remaining]() mutable
                            { run_shell(remaining); });
    tcl_thread.detach();

    // The close dialog's "Exit" hands off to the Tcl thread, so readline
    // restores the terminal.
    le::gui::set_exit_handler([]
                              { g_gui_exit_requested.store(true, std::memory_order_relaxed); });
    le::gui::run_main_thread_loop(g_injected_handle);
    return 0;
}
