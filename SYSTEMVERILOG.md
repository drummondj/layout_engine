SystemVerilog integration
=========================

SystemVerilog (or regular Verilog, a subset of the SystemVerilog syntax) can be used to read a design and create a Schematic view.

There are 2 flavors that need supporting:

- Netlist - full support and parsing
- RTL - partial support (see below for methodology)

When reading RTL we only care about connectivity of ports, and pre-existing instances. RTL code does not need to be understood by LayoutEngine, as this is a physical design tool. But, for early design anaylsis, SystemVerilog should still be parsed. The use-case for this is chip-level planning, specifically for IO cell and bump planning.

How to support this (please review this):

1. Read all ports, modules and instances as though they were a netlist
2. Convert RTL code into "LogicCloud" objects. LogicClouds act as Instances and can be connected like Instances, but just contain RTL code as a blog of text, rather than a compete understanding of the function.
3. Possibly allow invalid RTL to be read in and just stored in the LogicCloud as is.

Please choose a systemverilog parser that can handle the requirements above. I have used https://sv-lang.com inthe past, but it does have much more functionally than we require, and requires correct SystemVerilog syntax.

Then start a plan.



