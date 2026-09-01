#pragma once

#include "api.hpp"

#include <string>

namespace le::gui
{
    // Queues `command` (a plain Tcl command string, e.g. "set_mode edit")
    // for le_shell.cpp's own console thread to evaluate via le_repl_eval
    // shortly after - see le_enqueue_tcl_command's own doc comment
    // (api.hpp) for why a GUI component uses this instead of calling the
    // matching le_* function directly, for the specific subset of
    // actions the Flutter frontend's own LeProvider already routes
    // through a Tcl command instead of a direct FFI call (see
    // le_provider.dart's own runTclCommand call sites) - purely so the
    // action leaves the same command-history trail a typed command
    // would. Shared by every GUI component that needs this (layer_manager.cpp,
    // mode_selector.cpp, mode_toolbar.cpp), not reimplemented per file.
    inline void enqueue_tcl_command(LeHandle *handle, const std::string &command)
    {
        le_enqueue_tcl_command(handle, command.c_str());
    }
}
