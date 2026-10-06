# hello_ext's commands, sourced by le_shell after le_tcl_procs.tcl.

proc hello_library_count {args} {
    if {[lsearch -exact $args "-help"] >= 0} {
        return "hello_library_count \[-help\] - Returns how many libraries the session has"
    }
    return [hello_library_count_cmd]
}
register_command_help hello_library_count \
    "hello_library_count \[-help\]" \
    "Returns how many libraries the session has. From the hello_ext example extension." \
    {
        {-help {type flag required 0 description {Show this usage message and return immediately}}}
    }

proc hello_add_library {name args} {
    if {$name eq "-help" || [lsearch -exact $args "-help"] >= 0} {
        return "hello_add_library <name> \[-help\] - Adds a library named hello_<name>"
    }
    if {[hello_add_library_cmd $name] != 0} {
        error "hello_add_library: couldn't add hello_$name"
    }
    return ""
}
register_command_help hello_add_library \
    "hello_add_library <name> \[-help\]" \
    "Adds a library named hello_<name>, as one undo step. From the hello_ext example extension." \
    {
        {<name> {type str required 1 description {The new library is named hello_<name>}}}
        {-help {type flag required 0 description {Show this usage message and return immediately}}}
    }

proc hello_libraries_added {args} {
    if {[lsearch -exact $args "-help"] >= 0} {
        return "hello_libraries_added \[-help\] - Returns how many libraries this session added"
    }
    return [hello_libraries_added_cmd]
}
register_command_help hello_libraries_added \
    "hello_libraries_added \[-help\]" \
    "Returns how many libraries hello_add_library has added in this session. From the hello_ext example extension." \
    {
        {-help {type flag required 0 description {Show this usage message and return immediately}}}
    }
