# Runs through le_shell with every configured extension loaded (tcl_tests);
# any error fails the test.

proc expect {what actual expected} {
    if {$actual ne $expected} {
        error "$what: expected \"$expected\", got \"$actual\""
    }
}

expect "starting libraries" [hello_library_count] 0
hello_add_greeting world
expect "after a greeting" [hello_library_count] 1
expect "greetings counted" [hello_greetings_added] 1
expect "library name" [get_libraries -filter {.name == hello_world}] library:hello_world

# The greeting was one undo step.
undo
expect "after undo" [hello_library_count] 0

# Help is registered like a core command's.
if {![string match "*hello_<name>*" [man hello_add_greeting]]} {
    error "hello_add_greeting has no help"
}
if {[lsearch -exact [info commands hello_*] hello_add_greeting] < 0} {
    error "hello_add_greeting isn't a command"
}

# The raw command from the TCL_INIT hook.
expect "hello_ext_info" [hello_ext_info] "hello_ext 0.1.0"

if {![catch {hello_add_greeting ""}]} {
    error "an empty greeting should fail"
}
puts "hello_ext Tcl test passed"
