This is a deep research task. I require mutliple solutions with pros and cons for each.

The next step is to link Schematic and Layout data. A typical file reading flow is:

1. read_lef for technology and leaf-cell Abstract views
2. read_verilog to create a Schematic view
3. read_def to create Placement and Route data
4. link - Link instances to reference Designs (already implemented) plus link Placements to Instances and Routes to Nets (new feature).

In an ideal world, all data in the DEF should match the Verilog, but obviously this isn't always the case. So here is a list of different scenarios and what do to in each:

1. Placement in DEF missing Instance in Schematic - still create the Placement as this can be a "physical-only" cell. Add a field to Placements to mark them as physical_only. Log a warning for each instance this happens to.
2. Route in DEF missing Net in Schematic - this is always an error. Sometimes DEF contains power/ground nets that may not be present in the Schematic, but for now we can treat this as an error.
3. Instance in Schematic missing Placement in Layout - this is fine, sometimes DEF files don't have any Placement data, and may just contain Nets. A placement at 0,0 orientation R0 should be created for the Instance.
4. Net in Schematic missing in Route Layout - this is fine, sometimes DEF files don't have any routing information, just Placement data.
5. Something I haven't thought about!

SIDE NOTE: Black-box support - sometimes EDA tools support black-boxes for missing data, but we will *NOT* support this for now. Maybe consider this for a future enhancement.

Mapping DEF to Verilog
----------------------

How DEF names map to Verilog is not straight forward. DEF contains COMPONENTS and NETS with hierarchical names, including the hierarchy delimiter "/". Verilog has resursive modules and instances. We need to resursively search the Schematic hierachy based on the DEF names, so this should be as fast as possible.

It should also be possible for the user to search for hierarchical nets and instances in the Schematic view. For example: get_nets a/b/c/n1 - where a/b/c and instance names an n1 is the net name.


What happens during mutations?
------------------------------

Certain mutations will require changes in a sibling view. For example if a net is removed from the Schematic view, it's Route should also be deleted from the Layout view. I would like you to research a few different ways to achieve "side-effects" like this. Ideally, can rules be specified in the schema.py about mutation policies?

Also, within the Schematic, if a pin or port is deleted, then it should also be disconnected from it's Net.

