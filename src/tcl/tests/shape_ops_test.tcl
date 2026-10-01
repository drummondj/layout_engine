# End-to-end check of the shape_* commands (NEW_FEATURES_SEPT_2026.md
# item 1) through the real TCL layer: token-list parsing, -layer/-parent
# resolution, results landing in Abstract/Layout.free_shapes, and those
# free-standing shapes staying out of write_def's output.
#
# argv: <le_tcl shared module> <le_tcl_procs.tcl> <many_routing_layers.lef>
#       <testcell.lef> <testcell.def>

if {[llength $argv] != 5} {
    puts stderr "usage: shape_ops_test.tcl <le_tcl.so> <le_tcl_procs.tcl> <many_routing_layers.lef> <testcell.lef> <testcell.def>"
    exit 2
}
lassign $argv module_path procs_path layers_lef cell_lef cell_def

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

proc check_error {what script pattern} {
    if {![catch {uplevel 1 $script} err]} {
        puts stderr "FAIL: $what - expected an error, got {$err}"
        exit 1
    }
    if {![string match $pattern $err]} {
        puts stderr "FAIL: $what - error {$err} doesn't match {$pattern}"
        exit 1
    }
    puts "ok: $what -> {$err}"
}

proc layer_of {shape} {
    return [get_properties $shape .layer.name]
}

load $module_path le_tcl
source $procs_path

# M1..M12 first; testcell.lef's own M1 is then a harmless duplicate
# (warned about, first definition kept).
check "read layers" 0 [read_lef -library shape_ops $layers_lef]
check "read cell" 0 [read_lef -library shape_ops $cell_lef]
open_design TESTCELL
set abstract [get_abstracts]

set a [create_shape -in_abstract $abstract -layer layer:M1 -rects {{{0 0} {10 10}}}]
set b [create_shape -in_abstract $abstract -layer layer:M1 -rects {{{5 0} {15 10}}}]
set far [create_shape -in_abstract $abstract -layer layer:M1 -rects {{{50 50} {60 60}}}]

# --- create_shape with no parent flag: a free shape, never the boundary ---
# get_shapes -of an Abstract covers its boundary and its free shapes:
# replacing the boundary would leave the count unchanged.
set shape_count [llength [get_shapes -of $abstract]]
set plain [create_shape -layer layer:M1 -rects {{{0 20} {5 25}}}]
check "create_shape defaults to a new shape of the current abstract" [expr {$shape_count + 1}] [llength [get_shapes -of $abstract]]
check_true "...that get_shapes finds" [expr {[lsearch -exact [get_shapes -of $abstract] $plain] >= 0}]
set dbg_created [create_shape -layer DEBUG -rects {{{0 30} {5 35}}}]
check "create_shape -layer debug sets the DEBUG purpose" DEBUG [get_properties $dbg_created .purpose]
check "...and no layer" {} [layer_of $dbg_created]
check_error "create_shape with a bare layer name" {create_shape -layer M1 -rects {{{0 0} {1 1}}}} {create_shape: unknown layer "M1" - expected a layer:<name> token}
check_error "create_shape with an unknown layer" {create_shape -layer layer:NOPE -rects {{{0 0} {1 1}}}} "create_shape: unknown layer*"
check_error "update_shape with an unknown layer" {update_shape $plain -layer layer:NOPE} "update_shape: unknown layer*"
check "...leaves the layer unchanged" M1 [layer_of $plain]
delete_shape $plain
delete_shape $dbg_created

# --- boolean ---
set or_shape [shape_or $a -with $b]
check "shape_or makes one shape" 1 [llength $or_shape]
check "shape_or merges into one rect" 1 [shape_rect_count $or_shape]
check "shape_or bbox" {{0 0} {15 10}} [shape_bbox $or_shape]
check "shape_or result is a free shape of the current abstract" 1 [expr {[lsearch -exact [get_shapes] $or_shape] >= 0}]
check "shape_or keeps the first input's layer" M1 [layer_of $or_shape]

check "shape_and bbox" {{5 0} {10 10}} [shape_bbox [shape_and $a -with $b]]
check "shape_not bbox" {{0 0} {5 10}} [shape_bbox [shape_not $a -with $b]]
check "an empty shape_and creates nothing" {} [shape_and $a -with $far]
check "shape_or -layer" M2 [layer_of [shape_or $a -with $b -layer layer:M2]]
# A list-valued argument (e.g. [get_selection]) is flattened in.
check "list arguments are flattened" {{0 0} {60 60}} [shape_bbox [shape_or [list $a $b] -with [list $far]]]

