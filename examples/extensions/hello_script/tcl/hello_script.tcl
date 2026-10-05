# hello_script's commands, sourced by le_shell after le_tcl_procs.tcl.

proc hello_script_greet {name args} {
    if {$name eq "-help" || [lsearch -exact $args "-help"] >= 0} {
        return "hello_script_greet <name> \[-help\] - Returns a greeting"
    }
    return "Hello, $name!"
}
register_command_help hello_script_greet \
    "hello_script_greet <name> \[-help\]" \
    "Returns a greeting for <name>. From the hello_script example extension." \
    {
        {<name> {type str required 1 description {Who to greet}}}
        {-help {type flag required 0 description {Show this usage message and return immediately}}}
    }
