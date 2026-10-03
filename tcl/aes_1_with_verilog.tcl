read_lef -library nangate test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef
read_verilog -netlist -library aes_1 test_data/ISPD22__final_benchmarks/AES_1/design_original.v
read_def -library aes_1 test_data/ISPD22__final_benchmarks/AES_1/design_original.def

link