# --- copy / change layer ---
set copy [shape_copy $a -layer layer:M2]
check "shape_copy layer" M2 [layer_of $copy]
check "shape_copy leaves the original" M1 [layer_of $a]
check "shape_copy geometry" {{0 0} {10 10}} [shape_bbox $copy]
check "shape_copy one per input" 2 [llength [shape_copy $a $b -layer layer:M3]]

check "shape_change_layer returns its inputs" $copy [shape_change_layer $copy -layer layer:M4]
check "shape_change_layer changes the layer in place" M4 [layer_of $copy]

# --- conversions ---
set l_shape [create_shape -in_abstract $abstract -layer layer:M1 -polygons {{{0 0} {20 0} {20 10} {10 10} {10 20} {0 20}}}]
set h [shape_to_rects $l_shape]
check "shape_to_rects horizontal count" 2 [shape_rect_count $h]
check "shape_to_rects horizontal first strip" {{0 0} {20 10}} [shape_rect_at $h 0]
set v [shape_to_rects $l_shape -direction vertical]
check "shape_to_rects vertical first strip" {{0 0} {10 20}} [shape_rect_at $v 0]
check "shape_to_polygon" 1 [shape_polygon_count [shape_to_polygon $a]]
check "shape_to_polygon has no rects" 0 [shape_rect_count [shape_to_polygon $a]]

# --- size ---
check "shape_size -by" {{-1 -1} {11 11}} [shape_bbox [shape_size $a -by 1]]
check "shape_size -x/-y mixed signs" {{-2 1} {12 9}} [shape_bbox [shape_size $a -x 2 -y -1]]
check "shape_size -x overrides -by" {{-3 -1} {13 11}} [shape_bbox [shape_size $a -by 1 -x 3]]
check "shrinking away creates nothing" {} [shape_size $a -by -6]

# --- path ---
set path_shape [shape_path $a -width 0.5]
check "shape_path makes one path" 1 [shape_path_count $path_shape]
check "shape_path width" 0.5 [shape_path_width_um $path_shape 0]

# --- bbox ---
check "shape_bbox of several shapes" {{0 0} {15 10}} [shape_bbox $a $b]

# --- -parent ---
set obstruction [create_obstruction -abstract $abstract]
set obs_copy [shape_copy $a -layer layer:M1 -parent $obstruction]
check "-parent obstruction" $obs_copy [get_shapes -of $obstruction]

# --- errors ---
check_error "shape_copy needs -layer" {shape_copy $a} "shape_copy: -layer is required"
check_error "unknown -layer" {shape_or $a -with $b -layer layer:NOPE} "shape_or: unknown -layer token"
check_error "unsupported -parent" {shape_copy $a -layer layer:M1 -parent layer:M1} "shape_copy: -parent must name*"
check_error "unknown option" {shape_to_rects $a -bogus 1} "shape_to_rects: unknown option*"
check_error "bad -direction" {shape_to_rects $a -direction diagonal} "shape_to_rects: -direction must be*"
check_error "no shapes" {shape_bbox} "shape_bbox: expected at least one shape token"
check_error "boolean needs -with" {shape_and $a} "shape_and: -with requires*"
check_error "unknown shape" {shape_to_polygon shape:999999} "shape_to_polygon: failed*"
set triangle [create_shape -in_abstract $abstract -layer layer:M1 -polygons {{{0 0} {10 0} {0 10}}}]
check_error "asymmetric size of a diagonal shape" {shape_size $triangle -x 1 -y 2} "shape_size: failed*"
check_true "-help returns usage" [string match "shape_or *" [shape_or -help]]

# --- shape_bbox returns the same {{llx lly} {urx ury}} Rect form -rects and zoom_area take ---
set bbox [shape_bbox $a]
check "shape_bbox result round-trips through -rects" $bbox \
    [shape_bbox [create_shape -in_abstract $abstract -layer layer:M1 -rects [list $bbox]]]
check "zoom_area accepts shape_bbox's result" {} [zoom_area $bbox]
check_error "zoom_area rejects the old flat form" {zoom_area {0 0 10 10}} "zoom_area: rect must be {{llx lly} {urx ury}}*"

