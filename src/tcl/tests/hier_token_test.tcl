# Tokens for objects below the current schematic carry their instance
# path from it and resolve back to the same object; a token that names
# nothing makes get_properties/report_properties fail.
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>
#       <path to hier_netlist.v fixture>

if {[llength $argv] != 3} {
    puts stderr "usage: hier_token_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <hier_netlist.v>"
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

proc check_error {what script pattern} {
    if {![catch {uplevel 1 $script} message]} {
        puts stderr "FAIL: $what - expected an error, got {$message}"
        exit 1
    }
    if {![string match $pattern $message]} {
        puts stderr "FAIL: $what - error {$message} doesn't match {$pattern}"
        exit 1
    }
    puts "ok: $what errors"
}

load $module_path le_tcl
source $procs_path

check "read_verilog return code" 0 [read_verilog -netlist -library lib $sv_path]
set top [get_schematics -of [get_designs top]]
set mid [get_schematics -of [get_designs mid]]
current_schematic $top

set nested [get_instances u_mid/u_leaf]
check "nested instance token" instance:u_mid/u_leaf $nested
check "nested .name" u_leaf [get_properties $nested .name]
check "nested .reference_name" leaf [get_properties $nested .reference_name]
check "nested pins" 2 [llength [get_pins -of $nested]]

# mid has one Schematic however often it's placed: its objects take the
# first, shortest path.
check "second placement's path" instance:u_mid/u_leaf [get_instances u_mid2/u_leaf]

check "top-level same-named instance" instance:u_leaf [get_instances u_leaf]
check "top-level .reference_name" leaf [get_properties instance:u_leaf .reference_name]

check "recursive search" \
    [list instance:u_leaf instance:u_mid {instance:u_mid/u\/slash} instance:u_mid/u_leaf instance:u_mid2] \
    [get_instances **]
check "escaped name resolves" u/slash [get_properties {instance:u_mid/u\/slash} .name]

check "nested net" net:u_mid/n_mid [get_nets u_mid/n_mid]
check "nested net .name" n_mid [get_properties net:u_mid/n_mid .name]
check "nested port" port:u_mid/a [get_ports u_mid/a]
check "nested port .name" a [get_properties port:u_mid/a .name]

check "-of a schematic below the current one" \
    [list {instance:u_mid/u\/slash} instance:u_mid/u_leaf] [get_instances -of $mid]

# Tokens are relative to the current schematic.
current_schematic $mid
check "token within the current schematic" instance:u_leaf [get_instances u_leaf]
check "nested token after current_schematic" leaf \
    [get_properties instance:u_leaf .reference_name]

current_schematic $top
check_error "unknown instance" {get_properties instance:nope .name} \
    {get_properties: "instance:nope" names no object*}
check_error "unknown nested instance" {get_properties instance:u_mid/nope .name} \
    {get_properties: "instance:u_mid/nope" names no object*}
check_error "unknown net, all properties" {get_properties net:nope} \
    {get_properties: "net:nope" names no object*}
check_error "report_properties" {report_properties instance:nope} \
    {report_properties: "instance:nope" names no object*}

puts "PASS"
