# Full-chip caravel (sky130A): the top-level caravel DEF placing chip_io
# (the pad ring) and caravel_core (placed and routed, ~400k cells), each
# read as a layout beneath it. Fetch the data with scripts/fetch_caravel.sh.
set dir test_data/caravel

read_lef -library sky130_fd_sc_hd $dir/pdk/sky130_fd_sc_hd__nom.tlef
read_lef -library sky130_fd_sc_hd $dir/pdk/sky130_fd_sc_hd.lef
read_lef -library sky130_fd_sc_hd $dir/pdk/sky130_ef_sc_hd.lef
read_lef -library sky130_fd_io $dir/pdk/sky130_fd_io.lef
read_lef -library sky130_fd_io $dir/pdk/sky130_ef_io.lef
foreach lef [lsort [glob $dir/lef/*.lef]] {
    read_lef -library caravel $lef
}

read_def -library caravel $dir/def/caravel_core.def
read_def -library caravel $dir/def/chip_io.def
read_def -library caravel $dir/def/caravel.def

# Depth 2 reaches the standard cells inside caravel_core.
set_hierarchy_depth 2
open_design caravel -view layout
zoom_area {{0 0} {3588 5188}}
