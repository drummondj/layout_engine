read_lef ../test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef
read_def ../test_data/ISPD22__final_benchmarks/AES_1/design_original.def
read_def ../test_data/aes_5x5.def
set_hierarchy_depth 2
open_design aes_5x5 -view layout
zoom -factor 0.3
