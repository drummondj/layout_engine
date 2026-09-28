# Regression check for the native database file commands (write_db/read_db/
# db_info - NATIVE_FILE_FORMAT_RESEARCH.md) through the real TCL layer.
# read_db only loads into an empty session, so the load half runs in a
# second tclsh process: this same script, re-run with "load".
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>
#       <path to gate_netlist_clean.v fixture>
#   or: load <le_tcl module> <le_tcl_procs.tcl> <.led file> <expected nets>

proc check {what expected actual} {
    if {$expected ne $actual} {
        puts stderr "FAIL: $what - expected {$expected}, got {$actual}"
        exit 1
    }
    puts "ok: $what = {$actual}"
}

if {[lindex $argv 0] eq "load"} {
    lassign $argv - module_path procs_path db_path expected_nets
    load $module_path le_tcl
    source $procs_path
    check "read_db return code" 0 [read_db $db_path]
    check "net count after read_db" $expected_nets [llength [get_nets -of [get_schematics -of [get_designs top]]]]
    check "second read_db into the now non-empty session fails" 1 [expr {[read_db $db_path] != 0}]
    exit 0
}

if {[llength $argv] != 3} {
    puts stderr "usage: native_db_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <gate_netlist_clean.v>"
    exit 2
}
lassign $argv module_path procs_path sv_path

load $module_path le_tcl
source $procs_path

check "read_verilog return code" 0 [read_verilog -netlist -library sv_lib $sv_path]
set nets [llength [get_nets -of [get_schematics -of [get_designs top]]]]
puts "ok: top has $nets nets"

set tmp_dir [expr {[info exists ::env(TMPDIR)] ? $::env(TMPDIR) : "/tmp"}]
set db_path [file join $tmp_dir "native_db_test_[pid].led"]
check "write_db result" "" [write_db $db_path]
check "db file exists" 1 [file exists $db_path]

set info [db_info $db_path]
check "db_info reports the same schema" 1 [string match "*same schema*" $info]
check "db_info lists Net" 1 [string match "*Net *" $info]
check "db_info on a missing file errors" 1 [catch {db_info $db_path.missing}]
check "write_db with no filename errors" 1 [catch {write_db}]
check "read_db -help" 1 [string match "read_db*" [read_db -help]]

set output [exec [info nameofexecutable] [info script] load $module_path $procs_path $db_path $nets]
puts $output
file delete $db_path
puts "all native db checks passed"
