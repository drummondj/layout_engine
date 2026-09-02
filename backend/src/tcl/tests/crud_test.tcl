# Phase 5 regression check (TCL_EXPLORATION.md): exercises the full
# Phase 4 CRUD/search surface through the ergonomic (item 15 -shaped)
# command layer - not just that SWIG can wrap it (smoke_test.tcl already
# covers that for the Phase 0 scalar slice), but that a real Tcl caller
# can create a Terminal/TerminalPort/Obstruction, attach rect/polygon/
# path geometry to a Shape via the coordinate-list typemap (a plain Tcl
# list, not a pre-flattened C array), search with a filter expression,
# update the Abstract boundary, and delete with cascade - end to end,
# against the real backend, not a mock.
#
# argv: <path to le_tcl shared module> <path to le_tcl_procs.tcl>
#       <path to testcell.lef fixture> <path to othercell.lef fixture>
#
# othercell.lef (MACRO OTHERCELL, PIN B) is read in as a second Design
# late in this script purely to prove UPDATES.md item 17's scoping: that
# open_design's current-view selection actually confines
# get_terminals/get_obstructions/get_terminal_ports to the Abstract in
# view, not every Abstract ever read into the session.

if {[llength $argv] != 4} {
    puts stderr "usage: crud_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <testcell.lef> <othercell.lef>"
    exit 2
}
lassign $argv module_path procs_path lef_path other_lef_path

proc check {what expected actual} {
    if {$expected ne $actual} {
        puts stderr "FAIL: $what - expected {$expected}, got {$actual}"
        exit 1
    }
    puts "ok: $what = {$actual}"
}

proc check_true {what condition} {
    if {!$condition} {
        puts stderr "FAIL: $what"
        exit 1
    }
    puts "ok: $what"
}

load $module_path le_tcl
source $procs_path

check "read_lef return code" 0 [read_lef $lef_path]
check "message_count after read_lef" 0 [message_count]

set abstract_id [design_abstract_id 0]
check_true "design_abstract_id is valid" [expr {$abstract_id != $kInvalidId}]

check "open_design returns a friendly design id" design:TESTCELL [open_design TESTCELL]

# create_terminal/create_obstruction (generated) take a friendly-id
# token for -abstract, not the raw packed id above - update_abstract_boundary
# still takes the packed form (out of this round's scope), so both are
# kept around under separate names.
set abstract_token [get_abstracts]
check_true "abstract_token is a friendly id" [expr {$abstract_token ne {}}]

if {[catch {open_design DOES_NOT_EXIST} err]} {
    check "open_design unknown name error message" "open_design: no such design \"DOES_NOT_EXIST\"" $err
} else {
    puts stderr "FAIL: open_design on an unknown name did not raise a Tcl error"
    exit 1
}

# --- Library/Design/Abstract search (UPDATES.md item 19.1) ---

check "get_libraries finds the loaded library" library:testcell [get_libraries]
check "get_libraries with a matching name expression" library:testcell [get_libraries testcell]
check "get_libraries with a non-matching name expression" {} [get_libraries DOES_NOT_EXIST]

check "get_designs -of library:testcell" design:TESTCELL [get_designs -of library:testcell]
check "get_designs default scope uses the current view's Library" design:TESTCELL [get_designs]

check "get_abstracts -of design:TESTCELL" abstract:0 [get_abstracts -of design:TESTCELL]
check "get_abstracts default scope uses the current view" abstract:0 [get_abstracts]

if {[catch {get_abstracts -of library:testcell} err]} {
    check "get_abstracts wrong -of type error message" \
        "get_abstracts: -of only accepts design: tokens (got \"library:testcell\") - only design objects are valid parents for get_abstracts" \
        $err
} else {
    puts stderr "FAIL: get_abstracts -of library:... did not raise a Tcl error"
    exit 1
}

# --- Library/Design/Abstract properties (UPDATES.md item 19.2 - new
# types, exercising get_properties beyond the Terminal-only examples in
# the item's own text) ---

check "get_properties on a library token" testcell \
    [dict get [get_properties [get_libraries]] name]
check "get_properties on a design token" TESTCELL \
    [dict get [get_properties [get_designs]] name]
check "get_properties on an abstract token" 0 \
    [dict get [get_properties [get_abstracts]] is_fixed_mask]

# --- Terminal ---

set in0 [create_terminal -abstract $abstract_token -name IN0 -direction INPUT]
check_true "create_terminal returned a valid friendly id" [expr {$in0 ne {}}]
check "create_terminal friendly id is name-based" terminal:IN0 $in0

set out0 [create_terminal -abstract $abstract_token -name OUT0 -direction OUTPUT]
check_true "second create_terminal returned a valid friendly id" [expr {$out0 ne {}}]

# --- -abstract defaults to current_abstract when omitted (Abstract has
# has_current_access=True - open_design above already selected it) - the
# same defaulting create_<type> gives any has_current_access parent flag
# uniformly (Klass.create_tcl_current_defaults()), not a Terminal-only
# special case. ---

check "current_abstract matches abstract_token" $abstract_token [current_abstract]
set from_current [create_terminal -name FROM_CURRENT -direction INPUT]
check_true "create_terminal without -abstract returned a valid friendly id" [expr {$from_current ne {}}]
check "create_terminal without -abstract lands on current_abstract" $from_current [get_terminals -of $abstract_token FROM_CURRENT]
check "delete_terminal (current-abstract-default fixture) return code" 0 [delete_terminal $from_current]

