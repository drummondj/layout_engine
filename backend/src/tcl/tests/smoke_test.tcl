# Phase 0 SWIG toolchain spike regression check (TCL_EXPLORATION.md's
# "Tcl ergonomics layer" section). Proves the *representative* command
# shape works end to end, not just that SWIG can wrap something: no
# visible handle (a hidden session inside le_tcl_shim.cpp), domain-verb
# command names (read_lef, not le_read_lef), and a real -flag-style
# command (set_viewport_size) parsed in Tcl (le_tcl_procs.tcl) before
# reaching the positional SWIG-wrapped *_cmd form. Still nothing to do
# with CRUD/filter-search - that's Phases 1-4 - this only re-shapes
# calls api.hpp already supports (read_lef, design enumeration, viewport
# size) into the ergonomics the eventual CRUD surface will also use.
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>
#       <path to testcell.lef fixture>

if {[llength $argv] != 3} {
    puts stderr "usage: smoke_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <testcell.lef>"
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
check "message_count" 0 [message_count]
check "design_count" 1 [design_count]
check "design_name 0" "TESTCELL" [design_name 0]

set_viewport_size -width 800 -height 600
check "viewport_width after set_viewport_size" 800 [viewport_width]
check "viewport_height after set_viewport_size" 600 [viewport_height]

if {[catch {set_viewport_size -bogus 1} err]} {
    puts "ok: set_viewport_size rejects an unknown flag ($err)"
} else {
    puts stderr "FAIL: set_viewport_size accepted an unknown flag"
    exit 1
}

puts "le_tcl smoke test passed"
