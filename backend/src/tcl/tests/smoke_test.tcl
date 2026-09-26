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

check "read_lef return code" 0 [read_lef -library testcell $lef_path]
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

check "placement snap mode defaults to site" "site" [get_placement_snap_mode]
foreach snap_mode {fin manufacturing none site} {
    set_placement_snap_mode $snap_mode
    check "set_placement_snap_mode $snap_mode round-trips" $snap_mode [get_placement_snap_mode]
}
check "no snap is always available" 1 [placement_snap_mode_available none]
check "no fin grid without a technology" 0 [placement_snap_mode_available fin]
if {[catch {set_placement_snap_mode not_a_real_mode} err]} {
    puts "ok: set_placement_snap_mode rejects an unknown mode keyword ($err)"
} else {
    puts stderr "FAIL: set_placement_snap_mode accepted an unknown mode keyword"
    exit 1
}
if {[catch {rotate_placement} err]} {
    puts "ok: rotate_placement errors with no placement selected ($err)"
} else {
    puts stderr "FAIL: rotate_placement succeeded with no placement selected"
    exit 1
}
if {[catch {flip_placement diagonal} err]} {
    puts "ok: flip_placement rejects an unknown direction ($err)"
} else {
    puts stderr "FAIL: flip_placement accepted an unknown direction"
    exit 1
}

check "rect resize snap defaults to user" "user" [get_shape_snap_mode rect]
set_shape_snap_mode path tracks
check "set_shape_snap_mode path tracks round-trips" "tracks" [get_shape_snap_mode path]
set_shape_snap_mode polygon none
check "set_shape_snap_mode polygon none round-trips" "none" [get_shape_snap_mode polygon]
set_shape_snap_mode via tracks
check "set_shape_snap_mode via tracks round-trips" "tracks" [get_shape_snap_mode via]
if {[catch {set_shape_snap_mode via fin} err]} {
    puts "ok: set_shape_snap_mode rejects fin for a via ($err)"
} else {
    puts stderr "FAIL: set_shape_snap_mode accepted fin for a via"
    exit 1
}
if {[catch {set_shape_snap_mode rect tracks} err]} {
    puts "ok: set_shape_snap_mode rejects a mode the kind doesn't offer ($err)"
} else {
    puts stderr "FAIL: set_shape_snap_mode accepted tracks for a rect"
    exit 1
}
if {[catch {set_shape_snap_mode circle user} err]} {
    puts "ok: set_shape_snap_mode rejects an unknown kind ($err)"
} else {
    puts stderr "FAIL: set_shape_snap_mode accepted an unknown kind"
    exit 1
}
check "user-grid snapping is always available" 1 [shape_snap_mode_available rect user]
arm_resize
puts "ok: arm_resize is a harmless no-op outside Edit mode"

check "flightline fanout limit defaults to 10" 10 [get_flightline_max_fanout]
set_flightline_max_fanout 25
check "set_flightline_max_fanout round-trips" 25 [get_flightline_max_fanout]
if {[catch {set_flightline_max_fanout -1} err]} {
    puts "ok: set_flightline_max_fanout rejects a negative value ($err)"
} else {
    puts stderr "FAIL: set_flightline_max_fanout accepted a negative value"
    exit 1
}

# NEW_FEATURES_SEPT_2026.md item 9 - settings commands.
set_ruler_label_size 15
check "set_ruler_label_size round-trips" 15.0 [get_ruler_label_size]
set_label_min_size 10
check "set_label_min_size round-trips" 10.0 [get_label_min_size]
set_label_max_size 20
check "set_label_max_size round-trips" 20.0 [get_label_max_size]
# NEW_FEATURES_SEPT_2026.md item 17 - layer colors.
set_layer_color M1 #1234ab
check "set_layer_color round-trips" "#1234ab" [get_layer_color M1]
reset_layer_color M1
if {[get_layer_color M1] eq "#1234ab"} {
    puts stderr "FAIL: reset_layer_color left M1's picked color"
    exit 1
}
if {[catch {set_layer_color M1 notacolor} err]} {
    puts "ok: set_layer_color rejects a non-#rrggbb color ($err)"
} else {
    puts stderr "FAIL: set_layer_color accepted \"notacolor\""
    exit 1
}
if {[catch {get_layer_color NO_SUCH_LAYER} err]} {
    puts "ok: get_layer_color rejects an unknown layer ($err)"
} else {
    puts stderr "FAIL: get_layer_color accepted an unknown layer"
    exit 1
}
set_grid_spacing -minor 0.25 -major 2.5
check "set_grid_spacing -minor round-trips in microns" 0.25 [get_grid_spacing]
check "set_grid_spacing -major round-trips in microns" 2.5 [get_grid_spacing -major]
if {[catch {set_grid_spacing -minor -1} err]} {
    puts "ok: set_grid_spacing rejects a non-positive spacing ($err)"
} else {
    puts stderr "FAIL: set_grid_spacing accepted a negative spacing"
    exit 1
}
if {[info exists ::env(TMPDIR)]} {
    set settings_file [file join $::env(TMPDIR) le_smoke_settings.json]
} else {
    set settings_file /tmp/le_smoke_settings.json
}
save_settings $settings_file
set_label_max_size 30
load_settings $settings_file
check "load_settings restores what save_settings wrote" 20.0 [get_label_max_size]
file delete $settings_file
if {[catch {load_settings $settings_file} err]} {
    puts "ok: load_settings fails on a missing file ($err)"
} else {
    puts stderr "FAIL: load_settings accepted a missing file"
    exit 1
}

clear_rulers
puts "ok: clear_rulers"
select_all
puts "ok: select_all"
deselect_all
puts "ok: deselect_all"

check "get_selection is empty with nothing selected" {} [get_selection]
if {[catch {select "not_a_real_token"} err]} {
    puts "ok: select rejects an unrecognized token ($err)"
} else {
    puts stderr "FAIL: select accepted an unrecognized token"
    exit 1
}

arm_move
puts "ok: arm_move"

puts "le_tcl smoke test passed"