# --- Terminal-name uniqueness enforcement (UPDATES.md's friendly-id item) ---

set messages_before_duplicate [message_count]
check "create_terminal with a colliding name returns an empty id" {} \
    [create_terminal -abstract $abstract_token -name IN0 -direction INPUT]
check_true "create_terminal name collision pushed an error message" \
    [expr {[message_count] > $messages_before_duplicate}]

set messages_before_rename_collision [message_count]
if {![catch {update_terminal $out0 -name IN0}]} {
    puts stderr "FAIL: update_terminal -name to a colliding name did not raise a Tcl error"
    exit 1
}
puts "ok: update_terminal -name to a colliding name fails"
check_true "update_terminal -name collision pushed an error message" \
    [expr {[message_count] > $messages_before_rename_collision}]
check "update_terminal -name failure left OUT0 untouched" OUT0 [dict get [get_properties $out0] name]

set props [get_properties $in0]
check "get_properties name" IN0 [dict get $props name]
check "get_properties direction" INPUT [dict get $props direction]

# --- get_properties/report_properties shape (UPDATES.md item 19.2) ---
# Verifies all four of the item's own worked examples: single-token/no
# names -> dict (above); single-token/one-name -> scalar; single-token/
# many-names -> flat list; many-tokens/many-names -> list of flat lists.

check "get_properties single token, one name -> scalar" IN0 [get_properties $in0 .name]
check "get_properties single token, many names -> flat list" {IN0 INPUT} [get_properties $in0 {.name .direction}]

set multi_props [get_properties [list $in0 $out0] {.name .direction}]
check "get_properties many tokens, many names -> list of flat lists" \
    [list [list IN0 INPUT] [list OUT0 OUTPUT]] $multi_props

set multi_dicts [get_properties [list $in0 $out0]]
check "get_properties many tokens, no names -> list of dicts count" 2 [llength $multi_dicts]
check "get_properties many tokens, no names -> each entry is a dict" IN0 [dict get [lindex $multi_dicts 0] name]

if {[catch {get_properties {} .name}]} {
    puts "note: get_properties on an empty token list raised an error (acceptable, not exercised further)"
} else {
    check "get_properties on an empty token list returns empty" {} [get_properties {} .name]
}

if {[catch {get_properties bogus_token:1} err]} {
    # Prefix list is generated (property_accessors_for_token, see
    # generated/le_tcl_procs_generated.tcl) and covers every TCL-readable
    # class, not just the 7 with hand-written CRUD.
    check "get_properties unrecognized token error message" \
        "get_properties: unrecognized token \"bogus_token:1\" - expected a friendly id (technology:/property_definition:/layer:/antenna_model:/array_spacing:/two_widths_spacing_entry:/prefer_enclosure_entry:/enclosure_entry:/layer_density_entry:/macro_site_placement:/site:/non_default_rule_layer:/non_default_rule_via:/non_default_rule:/minimum_cut:/min_step:/influence_spacing_entry:/spacing_rule:/via_layer:/via_rule_reference:/via_rule_layer:/via:/via_rule:/shape:/library:/design:/abstract:/macro_density_layer:/foreign:/terminal:/pin_antenna_model:/terminal_port:/obstruction:/schematic:/instance:/layout:/row:/track:/g_cell_grid:/placement:/physical_port_segment:/physical_port:/blockage:/layout_via:/route:/region:)" \
        $err
} else {
    puts stderr "FAIL: get_properties on an unrecognized token did not raise a Tcl error"
    exit 1
}

if {[catch {get_properties $in0 bogus_property}]} {
    puts "ok: get_properties with a missing leading '.' raised an error"
} else {
    puts stderr "FAIL: get_properties with a missing leading '.' did not raise a Tcl error"
    exit 1
}

if {[catch {get_properties $in0 .bogus_property}]} {
    puts "ok: get_properties with an unknown property name raised an error"
} else {
    puts stderr "FAIL: get_properties with an unknown property name did not raise a Tcl error"
    exit 1
}

# report_properties doesn't return anything meaningful to assert against
# (it prints) - just confirm it runs without error, same "prints, doesn't
# assert exact stdout" precedent as show_gui below.
report_properties [list $in0 $out0]

# Renaming changes what the friendly id refers to (it *is* the name) -
# $in0 ("terminal:IN0") goes stale the instant this succeeds; update_terminal
# itself returns the new token (rather than echoing the caller's input),
# exactly so a script doesn't have to re-derive it separately.
set in0 [update_terminal $in0 -name IN0_RENAMED]
check "update_terminal -name returns the new friendly id" terminal:IN0_RENAMED $in0
check "renamed terminal is findable via its new friendly id" terminal:IN0_RENAMED [get_terminals IN0_RENAMED]
check "renamed get_properties name" IN0_RENAMED [dict get [get_properties $in0] name]

set in0 [update_terminal $in0 -name IN0]
check "update_terminal -name restore returns the restored friendly id" terminal:IN0 $in0
check "restored terminal is findable via its restored friendly id" terminal:IN0 [get_terminals IN0]

set in0 [update_terminal $in0 -direction INOUT]
check "update_terminal -direction leaves the (name-based) friendly id unchanged" terminal:IN0 $in0
check "changed get_properties direction" INOUT [dict get [get_properties $in0] direction]
set in0 [update_terminal $in0 -direction INPUT]

if {![catch {update_terminal $in0}]} {
    puts stderr "FAIL: update_terminal with zero flags did not raise a Tcl error"
    exit 1
}
puts "ok: update_terminal with zero flags fails"

