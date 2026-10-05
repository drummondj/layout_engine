# Runs through le_shell with every configured extension loaded (tcl_tests).
if {[hello_script_greet world] ne "Hello, world!"} {
    error "hello_script_greet returned \"[hello_script_greet world]\""
}
if {![string match "*Returns a greeting*" [man hello_script_greet]]} {
    error "hello_script_greet has no help"
}
puts "hello_script Tcl test passed"
