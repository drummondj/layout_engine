read_lef ../test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef
read_def ../test_data/ISPD22__final_benchmarks/AES_1/design_original.def
read_def ../test_data/aes_5x5.def
show_gui
set_hierarchy_depth 2
open_design aes -view layout
set_viewport_size -width 1200 -height 1200
zoom_area {0.5 221.5 3.5 225.5}
dump_png show_gui.png
