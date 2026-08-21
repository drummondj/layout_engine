# Schema Overview

LEF/DEF and Verilog both use different terminology for various aspects of a database. In our database we use the following:

| Class/Struct name | Description                                              | Equivalent           |
| ----------------- | -------------------------------------------------------- | -------------------- |
| Design            | A design containing Abstract, Schematic and Layout views |                      |
| **Logical View**  |                                                          |                      |
| Schematic         | Logical connectivity from Verilog netlist                | Verilog module       |
| Port              | Logical top-level port of a Schematic                    | Verilog input/output |
| Instance          | Logical instance in a Schematic                          | Verilog instance     |
| Pin               | Logical pin on an Instance                               | Verilog instance pin |
| Net               | Logical connects Pins in a Schematic                     | Verilog wire         |
| **Physical View** |                                                          |                      |
| Layout            | Physical place and route data                            | DEF DESIGN           |
| Placement         | Physical placement of an Instance                        | DEF COMPONENT        |
| Route             | Physical routing of a Net                                | DEF NET/SPECIALNET   |
| Abstract          | Footprint of cell defined from LEF                       | LEF MACRO            |
| Terminal          | Physical top-level port of an Abstract                   | LEF PIN              |
| TerminalPort      | Physically separate part of a Terminal                   | LEF PORT             |

## LEF Reading Flow

When a LEF is read, if a schematic does not exist for the same design, then it is created with just the Port definitions.