set matches [get_terminals IN*]
check "get_terminals name-expression glob match" $in0 $matches

set both [get_terminals -filter {.direction == INPUT || .direction == OUTPUT}]
check "get_terminals -filter or-expression match count" 3 [llength $both]

check "get_terminals name-expression no match" {} [get_terminals DOES_NOT_EXIST]

# A bare positional name-expression is never a parse error (it's just a
# glob pattern, matched literally if it contains no wildcard) - the
# parse-error path only exists behind -filter now.
set messages_before_parse_error [message_count]
check "get_terminals -filter parse-error result" {} [get_terminals -filter {not a filter expression}]
check_true "get_terminals -filter parse error was logged" [expr {[message_count] > $messages_before_parse_error}]

set messages_before_bad_field [message_count]
check "get_terminals -filter with an unknown field returns empty" {} [get_terminals -filter {.bogus_field == 1}]
check_true "get_terminals -filter unknown-field error was logged" [expr {[message_count] > $messages_before_bad_field}]

# --- TerminalPort + Shape (rect/polygon/path via the coordinate typemap) ---

set port [create_terminal_port -terminal $in0]
check_true "create_terminal_port returned a valid friendly id" [expr {$port ne {}}]

# --- get_properties chained/dot-notation paths (dot-notation follow-up to
# item 19.2) ---

check "get_properties chained path through a hop" IN0 [get_properties $port .terminal.name]
check "get_properties chained path, many names -> flat list" {IN0 INPUT} \
    [get_properties $port {.terminal.name .terminal.direction}]
check "get_properties chained list-hop path before any shape exists -> empty" {} \
    [get_properties $port .shapes.layer.name]

if {[catch {get_properties $port .bogus_hop.name}]} {
    puts "ok: get_properties with an unknown hop in a chained path raised an error"
} else {
    puts stderr "FAIL: get_properties with an unknown hop in a chained path did not raise a Tcl error"
    exit 1
}

set shape [create_shape -terminal_port $port -layer layer:M1]
check_true "create_shape (-terminal_port) returned a valid friendly id" [expr {$shape ne {}}]

check "get_properties chained list-hop path after a shape exists" M1 \
    [get_properties $port .shapes.layer.name]

check "shape_layer_name" M1 [shape_layer_name $shape]
check "update_shape -layer_name returns the (unchanged, non-name-based) friendly id" $shape [update_shape $shape -layer layer:M1]

# Shape has multiple parent fields (terminal_port/obstruction) -
# update_shape gets no parent-reassignment flag at all (decision 5 -
# reassigning one alone would violate "exactly one parent set").
if {![catch {update_shape $shape -terminal_port $port}]} {
    puts stderr "FAIL: update_shape -terminal_port did not raise a Tcl error (multi-parent classes get no reparent flag)"
    exit 1
}
puts "ok: update_shape -terminal_port is an unknown flag (no reparenting for multi-parent classes)"

# Geometry is set through update_shape's own -rects/-polygons/-paths
# flags now (add_shape_rect/_polygon/_path are gone - create_shape could
# have set all three at creation time too, see the compound-flag tests
# further down for that path).
check "update_shape geometry return code" $shape \
    [update_shape $shape -rects {{{2 2} {8 8}}} -polygons {{{0 0} {5 0} {5 5} {0 5}}} -paths {{0.5 {{0 0} {10 10} {20 0}}}}]

set rects [shape_rects $shape]
check "shape_rects count" 1 [llength $rects]
check "shape_rects contents" {{2 2} {8 8}} [lindex $rects 0]

set polygons [shape_polygons $shape]
check "shape_polygons count" 1 [llength $polygons]
check "shape_polygons point count" 4 [llength [lindex $polygons 0]]
check "shape_polygons first point" {0 0} [lindex [lindex $polygons 0] 0]

set paths [shape_paths $shape]
check "shape_paths count" 1 [llength $paths]
check "shape_paths width" 0.5 [lindex [lindex $paths 0] 0]
check "shape_paths point count" 3 [llength [lindex [lindex $paths 0] 1]]

check "terminal_port_shapes lists the created shape" $shape [terminal_port_shapes $port]

set port_matches [get_terminal_ports -filter {.terminal.name =~ IN* && .shapes.layer.name == M1}]
check "get_terminal_ports -filter relational + list-hop match" $port $port_matches

check "get_shapes -of \$port finds only the created shape" $shape [get_shapes -of $port]
# The default (no -of) scope also picks up the fixture's own LEF-authored
# PIN A, which has its own Port+Shape (shape:0, created while reading
# testcell.lef) - not just the one this test just created (shape:2). Note
# shape:1 is skipped here: it's the Abstract's own boundary Shape, created
# by post_process() at the end of reading this same MACRO (see
# lef_reader.cpp) - correctly excluded from get_shapes' own default scope,
# which is only terminal_port/obstruction shapes, not abstract/layout ones.
check "get_shapes default scope finds every Shape in the current view" {shape:0 shape:2} [get_shapes]

check "get_properties on a shape token" M1 [get_properties $shape .layer.name]

