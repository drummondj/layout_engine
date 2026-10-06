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
# HelloNote, the extension's own database class, gets the generated commands
# and is saved with the design.
create_library -name notes_lib
set note [create_hello_note -library library:notes_lib -body "route the clock first"]
expect "notes" [get_hello_notes -of library:notes_lib] $note
expect "note body" [dict get [get_properties $note] body] "route the clock first"
set path /tmp/hello_ext_test_[pid].led
write_db $path
delete_library library:notes_lib
expect "notes after deleting their library" [llength [get_hello_notes]] 0
read_db $path
file delete $path
expect "notes read back" [llength [get_hello_notes -of library:notes_lib]] 1

puts "hello_ext Tcl test passed"
