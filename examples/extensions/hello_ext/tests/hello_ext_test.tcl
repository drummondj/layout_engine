# Runs through le_shell with every configured extension loaded (tcl_tests);
# any error fails the test.

proc expect {what actual expected} {
    if {$actual ne $expected} {
        error "$what: expected \"$expected\", got \"$actual\""
    }
}

expect "starting libraries" [hello_library_count] 0
hello_add_library world
expect "after adding a library" [hello_library_count] 1
expect "libraries counted" [hello_libraries_added] 1
expect "library name" [get_libraries -filter {.name == hello_world}] library:hello_world

# Adding the library was one undo step.
undo
expect "after undo" [hello_library_count] 0

# Help is registered like a core command's.
if {![string match "*hello_<name>*" [man hello_add_library]]} {
    error "hello_add_library has no help"
}
if {[lsearch -exact [info commands hello_*] hello_add_library] < 0} {
    error "hello_add_library isn't a command"
}

# The raw command from the TCL_INIT hook.
expect "hello_ext_info" [hello_ext_info] "hello_ext 0.1.0"

if {![catch {hello_add_library ""}]} {
    error "an empty library name should fail"
}
puts "hello_ext Tcl test passed"