# --- get_properties on non-pooled object-list fields (rects/polygons/
# paths/...) returns the full list with all coordinates, not a bare
# "<field>_count" - clean, micron-converted, brace-nested coordinates
# ("{{ll_x ll_y} {ur_x ur_y}}" per rect), generated generically for every
# class (codegen/codegen/schema.py's `dbu` field type + Field.wrap_with_*
# methods - see backend/CLAUDE.md's Database codegen section), not a
# Shape-only hand-written override anymore. A Path's own width is listed
# before its own "polygon" field (BUGS_AND_ENHANCEMENTS.md E21 - Path's
# own fields are declared width-then-polygon in schema.py specifically so
# this display order comes out that way), the polygon itself getting its
# own wrapping brace group like any other embedded-klass reference
# (Polygon's points list nested one level inside that). ---

set shape_props [get_properties $shape]
check "get_properties rects is a clean micron-converted list, not a count" \
    {{{2 2} {8 8}}} \
    [dict get $shape_props rects]
check "get_properties polygons is a clean micron-converted point list" \
    {{{0 0} {5 0} {5 5} {0 5}}} \
    [dict get $shape_props polygons]
check "get_properties paths is a clean micron-converted width plus point list" \
    {{0.500 {{0 0} {10 10} {20 0}}}} \
    [dict get $shape_props paths]
check_true "rects_count is no longer a property (removed, confusing once the list itself is available)" \
    [expr {![dict exists $shape_props rects_count]}]
check_true "polygons_count is no longer a property" [expr {![dict exists $shape_props polygons_count]}]
check_true "paths_count is no longer a property" [expr {![dict exists $shape_props paths_count]}]
# A list of a plain scalar (not an embedded Klass) is untouched - still a
# count, since there's no per-element to_property_string() to expand into.
check "rect_masks_count is unaffected (list of a plain int, not an object)" \
    0 [dict get $shape_props rect_masks_count]

# Two rects stay unambiguously grouped (not flattened into one 4-point
# list) - update_shape -rects replaces the whole list, so pass both the
# existing rect and a new one, then remove_shape_rect the new one again
# so the rest of this test still sees exactly the one rect it expects
# below (remove_shape_rect still works by index after create_shape/
# update_shape - it's just the incremental add that's gone).
check "update_shape -rects with two entries return code" $shape \
    [update_shape $shape -rects {{{2 2} {8 8}} {{10 10} {20 20}}}]
set two_rects [dict get [get_properties $shape] rects]
check "get_properties rects keeps each rect as its own list element" 2 [llength $two_rects]
check "get_properties rects first element" {{2 2} {8 8}} [lindex $two_rects 0]
check "get_properties rects second element" {{10 10} {20 20}} [lindex $two_rects 1]
check "remove second shape rect return code" 0 [remove_shape_rect $shape 1]

# --- get_properties single-segment dot-path lookup on rects/polygons/
# paths (get_properties $shape .rects) - a regression check for a real
# dangling-pointer bug: le_X_property_path used to build its LeProperty
# result from a std::optional<PropertyValue>/vector local to the
# function, whose .c_str() pointers went stale the instant the function
# returned. A short value ("IN0") happened to keep "working" by sheer
# luck (still-intact bytes in the just-freed stack slot, small enough for
# std::string's small-string optimization), which is exactly why this
# went unnoticed until a long, heap-allocated value (a formatted rects/
# polygons/paths coordinate list) came back corrupted instead. Fixed by
# routing every le_X_property_path result through a handle-owned cache
# slot (same "valid until the next call" convention every other
# LeProperty-returning accessor here already relies on). ---

check "get_properties single-segment .rects (not just the bare-token dict form)" \
    {{{2 2} {8 8}}} [get_properties $shape .rects]
check "get_properties single-segment .polygons" \
    {{{0 0} {5 0} {5 5} {0 5}}} [get_properties $shape .polygons]
check "get_properties single-segment .paths" \
    {{0.500 {{0 0} {10 10} {20 0}}}} [get_properties $shape .paths]
check "get_properties many single-segment object-list names together" \
    [list {{{2 2} {8 8}}} {{{0 0} {5 0} {5 5} {0 5}}} {{0.500 {{0 0} {10 10} {20 0}}}} M1] \
    [get_properties $shape {.rects .polygons .paths .layer.name}]

check "remove_shape_rect return code" 0 [remove_shape_rect $shape 0]
check "shape_rects after remove" {} [shape_rects $shape]

# --- Obstruction ---

set obstruction [create_obstruction -abstract $abstract_token]
check_true "create_obstruction returned a valid friendly id" [expr {$obstruction ne {}}]

# -rects (and -polygons/-paths) work directly on create_shape too, not
# just update_shape - a rect provided at creation time, not added
# afterward.
set obstruction_shape [create_shape -obstruction $obstruction -layer layer:M1 -rects {{{0 0} {1 1}}}]
check_true "create_shape (-obstruction, with -rects) returned a valid friendly id" [expr {$obstruction_shape ne {}}]
check "create_shape -rects took effect immediately" {{0 0} {1 1}} [lindex [shape_rects $obstruction_shape] 0]
check "obstruction_shapes lists the created shape" $obstruction_shape [obstruction_shapes $obstruction]

set obstruction_matches [get_obstructions -filter {.shapes.layer.name == M1}]
check "get_obstructions -filter list-hop match" $obstruction $obstruction_matches

check "delete_obstruction return code" 0 [delete_obstruction $obstruction]
check "get_obstructions after delete" {} [get_obstructions -filter {.shapes.layer.name == M1}]

