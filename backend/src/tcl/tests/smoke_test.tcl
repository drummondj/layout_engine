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

if {[catch {zoom -factor 0.3} err]} {
    puts stderr "FAIL: zoom -factor 0.3 raised an error: $err"
    exit 1
} else {
    puts "ok: zoom -factor 0.3"
}

if {[catch {zoom} err]} {
    puts "ok: zoom rejects a missing -factor ($err)"
} else {
    puts stderr "FAIL: zoom accepted a missing -factor"
    exit 1
}

if {[catch {zoom -bogus 1} err]} {
    puts "ok: zoom rejects an unknown flag ($err)"
} else {
    puts stderr "FAIL: zoom accepted an unknown flag"
    exit 1
}

# --- BUGS_AND_ENHANCEMENTS.md E20: LeProvider-facing TCL commands for
# discrete UI actions previously only reachable via a direct API call
# (layer/purpose selectability, mode, rulers, selection, move) - real
# round-trip checks, not just -help presence (that's help_test.tcl's own
# job), since these are genuinely new wiring, not already-tested
# pass-throughs.

check "layer selectable defaults to visible/selectable" 1 [get_layer_selectable M1]
set_layer_selectable M1 0
check "set_layer_selectable false round-trips" 0 [get_layer_selectable M1]
set_layer_selectable M1 1
check "set_layer_selectable true round-trips" 1 [get_layer_selectable M1]

check "purpose visible defaults to visible" 1 [get_purpose_visible obstruction]
set_purpose_visible obstruction 0
check "set_purpose_visible false round-trips" 0 [get_purpose_visible obstruction]
set_purpose_visible obstruction 1
check "set_purpose_visible true round-trips" 1 [get_purpose_visible obstruction]

check "purpose selectable defaults to selectable" 1 [get_purpose_selectable terminal]
set_purpose_selectable terminal 0
check "set_purpose_selectable false round-trips" 0 [get_purpose_selectable terminal]
set_purpose_selectable terminal 1
check "set_purpose_selectable true round-trips" 1 [get_purpose_selectable terminal]

if {[catch {set_purpose_visible not_a_real_purpose 1} err]} {
    puts "ok: set_purpose_visible rejects an unknown purpose keyword ($err)"
} else {
    puts stderr "FAIL: set_purpose_visible accepted an unknown purpose keyword"
    exit 1
}

check "mode defaults to select" "select" [get_mode]
set_mode edit
check "set_mode edit round-trips" "edit" [get_mode]
set_mode ruler
check "set_mode ruler round-trips" "ruler" [get_mode]
set_mode select
check "set_mode select round-trips" "select" [get_mode]

if {[catch {set_mode not_a_real_mode} err]} {
    puts "ok: set_mode rejects an unknown mode keyword ($err)"
} else {
    puts stderr "FAIL: set_mode accepted an unknown mode keyword"
    exit 1
}

clear_rulers
puts "ok: clear_rulers"
select_all
puts "ok: select_all"
deselect_all
puts "ok: deselect_all"
arm_move
puts "ok: arm_move"

puts "le_tcl smoke test passed"
