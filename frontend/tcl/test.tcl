read_lef ../test_data/asap7_tech_1x_201209.lef

set l [create_library -name my_library]
set d [create_design -library $l -name my_design]
set a [create_abstract -design $d -size {100 100}]
set_current_abstract $a

set in [create_terminal -name IN -direction INPUT]
set out [create_terminal -name OUT -direction OUTPUT]

set in_port [create_terminal_port -terminal $in]
set out_port [create_terminal_port -terminal $out]

create_shape -terminal_port $in_port -rects {{0 40 20 60}} -layer_name M1
create_shape -terminal_port $out_port -rects {{90 40 100 60}} -layer_name M1

set obs [create_obstruction]
foreach layer_name {M2 M3 M4 M5} {
    create_shape -obstruction $obs -rects {{0 0 100 100}} -layer_name $layer_name
}

create_shape -obstruction $obs -paths {{5 {0 0 50 0 50 100 100 100}}} -layer_name M1

