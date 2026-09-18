# Regression check for the Phase 5 mutation side-effect commands
# (LINKING_STRATEGY_RESEARCH.md section 5: delete_net cascade, update_net/
# update_instance rename propagation) through the real TCL layer - proves
# the le_tcl_procs.tcl overrides + SWIG-exposed shim functions
# (delete_net_cascade_cmd/rename_net_cmd/rename_instance_cmd) are wired
# up correctly end to end, not just the underlying C++ logic
# (src/api/tests/mutation_cascade_test.cpp already covers that directly).
#
# Builds every object by hand via create_<type> (no Verilog/DEF read) -
# a real Technology is still needed first (create_placement/create_route/
# create_physical_port all require one for micron-to-dbu conversion, even
# with no -location given), so this reads the same testcell.lef fixture
# crud_test.tcl already uses, purely to get a Technology in place.
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>
#       <path to testcell.lef>

if {[llength $argv] != 3} {
    puts stderr "usage: mutation_cascade_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <testcell.lef>"
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

check "read_lef return code" 0 [read_lef $lef_path]

set library [create_library -name lib]
set design [create_design -library $library -name TOP]
set schematic [create_schematic -design $design]
current_schematic $schematic

set instance [create_instance -schematic $schematic -reference_design $design -name u1]
set net [create_net -schematic $schematic -name n1]
set pin [create_pin -instance $instance -net $net -name A]
set port [create_port -schematic $schematic -net $net -name n1_port -direction OUTPUT]

set layout [create_layout -design $design]
current_layout $layout

set route [create_route -layout $layout -net $net -name n1]
set physical_port [create_physical_port -layout $layout -net $net -name n1_pin -net_name n1]

# --- delete_net cascade ---

check "delete_net return code" 0 [delete_net $net]
check "Route no longer resolvable by name after delete_net" {} [get_routes -of $layout n1]
check "PhysicalPort still exists after delete_net (net cleared, not deleted)" $physical_port \
    [get_physical_ports -of $layout n1_pin]
check "PhysicalPort net cleared" {} [get_properties $physical_port .net.name]
check "Pin net cleared" {} [get_properties $pin .net.name]
check "Port net cleared" {} [get_properties $port .net.name]

# --- update_net rename propagation ---

set net2 [create_net -schematic $schematic -name n2]
create_route -layout $layout -net $net2 -name n2
create_physical_port -layout $layout -net $net2 -name n2_pin -net_name n2

# Net/Route/PhysicalPort friendly ids are all name-based
# (unique_per_parent), so the tokens captured above go stale the moment
# their own .name changes - re-resolve everything by the *new* name
# afterward instead of reusing pre-rename tokens.
set net2 [update_net $net2 -name n2_renamed]
check "Net renamed" {n2_renamed} [get_properties $net2 .name]
check "Linked Route renamed" {route:n2_renamed} [get_routes -of $layout n2_renamed]
check "Linked PhysicalPort renamed" {physical_port:n2_renamed} [get_physical_ports -of $layout n2_renamed]

# --- update_instance rename propagation (nested case) ---

set leaf_design [create_design -library $library -name LEAF]
set a_design [create_design -library $library -name A]
set a_schematic [create_schematic -design $a_design]
current_schematic $a_schematic
set inst_b [create_instance -schematic $a_schematic -reference_design $leaf_design -name b]

current_schematic $schematic
set inst_a [create_instance -schematic $schematic -reference_design $a_design -name a]

set placement_a [create_placement -layout $layout -reference_design $a_design -instance $inst_a -name a -placement_status PLACED]

# -instance resolves by name within *current_schematic* (Instance's own
# scoping convention, unrelated to current_layout) - inst_b lives under
# a_schematic, not the TOP schematic current_schematic is left on above.
current_schematic $a_schematic
set placement_ab [create_placement -layout $layout -reference_design $leaf_design -instance $inst_b -name a/b -placement_status PLACED]
current_schematic $schematic

# Instance/Placement friendly ids are also name-based - re-resolve by
# the *new* name afterward, same reasoning as the Net rename above.
set inst_a [update_instance $inst_a -name a_renamed]
check "Instance renamed" {a_renamed} [get_properties $inst_a .name]
check "Own Placement renamed" {placement:a_renamed} [get_placements -of $layout a_renamed]
check "Descendant Placement renamed with new prefix" {placement:a_renamed/b} \
    [get_placements -of $layout a_renamed/b]

# A plain (non-rename) update_instance call still works unchanged.
set inst_a [update_instance $inst_a -reference_name "A"]
check "Non-rename update_instance still works" {A} [get_properties $inst_a .reference_name]

puts "ok: mutation cascade TCL wiring round trip"
