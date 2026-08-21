#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Thin Objective-C++ wrapper around `le::TclBridge` (`../../src/le_tcl_bridge.hpp`)
/// - the actual embedded-Tcl-interpreter logic is plain C++, shared
/// verbatim with Linux's own `layout_engine_plugin.cc` (see that file's
/// createTclConsole/evalTclCommand/disposeTclConsole cases); this class
/// only owns the NSString<->std::string marshaling and the
/// LE_TCL_MODULE_PATH/LE_TCL_PROCS_PATH macOS-specific path injection (see
/// `.mm`). See TCL_EXPLORATION.md's show_gui section for the full design
/// rationale.
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

/// Evaluates one Tcl command synchronously against this bridge's
/// interpreter, returning the interpreter's string result whether the
/// command succeeded or failed (Tcl already puts the error message in
/// the same place on `TCL_ERROR` - a caller doesn't need a different
/// path for errors, matching what a real interactive shell prints
/// either way). Also includes anything the command wrote via `puts`
/// (stdout or stderr - see LeTclBridge.mm's kCapturePutsBootstrap),
/// prepended ahead of the interpreter's own result - `puts` inside this
/// bridge's interpreter never reaches the app's real stdout/stderr, so a
/// script's own output only ever reaches the caller through here.
- (NSString *)evalTcl:(NSString *)command;

@end

NS_ASSUME_NONNULL_END
