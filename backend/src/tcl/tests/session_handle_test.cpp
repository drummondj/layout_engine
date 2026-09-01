// Regression check for TCL_EXPLORATION.md's show_gui design: proves
// set_session_handle() actually redirects le_tcl_shim.cpp's session()
// to an externally-owned LeHandle*, not just that the Tcl command runs
// without error. This is what a Flutter-embedded Tcl console (a
// Tcl_Interp created directly, not via Tcl_Main - see
// flutter_plugin/macos/Classes/LeTclBridge.mm) actually relies on: the
// same handle Dart's LeEditor already created via le_create() must be
// the one every Tcl CRUD/search command operates on.
//
// Two levels of proof, not one:
// 1. A handle created and pre-loaded directly via api.hpp (no Tcl
//    involved at all) is visible through Tcl once injected - rules out
//    "Tcl silently fell back to its own fresh self-created handle and
//    happened to still return something plausible."
// 2. A Terminal created *via a Tcl command* is visible through a
//    *direct* api.hpp call on the same raw LeHandle* pointer afterward -
//    the strongest possible proof this is genuinely one shared handle,
//    not two handles that happen to agree on one read.
//
// Each TEST() below runs as its own ctest case (gtest_discover_tests
// invokes this binary once per test, with its own --gtest_filter - see
// CMakeLists.txt), so each gets a fresh process; that matters below.

#include <gtest/gtest.h>

#include "api.hpp"

#include <tcl.h>

#include <cstdint>
#include <string>

