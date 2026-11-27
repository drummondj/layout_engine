# read_def -placements_only: an existing layout's placements from a DEF's
# COMPONENTS, as one undoable command.
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>
#       <path to testcell.lef>

if {[llength $argv] != 3} {
    puts stderr "usage: def_placements_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <testcell.lef>"
    exit 2
}
lassign $argv module_path procs_path lef_path

proc check {what expected actual} {
    if {$expected ne $actual} {
        puts stderr "FAIL: $what - expected {$expected}, got {$actual}"
        exit 1
    }
    puts "ok: $what = {$actual}"
}

load $module_path le_tcl
source $procs_path

set fd [file tempfile def_path le_def_placements_[pid].def]
puts $fd "VERSION 5.8 ;
DESIGN TOP ;
UNITS DISTANCE MICRONS 1000 ;
COMPONENTS 2 ;
- U1 TESTCELL + FIXED ( 5000 6000 ) FS ;
- U9 TESTCELL + PLACED ( 0 0 ) N ;
END COMPONENTS
END DESIGN"
close $fd

check "read_lef return code" 0 [read_lef -library testcell $lef_path]
set library [create_library -name lib]
set design [create_design -library $library -name TOP]
set layout [create_layout -design $design]
set cell [lindex [get_designs TESTCELL] 0]
set u1 [create_placement -layout $layout -reference_design $cell -name U1 -placement_status PLACED -location {0 0}]
set u2 [create_placement -layout $layout -reference_design $cell -name U2 -placement_status PLACED -location {1 1}]

check "-library with -placements_only is an error" 1 [catch {read_def -placements_only -library lib $def_path}]
check "an unknown layout fails" 1 [read_def -placements_only -layout layout:nope $def_path]
check "no current layout fails" 1 [read_def -placements_only $def_path]

current_layout $layout
check "read with -layout" 0 [le_repl_eval [list read_def -placements_only -layout $layout $def_path]]
check "U1 moved" {{5 6}} [get_properties $u1 .location]
check "U1 orientation" FS [get_properties $u1 .orientation]
check "U1 status" FIXED [get_properties $u1 .placement_status]
check "U2 unchanged" {{1 1}} [get_properties $u2 .location]
check "U9 not created" {} [get_placements -of $layout U9]

le_repl_eval undo
check "undo restores U1" {{0 0}} [get_properties $u1 .location]
check "undo restores U1 status" PLACED [get_properties $u1 .placement_status]

check "the current layout by default" 0 [read_def -placements_only $def_path]
check "U1 moved again" {{5 6}} [get_properties $u1 .location]

file delete $def_path
puts "all def_placements checks passed"