# --- update_abstract - compound (embedded-struct) create/update flags
# (-size/-origin/-bbox/-symmetry), exploded into one C slot per scalar
# leaf rather than a coordinate-list typemap (see Klass.cmd_tcl_preamble's
# own docstring, codegen/codegen/schema.py). update_abstract_boundary
# (the one hand-written update_* command) is gone - Abstract.boundary is
# now a real child Shape (create_shape -abstract <token>, same as any
# other Shape owner), not a bare polygon list, so it needs no bespoke
# update command at all. ---

set updated_abstract [update_abstract $abstract_token -size {2.0 3.0} -origin {0.5 0.5} -bbox {{0 0} {10 10}} -symmetry {X R90}]
check "update_abstract returns the (unchanged, non-name-based) friendly id" $abstract_token $updated_abstract
set abstract_props [get_properties $abstract_token]
# Every one of these is an is_optional field - its property value is a
# 0-or-1-element list (empty if unset, one element - itself the
# point/rect/symmetry's own nested representation - if set), same
# convention this codebase already used for every other optional field's
# to_property_string(), not a bare flat tuple.
check "update_abstract -size round-trips through get_properties" {{2 3}} [dict get $abstract_props size]
check "update_abstract -origin round-trips through get_properties" {{0.500 0.500}} [dict get $abstract_props origin]
check "update_abstract -bbox round-trips through get_properties" {{{0 0} {10 10}}} [dict get $abstract_props bbox]
# Symmetry's own property value is r90/x/y raw flags, in that declaration
# order (not the -symmetry flag's own keyword-list input spelling) - X R90
# above sets x and r90, leaves y clear.
check "update_abstract -symmetry round-trips through get_properties (r90/x set, y clear)" {{1 1 0}} [dict get $abstract_props symmetry]

if {[catch {update_abstract $abstract_token -size {1.0}} err]} {
    check "update_abstract -size wrong-arity error message" \
        "update_abstract: -size expects 2 values {x y}, got 1" $err
} else {
    puts stderr "FAIL: update_abstract -size with the wrong arity did not raise a Tcl error"
    exit 1
}

if {[catch {update_abstract $abstract_token -symmetry {Z}} err]} {
    check "update_abstract -symmetry unknown-keyword error message" \
        "update_abstract: -symmetry: unknown keyword \"Z\" - expected R90/X/Y" $err
} else {
    puts stderr "FAIL: update_abstract -symmetry with an unknown keyword did not raise a Tcl error"
    exit 1
}

if {![catch {update_abstract $abstract_token}]} {
    puts stderr "FAIL: update_abstract with zero flags did not raise a Tcl error"
    exit 1
}
puts "ok: update_abstract with zero flags fails"

# --- Terminal reparenting (update_terminal -abstract) - needs a second,
# independent Abstract to reparent onto, so read othercell.lef (MACRO
# OTHERCELL, PIN B) here rather than down in the current-view-scoping
# section below, which already expects it loaded by the time it gets
# there. ---

check "read_lef (othercell.lef) return code" 0 [read_lef $other_lef_path]
set other_abstract_token [get_abstracts -of design:OTHERCELL]
check_true "other_abstract_token is a friendly id" [expr {$other_abstract_token ne {}}]

set reparent_test [create_terminal -abstract $abstract_token -name REPARENT_TEST -direction INPUT]
check_true "create_terminal (reparent fixture) returned a valid friendly id" [expr {$reparent_test ne {}}]

set reparent_test [update_terminal $reparent_test -abstract $other_abstract_token]
check "update_terminal -abstract returns a friendly id still based on the (unchanged) name" terminal:REPARENT_TEST $reparent_test
check "reparented terminal is found under its new Abstract" $reparent_test [get_terminals -of $other_abstract_token REPARENT_TEST]
check "reparented terminal is no longer found under its original Abstract" {} [get_terminals -of $abstract_token REPARENT_TEST]

# Reparent collision: othercell.lef's own PIN B already occupies "B" on
# other_abstract_token - reparenting a same-named Terminal onto it must
# fail, leaving the Terminal under its original Abstract untouched.
set collision_test [create_terminal -abstract $abstract_token -name B -direction INPUT]
check_true "create_terminal (reparent-collision fixture) returned a valid friendly id" [expr {$collision_test ne {}}]
if {![catch {update_terminal $collision_test -abstract $other_abstract_token}]} {
    puts stderr "FAIL: update_terminal -abstract onto a same-named sibling did not raise a Tcl error"
    exit 1
}
puts "ok: update_terminal -abstract onto a colliding Abstract fails"
check "reparent-collision terminal is still under its original Abstract" $collision_test [get_terminals -of $abstract_token B]

# collision_test never moved, so it's still resolvable in the current
# (TESTCELL) view; reparent_test now lives under OTHERCELL, and
# resolve_terminal_id (le_tcl_shim.cpp) is itself current-view-scoped
# (same as get_terminals) - so deleting it by its own friendly id needs
# the view switched to match first.
check "delete_terminal (reparent-collision fixture) return code" 0 [delete_terminal $collision_test]
open_design OTHERCELL
check "delete_terminal (reparent fixture) return code" 0 [delete_terminal $reparent_test]
open_design TESTCELL

# --- Cascade delete: deleting the Terminal must take its TerminalPort
# *and that port's own Shape* with it (a real 2-level cascade), since
# neither is reachable any other way once the Terminal is gone - see
# Klass.delete_api_body()'s own docstring (codegen/codegen/schema.py) for
# the recursive-cascade mechanism this exercises. $port/$shape were
# created earlier in this file (Terminal $in0 -> TerminalPort $port ->
# Shape $shape) - this is a regression check for a real bug in the
# formerly hand-written le_delete_terminal, which only ever cascaded one
# level deep (deleting each TerminalPort but never that port's own
# Shapes, leaving them as permanently-unreachable orphaned pool entries).
# get_properties on a deleted object's own now-stale friendly id degrades
# to an empty dict (0 properties - same "unknown id -> 0 rows" contract
# every property-table accessor already has), which only holds for
# $shape too if the cascade actually reached two levels deep, not just
# one. ---