TEST(SessionHandle, InjectedHandleIsSharedNotFresh)
{
    LeHandle *handle = le_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(le_read_lef(handle, API_TEST_FIXTURES_DIR "/testcell.lef"), 0);
    ASSERT_EQ(le_design_count(handle), 1);

    Tcl_FindExecutable(nullptr);
    Tcl_Interp *interp = Tcl_CreateInterp();
    ASSERT_NE(interp, nullptr);
    ASSERT_EQ(Tcl_Init(interp), TCL_OK) << Tcl_GetStringResult(interp);

    const std::string load_command = std::string("load {") + LE_TCL_MODULE_PATH + "} le_tcl";
    ASSERT_EQ(Tcl_Eval(interp, load_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);

    const std::string inject_command =
        "set_session_handle " + std::to_string(reinterpret_cast<int64_t>(handle));
    ASSERT_EQ(Tcl_Eval(interp, inject_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);

    // Proof 1: Tcl sees the C++-side pre-loaded content, not an empty
    // freshly self-created handle.
    ASSERT_EQ(Tcl_Eval(interp, "design_count"), TCL_OK) << Tcl_GetStringResult(interp);
    EXPECT_STREQ(Tcl_GetStringResult(interp), "1");

    // This test only `load`s the raw SWIG module - le_tcl_procs.tcl (and
    // its ergonomic open_design/get_abstracts procs) isn't sourced - so
    // the abstract token has to come from the raw generated shim
    // functions directly, scoped by an explicit design: token rather
    // than relying on a "current view".
    ASSERT_EQ(Tcl_Eval(interp, "get_abstracts_cmd design:TESTCELL {}"), TCL_OK) << Tcl_GetStringResult(interp);
    ASSERT_STREQ(Tcl_GetStringResult(interp), "1");
    ASSERT_EQ(Tcl_Eval(interp, "get_abstracts_at 0"), TCL_OK) << Tcl_GetStringResult(interp);
    const std::string abstract_token = Tcl_GetStringResult(interp);

    // create_terminal_cmd is generated (le_tcl_shim_generated.hpp) - one
    // positional argument per Terminal create-field, in schema order
    // (see codegen/codegen/templates/tcl/le_tcl_shim_generated_hpp_j2.py).
    // Only abstract_id/name/direction are exercised here; every other
    // (optional) field is passed as {} (empty), which create_terminal_cmd's
    // own body treats as "omitted" - see Field.create_forward_expr()'s
    // own docstring.
    const std::string create_command =
        "create_terminal_cmd " + abstract_token + " SESSION_HANDLE_TEST INPUT {} {} {} {} {} {} {} {} 0 0 0 0 0 0 0 0";
    ASSERT_EQ(Tcl_Eval(interp, create_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);

    // Proof 2: a Terminal created via Tcl is visible through a direct
    // api.hpp call on the same raw pointer, bypassing Tcl entirely.
    EXPECT_EQ(le_search_terminal(handle, ".name == SESSION_HANDLE_TEST"), 1);

    Tcl_DeleteInterp(interp);
    le_destroy(handle);
}

// Reproduces a real crash reported after embedding this same le_tcl.so
// module inside layout_engine_plugin.framework (see LeTclBridge.mm): both
// this TEST BINARY (which links `api` - and transitively `io`'s
// lef_reader.cpp and the vendored liblef.a - directly, mirroring
// layout_engine_plugin.framework's own static link) and the dynamically
// `load`ed le_tcl.so (which ALSO links `api`/`io`/liblef.a) end up with
// two independently-compiled copies of the vendored LEF parser's C++
// code loaded into the *same* process - two separate Mach-O images. This
// test calls `read_lef` *through Tcl* (le_tcl.so's own copy of the
// parser) *first*, with no prior direct api.hpp call in this process -
// reproducing exactly what happened in the app (the Tcl console's
// read_lef was the first LEF parse in that process). Before the fix
// (le_tcl's -unexported_symbols_list in CMakeLists.txt), le_tcl.so's
// internal call to LefDefParser::lefGetKeyword()'s function-local static
// keyword-table std::map got bound by dyld to this test binary's own
// (separately-linked, differently-initialized) copy instead of its own,
// reading through a garbage pointer - crashing exactly like the app did.
TEST(SessionHandle, ReadLefThroughTclFirstDoesNotCrash)
{
    Tcl_FindExecutable(nullptr);
    Tcl_Interp *interp = Tcl_CreateInterp();
    ASSERT_NE(interp, nullptr);
    ASSERT_EQ(Tcl_Init(interp), TCL_OK) << Tcl_GetStringResult(interp);

    const std::string load_command = std::string("load {") + LE_TCL_MODULE_PATH + "} le_tcl";
    ASSERT_EQ(Tcl_Eval(interp, load_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);

    const std::string read_command = std::string("read_lef {") + API_TEST_FIXTURES_DIR + "/testcell.lef}";
    ASSERT_EQ(Tcl_Eval(interp, read_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);
    EXPECT_STREQ(Tcl_GetStringResult(interp), "0");

    ASSERT_EQ(Tcl_Eval(interp, "design_count"), TCL_OK) << Tcl_GetStringResult(interp);
    EXPECT_STREQ(Tcl_GetStringResult(interp), "1");

    Tcl_DeleteInterp(interp);
}

// Covers the other real, reachable ordering: a LEF already read through
// this process's own *direct* api.hpp copy (matching the app's "Import
// LEF ..." button, which calls le_read_lef straight via Dart FFI into
// layout_engine_plugin.framework's own statically-linked copy), *then* a
// single Tcl_Interp - the same shape LeTclBridge actually uses (one
// Tcl_Interp, created once, reused for the whole console session, never
// recreated) - loads le_tcl.so and reads a LEF through it too. Exists
// separately from ReadLefThroughTclFirstDoesNotCrash because gtest runs
// every TEST() in a file within *one* process when the binary itself is
// invoked directly (not through ctest) - during development, chaining
// this scenario directly after InjectedHandleIsSharedNotFresh's own
// create+delete of a *separate* Tcl_Interp surfaced a second, unrelated
// issue (std::system_error failing to RTTI-match a `catch (const
// std::exception&)` across the dylib boundary - see the
// -unexported_symbols_list comment in CMakeLists.txt) specific to two
// *sequential, independent* Tcl_Interps each loading the same
// already-resident .so. That specific shape doesn't occur here (this
// test creates exactly one Tcl_Interp) and isn't how LeTclBridge or
// le_shell (Tcl_Main - also exactly one Tcl_Interp per process) are
// actually used, so it isn't chased further - each ctest case already
// runs in its own process anyway (see this file's header comment).
// Regression check for a reported get_shapes crash (investigated after
// the fact - not reproduced from scratch even before the fix, see
// le_tcl_unexported_symbols.txt's own comment in CMakeLists.txt for the
// full writeup). le::Root's generated accessors (get_terminal_port_shapes,
// get_obstruction_shapes, get_abstract_terminals, get_terminal_ports, ...)
// are header-only and each return a function-local `static const ... empty;`
// sentinel - the exact bug shape ReadLefThroughTclFirstDoesNotCrash above
// already covers for LefDefParser::lefGetKeyword's keyword table, just in
// a second place. This drives every one of get_shapes' own call-chain
// accessors (via a real Terminal/TerminalPort/Shape and an Obstruction/
// Shape, so both the TerminalPort and Obstruction branches run) through
// Tcl first, matching "get_shapes on an open abstract" as reported.
TEST(SessionHandle, GetShapesThroughTclDoesNotCrash)
{
    LeHandle *handle = le_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(le_read_lef(handle, API_TEST_FIXTURES_DIR "/testcell.lef"), 0);

    Tcl_FindExecutable(nullptr);
    Tcl_Interp *interp = Tcl_CreateInterp();
    ASSERT_NE(interp, nullptr);
    ASSERT_EQ(Tcl_Init(interp), TCL_OK) << Tcl_GetStringResult(interp);

    const std::string load_command = std::string("load {") + LE_TCL_MODULE_PATH + "} le_tcl";
    ASSERT_EQ(Tcl_Eval(interp, load_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);

    const std::string inject_command =
        "set_session_handle " + std::to_string(reinterpret_cast<int64_t>(handle));
    ASSERT_EQ(Tcl_Eval(interp, inject_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);

    ASSERT_EQ(Tcl_Eval(interp, "source " LE_TCL_PROCS_PATH), TCL_OK) << Tcl_GetStringResult(interp);
    ASSERT_EQ(Tcl_Eval(interp, "open_design TESTCELL"), TCL_OK) << Tcl_GetStringResult(interp);

    ASSERT_EQ(Tcl_Eval(interp, "get_terminal_ports"), TCL_OK) << Tcl_GetStringResult(interp);
    ASSERT_EQ(Tcl_Eval(interp, "get_shapes"), TCL_OK) << Tcl_GetStringResult(interp);
    EXPECT_STRNE(Tcl_GetStringResult(interp), "");
    ASSERT_EQ(Tcl_Eval(interp, "get_shapes -filter {.layer_name == M1}"), TCL_OK) << Tcl_GetStringResult(interp);
    ASSERT_EQ(Tcl_Eval(interp, "get_properties shape:0"), TCL_OK) << Tcl_GetStringResult(interp);

    Tcl_DeleteInterp(interp);
    le_destroy(handle);
}

TEST(SessionHandle, DirectReadThenSingleTclInterpReadBothSucceed)
{
    LeHandle *handle = le_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(le_read_lef(handle, API_TEST_FIXTURES_DIR "/testcell.lef"), 0);
    le_destroy(handle);

    Tcl_FindExecutable(nullptr);
    Tcl_Interp *interp = Tcl_CreateInterp();
    ASSERT_NE(interp, nullptr);
    ASSERT_EQ(Tcl_Init(interp), TCL_OK) << Tcl_GetStringResult(interp);

    const std::string load_command = std::string("load {") + LE_TCL_MODULE_PATH + "} le_tcl";
    ASSERT_EQ(Tcl_Eval(interp, load_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);

    const std::string read_command = std::string("read_lef {") + API_TEST_FIXTURES_DIR + "/testcell.lef}";
    ASSERT_EQ(Tcl_Eval(interp, read_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);
    EXPECT_STREQ(Tcl_GetStringResult(interp), "0");

    Tcl_DeleteInterp(interp);
}

// Regression check for src/gui/components/layer_manager.cpp's own
// enqueue() (and the broader le_enqueue_tcl_command/
// le_take_next_pending_tcl_command mechanism, api.hpp) - the whole
// reason a GUI component queues a Tcl command instead of calling the
// matching le_* function directly is so the action leaves a
// command_history entry a user can see, same as a typed line would (see
// le_enqueue_tcl_command's own doc comment). This drives the queue +
// drain + le_repl_eval chain exactly like le_shell.cpp's own
// drain_pending_gui_commands does (minus its own readline terminal-
// display bookkeeping, irrelevant here - readline itself isn't under
// test, just that draining actually applies the command and records it).
TEST(SessionHandle, EnqueuedTclCommandDrainsThroughLeReplEvalAndRecordsHistory)
{
    LeHandle *handle = le_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(le_read_lef(handle, API_TEST_FIXTURES_DIR "/testcell.lef"), 0);

    Tcl_FindExecutable(nullptr);
    Tcl_Interp *interp = Tcl_CreateInterp();
    ASSERT_NE(interp, nullptr);
    ASSERT_EQ(Tcl_Init(interp), TCL_OK) << Tcl_GetStringResult(interp);

    const std::string load_command = std::string("load {") + LE_TCL_MODULE_PATH + "} le_tcl";
    ASSERT_EQ(Tcl_Eval(interp, load_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);

    const std::string inject_command =
        "set_session_handle " + std::to_string(reinterpret_cast<int64_t>(handle));
    ASSERT_EQ(Tcl_Eval(interp, inject_command.c_str()), TCL_OK) << Tcl_GetStringResult(interp);

    ASSERT_EQ(Tcl_Eval(interp, "source " LE_TCL_PROCS_PATH), TCL_OK) << Tcl_GetStringResult(interp);

    EXPECT_EQ(le_take_next_pending_tcl_command(handle), nullptr) << "nothing queued yet";

    // Simulates what a GUI component does (layer_manager.cpp's own
    // enqueue()) - queues the same command text a typed line would use
    // instead of evaluating it directly.
    le_enqueue_tcl_command(handle, "set_layer_visible {M1} 0");

    // Simulates le_shell.cpp's own drain_pending_gui_commands - drains
    // the queue and evaluates through le_repl_eval, same as a typed line.
    const char *command = le_take_next_pending_tcl_command(handle);
    ASSERT_NE(command, nullptr);
    const std::string command_copy = command;
    Tcl_SetVar(interp, "le_pending_command", command_copy.c_str(), TCL_GLOBAL_ONLY);
    ASSERT_EQ(Tcl_Eval(interp, "le_repl_eval $le_pending_command"), TCL_OK) << Tcl_GetStringResult(interp);

    EXPECT_EQ(le_take_next_pending_tcl_command(handle), nullptr) << "queue should be drained";

    // The queued command actually took effect...
    EXPECT_FALSE(le_is_layer_name_visible(handle, "M1"));

    // ...and left a command_history entry, same as if it had been typed
    // directly at the prompt - the entire point of routing this action
    // through Tcl at all instead of a direct le_set_layer_name_visible()
    // call.
    ASSERT_EQ(Tcl_Eval(interp, "command_history_count"), TCL_OK) << Tcl_GetStringResult(interp);
    EXPECT_STREQ(Tcl_GetStringResult(interp), "1");
    ASSERT_EQ(Tcl_Eval(interp, "command_history_at 0"), TCL_OK) << Tcl_GetStringResult(interp);
    EXPECT_STREQ(Tcl_GetStringResult(interp), "set_layer_visible {M1} 0");

    Tcl_DeleteInterp(interp);
    le_destroy(handle);
}
