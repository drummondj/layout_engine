# Open design and set hierarhcy depth so that leaf cells are visible
read_lef ../test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef
read_def ../test_data/ISPD22__final_benchmarks/AES_1/design_original.def
read_def ../test_data/aes_5x5.def
# set_viewport_size -width 800 -height 800
set_hierarchy_depth 2
open_design aes_5x5 -view layout

# # Make all routing layer invisible apart from metal1, which is used in the leaf cells
# foreach layer {via1 metal2 via2 metal3 via3 metal4 via4 metal5 via5 metal6 via6 metal7 via7 metal8 via8 metal9 via9 metal10 } {
#     set_layer_visible $layer false
# }

# file mkdir screenshots
# dump_png screenshots/aes_5x5_zoom_fit.png

# # Zoom to bottom left instance
# # Leaf cell boundaries are visible
# zoom_area {0 0 230 230}
# dump_png screenshots/aes_5x5_0_0_230_230.png

# # Leaf cell boundaries still visible
# # metal1 obstructions and terminals/text not visibile
# zoom_area {0 0 60 60}
# dump_png screenshots/aes_5x5_0_0_60_60.png

# # Leaf cell boundaries still visible
# # metal1 obstructions and terminals/text not visibile
# zoom_area {0 0 20 20}
# dump_png screenshots/aes_5x5_0_0_20_20.png

# # Boundary disappears, no leaf cell contents at all now
# zoom_area {5 5 10 10}
# dump_png screenshots/aes_5x5_5_5_10_10.png