check "delete_terminal return code" 0 [delete_terminal $in0]
check "get_terminal_ports after cascade delete" {} [get_terminal_ports -filter {.terminal.name == IN0}]
check "get_properties on the cascade-deleted TerminalPort is empty (also deleted)" {} [get_properties $port]
check "get_properties on the cascade-deleted Shape is empty (also deleted - the 2-level cascade bug fix)" {} [get_properties $shape]

check "delete_terminal (out0) return code" 0 [delete_terminal $out0]

# The fixture's own LEF-authored PIN A is untouched by any of the above -
# it's the only Terminal left once both terminals this test created are
# gone.
check "get_terminals after deleting both created terminals" A [dict get [get_properties [get_terminals]] name]

# --- Current-view scoping (UPDATES.md item 17) ---
#
# othercell.lef (OTHERCELL/PIN B) was already read above (the Terminal
# reparenting section needed a second, independent Abstract) - confirm
# switching the current view via open_design actually confines
# get_terminals to the Abstract in view - neither Design's terminals leak
# into the other's results. get_obstructions/get_terminal_ports share
# get_terminals' own current-abstract scoping mechanism (api.cpp), so
# this one check exercises the same code path all three rely on.

check "open_design OTHERCELL returns a friendly design id" design:OTHERCELL [open_design OTHERCELL]

set other_terminal_names {}
foreach id [get_terminals] {
    lappend other_terminal_names [dict get [get_properties $id] name]
}
check "get_terminals default scope in OTHERCELL's view only sees OTHERCELL's own terminal" B $other_terminal_names

check "open_design TESTCELL returns a friendly design id" design:TESTCELL [open_design TESTCELL]

set testcell_terminal_names {}
foreach id [get_terminals] {
    lappend testcell_terminal_names [dict get [get_properties $id] name]
}
check "get_terminals default scope back in TESTCELL's view only sees TESTCELL's own terminal, not OTHERCELL's" A $testcell_terminal_names

# --- Building a Library/Design/Abstract entirely from scratch (no
# read_lef'd Design to open_design into at all - just create_library/
# create_design/create_abstract, then current_abstract $id directly) -
# a regression check for a real user-reported bug: le_terminal_by_name
# (api.cpp), Terminal's own hand-written friendly-id resolver, used to
# read handle->scene.current_abstract() - a separate GUI-rendering
# "current view" only ever moved as a side effect of selecting a Design
# (le_set_current_design_abstract/le_set_current_design_abstract_by_id) - instead of
# handle->current_abstract_id, the same field current_abstract itself
# sets when given an id, and get_terminals' own default scope already
# derives from. open_design happens to touch both together (it selects
# a Design first), which is exactly why this went unnoticed until a
# script had no existing Design to open_design into at all. ---

set scratch_library [create_library -name SCRATCH_LIB]
check_true "create_library (from-scratch flow) returned a valid friendly id" [expr {$scratch_library ne {}}]

set scratch_design [create_design -library $scratch_library -name SCRATCH_DESIGN]
check_true "create_design (from-scratch flow) returned a valid friendly id" [expr {$scratch_design ne {}}]

set scratch_abstract [create_abstract -design $scratch_design]
check_true "create_abstract (from-scratch flow) returned a valid friendly id" [expr {$scratch_abstract ne {}}]

check "current_abstract $scratch_abstract selects it and returns it" $scratch_abstract [current_abstract $scratch_abstract]
check "current_abstract with no argument reflects the from-scratch Abstract" $scratch_abstract [current_abstract]

# create_terminal with no -abstract (defaults to current_abstract - see
# Klass.create_tcl_current_defaults()), then create_terminal_port
# resolving that Terminal by name - the exact two calls the original
# bug report's own script made, in the same order.
set scratch_in [create_terminal -name IN -direction INPUT]
check_true "create_terminal (from-scratch flow) returned a valid friendly id" [expr {$scratch_in ne {}}]

set scratch_port [create_terminal_port -terminal $scratch_in]
check_true "create_terminal_port successfully resolved the just-created Terminal by name" [expr {$scratch_port ne {}}]

set scratch_shape [create_shape -terminal_port $scratch_port -layer layer:M1 -rects {{{0 0} {10 10}}}]
check_true "create_shape on the from-scratch TerminalPort returned a valid friendly id" [expr {$scratch_shape ne {}}]
check "the from-scratch shape's rect round-trips" {{0 0} {10 10}} [lindex [shape_rects $scratch_shape] 0]

# --- Undo/redo (UPDATES.md item 21) - via le_repl_eval, the same bracket
# point a typed console command goes through, exercising the real
# create_terminal/update_shape/delete_terminal command names (not the
# raw C++/Root layer - see editing_test.cpp for that). Reuses the
# from-scratch scratch_abstract/scratch_shape set up just above -
# current_abstract is already $scratch_abstract at this point. ---

check "command_history is empty before any le_repl_eval call" {} [command_history]