# --- undo/redo, through le_repl_eval: the same per-line transaction the
# le_shell console wraps every typed command in ---
set shapes_before [get_shapes]
set shape_count [llength $shapes_before]
le_repl_eval "shape_or $a -with $b"
check "shape_or adds a shape" [expr {$shape_count + 1}] [llength [get_shapes]]
le_repl_eval undo
check "undo removes it" $shapes_before [get_shapes]
le_repl_eval redo
set redone [lmap s [get_shapes] {expr {$s in $shapes_before ? [continue] : $s}}]
check "redo restores one shape" 1 [llength $redone]
check "redo restores its geometry" {{0 0} {15 10}} [shape_bbox $redone]
# One transaction per command, even when a command creates several shapes.
le_repl_eval "shape_copy $a $b -layer layer:M5"
check "shape_copy of two adds two" [expr {$shape_count + 3}] [llength [get_shapes]]
le_repl_eval undo
check "one undo removes both copies" [expr {$shape_count + 1}] [llength [get_shapes]]

# --- the debug layer ---
set dbg [shape_copy $a -layer debug]
check "shape_copy -layer debug sets the DEBUG purpose" DEBUG [get_properties $dbg .purpose]
check "a debug shape keeps its geometry" {{0 0} {10 10}} [shape_bbox $dbg]
check "per-input ops inherit the debug purpose" DEBUG [get_properties [shape_size $dbg -by 1] .purpose]
check "shape_or -layer debug" DEBUG [get_properties [shape_or $a -with $b -layer debug] .purpose]

set mv [shape_copy $a -layer layer:M1]
shape_change_layer $mv -layer DEBUG
check "shape_change_layer -layer debug (any case)" DEBUG [get_properties $mv .purpose]
shape_change_layer $mv -layer layer:M2
check "shape_change_layer back onto a layer" M2 [layer_of $mv]
check "...clears the debug purpose" {} [get_properties $mv .purpose]
le_repl_eval "shape_change_layer $mv -layer debug"
check "shape_change_layer through the console" DEBUG [get_properties $mv .purpose]
le_repl_eval undo
check "undo of a layer change restores the layer" M2 [layer_of $mv]
check "...and clears the purpose again" {} [get_properties $mv .purpose]
check_error "shape_change_layer unknown -layer" {shape_change_layer $a -layer layer:NOPE} "shape_change_layer: unknown -layer token"
check_error "shape_change_layer unknown shape" {shape_change_layer shape:999999 -layer layer:M1} "shape_change_layer: failed*"

# --- free-standing shapes aren't written by write_def ---
check "read def" 0 [read_def -library shape_ops $cell_def]
set layout [get_layouts -of design:TESTCELL]

proc write_def_text {layout} {
    close [file tempfile path shape_ops_test.def]
    write_def $path -layout $layout
    set channel [open $path]
    set text [read $channel]
    close $channel
    file delete $path
    return $text
}
set before [write_def_text $layout]
set layout_shape [shape_copy $a -layer layer:M1 -parent $layout]
# -of a Layout covers both its diearea Shape and its free shapes.
check_true "-parent layout" [expr {[lsearch -exact [get_shapes -of $layout] $layout_shape] >= 0}]
check "write_def output unchanged by a free-standing shape" $before [write_def_text $layout]

# In a Layout view, create_shape with no parent adds to the Layout's
# free_shapes - it used to replace the Layout's single diearea Shape, so
# a second shape made the first disappear.
open_design TESTCELL -view layout
set shape_count [llength [get_shapes -of $layout]]
set d1 [create_shape -layer debug -rects {{{0 0} {1 1}}}]
set d2 [create_shape -layer debug -rects {{{2 2} {3 3}}}]
check "two create_shapes in a layout view add two shapes" [expr {$shape_count + 2}] [llength [get_shapes -of $layout]]
check_true "both are still there" [expr {[lsearch -exact [get_shapes -of $layout] $d1] >= 0 && [lsearch -exact [get_shapes -of $layout] $d2] >= 0}]
set m1 [create_shape -layer layer:M1 -rects {{{4 4} {5 5}}}]
check "create_shape -layer layer:M1 in a layout view" M1 [layer_of $m1]

puts "all shape_ops checks passed"
