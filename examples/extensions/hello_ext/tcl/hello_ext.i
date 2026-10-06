// hello_ext's commands, %include'd into the le_tcl module (TCL_SWIG).
%{
#include "hello_ext/hello_tcl.hpp"
%}

int hello_library_count_cmd();
int hello_add_greeting_library_cmd(const char *name);
int hello_greeting_libraries_added_cmd();
