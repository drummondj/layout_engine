# Regression check for le_shell: runs as a batch script
# under the real compiled le_shell binary (not tclsh directly loading
# le_tcl - smoke_test.tcl/crud_test.tcl already cover that), so this is
# what actually exercises le_shell.cpp's own bootstrap: -module/-procs
# argv handling, `load`, and sourcing le_tcl_procs.tcl, before Tcl_Main
# hands control to this script. If any of that bootstrapping were broken,
# every command below would be an "invalid command name" error instead
# of running - CRUD/search correctness itself is crud_test.tcl's job, not
# this file's.
#
# argv (Tcl_Main's own script-argument convention, not this project's
# usual <module> <procs> <lef> triple - le_shell already consumed
# -module/-procs itself before Tcl_Main ever saw this script):
#   <path to testcell.lef fixture>

if {[llength $argv] != 1} {
    puts stderr "usage: le_shell -module <le_tcl.so> -procs <le_tcl_procs.tcl> shell_test.tcl <testcell.lef>"
    exit 2
}
lassign $argv lef_path

proc check {what expected actual} {
    if {$expected ne $actual} {
        puts stderr "FAIL: $what - expected {$expected}, got {$actual}"
        exit 1
    }
    puts "ok: $what = {$actual}"
}

check "read_lef return code" 0 [read_lef -library testcell $lef_path]
check "design_count" 1 [design_count]

open_design TESTCELL
set abstract_token [get_abstracts]
set terminal [create_terminal -abstract $abstract_token -name SHELL_TEST -direction INPUT]
check "created terminal is searchable" $terminal [get_terminals SHELL_TEST]
check "delete_terminal return code" 0 [delete_terminal $terminal]

# A batch script can't open or close a window.
check "show_gui errors in a batch script" 1 [catch {show_gui} message]
check "show_gui's batch-mode message" 1 [string match "show_gui: not available in a batch script*" $message]
check "close_gui errors in a batch script" 1 [catch {close_gui}]

puts "le_shell batch-mode test passed"
exit 0
