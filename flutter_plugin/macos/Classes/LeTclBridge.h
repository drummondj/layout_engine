#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// A single poll() result - see le::TclBridge::PollResult
/// (`../../src/le_tcl_bridge.hpp`) for the field semantics this mirrors
/// exactly.
@interface LeTclPollResult : NSObject
@property(nonatomic, readonly) BOOL running;
@property(nonatomic, readonly) NSString *output;
@property(nonatomic, readonly) BOOL hasResult;
@property(nonatomic, readonly) NSString *result;
@end

/// Thin Objective-C++ wrapper around `le::TclBridge` (`../../src/le_tcl_bridge.hpp`)
/// - the actual embedded-Tcl-interpreter logic (including running each
/// command on its own worker thread so a long-running script doesn't block
/// the platform thread - see BUGS_AND_ENHANCEMENTS.md item E3) is plain
/// C++, shared verbatim with Linux's own `layout_engine_plugin.cc` (see
/// that file's createTclConsole/startTclEval/pollTclEval/disposeTclConsole
/// cases); this class only owns the NSString<->std::string marshaling and
/// the LE_TCL_MODULE_PATH/LE_TCL_PROCS_PATH macOS-specific path injection
/// (see `.mm`). See TCL_EXPLORATION.md's show_gui section for the full
/// design rationale.
@interface LeTclBridge : NSObject

/// Creates a fresh `Tcl_Interp`, loads the SWIG-built `le_tcl` module,
/// points its session at `handleAddress` via `set_session_handle` (see
/// le_tcl_shim.hpp) so every CRUD/search command mutates the exact same
/// database the GUI is already rendering, then sources
/// `le_tcl_procs.tcl` for the `-flag value` ergonomic command layer.
/// Never destroys `handleAddress` - ownership stays with whoever created
/// it (the Dart-owned `LeEditor`), same as `le_tcl_shim.cpp`'s own
/// injected-handle contract.
- (instancetype)initWithHandleAddress:(int64_t)handleAddress NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// Hands `command` off to this bridge's own worker thread and returns
/// immediately - not reentrant while a previous eval is still running
/// (see -poll's own `running` flag).
- (void)startEval:(NSString *)command;

/// Drains whatever `puts` has captured since the last call and reports
/// whether the in-flight eval (if any) has just finished (`hasResult`,
/// true on exactly one poll after it does) - see le::TclBridge::poll's own
/// doc comment.
- (LeTclPollResult *)poll;

@end

NS_ASSUME_NONNULL_END
