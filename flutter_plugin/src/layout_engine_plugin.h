#ifndef LEF_EDITOR_PLUGIN_H_
#define LEF_EDITOR_PLUGIN_H_

// This shared library's real public API is backend/src/api/api.hpp - the
// le_*() functions Dart FFI binds to (see ../ffigen.yaml) are implemented
// there and linked into this library from the backend build (see
// CMakeLists.txt on Linux, ../macos/layout_engine_plugin.podspec on macOS),
// not defined in this file. Included here only so this translation unit
// fails to compile if that header ever stops being C-parseable.
#include "../../backend/src/api/api.hpp"

#endif  // LEF_EDITOR_PLUGIN_H_
