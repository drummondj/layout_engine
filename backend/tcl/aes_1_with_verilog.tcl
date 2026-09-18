read_lef ../test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef
read_verilog -netlist ../test_data/ISPD22__final_benchmarks/AES_1/design_original.v
read_def ../test_data/ISPD22__final_benchmarks/AES_1/design_original.def

link

set d [get_designs aes]
current_schematic [get_schematics -of $d]
get_instances


