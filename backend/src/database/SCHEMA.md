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
| PhysicalPort      | Physical top-level port of a Layout                      | DEF PIN              |
| Placement         | Physical placement of an Instance                        | DEF COMPONENT        |
| Route             | Physical routing of a Net                                | DEF NET/SPECIALNET   |
| Abstract          | Footprint of cell defined from LEF                       | LEF MACRO            |
| Terminal          | Physical top-level port of an Abstract                   | LEF PIN              |
| TerminalPort      | Physically separate part of a Terminal                   | LEF PORT             |

Note: an `Instance` also stands in for RTL source the SystemVerilog/Verilog
reader couldn't structurally parse/elaborate (invalid syntax, or a
construct this project doesn't model, such as a procedural block) - see
`Instance.rtl_text` - rather than a separate klass, so a connectivity
trace (`Net` → `Pin` → `Instance`) never needs to special-case it.

## LEF Reading Flow

When a LEF is read, if a schematic does not exist for the same design, then it is created with just the Port definitions.

## SystemVerilog Reading Flow

When a SystemVerilog/Verilog file is read (`read_verilog -netlist|-rtl`),
each distinct module becomes a `Design` with a populated `Schematic` -
reusing an existing same-name `Design` if one was already created by an
earlier LEF/DEF/SystemVerilog read, the same reuse-not-duplicate rule the
LEF Reading Flow above follows. A module referenced by name but not
defined in the files being read (the common case for a real gate-level
netlist referencing standard cells) becomes an `Instance` whose
`reference_design` stays unresolved until a later read (of a defining
SystemVerilog file, or the cell's own LEF) supplies a matching `Design` -
see `link_unresolved_instances` (the `link` TCL command).

`-netlist` fully elaborates the design (accurate parameter/generate-block
resolution, so instance counts/names are correct) but does not tolerate
errors in a module's own structural content (ports/parameters/generate/
instantiation). `-rtl` is syntax-only (no elaboration - a parameter-driven
port width isn't evaluated) and tolerant: a module whose header isn't
ANSI-style or has its own diagnostic becomes a single opaque `Instance`
for the whole body; otherwise each top-level member is either a
diagnostic-clean instantiation (extracted normally) or its own opaque
`Instance` (procedural blocks, assigns, generate constructs, or an
instantiation with its own diagnostic).
