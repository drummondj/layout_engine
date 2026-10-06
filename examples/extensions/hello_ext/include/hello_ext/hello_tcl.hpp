#pragma once
// hello_ext's Tcl commands (built into the le_tcl module only), declared to
// SWIG by tcl/hello_ext.i and wrapped as procs by tcl/hello_ext.tcl.

/// @brief The session's library count.
int hello_library_count_cmd();
/// @brief 0 if hello_<name> was added (one undo step), nonzero otherwise.
int hello_add_greeting_library_cmd(const char *name);
/// @brief How many greeting libraries this session has added.
int hello_greeting_libraries_added_cmd();
