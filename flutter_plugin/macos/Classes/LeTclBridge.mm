#import "LeTclBridge.h"

#include "../../src/le_tcl_bridge.hpp"

#include <memory>

// LE_TCL_MODULE_PATH/LE_TCL_PROCS_PATH are injected as quoted C string
// literals via GCC_PREPROCESSOR_DEFINITIONS in ../layout_engine_plugin.podspec
// (mirroring how that podspec already hardcodes backend/build_release's
// absolute path elsewhere) - dev-machine functionality only, same
// explicitly-accepted scope limit as every other backend path this
// podspec hardcodes (see this plugin's CLAUDE.md "Open design
// questions" / Packaging).
#ifndef LE_TCL_MODULE_PATH
#error "LE_TCL_MODULE_PATH must be set by layout_engine_plugin.podspec"
#endif
#ifndef LE_TCL_PROCS_PATH
#error "LE_TCL_PROCS_PATH must be set by layout_engine_plugin.podspec"
#endif

@implementation LeTclPollResult

- (instancetype)initWithRunning:(BOOL)running
                          output:(NSString *)output
                       hasResult:(BOOL)hasResult
                          result:(NSString *)result {
  self = [super init];
  if (self) {
    _running = running;
    _output = output;
    _hasResult = hasResult;
    _result = result;
  }
  return self;
}

@end

@implementation LeTclBridge {
  std::unique_ptr<le::TclBridge> _bridge;
}

- (instancetype)initWithHandleAddress:(int64_t)handleAddress {
  self = [super init];
  if (self) {
    _bridge = std::make_unique<le::TclBridge>(handleAddress, LE_TCL_MODULE_PATH, LE_TCL_PROCS_PATH);
  }
  return self;
}

- (void)startEval:(NSString *)command {
  _bridge->startEval(command.UTF8String);
}

- (LeTclPollResult *)poll {
  const le::TclBridge::PollResult r = _bridge->poll();
  return [[LeTclPollResult alloc] initWithRunning:r.running
                                            output:[NSString stringWithUTF8String:r.output.c_str()]
                                         hasResult:r.has_result
                                            result:[NSString stringWithUTF8String:r.result.c_str()]];
}

@end
