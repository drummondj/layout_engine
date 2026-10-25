# set_placement_status: the selected placements by default, given ones
# otherwise, and a hidden status shown again.
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>
#       <path to testcell.lef>

if {[llength $argv] != 3} {
    puts stderr "usage: placement_status_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <testcell.lef>"
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

check "read_lef return code" 0 [read_lef -library testcell $lef_path]
set library [create_library -name lib]
set design [create_design -library $library -name TOP]
set layout [create_layout -design $design]
current_layout $layout
set cell [lindex [get_designs TESTCELL] 0]
set u1 [create_placement -layout $layout -reference_design $cell -name U1 -placement_status UNPLACED -location {0 0}]
set u2 [create_placement -layout $layout -reference_design $cell -name U2 -placement_status PLACED -location {20 0}]

check "nothing selected is an error" 1 [catch {set_placement_status FIXED}]
check "an unknown status is an error" 1 [catch {set_placement_status STUCK $u1}]

select $u1 $u2
set_placement_status_visible FIXED 0
check "the selected placements by default" 2 [set_placement_status fixed]
check "U1 fixed" FIXED [get_properties $u1 .placement_status]
check "U2 fixed" FIXED [get_properties $u2 .placement_status]
check "a hidden status is shown again" 1 [get_placement_status_visible FIXED]

check "given placements" 1 [set_placement_status PLACED $u2]
check "U2 placed" PLACED [get_properties $u2 .placement_status]
check "U1 left alone" FIXED [get_properties $u1 .placement_status]
check "a list of placements" 2 [set_placement_status PLACED [list $u1 $u2]]

set_mode edit
select $u1
set_placement_status FIXED
check "arm_move refuses a FIXED placement" 1 [catch {arm_move} err]
check "...saying why" 1 [string match "*unlock them first*" $err]
set_placement_status PLACED
check "arm_move accepts it once unlocked" 0 [catch {arm_move}]

puts "PASS"