set undo_term [le_repl_eval {create_terminal -name UNDOTEST -direction INPUT}]
check_true "le_repl_eval create_terminal returned a valid friendly id" [expr {$undo_term ne {}}]
check "get_terminals sees the new terminal" $undo_term [get_terminals UNDOTEST]
check "command_history records the successful create_terminal command" \
    "0: create_terminal -name UNDOTEST -direction INPUT" [command_history]

check "undo removes the just-created terminal" 1 [undo]
check "get_terminals no longer sees it after undo" {} [get_terminals UNDOTEST]

check "redo re-creates it" 1 [redo]
check_true "get_terminals sees it again after redo" [expr {[get_terminals UNDOTEST] ne {}}]

# update_shape round trip - scratch_shape's layer is M1 (set above). M2
# is created here (not read from either fixture LEF) purely as a second,
# distinct layer to rename onto - outside le_repl_eval's own recording so
# this create isn't itself swept into the same undo/redo transaction as
# the rename below (a Layer created *inside* a recording transaction
# would become its own undo/redo step, and Pool::create() always
# allocates a fresh generation on redo - never reproducing the original
# id - so the rename's own recorded "after" LayerId would go stale the
# moment that Layer-create step is undone and redone alongside it).
create_layer -name M2 -type ROUTING -direction NONE
le_repl_eval "update_shape $scratch_shape -layer layer:M2"
check "update_shape via le_repl_eval renamed the layer" M2 [get_properties $scratch_shape .layer.name]

check "undo restores the shape's original layer name" 1 [undo]
check "layer name is back to M1 after undo" M1 [get_properties $scratch_shape .layer.name]

check "redo re-applies the rename" 1 [redo]
check "layer name is M2 again after redo" M2 [get_properties $scratch_shape .layer.name]

# A deliberately-errored command is still added to command_history
# (BUGS_AND_ENHANCEMENTS.md E5) - unlike every other entry so far, its
# own text is the literal, unresolved command as submitted (there's no
# successful/generated form to record instead).
set count_before [command_history_count]
le_repl_eval {this_command_does_not_exist}
check "command_history gained one entry after a failed command" [expr {$count_before + 1}] [command_history_count]
check "the failed command's own text is recorded verbatim" "this_command_does_not_exist" \
    [command_history_at [expr {[command_history_count] - 1}]]

# complete_command itself is never added to command_history
# (BUGS_AND_ENHANCEMENTS.md E5's other half) - Tab-completion shouldn't
# pollute the recall log with its own lookups.
set count_before_completion [command_history_count]
le_repl_eval {complete_command get_t}
check "command_history is unchanged after a complete_command call" $count_before_completion [command_history_count]

# le_repl_eval truncates a long result for display (BUGS_AND_ENHANCEMENTS.md
# E6) - a script evaluating the same command directly (not through
# le_repl_eval) still gets the full, untruncated value.
set long_result [le_repl_eval {string repeat x 20000}]
check "a long le_repl_eval result is truncated to kMaxResultDisplayLength plus the suffix" \
    10011 [string length $long_result]
check_true "the truncated result ends with the ..truncated suffix" \
    [expr {[string range $long_result end-10 end] eq "..truncated"}]
check "evaluating the same command directly (not via le_repl_eval) is not truncated" \
    20000 [string length [string repeat x 20000]]

# help/man/generate_command_docs are exempt from truncation even via
# le_repl_eval (BUGS_AND_ENHANCEMENTS.md E6) - bounded, deliberately-
# readable reference text, not a risky database query's own return
# value; `help` (no pattern - every registered command) is comfortably
# past kMaxResultDisplayLength with this many commands registered.
set help_via_repl [le_repl_eval {help}]
check "help via le_repl_eval matches the direct (untruncated) result exactly" \
    [help] $help_via_repl
check_true "help via le_repl_eval is long enough that it would have been truncated otherwise" \
    [expr {[string length $help_via_repl] > $kMaxResultDisplayLength}]

# delete_terminal round trip - undo recreates the terminal (at a new id,
# not necessarily $delete_target - see Transaction's own id-cell doc
# comment), which get_terminals (name-scoped, not id-scoped) still finds.
set delete_target [le_repl_eval {create_terminal -name DELETETEST -direction OUTPUT}]
le_repl_eval "delete_terminal $delete_target"
check "delete_terminal via le_repl_eval removed the terminal" {} [get_terminals DELETETEST]

check "undo recreates the deleted terminal" 1 [undo]
check_true "get_terminals sees the recreated terminal" [expr {[get_terminals DELETETEST] ne {}}]

# --- write_lef/write_def (BUGS_AND_ENHANCEMENTS.md E28) - reuses
# scratch_abstract (still the current Abstract from the from-scratch
# section above) and a from-scratch Layout on scratch_design, so this
# needs no extra fixture file. ---

set write_scratch_dir [file join [expr {
    [info exists ::env(TMPDIR)] ? $::env(TMPDIR) : "/tmp"
}] "le_tcl_crud_test_write_scratch"]
file delete -force $write_scratch_dir
file mkdir $write_scratch_dir

set scratch_lef_path [file join $write_scratch_dir "scratch.lef"]
check "write_lef -abstract writes a real, non-empty LEF file" "" [write_lef -abstract $scratch_abstract $scratch_lef_path]
check_true "write_lef -abstract's output file is non-empty" [expr {[file size $scratch_lef_path] > 0}]

check "current_abstract still reflects the from-scratch Abstract" $scratch_abstract [current_abstract]
set scratch_lef_current_path [file join $write_scratch_dir "scratch_current.lef"]
check "write_lef with no -abstract falls back to current_abstract" "" [write_lef $scratch_lef_current_path]
check_true "write_lef's current-Abstract-fallback output file is non-empty" [expr {[file size $scratch_lef_current_path] > 0}]

