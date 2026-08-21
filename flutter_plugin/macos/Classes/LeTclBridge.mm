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

- (NSString *)evalTcl:(NSString *)command {
  const std::string result = _bridge->evalTcl(command.UTF8String);
  return [NSString stringWithUTF8String:result.c_str()];
}

@end
