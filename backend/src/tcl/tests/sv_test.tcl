# Regression check for SystemVerilog/Verilog TCL wiring (SYSTEMVERILOG.md,
# see its own linked plan) - loads the built module under tclsh, sources
# le_tcl_procs.tcl, and exercises read_verilog -netlist against the same
# gate_netlist_clean.v fixture src/sv/tests's own C++ tests already use,
# confirming the generated get_designs/get_schematics/get_instances/
# get_ports/get_pins surface round-trips real data end to end through the
# TCL layer, not just the C++ reader/database layers those other tests
# already cover.
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>
#       <path to gate_netlist_clean.v fixture>

if {[llength $argv] != 3} {
    puts stderr "usage: sv_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <gate_netlist_clean.v>"
    exit 2
}
lassign $argv module_path procs_path sv_path

proc check {what expected actual} {
    if {$expected ne $actual} {
        puts stderr "FAIL: $what - expected {$expected}, got {$actual}"
        exit 1
    }
    puts "ok: $what = {$actual}"
}

load $module_path le_tcl
source $procs_path

check "read_verilog -netlist return code" 0 [read_verilog -netlist $sv_path]
check "message_count" 0 [message_count]

set top_tokens [get_designs top]
check "get_designs top count" 1 [llength $top_tokens]
set top_token [lindex $top_tokens 0]
puts "ok: top design token = $top_token"

set schematic_tokens [get_schematics -of $top_token]
check "get_schematics -of top count" 1 [llength $schematic_tokens]
set schematic_token [lindex $schematic_tokens 0]
puts "ok: top schematic token = $schematic_token"

# WIDTH defaults to 2 - two generate-created INV instances plus the two
# hand-written AND2 instances (see gate_netlist_clean.v).
check "instance count" 4 [llength [get_instances -of $schematic_token]]

# clk, in, out.
check "port count" 3 [llength [get_ports -of $schematic_token]]

# Every instance has exactly two pins (INV: A/Y, AND2: A/B/Y - wait, AND2
# has 3; INV has 2 - so pin counts differ per instance. Just confirm the
# total ports-connected-per-instance sum is nonzero and every instance
# has at least one pin, rather than assuming a single per-instance count.
set total_pins 0
foreach instance_token [get_instances -of $schematic_token] {
    set pin_count [llength [get_pins -of $instance_token]]
    if {$pin_count == 0} {
        puts stderr "FAIL: instance $instance_token has no pins"
        exit 1
    }
    incr total_pins $pin_count
}
puts "ok: total pins across all instances = $total_pins"

puts "ok: SystemVerilog TCL wiring round trip"