set scratch_layout [create_layout -design $scratch_design]
check_true "create_layout (from-scratch flow) returned a valid friendly id" [expr {$scratch_layout ne {}}]

set scratch_def_path [file join $write_scratch_dir "scratch.def"]
check "write_def -layout writes a real, non-empty DEF file" "" [write_def -layout $scratch_layout $scratch_def_path]
check_true "write_def -layout's output file is non-empty" [expr {[file size $scratch_def_path] > 0}]

check "current_layout $scratch_layout selects it and returns it" $scratch_layout [current_layout $scratch_layout]
set scratch_def_current_path [file join $write_scratch_dir "scratch_current.def"]
check "write_def with no -layout falls back to current_layout" "" [write_def $scratch_def_current_path]
check_true "write_def's current-Layout-fallback output file is non-empty" [expr {[file size $scratch_def_current_path] > 0}]

# --- write_lef -library/-abstracts (BUGS_AND_ENHANCEMENTS.md E28.b) - a
# second from-scratch Design/Abstract under the same $scratch_library,
# so -library has two real Abstracts to write MACROs for. ---

set scratch_design2 [create_design -library $scratch_library -name SCRATCH_DESIGN2]
check_true "create_design (second design, same library) returned a valid friendly id" [expr {$scratch_design2 ne {}}]
set scratch_abstract2 [create_abstract -design $scratch_design2]
check_true "create_abstract (second design's own Abstract) returned a valid friendly id" [expr {$scratch_abstract2 ne {}}]

set scratch_lef_library_path [file join $write_scratch_dir "scratch_library.lef"]
check "write_lef -library writes a real, non-empty LEF file" "" [write_lef -library $scratch_library $scratch_lef_library_path]
check_true "write_lef -library's output file is non-empty" [expr {[file size $scratch_lef_library_path] > 0}]
set scratch_lef_library_text [read [set f [open $scratch_lef_library_path r]]]
close $f
check_true "write_lef -library's output has both Designs' own MACRO" \
    [expr {[string first "MACRO SCRATCH_DESIGN\n" $scratch_lef_library_text] >= 0 && [string first "MACRO SCRATCH_DESIGN2\n" $scratch_lef_library_text] >= 0}]

set scratch_lef_abstracts_path [file join $write_scratch_dir "scratch_abstracts.lef"]
check "write_lef -abstracts (narrowed to one) writes a real, non-empty LEF file" "" \
    [write_lef -library $scratch_library -abstracts $scratch_abstract2 $scratch_lef_abstracts_path]
set scratch_lef_abstracts_text [read [set f [open $scratch_lef_abstracts_path r]]]
close $f
check_true "write_lef -abstracts wrote only SCRATCH_DESIGN2's own MACRO, not SCRATCH_DESIGN's" \
    [expr {[string first "MACRO SCRATCH_DESIGN2\n" $scratch_lef_abstracts_text] >= 0 && [string first "MACRO SCRATCH_DESIGN\n" $scratch_lef_abstracts_text] < 0}]

if {[catch {write_lef -abstract $scratch_abstract -library $scratch_library [file join $write_scratch_dir "scratch_conflict.lef"]} err]} {
    check_true "write_lef -abstract combined with -library raises a Tcl error" \
        [expr {[string first "write_lef" $err] >= 0}]
} else {
    puts stderr "FAIL: write_lef -abstract combined with -library did not raise a Tcl error"
    exit 1
}

file delete -force $write_scratch_dir

# --- get_selection/select (BUGS_AND_ENHANCEMENTS.md E30) - reuses
# scratch_shape (one rect) and scratch_layout (from the sections above)
# for a real Row/Placement/Region to select too. ---

deselect_all
check "get_selection is empty before anything is selected" {} [get_selection]

select $scratch_shape
check "get_selection after select shape:N sees exactly that shape" $scratch_shape [get_selection]

deselect_all
set scratch_row [create_row -layout $scratch_layout -name SCRATCH_ROW -site_name CORE -orientation N]
check_true "create_row (from-scratch flow) returned a valid friendly id" [expr {$scratch_row ne {}}]
set scratch_region [create_region -layout $scratch_layout -name SCRATCH_REGION]
check_true "create_region (from-scratch flow) returned a valid friendly id" [expr {$scratch_region ne {}}]

select $scratch_row $scratch_region
check "select with multiple tokens adds both" [lsort [list $scratch_row $scratch_region]] [lsort [get_selection]]

select $scratch_shape
check "select is additive - a third select call adds to, not replaces, the selection" \
    [lsort [list $scratch_row $scratch_region $scratch_shape]] [lsort [get_selection]]

deselect_all
if {[catch {select "not_a_real_token"} err]} {
    check_true "select rejects an unrecognized token with a clear error" \
        [expr {[string first "select" $err] >= 0}]
} else {
    puts stderr "FAIL: select accepted an unrecognized token"
    exit 1
}
if {[catch {select "shape:999999"} err]} {
    check_true "select rejects a well-formed but unresolvable token with a clear error" \
        [expr {[string first "select" $err] >= 0}]
} else {
    puts stderr "FAIL: select accepted an unresolvable shape token"
    exit 1
}
check "get_selection is still empty after both rejected select calls" {} [get_selection]

deselect_all

puts "le_tcl CRUD test passed"
