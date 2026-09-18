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

set top_tokens [get_designs top]
check "get_designs top count" 1 [llength $top_tokens]
set top_token [lindex $top_tokens 0]
puts "ok: top design token = $top_token"

set schematic_tokens [get_schematics -of $top_token]
check "get_schematics -of top count" 1 [llength $schematic_tokens]
set schematic_token [lindex $schematic_tokens 0]
puts "ok: top schematic token = $schematic_token"

# Instance's friendly id is now name-based (unique_per_parent, scoped to
# its own Schematic - see schema.py's own Instance.name comment), so
# resolving one below (get_pins -of $instance_token) needs the same
# current_schematic selection Port/Net-by-name resolution already
# requires - see le_tcl_shim.hpp's own "IDs" comment.
current_schematic $schematic_token

# WIDTH defaults to 2 - two generate-created INV instances plus the two
# hand-written AND2 instances (see gate_netlist_clean.v).
check "instance count" 4 [llength [get_instances -of $schematic_token]]

# clk (scalar) + in[1:0]/out[1:0] (2-bit buses, WIDTH defaults to 2) - a
# multi-bit port has no Port object of its own, replaced by one Port per
# bit (see schema.py's own PortBus comment): clk, in[0], in[1], out[0], out[1].
check "port count" 5 [llength [get_ports -of $schematic_token]]

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

# Hierarchical path syntax (LINKING_STRATEGY_RESEARCH.md sections 3/4) -
# real multi-level nesting/escaping/"**"-fan-out coverage lives in the
# C++ HierarchicalResolver test suite; this just confirms the TCL wiring
# itself (get_instances/get_nets routing through the new
# get_<type>s_by_path_cmd, and SWIG actually exposing it) works end to
# end, not just at the C++ layer.
check "bare ** recursive descent (no nesting here, so same as flat count)" 4 \
    [llength [get_instances -of $schematic_token **]]
check "u_and0 exact bare-name match (still the old flat path, no '/')" {instance:u_and0} \
    [get_instances -of $schematic_token u_and0]
check "u_and0/nonexistent - a leaf instance has no nested Schematic, resolves to nothing" {} \
    [get_instances -of $schematic_token u_and0/nonexistent]
check "** combined with -filter still narrows the result" {instance:u_and0} \
    [get_instances -of $schematic_token ** -filter {.name == u_and0}]

puts "ok: SystemVerilog TCL wiring round trip"
