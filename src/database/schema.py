from codegen.schema import Schema, Klass, Field, Purpose

schema = Schema(
    name="layout_engine",
    description="Layout Engine Database Schema",
    namespace="le",
    version="0.51.0",
    classes=[
        Klass(
            name="Technology",
            description="Technology information such as layers and site definitions",
            # Session-singleton anchor for get_layers/get_vias/get_sites/
            # get_via_rules/get_non_default_rules' own default (-of
            # omitted) scope - see codegen/codegen/tcl_scope.py.
            has_current_access=True,
            fields=[
                Field(
                    name="layers",
                    description="Routing layers",
                    type="Layer",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="vias",
                    description="Fixed/default vias (LEF VIA)",
                    type="Via",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="via_rules",
                    description="Via generation rules (LEF VIARULE)",
                    type="ViaRule",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="sites",
                    description="Site definitions (LEF SITE)",
                    type="Site",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="non_default_rules",
                    description="Alternate per-net routing rules (LEF NONDEFAULTRULE)",
                    type="NonDefaultRule",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="property_definitions",
                    description="Property type declarations (LEF PROPERTYDEFINITIONS)",
                    type="PropertyDefinition",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="database_units_microns",
                    description="Database units per micron (LEF UNITS DATABASE MICRONS)",
                    type="double",
                    example=2000.0,
                ),
                Field(
                    name="capacitance_units_pf",
                    description="LEF UNITS CAPACITANCE PICOFARADS <value>",
                    type="double",
                    example=10.0,
                    is_optional=True,
                ),
                Field(
                    name="resistance_units_ohms",
                    description="LEF UNITS RESISTANCE OHMS <value>",
                    type="double",
                    example=10000.0,
                    is_optional=True,
                ),
                Field(
                    name="power_units_mw",
                    description="LEF UNITS POWER MILLIWATTS <value>",
                    type="double",
                    example=10000.0,
                    is_optional=True,
                ),
                Field(
                    name="current_units_ma",
                    description="LEF UNITS CURRENT MILLIAMPS <value>",
                    type="double",
                    example=10000.0,
                    is_optional=True,
                ),
                Field(
                    name="voltage_units_v",
                    description="LEF UNITS VOLTAGE VOLTS <value>",
                    type="double",
                    example=1000.0,
                    is_optional=True,
                ),
                Field(
                    name="frequency_units_mhz",
                    description="LEF UNITS FREQUENCY MEGAHERTZ <value>",
                    type="double",
                    example=10.0,
                    is_optional=True,
                ),
                Field(
                    name="bus_bit_chars",
                    description="The bus bit delimiter characters (LEF BUSBITCHARS), e.g. \"<>\"",
                    type="str",
                    example="<>",
                    is_optional=True,
                ),
                Field(
                    name="divider_char",
                    description="The hierarchy divider character (LEF DIVIDERCHAR), e.g. \"/\"",
                    type="str",
                    example="/",
                    is_optional=True,
                ),
                Field(
                    name="fixed_mask",
                    description="Top-level LEF FIXEDMASK was specified - distinct from Abstract.is_fixed_mask (LEF MACRO ... FIXEDMASK)",
                    type="bool",
                    example=False,
                ),
                Field(
                    name="use_min_spacing_obs",
                    description="LEF USEMINSPACING OBS ON|OFF",
                    type="bool",
                    example=True,
                    is_optional=True,
                ),
                Field(
                    name="use_min_spacing_pin",
                    description="LEF USEMINSPACING PIN ON|OFF",
                    type="bool",
                    example=True,
                    is_optional=True,
                ),
                Field(
                    name="clearance_measure",
                    description="LEF CLEARANCEMEASURE (EUCLIDEAN or MAXXY)",
                    type="str",
                    example="EUCLIDEAN",
                    is_optional=True,
                ),
                Field(
                    name="manufacturing_grid",
                    description="LEF MANUFACTURINGGRID, in microns",
                    type="double",
                    example=3.5,
                    is_optional=True,
                ),
                Field(
                    name="fin_pitch",
                    description="FinFET grid pitch, in database units - overrides the pitch of a LIBRARY LEF58_FINFET property",
                    type="dbu",
                    example=48,
                    is_optional=True,
                ),
                Field(
                    name="fin_offset",
                    description="FinFET grid offset, in database units - overrides LEF58_FINFET's OFFSET (0 if neither is given)",
                    type="dbu",
                    example=0,
                    is_optional=True,
                ),
                Field(
                    name="fin_direction",
                    description="FinFET direction (H: horizontal fins, the grid snaps Y; V: vertical fins, snaps X) - overrides LEF58_FINFET's HORIZONTAL/VERTICAL (HORIZONTAL if neither is given)",
                    type="RoutingDirection",
                    is_optional=True,
                ),
                Field(
                    name="max_via_stack",
                    description="LEF MAXVIASTACK value",
                    type="int",
                    example=4,
                    is_optional=True,
                ),
                Field(
                    name="max_via_stack_bottom_layer",
                    description="LEF MAXVIASTACK ... RANGE bottomLayer",
                    type="str",
                    example="M1",
                    is_optional=True,
                ),
                Field(
                    name="max_via_stack_top_layer",
                    description="LEF MAXVIASTACK ... RANGE topLayer",
                    type="str",
                    example="M7",
                    is_optional=True,
                ),
                Field(
                    name="antenna_input_gate_area",
                    description="Top-level LEF ANTENNAINPUTGATEAREA (legacy pre-5.0 default, distinct from a PIN's own per-pin ANTENNAGATEAREA)",
                    type="double",
                    example=45.0,
                    is_optional=True,
                ),
                Field(
                    name="antenna_inout_diff_area",
                    description="Top-level LEF ANTENNAINOUTDIFFAREA (legacy pre-5.0 default)",
                    type="double",
                    example=65.0,
                    is_optional=True,
                ),
                Field(
                    name="antenna_output_diff_area",
                    description="Top-level LEF ANTENNAOUTPUTDIFFAREA (legacy pre-5.0 default)",
                    type="double",
                    example=55.0,
                    is_optional=True,
                ),
            ],
        ),
        Klass(
            name="LefProperty",
            description="A single (name, value) LEF PROPERTY attached to a LAYER, VIA, VIARULE, NONDEFAULTRULE, SITE, MACRO or PIN",
            has_pool=False,
            fields=[
                Field(name="name", description="The property name (declared in advance by a PROPERTYDEFINITIONS entry)", type="str", example="lip"),
                Field(name="is_number", description="Whether this instance's value is numeric (INTEGER/REAL) rather than a string", type="bool", example=False),
                Field(name="string_value", description="The value, if is_number is false - empty string otherwise", type="str", example="top"),
                Field(name="number_value", description="The value, if is_number is true - 0.0 otherwise. Covers both LEF INTEGER and REAL", type="double", example=5.0),
            ],
        ),
        Klass(
            name="PropertyDefinition",
            description="One LEF PROPERTYDEFINITIONS entry - declares a property name's data type (and optional value range) before any PROPERTY statement is allowed to use it",
            fields=[
                Field(name="technology", description="Parent technology", type="Technology", parent="property_definitions"),
                Field(name="owner_type", description="Which construct this property applies to - LIBRARY, LAYER, VIA, VIARULE, NONDEFAULTRULE, MACRO, or PIN", type="str", example="LAYER"),
                Field(name="name", description="The property name", type="str", example="lip"),
                Field(name="data_type", description="The data type code - I(nteger), R(eal), S(tring), or Q(uoted string)", type="str", example="I"),
                Field(name="range_min", description="RANGE lower bound, in the property's own units", type="double", example=0.0, is_optional=True),
                Field(name="range_max", description="RANGE upper bound, in the property's own units", type="double", example=10.0, is_optional=True),
                Field(name="default_number", description="Default value, if data_type is I(nteger)/R(eal)", type="double", example=20.0, is_optional=True),
                Field(name="default_string", description="Default value, if data_type is S(tring)/Q(uoted string)", type="str", example="Cadence96", is_optional=True),
            ],
        ),
        Klass(
            name="Layer",
            description="A routing layer",
            fields=[
                Field(
                    name="technology",
                    description="Parent technology",
                    type="Technology",
                    parent="layers",
                ),
                Field(
                    name="name",
                    description="The name of the layer",
                    type="str",
                    example="M1",
                    index=True,
                ),
                Field(
                    name="type",
                    description="The type of the layer: ROUTING, CUT, IMPLANT, MASTERSLICE etc",
                    type="str",
                    example="ROUTING",
                ),
                Field(
                    name="direction",
                    description="The layer routing direction",
                    type="RoutingDirection",
                ),
                Field(
                    name="width",
                    description="The default routing width, in database units (LEF WIDTH)",
                    type="dbu",
                    example=1000,
                    is_optional=True,
                ),
                Field(
                    name="pitch",
                    description="The routing pitch, in database units (LEF PITCH, single-value form only)",
                    type="dbu",
                    example=2000,
                    is_optional=True,
                ),
                Field(
                    name="offset",
                    description="The routing offset, in database units (LEF OFFSET, single-value form only)",
                    type="dbu",
                    example=0,
                    is_optional=True,
                ),
                Field(
                    name="area",
                    description="The minimum area, in database units squared (LEF AREA)",
                    type="dbu2",
                    example=1000000,
                    is_optional=True,
                ),
                Field(
                    name="spacing_rules",
                    description="All SPACING statements for this layer (LEF SPACING, ROUTING and CUT variants)",
                    type="SpacingRule",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="minimum_cuts",
                    description="Minimum-cut rules (LEF MINIMUMCUT, CUT layers)",
                    type="MinimumCut",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="min_steps",
                    description="Minimum-step rules (LEF MINSTEP, ROUTING layers)",
                    type="MinStep",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="spacing_table_orthogonal",
                    description="Orthogonal cut spacing table entries (LEF SPACINGTABLE ORTHOGONAL, CUT layers)",
                    type="OrthogonalSpacingEntry",
                    is_list=True,
                    create_excluded=True,  # structurally list_compound-eligible but deferred - this round scoped to Shape.rects/polygons/paths only
                ),
                Field(
                    name="spacing_table_influence",
                    description="Influence spacing table entries (LEF SPACINGTABLE INFLUENCE, ROUTING layers)",
                    type="InfluenceSpacingEntry",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="spacing_table_parallel_run_length",
                    description="Parallel-run-length spacing table (LEF SPACINGTABLE PARALLELRUNLENGTH, ROUTING layers) - at most one per layer",
                    type="ParallelRunLengthSpacingTable",
                    is_optional=True,
                ),
                Field(
                    name="resistance",
                    description="Resistance per square (ROUTING) or per cut (CUT), in the LEF file's own declared units (LEF RESISTANCE)",
                    type="double",
                    example=0.4,
                    is_optional=True,
                ),
                Field(
                    name="capacitance",
                    description="Capacitance per square, in the LEF file's own declared units (LEF CAPACITANCE)",
                    type="double",
                    example=0.1,
                    is_optional=True,
                ),
                Field(
                    name="height",
                    description="Layer height, in database units (LEF HEIGHT)",
                    type="dbu",
                    is_optional=True,
                ),
                Field(
                    name="thickness",
                    description="Layer thickness, in database units (LEF THICKNESS)",
                    type="dbu",
                    is_optional=True,
                ),
                Field(
                    name="wire_extension",
                    description="Default wire extension beyond a pin, in database units (LEF WIREEXTENSION)",
                    type="dbu",
                    is_optional=True,
                ),
                Field(
                    name="shrinkage",
                    description="Shrinkage, in database units (LEF SHRINKAGE)",
                    type="dbu",
                    is_optional=True,
                ),
                Field(
                    name="cap_multiplier",
                    description="Capacitance multiplier (LEF CAPMULTIPLIER)",
                    type="double",
                    example=1.0,
                    is_optional=True,
                ),
                Field(
                    name="edge_cap",
                    description="Edge capacitance, in the LEF file's own declared units (LEF EDGECAPACITANCE)",
                    type="double",
                    example=0.1,
                    is_optional=True,
                ),
                Field(
                    name="antenna_length",
                    description="Antenna length, in database units (LEF ANTENNALENGTHFACTOR - deprecated 5.3 syntax, but still parsed)",
                    type="dbu",
                    example=1000,
                    is_optional=True,
                ),
                Field(
                    name="properties",
                    description="PROPERTY attachments (LEF PROPERTY)",
                    type="LefProperty",
                    is_list=True,
                ),
                Field(
                    name="antenna_models",
                    description="Antenna diffusion models, one per OXIDE1-4 (LEF ANTENNAMODEL, ROUTING and CUT layers both)",
                    type="AntennaModel",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="default_mask",
                    description="LEF MASK (layer-level default, LEF 5.8) - not to be confused with per-shape RECT/POLYGON/PATH MASK on Shape",
                    type="int",
                    example=2,
                    is_optional=True,
                ),
                Field(
                    name="pitch_xy",
                    description="Two-value PITCH form (LEF PITCH x y), in database units - mutually exclusive with pitch (single-value form)",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="offset_xy",
                    description="Two-value OFFSET form (LEF OFFSET x y), in database units - mutually exclusive with offset",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="diag_pitch",
                    description="Single-value DIAGPITCH, in database units - mutually exclusive with diag_pitch_xy",
                    type="dbu",
                    is_optional=True,
                ),
                Field(
                    name="diag_pitch_xy",
                    description="Two-value DIAGPITCH (diag45/diag135 distances), in database units",
                    type="Point",
                    is_optional=True,
                ),
                Field(name="diag_spacing", description="LEF DIAGSPACING, in database units", type="dbu", is_optional=True),
                Field(name="diag_width", description="LEF DIAGWIDTH, in database units", type="dbu", is_optional=True),
                Field(name="diag_min_edge_length", description="LEF DIAGMINEDGELENGTH, in database units", type="dbu", is_optional=True),
                Field(name="max_width", description="LEF MAXWIDTH, in database units", type="dbu", is_optional=True),
                Field(name="min_width", description="LEF MINWIDTH, in database units", type="dbu", is_optional=True),
                Field(
                    name="min_sizes",
                    description="LEF MINSIZE (width, length) pairs, in database units",
                    type="MinSizeEntry",
                    is_list=True,
                    create_excluded=True,  # structurally list_compound-eligible but deferred - this round scoped to Shape.rects/polygons/paths only
                ),
                Field(
                    name="min_enclosed_areas",
                    description="LEF MINENCLOSEDAREA entries",
                    type="MinEnclosedAreaEntry",
                    is_list=True,
                ),
                Field(
                    name="protrusion_width1",
                    description="LEF PROTRUSIONWIDTH's first width, in database units - all three protrusion_* fields are set together or not at all",
                    type="dbu",
                    is_optional=True,
                ),
                Field(name="protrusion_length", description="LEF PROTRUSIONWIDTH's LENGTH, in database units", type="dbu", is_optional=True),
                Field(name="protrusion_width2", description="LEF PROTRUSIONWIDTH's second WIDTH, in database units", type="dbu", is_optional=True),
                Field(
                    name="array_cuts",
                    description="LEF ARRAYCUTS entries (CUT layers)",
                    type="ArrayCutsEntry",
                    is_list=True,
                    create_excluded=True,  # structurally list_compound-eligible but deferred - this round scoped to Shape.rects/polygons/paths only
                ),
                Field(
                    name="array_spacing",
                    description="LEF ARRAYSPACING (at most one per layer, CUT layers)",
                    type="ArraySpacing",
                    is_optional=True,
                    is_child=True,
                ),
                Field(
                    name="spacing_table_two_widths",
                    description="LEF SPACINGTABLE TWOWIDTHS rows (5.7) - mutually exclusive with spacing_table_parallel_run_length/spacing_table_influence per SPACINGTABLE occurrence",
                    type="TwoWidthsSpacingEntry",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="prefer_enclosures",
                    description="LEF PREFERENCLOSURE entries (CUT layers)",
                    type="PreferEnclosureEntry",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="enclosures",
                    description="LEF ENCLOSURE entries (CUT layers, 5.6 - distinct from PREFERENCLOSURE)",
                    type="EnclosureEntry",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="split_wire_width",
                    description="LEF SPLITWIREWIDTH, in database units - read-only, not written back by write_lef",
                    type="dbu",
                    is_optional=True,
                ),
                Field(name="minimum_density", description="LEF MINIMUMDENSITY, a percentage (0-100)", type="double", example=4.0, is_optional=True),
                Field(name="maximum_density", description="LEF MAXIMUMDENSITY, a percentage (0-100)", type="double", example=10.0, is_optional=True),
                Field(name="density_check_step", description="LEF DENSITYCHECKSTEP, in database units", type="dbu", is_optional=True),
                Field(
                    name="density_check_window",
                    description="LEF DENSITYCHECKWINDOW (length, width), in database units",
                    type="DensityCheckWindow",
                    is_optional=True,
                ),
                Field(name="fill_active_spacing", description="LEF FILLACTIVESPACING, in database units", type="dbu", is_optional=True),
                Field(
                    name="ac_current_density",
                    description="LEF ACCURRENTDENSITY entries (PEAK/AVERAGE/RMS)",
                    type="LayerDensityEntry",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="dc_current_density",
                    description="LEF DCCURRENTDENSITY entries (always AVERAGE)",
                    type="LayerDensityEntry",
                    is_list=True,
                    is_child=True,
                ),
            ],
        ),
        Klass(
            name="AntennaPWLEntry",
            description="One (diffusion, ratio) row of a piecewise-linear antenna ratio (LEF PWL)",
            has_pool=False,
            fields=[
                Field(name="diffusion", description="The diffusion value", type="double", example=0.1),
                Field(name="ratio", description="The ratio at this diffusion value", type="double", example=1.0),
            ],
        ),
        Klass(
            name="AntennaModel",
            description="One LEF ANTENNAMODEL OXIDE1-4 block within a LAYER - each scalar and its PWL list are mutually exclusive",
            fields=[
                Field(name="layer", description="Parent layer", type="Layer", parent="antenna_models"),
                Field(name="oxide", description="OXIDE1, OXIDE2, OXIDE3, or OXIDE4", type="str", example="OXIDE1"),
                Field(name="area_ratio", description="LEF ANTENNAAREARATIO", type="double", example=100.0, is_optional=True),
                Field(name="cum_area_ratio", description="LEF ANTENNACUMAREARATIO", type="double", example=100.0, is_optional=True),
                Field(name="area_factor", description="LEF ANTENNAAREAFACTOR", type="double", example=1.0, is_optional=True),
                Field(name="area_factor_diffuse_only", description="Whether ANTENNAAREAFACTOR's DIFFUSEONLY suffix was specified", type="bool", example=False),
                Field(name="side_area_ratio", description="LEF ANTENNASIDEAREARATIO", type="double", example=100.0, is_optional=True),
                Field(name="cum_side_area_ratio", description="LEF ANTENNACUMSIDEAREARATIO", type="double", example=100.0, is_optional=True),
                Field(name="side_area_factor", description="LEF ANTENNASIDEAREAFACTOR", type="double", example=1.0, is_optional=True),
                Field(name="side_area_factor_diffuse_only", description="Whether ANTENNASIDEAREAFACTOR's DIFFUSEONLY suffix was specified", type="bool", example=False),
                Field(name="diff_area_ratio", description="LEF ANTENNADIFFAREARATIO (scalar form)", type="double", example=100.0, is_optional=True),
                Field(name="diff_area_ratio_pwl", description="LEF ANTENNADIFFAREARATIO PWL (list form) - mutually exclusive with diff_area_ratio", type="AntennaPWLEntry", is_list=True, create_excluded=True),
                Field(name="cum_diff_area_ratio", description="LEF ANTENNACUMDIFFAREARATIO (scalar form)", type="double", example=100.0, is_optional=True),
                Field(name="cum_diff_area_ratio_pwl", description="LEF ANTENNACUMDIFFAREARATIO PWL (list form)", type="AntennaPWLEntry", is_list=True, create_excluded=True),
                Field(name="diff_side_area_ratio", description="LEF ANTENNADIFFSIDEAREARATIO (scalar form)", type="double", example=100.0, is_optional=True),
                Field(name="diff_side_area_ratio_pwl", description="LEF ANTENNADIFFSIDEAREARATIO PWL (list form)", type="AntennaPWLEntry", is_list=True, create_excluded=True),
                Field(name="cum_diff_side_area_ratio", description="LEF ANTENNACUMDIFFSIDEAREARATIO (scalar form)", type="double", example=100.0, is_optional=True),
                Field(name="cum_diff_side_area_ratio_pwl", description="LEF ANTENNACUMDIFFSIDEAREARATIO PWL (list form)", type="AntennaPWLEntry", is_list=True, create_excluded=True),
                # All four *_pwl fields above are structurally list_compound-eligible but deferred - this round scoped to Shape.rects/polygons/paths only.
            ],
        ),
        Klass(
            name="MinSizeEntry",
            description="One (width, length) pair of a LAYER's LEF MINSIZE",
            has_pool=False,
            fields=[
                Field(name="width", description="In database units", type="dbu", example=140),
                Field(name="length", description="In database units", type="dbu", example=300),
            ],
        ),
        Klass(
            name="MinEnclosedAreaEntry",
            description="One LEF MINENCLOSEDAREA entry - a layer can have several",
            has_pool=False,
            fields=[
                Field(name="area", description="In database units squared", type="dbu2", example=400000),
                Field(name="width", description="Optional MINENCLOSEDAREAWIDTH, in database units", type="dbu", is_optional=True),
            ],
        ),
        Klass(
            name="ArrayCutsEntry",
            description="One LEF ARRAYCUTS entry (CUT layers) - a layer can have several",
            has_pool=False,
            fields=[
                Field(name="cuts", description="The number of cuts (LEF ARRAYCUTS n)", type="int", example=4),
                Field(name="spacing", description="In database units", type="dbu", example=1500),
            ],
        ),
        Klass(
            name="ArraySpacing",
            description="LEF ARRAYSPACING (CUT layers, at most one per layer)",
            fields=[
                Field(name="layer", description="Parent layer", type="Layer", parent="array_spacing"),
                Field(name="long_array", description="Whether LONGARRAY was specified", type="bool", example=False),
                Field(name="via_width", description="Optional WIDTH, in database units", type="dbu", is_optional=True),
                Field(name="cut_spacing", description="CUTSPACING, in database units", type="dbu", example=200),
            ],
        ),
        Klass(
            name="TwoWidthsSpacingEntry",
            description="One WIDTH row of a LEF SPACINGTABLE TWOWIDTHS block (5.7) - Layer.spacing_table_two_widths holds every row",
            fields=[
                Field(name="layer", description="Parent layer", type="Layer", parent="spacing_table_two_widths"),
                Field(name="width", description="In database units", type="dbu", example=1500),
                Field(name="prl", description="Optional PRL (parallel run length), in database units", type="dbu", is_optional=True),
                Field(name="spacings", description="One spacing value per column, in database units", type="dbu", is_list=True),
            ],
        ),
        Klass(
            name="PreferEnclosureEntry",
            description="One LEF PREFERENCLOSURE entry (CUT layers) - a layer can have several",
            fields=[
                Field(name="layer", description="Parent layer", type="Layer", parent="prefer_enclosures"),
                Field(name="location", description="ABOVE or BELOW", type="str", example="BELOW", is_optional=True),
                Field(name="overhang1", description="In database units", type="dbu", example=60),
                Field(name="overhang2", description="In database units", type="dbu", example=10),
                Field(name="min_width", description="Optional WIDTH, in database units", type="dbu", is_optional=True),
            ],
        ),
        # width/except_extra_cut/min_length are mutually exclusive per the
        # vendored writer's own three ENCLOSURE writer variants (plain,
        # WIDTH[+EXCEPTEXTRACUT], LENGTH).
        Klass(
            name="EnclosureEntry",
            description="One LEF ENCLOSURE entry (CUT layers, 5.6) - a layer can have several.",
            fields=[
                Field(name="layer", description="Parent layer", type="Layer", parent="enclosures"),
                Field(name="location", description="ABOVE or BELOW", type="str", example="BELOW", is_optional=True),
                Field(name="overhang1", description="In database units", type="dbu", example=30),
                Field(name="overhang2", description="In database units", type="dbu", example=10),
                Field(name="width", description="Optional WIDTH, in database units", type="dbu", is_optional=True),
                Field(name="except_extra_cut", description="Optional WIDTH ... EXCEPTEXTRACUT (5.7), in database units - only meaningful alongside width", type="dbu", is_optional=True),
                Field(name="min_length", description="Optional LENGTH (5.7), in database units - mutually exclusive with width", type="dbu", is_optional=True),
            ],
        ),
        Klass(
            name="DensityCheckWindow",
            description="LEF DENSITYCHECKWINDOW (length, width)",
            has_pool=False,
            fields=[
                Field(name="length", description="In database units", type="dbu", example=1000),
                Field(name="width", description="In database units", type="dbu", example=1000),
            ],
        ),
        # Owned by exactly one of Layer's two independent is_child lists
        # (ac_current_density/dc_current_density, mutually exclusive per
        # instance - same multi-parent-field pattern as Shape's
        # terminal_port/obstruction, just both roles happening to be the
        # same owner class).
        Klass(
            name="LayerDensityEntry",
            description="One LEF ACCURRENTDENSITY/DCCURRENTDENSITY block (PEAK, AVERAGE, or RMS) - either a plain scalar or a table, never both.",
            fields=[
                Field(name="ac_layer", description="Owning Layer, if this entry belongs to its ac_current_density list", type="Layer", parent="ac_current_density"),
                Field(name="dc_layer", description="Owning Layer, if this entry belongs to its dc_current_density list", type="Layer", parent="dc_current_density"),
                Field(name="type", description="PEAK, AVERAGE, or RMS", type="str", example="AVERAGE"),
                Field(name="one_entry", description="Plain-scalar form value, in the LEF file's declared units", type="double", example=5.5, is_optional=True),
                Field(name="frequency", description="AC-only FREQUENCY row, in Hz - no dbu conversion", type="double", example=1000000.0, is_list=True),
                Field(name="width", description="WIDTH row (ROUTING layers), in database units", type="dbu", is_list=True),
                Field(name="cutarea", description="CUTAREA row (CUT layers), in database units squared", type="dbu2", is_list=True),
                Field(name="table_entries", description="Flat TABLEENTRIES row - interpreted against the frequency/width/cutarea counts", type="double", example=0.0000005, is_list=True),
            ],
        ),
        Klass(
            name="RowPatternEntry",
            description="One entry of a SITE's ROWPATTERN (LEF 5.6) - a reference to another site plus the orientation it's placed at",
            has_pool=False,
            fields=[
                Field(name="site_name", description="The referenced site's name", type="str", example="CORE"),
                Field(name="orient", description="The orientation the referenced site is placed at", type="Orientation"),
            ],
        ),
        Klass(
            name="MacroSitePlacement",
            description="One LEF MACRO SITE array placement (Abstract.site_placements) - a macro can have several",
            fields=[
                Field(name="abstract", description="Parent abstract", type="Abstract", parent="site_placements"),
                Field(name="site_name", description="The name of the site", type="str", example="CORE"),
                Field(name="origin", description="In database units", type="Point", is_optional=True),
                Field(name="orient", description="The orientation of this placement", type="Orientation"),
                Field(name="num_x", description="Optional LEF DO n", type="int", example=2, is_optional=True),
                Field(name="num_y", description="Optional LEF BY m", type="int", example=2, is_optional=True),
                Field(name="step_x", description="Optional LEF STEP x, in database units", type="dbu", is_optional=True),
                Field(name="step_y", description="Optional LEF STEP y, in database units", type="dbu", is_optional=True),
            ],
        ),
        Klass(
            name="Site",
            description="A site definition (LEF SITE) - the placement grid unit a MACRO's own SITE statement (Abstract.site, a plain name reference) refers to",
            fields=[
                Field(
                    name="technology",
                    description="Parent technology",
                    type="Technology",
                    parent="sites",
                ),
                Field(name="name", description="The name of the site", type="str", example="CORE", index=True),
                Field(name="site_class", description="PAD, CORE or VIRTUAL (LEF CLASS)", type="str", example="CORE", is_optional=True),
                Field(name="size", description="The site size, in database units (LEF SIZE)", type="Point", is_optional=True),
                Field(name="symmetry", description="Which flips/rotations this site allows (LEF SYMMETRY)", type="Symmetry", is_optional=True),
                Field(name="row_pattern", description="An ordered list of other-site references this site is built from (LEF ROWPATTERN, 5.6)", type="RowPatternEntry", is_list=True),
                Field(name="properties", description="PROPERTY attachments (LEF PROPERTY) - read-only, not written back by write_lef", type="LefProperty", is_list=True),
            ],
        ),
        Klass(
            name="NonDefaultRuleLayer",
            description="One LAYER override within a NONDEFAULTRULE",
            fields=[
                Field(name="non_default_rule", description="Parent non-default rule", type="NonDefaultRule", parent="layers"),
                Field(name="layer_name", description="The name of the layer being overridden", type="str", example="M1"),
                Field(name="width", description="Overridden width, in database units", type="dbu", example=2000, is_optional=True),
                Field(name="spacing", description="Overridden minimum spacing, in database units", type="dbu", example=2000, is_optional=True),
                Field(name="wire_extension", description="Overridden wire extension, in database units", type="dbu", example=0, is_optional=True),
                Field(name="resistance", description="Overridden resistance per square, in the LEF file's own declared units (obsolete since LEF 5.6, but still readable)", type="double", example=0.4, is_optional=True),
                Field(name="capacitance", description="Overridden capacitance per square, in the LEF file's own declared units (obsolete since LEF 5.6, but still readable)", type="double", example=0.1, is_optional=True),
                Field(name="edge_cap", description="Overridden edge capacitance, in the LEF file's own declared units (obsolete since LEF 5.6, but still readable)", type="double", example=0.1, is_optional=True),
                Field(name="diag_width", description="Overridden diagonal width, in database units (LEF 5.6) - read-only, not written back by write_lef", type="dbu", example=1500, is_optional=True),
            ],
        ),
        Klass(
            name="NonDefaultRuleVia",
            description="A VIA defined inline within a NONDEFAULTRULE - the same content as a technology-level Via, but owned by the rule",
            fields=[
                Field(name="non_default_rule", description="Parent non-default rule", type="NonDefaultRule", parent="vias"),
                Field(name="name", description="The name of the via", type="str", example="M1_M2_ND"),
                Field(name="is_default", description="Whether this via is marked DEFAULT", type="bool", example=False),
                Field(name="resistance", description="Resistance, in the LEF file's own declared units", type="double", example=0.5, is_optional=True),
                Field(name="foreign", description="A FOREIGN cell reference", type="Foreign", is_optional=True, is_child=True),
                Field(name="layers", description="Per-layer geometry", type="ViaLayer", is_list=True, is_child=True),
                Field(name="properties", description="PROPERTY attachments (LEF PROPERTY)", type="LefProperty", is_list=True),
            ],
        ),
        Klass(
            name="MinCutOverride",
            description="One LEF MINCUTS entry within a NONDEFAULTRULE (5.6) - a per-cut-layer override of the default via cut count",
            has_pool=False,
            fields=[
                Field(name="cut_layer_name", description="The name of the cut layer", type="str", example="V1"),
                Field(name="num_cuts", description="The overridden number of cuts", type="int", example=2),
            ],
        ),
        Klass(
            name="NonDefaultRule",
            description="An alternate per-net routing rule (LEF NONDEFAULTRULE) - layer width/spacing overrides plus optional embedded or referenced vias",
            fields=[
                Field(
                    name="technology",
                    description="Parent technology",
                    type="Technology",
                    parent="non_default_rules",
                ),
                Field(name="name", description="The name of the rule", type="str", example="WIDE_M1", index=True),
                Field(name="hard_spacing", description="Whether HARDSPACING was specified (5.6)", type="bool", example=False),
                Field(name="layers", description="Per-layer width/spacing/... overrides", type="NonDefaultRuleLayer", is_list=True, is_child=True),
                Field(name="vias", description="Vias defined inline within this rule", type="NonDefaultRuleVia", is_list=True, is_child=True),
                Field(name="use_via_names", description="Names of technology-level VIAs this rule uses (5.6 USEVIA) - empty for a rule with no explicit USEVIA statements", type="str", example="V1_0", is_list=True),
                Field(name="use_via_rule_names", description="Names of technology-level VIARULEs this rule uses (5.6 USEVIARULE)", type="str", example="ViaRule1", is_list=True),
                Field(name="min_cuts", description="Per-cut-layer via-count overrides (5.6 MINCUTS)", type="MinCutOverride", is_list=True),
                Field(name="properties", description="PROPERTY attachments (LEF PROPERTY) - read-only, not written back by write_lef", type="LefProperty", is_list=True),
            ],
        ),
        Klass(
            name="MinimumCut",
            description="One LEF MINIMUMCUT rule (CUT layers)",
            fields=[
                Field(name="layer", description="Parent layer", type="Layer", parent="minimum_cuts"),
                Field(name="cuts", description="Number of cuts required (LEF MINIMUMCUT)", type="int", example=2),
                Field(name="width", description="Width above which the rule applies, in database units", type="dbu", example=1000),
                Field(name="within", description="MINIMUMCUT ... WITHIN distance, in database units (5.7)", type="dbu", example=250, is_optional=True),
                Field(name="connection", description="FROMABOVE or FROMBELOW", type="str", example="FROMABOVE", is_optional=True),
                Field(name="length", description="MINIMUMCUT ... LENGTH value, in database units", type="dbu", example=1000, is_optional=True),
                Field(name="distance", description="MINIMUMCUT ... LENGTH ... WITHIN distance, in database units", type="dbu", example=1000, is_optional=True),
            ],
        ),
        Klass(
            name="MinStep",
            description="One LEF MINSTEP rule (ROUTING layers)",
            fields=[
                Field(name="layer", description="Parent layer", type="Layer", parent="min_steps"),
                Field(name="distance", description="The minimum step distance, in database units (LEF MINSTEP)", type="dbu", example=50),
                Field(name="min_step_type", description="INSIDECORNER, OUTSIDECORNER or STEP", type="str", example="INSIDECORNER", is_optional=True),
                Field(name="lengthsum", description="MINSTEP ... LENGTHSUM value, in database units", type="dbu", example=80, is_optional=True),
                Field(name="max_edges", description="MINSTEP ... MAXEDGES value (5.7)", type="int", example=2, is_optional=True),
            ],
        ),
        Klass(
            name="OrthogonalSpacingEntry",
            description="One row of a LEF SPACINGTABLE ORTHOGONAL table (CUT layers)",
            has_pool=False,
            fields=[
                Field(name="cut_within", description="The CUTWITHIN distance, in database units", type="dbu", example=200),
                Field(name="ortho_spacing", description="The orthogonal spacing value, in database units", type="dbu", example=300),
            ],
        ),
        Klass(
            name="InfluenceSpacingEntry",
            description="One row of a LEF SPACINGTABLE INFLUENCE table (ROUTING layers)",
            fields=[
                Field(name="layer", description="Parent layer", type="Layer", parent="spacing_table_influence"),
                Field(name="width", description="The width this row applies above, in database units", type="dbu", example=100),
                Field(name="distance", description="The influence distance, in database units", type="dbu", example=100),
                Field(name="spacing", description="The resulting spacing, in database units", type="dbu", example=100),
            ],
        ),
        Klass(
            name="ParallelRunLengthSpacingTable",
            description="A LEF SPACINGTABLE PARALLELRUNLENGTH table (ROUTING layers) - a width x length spacing grid, stored flat, width-major",
            has_pool=False,
            fields=[
                Field(name="lengths", description="The table's length column headers, in database units", type="dbu", is_list=True),
                Field(name="widths", description="The table's width row headers, in database units", type="dbu", is_list=True),
                Field(name="spacings", description="The width x length spacing grid, in database units, flat/width-major", type="dbu", is_list=True),
            ],
        ),
        Klass(
            name="SpacingRule",
            description="One LEF SPACING statement for a layer - ROUTING and CUT layers use disjoint sets of modifiers.",
            fields=[
                Field(name="layer", description="Parent layer", type="Layer", parent="spacing_rules"),
                Field(name="distance", description="The spacing distance, in database units (LEF SPACING)", type="dbu", example=200),
                # ROUTING-layer modifiers
                Field(name="range_min", description="SPACING ... RANGE min, in database units", type="dbu", example=100, is_optional=True),
                Field(name="range_max", description="SPACING ... RANGE max, in database units", type="dbu", example=200, is_optional=True),
                Field(name="range_use_length_threshold", description="RANGE ... USELENGTHTHRESHOLD was specified", type="bool", example=False),
                Field(name="range_influence", description="RANGE ... INFLUENCE value, in database units", type="dbu", example=100, is_optional=True),
                Field(name="range_influence_range_min", description="RANGE ... INFLUENCE ... RANGE min, in database units", type="dbu", example=100, is_optional=True),
                Field(name="range_influence_range_max", description="RANGE ... INFLUENCE ... RANGE max, in database units", type="dbu", example=200, is_optional=True),
                Field(name="range_range_min", description="SPACING ... RANGE a b RANGE min (the second, non-INFLUENCE RANGE), in database units", type="dbu", example=100, is_optional=True),
                Field(name="range_range_max", description="The second RANGE's max, in database units", type="dbu", example=200, is_optional=True),
                Field(name="length_threshold", description="SPACING ... LENGTHTHRESHOLD value, in database units", type="dbu", example=900, is_optional=True),
                Field(name="length_threshold_range_min", description="SPACING ... LENGTHTHRESHOLD value RANGE min, in database units - separate from range_min", type="dbu", example=0, is_optional=True),
                Field(name="length_threshold_range_max", description="SPACING ... LENGTHTHRESHOLD value RANGE max - see length_threshold_range_min, in database units", type="dbu", example=100, is_optional=True),
                Field(name="center_to_center", description="CENTERTOCENTER was specified", type="bool", example=False),
                Field(name="same_net", description="SAMENET was specified", type="bool", example=False),
                Field(name="same_net_pg_only", description="SAMENET PGONLY was specified", type="bool", example=False),
                Field(name="parallel_overlap", description="PARALLELOVERLAP was specified", type="bool", example=False),
                Field(name="end_of_line_width", description="ENDOFLINE width, in database units", type="dbu", example=1300, is_optional=True),
                Field(name="end_of_line_within", description="ENDOFLINE ... WITHIN distance, in database units", type="dbu", example=600, is_optional=True),
                Field(name="parallel_edge_space", description="ENDOFLINE ... PARALLELEDGE space, in database units", type="dbu", example=1100, is_optional=True),
                Field(name="parallel_edge_within", description="PARALLELEDGE ... WITHIN distance, in database units", type="dbu", example=500, is_optional=True),
                Field(name="two_edges", description="PARALLELEDGE ... TWOEDGES was specified", type="bool", example=False),
                Field(name="notch_length", description="SPACING ... NOTCHLENGTH, in database units - read-only, not written back by write_lef", type="dbu", is_optional=True),
                Field(name="end_of_notch_width", description="SPACING ... ENDOFNOTCHWIDTH, in database units - read-only, not written back by write_lef", type="dbu", is_optional=True),
                Field(name="end_of_notch_spacing", description="ENDOFNOTCHWIDTH ... NOTCHSPACING, in database units", type="dbu", is_optional=True),
                Field(name="end_of_notch_length", description="ENDOFNOTCHWIDTH ... NOTCHLENGTH, in database units", type="dbu", is_optional=True),
                # CUT-layer modifiers
                Field(name="second_layer_name", description="SPACING ... LAYER name (CUT inter-layer spacing)", type="str", example="RX", is_optional=True),
                Field(name="second_layer_stack", description="LAYER ... STACK was specified", type="bool", example=False),
                Field(name="adjacent_cuts", description="ADJACENTCUTS count", type="int", example=3, is_optional=True),
                Field(name="adjacent_within", description="ADJACENTCUTS ... WITHIN distance, in database units", type="dbu", example=250, is_optional=True),
                Field(name="adjacent_except_same_pg_net", description="ADJACENTCUTS ... EXCEPTSAMEPGNET was specified", type="bool", example=False),
                Field(name="area", description="SPACING ... AREA value, in database units squared (LEF 5.7, CUT layers only)", type="dbu2", example=200000, is_optional=True),
            ],
        ),
        Klass(
            # Owned by exactly one of Via/NonDefaultRuleVia/LayoutVia
            # (mutually exclusive, same pattern as Shape's
            # terminal_port/obstruction/... parents).
            name="ViaLayer",
            description="One layer's geometry within a VIA - LEF VIA geometry supports RECT/POLYGON only, no PATH/ITERATE.",
            fields=[
                Field(
                    name="via",
                    description="Owning Via, if this ViaLayer belongs to one",
                    type="Via",
                    parent="layers",
                ),
                Field(
                    name="non_default_rule_via",
                    description="Owning NonDefaultRuleVia, if this ViaLayer belongs to one",
                    type="NonDefaultRuleVia",
                    parent="layers",
                ),
                Field(
                    name="layout_via",
                    description="Owning LayoutVia, if this ViaLayer belongs to one",
                    type="LayoutVia",
                    parent="layers",
                ),
                Field(
                    name="layer_name",
                    description="The name of the layer",
                    type="str",
                    example="M1",
                ),
                Field(
                    name="rects",
                    description="A list of rects",
                    type="Rect",
                    is_list=True,
                    create_excluded=True,  # structurally list_compound-eligible but deferred - this round scoped to Shape.rects/polygons/paths only
                ),
                Field(
                    name="polygons",
                    description="A list of polygons",
                    type="Polygon",
                    is_list=True,
                    create_excluded=True,
                ),
                Field(
                    name="rect_masks",
                    description="MASK color per entry of rects (LEF RECT MASK n, 5.8) - parallel array, 0 meaning no mask; empty if no rect in this layer ever set one - same convention as Shape.rect_masks",
                    type="int",
                    example=0,
                    is_list=True,
                ),
                Field(
                    name="polygon_masks",
                    description="MASK color per entry of polygons (LEF POLYGON MASK n, 5.8) - parallel array, same convention as rect_masks",
                    type="int",
                    example=0,
                    is_list=True,
                ),
            ],
        ),
        # ROWCOL (num_cut_rows/num_cut_cols below) is modeled - a via
        # *array* is exactly a ROWCOL clause with more than one row/col of
        # cuts, synthesized into concrete cut rects by via_shapes.hpp at
        # render time rather than stored as one rect per cut.
        # ORIGIN/OFFSET (origin/bot_offset/top_offset below) are also
        # modeled - real caller-supplied overrides for where the cut
        # array's own center
        # (ORIGIN) and each metal layer's own enclosure-rect center
        # (OFFSET) land, relative to the via's own placement point, used
        # e.g. when a via needs to sit off-center from its own connection
        # point. PATTERN (a sparse cut-presence bitmap - which grid cells
        # in a rows x cols array actually have a cut, vs. the simpler
        # always-fully-populated grid this schema assumes) is deliberately
        # still not modeled - the reader logs a warning when one is
        # present rather than silently ignoring it, the same documented-
        # gap convention as everywhere else in this schema.
        Klass(
            name="ViaRuleReference",
            description="A VIA's own reference to a VIARULE with explicit cut geometry (LEF 5.6 VIARULE-inside-VIA) - not the same thing as a VIARULE block itself, see ViaRule. Owned by exactly one of Via/LayoutVia.",
            fields=[
                Field(
                    name="via",
                    description="Owning Via, if this ViaRuleReference belongs to one",
                    type="Via",
                    parent="via_rule",
                ),
                Field(
                    name="layout_via",
                    description="Owning LayoutVia, if this ViaRuleReference belongs to one",
                    type="LayoutVia",
                    parent="via_rule",
                ),
                Field(
                    name="via_rule_name",
                    description="The name of the referenced VIARULE",
                    type="str",
                    example="ViaRule1",
                ),
                Field(
                    name="cut_size",
                    description="The cut size, in database units (LEF CUTSIZE)",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="bot_layer_name",
                    description="The bottom metal layer name (LEF LAYERS botLayer cutLayer topLayer)",
                    type="str",
                    example="M1",
                ),
                Field(
                    name="cut_layer_name",
                    description="The cut layer name (LEF LAYERS botLayer cutLayer topLayer)",
                    type="str",
                    example="V1",
                ),
                Field(
                    name="top_layer_name",
                    description="The top metal layer name (LEF LAYERS botLayer cutLayer topLayer)",
                    type="str",
                    example="M2",
                ),
                Field(
                    name="cut_spacing",
                    description="The cut spacing, in database units (LEF CUTSPACING)",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="bot_enclosure",
                    description="The bottom layer enclosure, in database units (LEF ENCLOSURE, bottom pair)",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="top_enclosure",
                    description="The top layer enclosure, in database units (LEF ENCLOSURE, top pair)",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="num_cut_rows",
                    description="Number of cut rows (LEF ROWCOL) - a single cut if not given",
                    type="int",
                    example=2,
                    is_optional=True,
                ),
                Field(
                    name="num_cut_cols",
                    description="Number of cut columns (LEF ROWCOL) - a single cut if not given",
                    type="int",
                    example=2,
                    is_optional=True,
                ),
                Field(
                    name="origin",
                    description="Offset of the cut array's own center from the via's own placement point, in database units (LEF VIARULE-inside-VIA ORIGIN) - the array is centered on the placement point if not given",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="bot_offset",
                    description="Offset of the bottom metal layer's own enclosure-rect center from the cut array's own center, in database units (LEF VIARULE-inside-VIA OFFSET, bottom pair) - the enclosure rect is centered on the cut array if not given",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="top_offset",
                    description="Same as bot_offset, for the top metal layer's own enclosure rect (LEF VIARULE-inside-VIA OFFSET, top pair)",
                    type="Point",
                    is_optional=True,
                ),
            ],
        ),
        Klass(
            name="ViaRuleLayer",
            description="One layer's rule within a VIARULE - 2 (non-GENERATE) or 3 (GENERATE, the 3rd being the cut layer) per ViaRule",
            fields=[
                Field(
                    name="via_rule",
                    description="Parent via rule",
                    type="ViaRule",
                    parent="layers",
                ),
                Field(
                    name="layer_name",
                    description="The name of the layer",
                    type="str",
                    example="M1",
                ),
                Field(
                    name="direction",
                    description="The layer direction (LEF DIRECTION)",
                    type="RoutingDirection",
                ),
                Field(
                    name="width_min",
                    description="Minimum width, in database units (LEF WIDTH min)",
                    type="dbu",
                    example=1000,
                    is_optional=True,
                ),
                Field(
                    name="width_max",
                    description="Maximum width, in database units (LEF WIDTH max)",
                    type="dbu",
                    example=2000,
                    is_optional=True,
                ),
                Field(
                    name="overhang",
                    description="Overhang, in database units (LEF OVERHANG)",
                    type="dbu",
                    example=500,
                    is_optional=True,
                ),
                Field(
                    name="metal_overhang",
                    description="Metal overhang, in database units (LEF METALOVERHANG)",
                    type="dbu",
                    example=500,
                    is_optional=True,
                ),
                Field(
                    name="enclosure_overhang1",
                    description="First enclosure overhang, in database units (LEF ENCLOSURE overhang1 - 5.5 alternative to OVERHANG)",
                    type="dbu",
                    example=500,
                    is_optional=True,
                ),
                Field(
                    name="enclosure_overhang2",
                    description="Second enclosure overhang, in database units (LEF ENCLOSURE overhang2 - 5.5 alternative to OVERHANG)",
                    type="dbu",
                    example=500,
                    is_optional=True,
                ),
                Field(
                    name="spacing_step_x",
                    description="Spacing step in x, in database units (LEF SPACING x)",
                    type="dbu",
                    example=1000,
                    is_optional=True,
                ),
                Field(
                    name="spacing_step_y",
                    description="Spacing step in y, in database units (LEF SPACING y)",
                    type="dbu",
                    example=1000,
                    is_optional=True,
                ),
                Field(
                    name="rect",
                    description="The cut rect, in database units (LEF RECT - GENERATE's cut layer only)",
                    type="Rect",
                    is_optional=True,
                ),
                Field(
                    name="resistance",
                    description="Resistance, in the LEF file's own declared units (LEF RESISTANCE)",
                    type="double",
                    example=0.5,
                    is_optional=True,
                ),
            ],
        ),
        Klass(
            name="Via",
            description="A fixed/default via definition (LEF VIA)",
            fields=[
                Field(
                    name="technology",
                    description="Parent technology",
                    type="Technology",
                    parent="vias",
                ),
                Field(
                    name="name",
                    description="The name of the via",
                    type="str",
                    example="V1_0",
                    index=True,
                ),
                Field(
                    name="is_default",
                    description="Whether this via is the DEFAULT via for its layer pair (LEF VIA name DEFAULT)",
                    type="bool",
                    example=False,
                ),
                Field(
                    name="resistance",
                    description="Resistance, in the LEF file's own declared units (LEF RESISTANCE) - mutually exclusive with via_rule",
                    type="double",
                    example=0.5,
                    is_optional=True,
                ),
                Field(
                    name="foreign",
                    description="Foreign cell reference (LEF FOREIGN)",
                    type="Foreign",
                    is_optional=True,
                    is_child=True,
                ),
                Field(
                    name="layers",
                    description="Per-layer geometry",
                    type="ViaLayer",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="via_rule",
                    description="Reference to a VIARULE with explicit cut geometry (LEF 5.6 VIARULE-inside-VIA) - mutually exclusive with resistance",
                    type="ViaRuleReference",
                    is_optional=True,
                    is_child=True,
                ),
                Field(
                    name="properties",
                    description="PROPERTY attachments (LEF PROPERTY)",
                    type="LefProperty",
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="ViaRule",
            description="A via generation rule (LEF VIARULE, GENERATE or not)",
            fields=[
                Field(
                    name="technology",
                    description="Parent technology",
                    type="Technology",
                    parent="via_rules",
                ),
                Field(
                    name="name",
                    description="The name of the via rule",
                    type="str",
                    example="ViaRule1",
                    index=True,
                ),
                Field(
                    name="is_generate",
                    description="Whether this is a VIARULE ... GENERATE (vs. a plain VIARULE listing specific vias)",
                    type="bool",
                    example=True,
                ),
                Field(
                    name="is_default",
                    description="Whether this is the GENERATE DEFAULT via rule (LEF VIARULE ... GENERATE DEFAULT)",
                    type="bool",
                    example=False,
                ),
                Field(
                    name="layers",
                    description="2 layers (non-GENERATE) or 3 layers (GENERATE, the 3rd being the cut layer)",
                    type="ViaRuleLayer",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="via_names",
                    description="Specific via names this rule applies to (LEF VIA name - non-GENERATE only)",
                    type="str",
                    example="V1_0",
                    is_list=True,
                ),
                Field(
                    name="properties",
                    description="PROPERTY attachments (LEF PROPERTY) - write_lef writes them back for non-GENERATE rules only",
                    type="LefProperty",
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="RoutingDirection",
            description="A routing direction: H, V or NONE",
            is_enum=True,
            has_pool=False,
            fields=[
                Field(
                    name="H",
                    description="Horizontal routing",
                    type="int",
                    value=0,
                ),
                Field(
                    name="V",
                    description="Vertical routing",
                    type="int",
                    value=1,
                ),
                Field(
                    name="NONE",
                    description="No routing direction",
                    type="int",
                    value=2,
                ),
                Field(
                    name="DIAG45",
                    description="45-degree diagonal routing",
                    type="int",
                    value=3,
                ),
                Field(
                    name="DIAG135",
                    description="135-degree diagonal routing",
                    type="int",
                    value=4,
                ),
            ],
        ),
        Klass(
            name="Point",
            description="A coordinate",
            has_pool=False,
            fields=[
                Field(
                    name="x",
                    description="The x coordinate in database units",
                    type="dbu",
                    example=100,
                ),
                Field(
                    name="y",
                    description="The y coordinate in database units",
                    type="dbu",
                    example=200,
                ),
            ],
        ),
        Klass(
            name="Rect",
            description="A rectangle comprising of two points",
            has_pool=False,
            fields=[
                Field(
                    name="ll",
                    description="Lower-left point",
                    type="Point",
                ),
                Field(
                    name="ur",
                    description="Upper-right point",
                    type="Point",
                ),
            ],
        ),
        Klass(
            name="Polygon",
            description="A polygon",
            has_pool=False,
            fields=[
                Field(
                    name="points",
                    description="Points in the polygon",
                    type="Point",
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="Path",
            description="A path",
            has_pool=False,
            fields=[
                # width declared before polygon so every generated surface
                # that walks a Klass's own
                # fields in declaration order - property-table display
                # (get_properties/report_properties/Property Viewer) in
                # particular - shows {width {points}}, matching the
                # target convention (width first) rather than the reverse.
                # The create/update Tcl flag's own scalar-before-points
                # ordering (Field._list_compound_tcl_preamble's
                # "points_plus_scalars" branch) is independent of this -
                # it already puts scalar fields first unconditionally, so
                # this reorder only affects display.
                Field(
                    name="width",
                    description="Width of the path in database units",
                    type="dbu",
                    example=10,
                ),
                Field(
                    name="polygon",
                    description="Polygon representing the center line of the path",
                    type="Polygon",
                ),
            ],
        ),
        Klass(
            name="RectIterate",
            description="A LEF RECT ITERATE statement: a base rect repeated on a grid (LEF DO/STEP)",
            has_pool=False,
            fields=[
                Field(
                    name="rect",
                    description="The base rect (the ix=0, iy=0 instance)",
                    type="Rect",
                ),
                Field(
                    name="num_x",
                    description="Number of repetitions along x (LEF DO xStart)",
                    type="int",
                    example=3,
                ),
                Field(
                    name="num_y",
                    description="Number of repetitions along y (LEF DO yStart)",
                    type="int",
                    example=2,
                ),
                Field(
                    name="space_x",
                    description="Step between repetitions along x, in database units (LEF STEP xStep)",
                    type="dbu",
                    example=10000,
                ),
                Field(
                    name="space_y",
                    description="Step between repetitions along y, in database units (LEF STEP yStep)",
                    type="dbu",
                    example=20000,
                ),
                Field(name="mask", description="MASK color (5.8)", type="int", example=0, is_optional=True),
            ],
        ),
        Klass(
            name="PathIterate",
            description="A LEF PATH ITERATE statement: a base path repeated on a grid (LEF DO/STEP)",
            has_pool=False,
            fields=[
                Field(
                    name="path",
                    description="The base path (the ix=0, iy=0 instance)",
                    type="Path",
                ),
                Field(
                    name="num_x",
                    description="Number of repetitions along x (LEF DO xStart)",
                    type="int",
                    example=3,
                ),
                Field(
                    name="num_y",
                    description="Number of repetitions along y (LEF DO yStart)",
                    type="int",
                    example=2,
                ),
                Field(
                    name="space_x",
                    description="Step between repetitions along x, in database units (LEF STEP xStep)",
                    type="dbu",
                    example=10000,
                ),
                Field(
                    name="space_y",
                    description="Step between repetitions along y, in database units (LEF STEP yStep)",
                    type="dbu",
                    example=20000,
                ),
                Field(name="mask", description="MASK color (5.8)", type="int", example=0, is_optional=True),
            ],
        ),
        Klass(
            name="PolygonIterate",
            description="A LEF POLYGON ITERATE statement: a base polygon repeated on a grid (LEF DO/STEP)",
            has_pool=False,
            fields=[
                Field(
                    name="polygon",
                    description="The base polygon (the ix=0, iy=0 instance)",
                    type="Polygon",
                ),
                Field(
                    name="num_x",
                    description="Number of repetitions along x (LEF DO xStart)",
                    type="int",
                    example=3,
                ),
                Field(
                    name="num_y",
                    description="Number of repetitions along y (LEF DO yStart)",
                    type="int",
                    example=2,
                ),
                Field(
                    name="space_x",
                    description="Step between repetitions along x, in database units (LEF STEP xStep)",
                    type="dbu",
                    example=10000,
                ),
                Field(
                    name="space_y",
                    description="Step between repetitions along y, in database units (LEF STEP yStep)",
                    type="dbu",
                    example=20000,
                ),
            ],
        ),
        Klass(
            name="Text",
            description="A text object used to label items in a layout",
            has_pool=False,
            fields=[
                Field(
                    name="label",
                    description="The text value",
                    type="str",
                    example="VDD",
                ),
                Field(
                    name="location",
                    description="The text location",
                    type="Point",
                ),
                Field(
                    name="size",
                    description="Local width of the shape geometry at the label's location, in database units, used to size the rendered text",
                    type="double",
                    example=200.0,
                ),
            ],
        ),
        Klass(
            # Owned by exactly one of TerminalPort/Obstruction/
            # PhysicalPortSegment/Blockage/Route/Layout/Abstract/
            # in_abstract/in_layout (mutually exclusive - exactly one of
            # these parent fields is ever set on a given Shape). layout/
            # abstract are singular, non-list owners (Layout.diearea,
            # Abstract.boundary); in_abstract/in_layout are the list-typed
            # free-standing-shape owners (Abstract.free_shapes,
            # Layout.free_shapes) - two relationships between the same
            # class pair, which codegen's Klass.link() tells apart by each
            # field's own `parent=` name, not by type alone.
            name="Shape",
            description="A shape on a layer.",
            has_pool=True,
            compact_lists=True,
            fields=[
                Field(
                    name="terminal_port",
                    description="Owning TerminalPort, if this Shape belongs to one",
                    type="TerminalPort",
                    parent="shapes",
                    owner=True,
                ),
                Field(
                    name="obstruction",
                    description="Owning Obstruction, if this Shape belongs to one",
                    type="Obstruction",
                    parent="shapes",
                    owner=True,
                ),
                Field(
                    name="physical_port_segment",
                    description="Owning PhysicalPortSegment, if this Shape belongs to one",
                    type="PhysicalPortSegment",
                    parent="shapes",
                    owner=True,
                ),
                Field(
                    name="blockage",
                    description="Owning Blockage, if this Shape belongs to one",
                    type="Blockage",
                    parent="shapes",
                    owner=True,
                ),
                Field(
                    name="route",
                    description="Owning Route, if this Shape belongs to one",
                    type="Route",
                    parent="shapes",
                    owner=True,
                ),
                Field(
                    name="layout",
                    description="Owning Layout, if this Shape is that Layout's diearea",
                    type="Layout",
                    parent="diearea",
                    owner=True,
                ),
                Field(
                    name="abstract",
                    description="Owning Abstract, if this Shape is that Abstract's boundary",
                    type="Abstract",
                    parent="boundary",
                    owner=True,
                ),
                Field(
                    name="in_abstract",
                    description="Owning Abstract, if this is a free-standing Shape held directly by that Abstract (Abstract.free_shapes) rather than by one of its Terminals/Obstructions - e.g. the output of a shape_or/shape_copy/... TCL command. Distinct from abstract above (that Abstract's one boundary Shape). Never written by write_lef, which only walks an Abstract's Terminals/Obstructions.",
                    type="Abstract",
                    parent="free_shapes",
                    owner=True,
                ),
                Field(
                    name="in_layout",
                    description="Owning Layout, if this is a free-standing Shape held directly by that Layout (Layout.free_shapes) rather than by one of its Routes/Blockages/PhysicalPortSegments - e.g. the output of a shape_or/shape_copy/... TCL command. Distinct from layout above (that Layout's one diearea Shape). Never written by write_def, which only walks a Layout's own named DEF sections.",
                    type="Layout",
                    parent="free_shapes",
                    owner=True,
                ),
                Field(
                    name="layer",
                    description="The physical layer this shape is on. Exactly one of layer and purpose is set - a shape with no physical layer (a diearea, an abstract boundary, a placement blockage) uses purpose instead. From TCL, a layer:<name> token; create_shape -layer debug means -purpose DEBUG.",
                    type="Layer",
                    is_optional=True,
                    tcl_create_aliases={"debug": {"purpose": "DEBUG"}},
                ),
                Field(
                    name="purpose",
                    description="What this shape is, when it isn't on a physical layer - a boundary, a placement blockage or debug geometry. Exactly one of layer and purpose is set.",
                    type="ShapePurpose",
                    is_optional=True,
                ),
                Field(
                    name="paths",
                    description="A list of paths",
                    type="Path",
                    is_list=True,
                ),
                Field(
                    name="polygons",
                    description="A list of polygons",
                    type="Polygon",
                    is_list=True,
                ),
                Field(
                    name="rects",
                    description="A list of rects",
                    type="Rect",
                    is_list=True,
                ),
                Field(
                    name="rect_iterates",
                    description="A list of RECT ITERATE statements",
                    type="RectIterate",
                    is_list=True,
                    create_excluded=True,  # raw parser/writer artifact, never directly authored - see Field.create_excluded's own docstring
                ),
                Field(
                    name="path_iterates",
                    description="A list of PATH ITERATE statements",
                    type="PathIterate",
                    is_list=True,
                    create_excluded=True,
                ),
                Field(
                    name="polygon_iterates",
                    description="A list of POLYGON ITERATE statements",
                    type="PolygonIterate",
                    is_list=True,
                    create_excluded=True,
                ),
                Field(
                    name="texts",
                    description="A list of texts",
                    type="Text",
                    is_list=True,
                    create_excluded=True,  # Pipeline-computed render-time label, never LEF-authored data
                ),
                Field(
                    name="rect_masks",
                    description="MASK color per entry of rects (LEF RECT MASK n, 5.8) - parallel array, 0 meaning no mask; empty if no rect in this Shape ever set one",
                    type="int",
                    example=0,
                    is_list=True,
                ),
                Field(
                    name="polygon_masks",
                    description="MASK color per entry of polygons (LEF POLYGON MASK n, 5.8) - parallel array, same convention as rect_masks",
                    type="int",
                    example=0,
                    is_list=True,
                ),
                Field(
                    name="path_masks",
                    description="MASK color per entry of paths (LEF PATH MASK n, 5.8) - parallel array, same convention as rect_masks",
                    type="int",
                    example=0,
                    is_list=True,
                ),
                Field(
                    name="spacing",
                    description="LEF LAYER ... SPACING (OBS/PORT minimum-spacing override), in database units - the layer's own rules apply if not given. Mutually exclusive with design_rule_width.",
                    type="dbu",
                    example=1500,
                    is_optional=True,
                ),
                Field(
                    name="design_rule_width",
                    description="LEF LAYER ... DESIGNRULEWIDTH, in database units. Mutually exclusive with spacing.",
                    type="dbu",
                    example=1500,
                    is_optional=True,
                ),
                Field(
                    name="except_pg_net",
                    description="LEF LAYER ... EXCEPTPGNET (5.7) - write_lef writes it for OBS only",
                    type="bool",
                    example=False,
                ),
                Field(
                    name="vias",
                    description="VIA placements within this LAYER occurrence",
                    type="ShapeVia",
                    is_list=True,
                ),
                Field(
                    name="via_iterates",
                    description="VIA ITERATE placements within this LAYER occurrence",
                    type="ShapeViaIterate",
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="ShapeVia",
            description="One VIA placement within a Shape's LAYER occurrence (LEF PORT/OBS VIA, or a DEF NETS/SPECIALNETS routed path's own simple, non-arrayed VIA placement)",
            has_pool=False,
            fields=[
                Field(name="via_name", description="The name of the via", type="str", example="VIA12"),
                Field(name="origin", description="In database units", type="Point"),
                Field(name="orientation", description="The orientation of a DEF routed-path VIA placement (e.g. \"M1_M2 FN\") - LEF vias have no orientation", type="Orientation", is_optional=True),
                Field(name="mask", description="MASK color (5.8) - a combined up-to-3-digit number: top*100 + cut*10 + bottom", type="int", example=0, is_optional=True),
                Field(name="width", description="The width of the DEF routed path at this via, in database units - used to size a VIARULE GENERATE via's cut array; not set for a LEF PORT/OBS via", type="dbu", is_optional=True),
            ],
        ),
        Klass(
            # Stored as-is (same convention as RectIterate/PathIterate/
            # PolygonIterate) - expanded at render time by
            # pipelines/via_shapes.hpp. Also covers a DEF
            # NETS/SPECIALNETS routed path's own arrayed VIA placement
            # ("VIA DO n BY m STEP x y") - structurally the same shape as
            # LEF's VIA ITERATE.
            name="ShapeViaIterate",
            description="One raw VIA ITERATE statement within a Shape's LAYER occurrence, or a routed path's own arrayed VIA placement.",
            has_pool=False,
            fields=[
                Field(name="via_name", description="The name of the via", type="str", example="VIA12"),
                Field(name="origin", description="In database units", type="Point"),
                Field(name="orientation", description="Same as ShapeVia.orientation", type="Orientation", is_optional=True),
                Field(name="num_x", description="LEF DO n", type="int", example=2),
                Field(name="num_y", description="LEF BY m", type="int", example=2),
                Field(name="space_x", description="LEF STEP x, in database units", type="dbu"),
                Field(name="space_y", description="LEF STEP y, in database units", type="dbu"),
                Field(name="mask", description="MASK color (5.8) - same combined-digit convention as ShapeVia.mask", type="int", example=0, is_optional=True),
                Field(name="width", description="Same as ShapeVia.width - the width of the DEF routed path at this via array; not set for a LEF VIA ITERATE", type="dbu", is_optional=True),
            ],
        ),
        Klass(
            name="Orientation",
            description="Placement orientations, e.g. N, S, E, W, FN, FS, FE, FW",
            is_enum=True,
            has_pool=False,
            fields=[
                Field(
                    name="N",
                    description="North orientation",
                    type="int",
                    value=0,
                ),
                Field(
                    name="S",
                    description="South orientation",
                    type="int",
                    value=2,
                ),
                Field(
                    name="E",
                    description="East orientation",
                    type="int",
                    value=3,
                ),
                Field(
                    name="W",
                    description="West orientation",
                    type="int",
                    value=1,
                ),
                Field(
                    name="FN",
                    description="Flip-North orientation",
                    type="int",
                    value=4,
                ),
                Field(
                    name="FS",
                    description="Flip-South orientation",
                    type="int",
                    value=6,
                ),
                Field(
                    name="FE",
                    description="Flip-East orientation",
                    type="int",
                    value=7,
                ),
                Field(
                    name="FW",
                    description="Flip-West orientation",
                    type="int",
                    value=5,
                ),
            ],
        ),
        Klass(
            name="Library",
            description="A library containing designs",
            fields=[
                Field(
                    name="name",
                    description="The name of the library",
                    type="str",
                    example="my_library",
                    index=True,
                ),
                Field(
                    name="designs",
                    description="Designs contained in this library",
                    type="Design",
                    is_list=True,
                    is_child=True,
                ),
            ],
        ),
        Klass(
            name="Design",
            description="A library cell or logical module",
            fields=[
                Field(
                    name="library",
                    description="Parent library",
                    type="Library",
                    parent="designs",
                ),
                Field(
                    name="name",
                    description="The name of the design, unique within its library",
                    type="str",
                    example="top_level",
                    index=True,
                    unique_per_parent=True,
                ),
                Field(
                    name="abstract",
                    description="The abstract view",
                    type="Abstract",
                    is_child=True,
                ),
                Field(
                    name="schematic",
                    description="The schematic view",
                    type="Schematic",
                    is_child=True,
                ),
                Field(
                    name="layout",
                    description="The physical layout view (DEF)",
                    type="Layout",
                    is_child=True,
                ),
            ],
        ),
        Klass(
            name="Abstract",
            description="A physical abstract view (LEF)",
            # "Current view" anchor for get_terminals/get_terminal_ports/
            # get_obstructions/get_shapes' own default (-of omitted) scope
            # - see codegen/codegen/tcl_scope.py. Independent of LeHandle's
            # own hand-written current_abstract_ (GUI rendering state) -
            # deliberately not bridged, see CLAUDE.md's TCL
            # codegen section.
            has_current_access=True,
            fields=[
                Field(
                    name="design",
                    description="Parent design",
                    type="Design",
                    parent="abstract",
                ),
                Field(
                    name="type",
                    description="Type or class of abstract, e.g. CORE, PAD, SPACER, ENDCAP, COVER etc (LEF MACRO CLASS)",
                    type="str",
                    example="CORE",
                    is_optional=True,
                ),
                Field(
                    name="foreigns",
                    description="Information used to determine the abstract location and orientation wrt to the layout",
                    type="Foreign",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="size",
                    description="The width and height of the block",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="origin",
                    description="The origin of the block",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="bbox",
                    description="The bbox of the boundary",
                    type="Rect",
                    is_optional=True,
                ),
                Field(
                    name="boundary",
                    description="The boundary of the abstract, as a Shape with purpose=BOUNDARY (possibly several polygons). If the block is rectilinear, this matches the OVERLAP layer; otherwise it's the same as bbox.",
                    type="Shape",
                    is_child=True,
                    is_optional=True,
                ),
                Field(
                    name="free_shapes",
                    description="Free-standing Shapes held directly by this Abstract (Shape.in_abstract), not owned by any Terminal/Obstruction - e.g. results of the shape_* TCL commands (shape_or, shape_copy, ...). Not written by write_lef.",
                    type="Shape",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="symmetry",
                    description="The symmetry of the abstract",
                    type="Symmetry",
                    is_optional=True,
                ),
                Field(
                    name="site",
                    description="The name of the SITE - mutually exclusive with site_placements",
                    type="str",
                    example="MYSITE",
                    is_optional=True,
                ),
                Field(
                    name="site_placements",
                    description="LEF MACRO SITE array placements (multiple per macro) - mutually exclusive with site",
                    type="MacroSitePlacement",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="terminals",
                    description="A list of physical terminals",
                    type="Terminal",
                    is_child=True,
                    is_list=True,
                ),
                Field(
                    name="obstructions",
                    description="A list of obstructions",
                    type="Obstruction",
                    is_child=True,
                    is_list=True,
                ),
                Field(
                    name="eeq",
                    description="Electrically-equivalent macro name (LEF EEQ)",
                    type="str",
                    example="BUFX2",
                    is_optional=True,
                ),
                Field(
                    name="leq",
                    description="Logically-equivalent macro name (LEF LEQ)",
                    type="str",
                    example="BUFX2",
                    is_optional=True,
                ),
                Field(
                    name="power",
                    description="Macro-level power consumption, in the LEF file's own declared units (LEF POWER)",
                    type="double",
                    example=0.5,
                    is_optional=True,
                ),
                Field(
                    name="source",
                    description="How this macro was created - USER, GENERATE or BLOCK (LEF SOURCE, obsolete since 5.6 but still parsed)",
                    type="str",
                    example="USER",
                    is_optional=True,
                ),
                Field(
                    name="is_fixed_mask",
                    description="Whether FIXEDMASK was specified (LEF 5.8)",
                    type="bool",
                    example=False,
                ),
                Field(
                    name="densities",
                    description="Per-layer DENSITY rects (LEF MACRO-level DENSITY)",
                    type="MacroDensityLayer",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="properties",
                    description="PROPERTY attachments (LEF PROPERTY)",
                    type="LefProperty",
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="MacroDensityLayer",
            description="One layer's DENSITY rects within a MACRO's DENSITY statement",
            fields=[
                Field(name="abstract", description="Parent abstract", type="Abstract", parent="densities"),
                Field(name="layer_name", description="The name of the layer", type="str", example="M1"),
                Field(name="rects", description="The rects this layer's density values apply to", type="Rect", is_list=True, create_excluded=True),  # structurally list_compound-eligible but deferred - this round scoped to Shape.rects/polygons/paths only
                Field(name="values", description="The density percentage per rect (parallel to rects)", type="double", example=45.0, is_list=True),
            ],
        ),
        Klass(
            name="Foreign",
            description="A design abstract view foreign definition. Owned by exactly one of Via/NonDefaultRuleVia/Abstract/LayoutVia (mutually exclusive - Abstract can hold several, the others at most one)",
            fields=[
                Field(
                    name="via",
                    description="Owning Via, if this Foreign belongs to one",
                    type="Via",
                    parent="foreign",
                ),
                Field(
                    name="non_default_rule_via",
                    description="Owning NonDefaultRuleVia, if this Foreign belongs to one",
                    type="NonDefaultRuleVia",
                    parent="foreign",
                ),
                Field(
                    name="abstract",
                    description="Owning Abstract, if this Foreign belongs to one",
                    type="Abstract",
                    parent="foreigns",
                ),
                Field(
                    name="layout_via",
                    description="Owning LayoutVia, if this Foreign belongs to one",
                    type="LayoutVia",
                    parent="foreign",
                ),
                Field(
                    name="name",
                    description="The foreign cell name (usually the same as the design name)",
                    type="str",
                    example="BUFX1",
                ),
                Field(
                    name="origin",
                    description="The foreign origin - optional in LEF FOREIGN; if not given, no point was written (it is not (0,0))",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="orient",
                    description="The foreign orientation - also optional independently of origin ('FOREIGN name ( x y ) ;' with no orientation is legal)",
                    type="Orientation",
                    is_optional=True,
                ),
            ],
        ),
        Klass(
            name="Symmetry",
            description="Specifies if this design can be flipped and/or rotated",
            has_pool=False,
            fields=[
                Field(
                    name="r90",
                    description="The design can be rotated by 90 degrees",
                    type="bool",
                    example=True,
                ),
                Field(
                    name="x",
                    description="The design can be flipped horizontally",
                    type="bool",
                    example=True,
                ),
                Field(
                    name="y",
                    description="The design can be flipped vertically",
                    type="bool",
                    example=True,
                ),
            ],
        ),
        Klass(
            name="Terminal",
            description="Top-level pin name of an abstract",
            # Name uniqueness is enforced per-Abstract by the generated
            # unique_per_parent index (create_terminal returns an invalid
            # id on collision) rather than a global codegen index=True - see
            # Field.unique_per_parent's own docstring and le_tcl_shim.hpp's
            # own "IDs" comment. tcl_id_field is still needed even though
            # `name` is now index=True: tcl_indexed_id_field() deliberately
            # excludes unique_per_parent fields (no *global* Root lookup
            # exists for them), so the TCL generator still can't auto-derive
            # a name-based friendly id the way it does for a plain index=True
            # field (Layer, Via, Site, ...) - resolve_terminal_id stays
            # hand-written.
            tcl_id_field="name",
            fields=[
                Field(
                    name="abstract",
                    description="Parent abstract",
                    type="Abstract",
                    parent="terminals",
                ),
                Field(
                    name="name",
                    description="Name of the terminal - unique within its abstract",
                    type="str",
                    example="INPUT",
                    index=True,
                    unique_per_parent=True,
                ),
                Field(
                    name="direction",
                    description="The direction of the terminal",
                    type="SignalDirection",
                ),
                Field(
                    name="ports",
                    description="Physical ports",
                    type="TerminalPort",
                    is_child=True,
                    is_list=True,
                ),
                Field(
                    name="shape",
                    description="ABUTMENT, RING or FEEDTHRU (LEF PIN SHAPE)",
                    type="str",
                    example="ABUTMENT",
                    is_optional=True,
                ),
                Field(
                    name="use",
                    description="SIGNAL, POWER, GROUND, CLOCK, TIEOFF, ANALOG or SCAN (LEF PIN USE)",
                    type="str",
                    example="SIGNAL",
                    is_optional=True,
                ),
                Field(
                    name="must_join",
                    description="The name of another pin this one must be joined with (LEF PIN MUSTJOIN)",
                    type="str",
                    example="A2",
                    is_optional=True,
                ),
                Field(
                    name="net_expr",
                    description="A verbatim NETEXPR string (LEF 5.6), round-tripped as-is rather than parsed",
                    type="str",
                    example="A (A1 A2)",
                    is_optional=True,
                ),
                Field(
                    name="leq",
                    description="This pin's own logically-equivalent-pin name (LEF PIN LEQ) - distinct from the macro-level Abstract.leq",
                    type="str",
                    example="A2",
                    is_optional=True,
                ),
                Field(
                    name="properties",
                    description="PROPERTY attachments (LEF PROPERTY)",
                    type="LefProperty",
                    is_list=True,
                ),
                Field(
                    name="antenna_partial_metal_area",
                    description="LEF PIN ANTENNAPARTIALMETALAREA entries (5.4)",
                    type="PinAntennaValue",
                    is_list=True,
                ),
                Field(
                    name="antenna_partial_metal_side_area",
                    description="LEF PIN ANTENNAPARTIALMETALSIDEAREA entries (5.4)",
                    type="PinAntennaValue",
                    is_list=True,
                ),
                Field(
                    name="antenna_partial_cut_area",
                    description="LEF PIN ANTENNAPARTIALCUTAREA entries (5.4)",
                    type="PinAntennaValue",
                    is_list=True,
                ),
                Field(
                    name="antenna_diff_area",
                    description="LEF PIN ANTENNADIFFAREA entries (5.4)",
                    type="PinAntennaValue",
                    is_list=True,
                ),
                Field(
                    name="antenna_models",
                    description="Antenna diffusion models, one per OXIDE1-4 (LEF PIN ANTENNAMODEL, 5.5) - a distinct, narrower shape than Layer.antenna_models",
                    type="PinAntennaModel",
                    is_list=True,
                    is_child=True,
                ),
                Field(name="taper_rule", description="LEF PIN TAPERRULE", type="str", example="RULE1", is_optional=True),
                Field(name="supply_sensitivity", description="LEF PIN SUPPLYSENSITIVITY (5.6) - net name", type="str", example="vddpin1", is_optional=True),
                Field(name="ground_sensitivity", description="LEF PIN GROUNDSENSITIVITY (5.6) - net name", type="str", example="gndpin", is_optional=True),
                Field(name="rise_slew_limit", description="LEF PIN RISESLEWLIMIT, in the LEF file's declared units - read-only, not written back by write_lef", type="double", example=0.01, is_optional=True),
                Field(name="fall_slew_limit", description="LEF PIN FALLSLEWLIMIT, in the LEF file's declared units - read-only, not written back by write_lef", type="double", example=0.02, is_optional=True),
                Field(name="max_load", description="LEF PIN MAXLOAD, in the LEF file's declared units - read-only, not written back by write_lef", type="double", example=0.1, is_optional=True),
                Field(name="max_delay", description="LEF PIN MAXDELAY, in the LEF file's declared units - read-only, not written back by write_lef", type="double", example=21.0, is_optional=True),
            ],
        ),
        Klass(
            name="PinAntennaValue",
            description="One (value, layer) entry of a PIN's flat (non-oxide) antenna field",
            has_pool=False,
            fields=[
                Field(name="value", description="The antenna value", type="double", example=1.5),
                Field(name="layer_name", description="The layer this value applies to (LEF LAYER), if given", type="str", example="M1", is_optional=True),
            ],
        ),
        Klass(
            name="PinAntennaModel",
            description="One LEF PIN ANTENNAMODEL OXIDE1-4 block - each field is a list of (value, layer) entries",
            fields=[
                Field(name="terminal", description="Parent terminal", type="Terminal", parent="antenna_models"),
                Field(name="oxide", description="OXIDE1, OXIDE2, OXIDE3, or OXIDE4", type="str", example="OXIDE1"),
                Field(name="gate_area", description="LEF ANTENNAGATEAREA entries", type="PinAntennaValue", is_list=True),
                Field(name="max_area_car", description="LEF ANTENNAMAXAREACAR entries", type="PinAntennaValue", is_list=True),
                Field(name="max_side_area_car", description="LEF ANTENNAMAXSIDEAREACAR entries", type="PinAntennaValue", is_list=True),
                Field(name="max_cut_car", description="LEF ANTENNAMAXCUTCAR entries", type="PinAntennaValue", is_list=True),
            ],
        ),
        Klass(
            name="TerminalPort",
            description="Physical connection for a Terminal",
            fields=[
                Field(
                    name="terminal",
                    description="Parent terminal",
                    type="Terminal",
                    parent="ports",
                ),
                Field(
                    name="shapes",
                    description="The terminal port shapes",
                    type="Shape",
                    is_list=True,
                    is_child=True,
                ),
                Field(name="port_class", description="LEF PORT CLASS (NONE/CORE/BUMP)", type="str", example="BUMP", is_optional=True),
            ],
        ),
        Klass(
            name="Obstruction",
            description="An abstract routing blockage",
            fields=[
                Field(
                    name="abstract",
                    description="Parent abstract",
                    parent="obstructions",
                    type="Abstract",
                ),
                Field(
                    name="shapes",
                    description="The obstruction shapes",
                    type="Shape",
                    is_list=True,
                    is_child=True,
                ),
            ],
        ),
        Klass(
            name="Schematic",
            description="A logical connectivity view (netlist)",
            # "Current view" anchor for get_instances/get_ports/get_nets'
            # own default (-of omitted) scope - see
            # codegen/codegen/tcl_scope.py. Populated by the SystemVerilog/
            # Verilog reader (src/io/sv_reader.cpp).
            has_current_access=True,
            fields=[
                Field(
                    name="design",
                    description="Parent design",
                    type="Design",
                    parent="schematic",
                ),
                Field(
                    name="instances",
                    description="Instances contained in this design",
                    type="Instance",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="ports",
                    description="Top-level ports (Verilog module input/output/inout)",
                    type="Port",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="nets",
                    description="Internal connectivity nets (Verilog wire/reg/logic used for connectivity)",
                    type="Net",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="net_buses",
                    description="Multi-bit bus groupings of this Schematic's own per-bit Nets (see NetBus)",
                    type="NetBus",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="port_buses",
                    description="Multi-bit bus groupings of this Schematic's own per-bit Ports (see PortBus)",
                    type="PortBus",
                    is_list=True,
                    is_child=True,
                ),
            ],
        ),
        # An Instance may also stand in for source code that could not be
        # fully read (a "logic cloud" - see rtl_text below). This is
        # deliberately not a separate klass: it keeps every connectivity
        # walk (Net -> Pin -> Instance) a single, uniform edge, whether the
        # Instance is a fully resolved reference, a reference to a design
        # that hasn't been read yet, or unreadable source.
        Klass(
            name="Instance",
            description="An instance of another design, or a placeholder for source code that could not be fully read",
            # Name uniqueness is enforced per-Schematic by the generated
            # unique_per_parent index (create_instance returns an invalid
            # id on collision), same shape as Port.name/Net.name on this
            # same parent klass - see Field.unique_per_parent's own
            # docstring and le_tcl_shim.hpp's own "IDs" comment.
            # tcl_id_field is still needed even though `name` is now
            # index=True: tcl_indexed_id_field() deliberately excludes
            # unique_per_parent fields (no *global* Root lookup exists for
            # them), so the TCL generator still can't auto-derive a
            # name-based friendly id the way it does for a plain
            # index=True field - resolve_instance_id stays hand-written
            # (mirrors resolve_port_id/resolve_net_id).
            tcl_id_field="name",
            fields=[
                Field(
                    name="schematic",
                    description="Parent schematic",
                    type="Schematic",
                    parent="instances",
                ),
                Field(
                    name="name",
                    description="The name of the instance - unique within its schematic",
                    type="str",
                    example="U1",
                    index=True,
                    unique_per_parent=True,
                ),
                Field(
                    name="reference_name",
                    description="The name of the referenced design, if known",
                    type="str",
                    example="BUFX1",
                    is_optional=True,
                ),
                Field(
                    name="reference_design",
                    description="The referenced design, once resolved - never set for a placeholder instance (see rtl_text)",
                    type="Design",
                    is_optional=True,
                ),
                Field(
                    name="location",
                    description="The location of the lower-left corner of this instance",
                    type="Point",
                    is_optional=True,
                ),
                Field(
                    name="pins",
                    description="Pin connections on this instance",
                    type="Pin",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="rtl_text",
                    description="The original source text, if this instance is a placeholder for source code that could not be fully read",
                    type="str",
                    is_optional=True,
                ),
                Field(
                    name="source_file",
                    description="The file rtl_text came from, if known",
                    type="str",
                    example="cpu_core.sv",
                    is_optional=True,
                ),
                Field(
                    name="diagnostic_summary",
                    description="A short explanation of why this instance's source could not be fully read, if available",
                    type="str",
                    is_optional=True,
                ),
            ],
        ),
        # A multi-bit Verilog port ([msb:lsb]) has no Port object of its
        # own - it's fully replaced by one Port per bit (see Port.bus/
        # .bit_index below), each named with the DEF-style bracketed form
        # ("address[7]") so DEF PhysicalPort/Route hierarchical name
        # matching finds it directly, with
        # zero bit-select-aware logic anywhere in that matching code.
        # PortBus is purely the grouping/introspection record (msb/lsb,
        # and the bus's own bit-Ports found by querying Port.bus == this
        # id - Klass field lists can't hold a plain reference list, see
        # this schema's own is_plain_reference_field precedent elsewhere,
        # so the reference runs Port -> PortBus, not the other way).
        Klass(
            name="PortBus",
            description="A multi-bit bus grouping of a Schematic's own per-bit Ports (Verilog module port [msb:lsb])",
            fields=[
                Field(
                    name="schematic",
                    description="Parent schematic",
                    type="Schematic",
                    parent="port_buses",
                ),
                Field(
                    name="name",
                    description="The bus's own base name (without a bit index) - unique within its schematic",
                    type="str",
                    example="address",
                    index=True,
                    unique_per_parent=True,
                ),
                Field(
                    name="msb",
                    description="Most-significant bit index (Verilog [msb:lsb])",
                    type="int",
                    example=7,
                ),
                Field(
                    name="lsb",
                    description="Least-significant bit index",
                    type="int",
                    example=0,
                ),
            ],
        ),
        Klass(
            name="Port",
            description="Logical top-level port of a Schematic (Verilog module input/output/inout) - one Port per bit for a multi-bit port, see .bus",
            fields=[
                Field(
                    name="schematic",
                    description="Parent schematic",
                    type="Schematic",
                    parent="ports",
                ),
                Field(
                    name="name",
                    description="The name of the port - the DEF-style bracketed form (\"address[7]\") for one bit of a multi-bit port, plain for a scalar port - unique within its schematic",
                    type="str",
                    example="clk",
                    index=True,
                    unique_per_parent=True,
                ),
                Field(
                    name="direction",
                    description="The direction of the port",
                    type="SignalDirection",
                ),
                Field(
                    name="bus",
                    description="The PortBus this Port is one bit of, if it is part of a multi-bit port",
                    type="PortBus",
                    is_optional=True,
                ),
                Field(
                    name="bit_index",
                    description="Which bit of .bus this Port represents",
                    type="int",
                    example=7,
                    is_optional=True,
                ),
                Field(
                    name="net",
                    description="The net this port corresponds to, if any (Verilog gives every port an implicit net of the same name) - the matching per-bit Net for a multi-bit port",
                    type="Net",
                    is_optional=True,
                ),
            ],
        ),
        # Mirrors PortBus above, for Net - see PortBus's own comment for
        # the full rationale (DEF-matchable bracketed names, reference
        # direction, why there's no stored bit-list on NetBus itself).
        Klass(
            name="NetBus",
            description="A multi-bit bus grouping of a Schematic's own per-bit Nets (Verilog wire/reg/logic [msb:lsb])",
            fields=[
                Field(
                    name="schematic",
                    description="Parent schematic",
                    type="Schematic",
                    parent="net_buses",
                ),
                Field(
                    name="name",
                    description="The bus's own base name (without a bit index) - unique within its schematic",
                    type="str",
                    example="address",
                    index=True,
                    unique_per_parent=True,
                ),
                Field(
                    name="msb",
                    description="Most-significant bit index (Verilog [msb:lsb])",
                    type="int",
                    example=3,
                ),
                Field(
                    name="lsb",
                    description="Least-significant bit index",
                    type="int",
                    example=0,
                ),
            ],
        ),
        Klass(
            name="Net",
            description="Logical connectivity net within a Schematic (Verilog wire/reg/logic) - one Net per bit for a multi-bit net, see .bus",
            fields=[
                Field(
                    name="schematic",
                    description="Parent schematic",
                    type="Schematic",
                    parent="nets",
                ),
                Field(
                    name="name",
                    description="The name of the net - the DEF-style bracketed form (\"address[7]\") for one bit of a multi-bit net, plain for a scalar net - unique within its schematic",
                    type="str",
                    example="n42",
                    index=True,
                    unique_per_parent=True,
                ),
                Field(
                    name="bus",
                    description="The NetBus this Net is one bit of, if it is part of a multi-bit net",
                    type="NetBus",
                    is_optional=True,
                ),
                Field(
                    name="bit_index",
                    description="Which bit of .bus this Net represents",
                    type="int",
                    example=3,
                    is_optional=True,
                ),
            ],
        ),
        Klass(
            name="Pin",
            description="Logical connection point on an Instance (Verilog instance port connection)",
            fields=[
                Field(
                    name="instance",
                    description="Parent instance",
                    type="Instance",
                    parent="pins",
                ),
                Field(
                    name="name",
                    description="The pin name as connected in source, e.g. the A in .A(net23)",
                    type="str",
                    example="A",
                ),
                Field(
                    name="direction",
                    description="The pin's direction, if known",
                    type="SignalDirection",
                    is_optional=True,
                ),
                Field(
                    name="net",
                    description="The net this pin connects to, if any - a connection to one bit of a multi-bit net (e.g. .A(bus[2])) is that bit's Net (see Net.bus/.bit_index)",
                    type="Net",
                    is_optional=True,
                ),
                Field(
                    name="raw_expression",
                    description="The connection expression exactly as written in the source",
                    type="str",
                    example="{a, b}",
                    is_optional=True,
                ),
            ],
        ),
        Klass(
            name="SignalDirection",
            description="The direction of a signal such as a port or terminal",
            is_enum=True,
            has_pool=False,
            fields=[
                Field(
                    name="INPUT",
                    description="Input signal",
                    type="int",
                    value=0,
                ),
                Field(
                    name="OUTPUT",
                    description="Output signal",
                    type="int",
                    value=1,
                ),
                Field(
                    name="INOUT",
                    description="In/out signal",
                    type="int",
                    value=2,
                ),
                Field(
                    name="NONE",
                    description="No direction specified",
                    type="int",
                    value=3,
                ),
                Field(
                    name="OUTPUT_TRISTATE",
                    description="Tristate output signal (LEF DIRECTION OUTPUT TRISTATE)",
                    type="int",
                    value=4,
                ),
                Field(
                    name="FEEDTHRU",
                    description="Feedthrough signal (LEF DIRECTION FEEDTHRU)",
                    type="int",
                    value=5,
                ),
            ],
        ),
        Klass(
            name="PlacementStatus",
            description="Placement status of a Component or Pin (DEF COMPONENTS/PINS STATUS)",
            is_enum=True,
            has_pool=False,
            fields=[
                Field(name="UNPLACED", description="Not yet placed", type="int", value=0),
                Field(name="PLACED", description="Placed, movable", type="int", value=1),
                Field(name="FIXED", description="Placed, fixed", type="int", value=2),
                Field(name="COVER", description="Placed, cover (fills the area beneath it)", type="int", value=3),
                Field(name="SOFTFIXED", description="Placed, soft-fixed (DEF SOFTFIXED, components only)", type="int", value=4),
            ],
        ),
        Klass(
            name="BlockageKind",
            description="Whether a Blockage is a routing-layer blockage or a placement blockage (DEF BLOCKAGES LAYER/PLACEMENT)",
            is_enum=True,
            has_pool=False,
            fields=[
                Field(name="ROUTING", description="A routing-layer blockage (DEF BLOCKAGES LAYER)", type="int", value=0),
                Field(name="PLACEMENT", description="A placement blockage (DEF BLOCKAGES PLACEMENT)", type="int", value=1),
            ],
        ),
        # Mirrors src/pipelines/view_style.hpp's ViewLayerPurpose (the rendering-layer
        # concept), but persisted here on Shape.purpose itself rather than
        # derived at render time. Exactly one of Shape.layer/Shape.purpose
        # is ever set on a given Shape - documented convention, not
        # database-enforced, same as e.g. Blockage.spacing/
        # design_rule_width's own precedent.
        Klass(
            name="ShapePurpose",
            description="A role a Shape can play instead of sitting on a real routing Layer, for Shapes that aren't real LEF/DEF geometry.",
            is_enum=True,
            has_pool=False,
            fields=[
                Field(name="BOUNDARY", description="Abstract.boundary / Layout.diearea - the design/macro's own outline", type="int", value=0),
                Field(name="PLACEMENT_BLOCKAGE", description="A DEF PLACEMENT blockage's region (a ROUTING blockage is on a physical layer instead)", type="int", value=1),
                Field(name="DEBUG", description="User debug output (e.g. shape_or ... -layer debug) - drawn on top of everything else, never written by write_lef/write_def", type="int", value=2),
            ],
        ),
        Klass(
            # Net *connectivity* (which component pins a net connects) is
            # deferred to when SystemVerilog/Schematic linking lands - Net
            # here only holds routing geometry.
            name="Layout",
            description="A physical layout view (DEF) - placed components, rows, tracks, blockages, routed net geometry, etc.",
            has_current_access=True,
            fields=[
                Field(name="design", description="Parent design", type="Design", parent="layout"),
                Field(name="diearea", description="The chip/block boundary (DEF DIEAREA), as a Shape with purpose=BOUNDARY (not necessarily a plain 2-point rectangle - DEF 5.6+ allows more)", type="Shape", is_child=True, is_optional=True),
                Field(name="free_shapes", description="Free-standing Shapes held directly by this Layout (Shape.in_layout), not owned by any Route/Blockage/PhysicalPortSegment - e.g. results of the shape_* TCL commands (shape_or, shape_copy, ...). Not written by write_def.", type="Shape", is_list=True, is_child=True),
                Field(name="rows", description="Placement rows (DEF ROW)", type="Row", is_list=True, is_child=True),
                Field(name="tracks", description="Routing track patterns (DEF TRACKS)", type="Track", is_list=True, is_child=True),
                Field(name="gcell_grids", description="Global-routing gcell grid lines (DEF GCELLGRID)", type="GCellGrid", is_list=True, is_child=True),
                Field(name="placements", description="Placed instances (DEF COMPONENTS)", type="Placement", is_list=True, is_child=True),
                Field(name="physical_ports", description="Chip-boundary I/O pins (DEF PINS)", type="PhysicalPort", is_list=True, is_child=True),
                Field(name="blockages", description="Routing and placement blockages (DEF BLOCKAGES)", type="Blockage", is_list=True, is_child=True),
                Field(name="vias", description="Design-scoped named vias (DEF VIAS)", type="LayoutVia", is_list=True, is_child=True),
                Field(name="routes", description="Regular and special net routing geometry (DEF NETS/SPECIALNETS)", type="Route", is_list=True, is_child=True),
                Field(name="regions", description="Placement regions (DEF REGIONS)", type="Region", is_list=True, is_child=True),
            ],
        ),
        Klass(
            name="Row",
            description="A placement row (DEF ROW)",
            fields=[
                Field(name="layout", description="Parent layout", type="Layout", parent="rows"),
                Field(name="name", description="The name of the row", type="str", example="ROW_0", index=True, unique_per_parent=True),
                Field(name="site_name", description="The name of the site this row is built from, as read (DEF ROW macro name) - a plain name, not a reference to a Site", type="str", example="CORE"),
                Field(name="origin", description="The row's origin, in database units", type="Point", is_optional=True),
                Field(name="orientation", description="The row's orientation", type="Orientation"),
                Field(name="num_x", description="Number of site repeats in X (DEF ROW DO n)", type="int", example=100, is_optional=True),
                Field(name="num_y", description="Number of site repeats in Y (DEF ROW BY m)", type="int", example=1, is_optional=True),
                Field(name="step_x", description="Step in X between repeats, in database units (DEF ROW STEP)", type="dbu", is_optional=True),
                Field(name="step_y", description="Step in Y between repeats, in database units (DEF ROW STEP)", type="dbu", is_optional=True),
            ],
        ),
        Klass(
            name="Track",
            description="A routing track pattern (DEF TRACKS)",
            fields=[
                Field(name="layout", description="Parent layout", type="Layout", parent="tracks"),
                Field(name="is_x", description="True for an X-direction track pattern, false for Y (DEF TRACKS X/Y)", type="bool", example=True),
                Field(name="start", description="Starting coordinate, in database units (DEF TRACKS DO start)", type="dbu"),
                Field(name="count", description="Number of tracks (DEF TRACKS DO ... n)", type="int", example=100),
                Field(name="step", description="Spacing between tracks, in database units (DEF TRACKS STEP)", type="dbu"),
                Field(name="layer_names", description="Layers this track pattern applies to, as read - plain name references, same convention as Abstract.site", type="str", example="M1", is_list=True),
                Field(name="mask", description="MASK color (DEF 5.8)", type="int", example=0, is_optional=True),
                Field(name="same_mask", description="Whether SAMEMASK was specified (DEF 5.8)", type="bool", example=False),
            ],
        ),
        Klass(
            name="GCellGrid",
            description="A global-routing gcell grid line (DEF GCELLGRID)",
            fields=[
                Field(name="layout", description="Parent layout", type="Layout", parent="gcell_grids"),
                Field(name="is_x", description="True for an X-direction grid line, false for Y (DEF GCELLGRID X/Y)", type="bool", example=True),
                Field(name="start", description="Starting coordinate, in database units (DEF GCELLGRID DO start)", type="dbu"),
                Field(name="count", description="Number of grid lines (DEF GCELLGRID DO ... n)", type="int", example=10),
                Field(name="step", description="Spacing between grid lines, in database units (DEF GCELLGRID STEP)", type="dbu"),
            ],
        ),
        # Named Placement (not Component, DEF's own section name) to
        # mirror Net/Route's own logical-vs-physical naming split.
        # `instance`/`physical_only` are the Schematic-linking fields,
        # populated by `link` (SVReader::link_unresolved_instances'
        # own sibling pass, not this reader): a Placement whose DEF name
        # doesn't resolve to any Instance in the sibling Schematic is
        # still created as today, just with `instance` left unset and
        # `physical_only=True` (a real physical-only cell, e.g. a filler/
        # decap with no logical counterpart) rather than treated as an
        # error.
        Klass(
            name="Placement",
            description="A placed physical instance (DEF COMPONENTS).",
            fields=[
                Field(name="layout", description="Parent layout", type="Layout", parent="placements"),
                Field(name="name", description="The name of the instance - unique within its layout", type="str", example="U1", index=True, unique_per_parent=True),
                Field(name="reference_design", description="The reference Design, resolved from the referenced macro/design name at creation time - readers error rather than create a Placement with an unresolved reference", type="Design"),
                Field(name="instance", description="The logical Instance this placement corresponds to, resolved by `link` against the sibling Schematic - never set for a physical_only Placement", type="Instance", is_optional=True),
                Field(name="physical_only", description="Set by `link` when no Instance in the sibling Schematic matches this Placement's name (e.g. a filler/decap cell with no logical counterpart) - not a DEF-native concept, always False until `link` runs", type="bool", example=False),
                Field(name="placement_status", description="Placement status (DEF COMPONENTS FIXED/COVER/PLACED/UNPLACED/SOFTFIXED)", type="PlacementStatus"),
                Field(name="location", description="The location of the lower-left corner of this instance, in database units", type="Point", is_optional=True),
                Field(name="orientation", description="Placement orientation", type="Orientation", is_optional=True),
                Field(name="weight", description="DEF COMPONENTS WEIGHT", type="double", example=1.0, is_optional=True),
                Field(name="source", description="DEF COMPONENTS SOURCE (NETLIST/DIST/USER/TIMING)", type="str", example="NETLIST", is_optional=True),
            ],
        ),
        # Mirrors TerminalPort's own relationship to Terminal, just named
        # to avoid stuttering (PhysicalPort + Port). Unlike TerminalPort,
        # carries its own placement_status/location/orientation - DEF lets
        # each PORT of a multi-port pin be placed independently
        # (setPortPlacement, distinct from the pin's own top-level
        # setPlacement), and in DEF its LAYER/POLYGON coordinates are
        # relative to that placement, not the parent PhysicalPort's (e.g.
        # complete.5.8.def's PIN P0: 3 PORTs, 3 different placements).
        # Unset for the synthetic single segment a
        # pre-5.7 simple (no-PORT-wrapper) pin gets - that case's
        # placement lives on the parent PhysicalPort instead. The Shapes
        # themselves are stored in design coordinates (DEFReader runs
        # them through Geometry::pin_transform of the governing
        # placement; DEFWriter inverts it), so drawing, hit-testing,
        # selection and Move need no pin transform.
        Klass(
            name="PhysicalPortSegment",
            description="One physically separate part of a PhysicalPort (DEF PINS PORT, 5.7+ multi-port pins).",
            fields=[
                Field(name="physical_port", description="Parent physical port", type="PhysicalPort", parent="segments"),
                Field(name="placement_status", description="This segment's own placement status - for a simple (non-multi-port) pin the placement is on the parent PhysicalPort instead", type="PlacementStatus", is_optional=True),
                Field(name="location", description="This segment's own location, in database units", type="Point", is_optional=True),
                Field(name="orientation", description="This segment's own orientation", type="Orientation", is_optional=True),
                Field(name="shapes", description="This segment's shapes", type="Shape", is_list=True, is_child=True),
            ],
        ),
        # Named PhysicalPort (not Pin, DEF's own section name) since
        # Pin is reserved for a different concept (a logical pin on
        # a Schematic Instance, i.e. a Verilog instance pin). `net_name`
        # stays the raw string as read (DEF PINS NET); `net` is the
        # resolved link `link` populates against it, same
        # resolved-alongside-the-raw-string convention Instance's own
        # reference_name/reference_design pair already uses. Unlike
        # Route.net (an ERROR when unresolved), an unresolved
        # PhysicalPort.net only logs a WARNING (boundary power/ground pins
        # legitimately lack a netlist-level Net far more often than a real
        # signal Route does).
        Klass(
            name="PhysicalPort",
            description="A chip-boundary I/O pin (DEF PINS).",
            fields=[
                Field(name="layout", description="Parent layout", type="Layout", parent="physical_ports"),
                Field(name="name", description="The name of the pin - unique within its layout", type="str", example="clk", index=True, unique_per_parent=True),
                Field(name="net_name", description="The name of the net this pin connects to, as read (DEF PINS NET) - see `net` for the resolved link", type="str", example="clk", is_optional=True),
                Field(name="net", description="The logical Net this pin connects to, resolved by `link` against `net_name`", type="Net", is_optional=True),
                Field(name="direction", description="The direction of the pin", type="SignalDirection", is_optional=True),
                Field(name="use", description="SIGNAL, POWER, GROUND, CLOCK, ... (DEF PINS USE)", type="str", example="SIGNAL", is_optional=True),
                Field(name="placement_status", description="Placement status", type="PlacementStatus", is_optional=True),
                Field(name="location", description="The pin's location, in database units", type="Point", is_optional=True),
                Field(name="orientation", description="Placement orientation", type="Orientation", is_optional=True),
                Field(name="segments", description="Physically separate parts (5.7+ multi-port pins)", type="PhysicalPortSegment", is_list=True, is_child=True),
            ],
        ),
        Klass(
            name="Blockage",
            description="A routing or placement blockage (DEF BLOCKAGES)",
            fields=[
                Field(name="layout", description="Parent layout", type="Layout", parent="blockages"),
                Field(name="kind", description="Whether this is a routing-layer or placement blockage", type="BlockageKind"),
                Field(name="layer_name", description="The name of the blocked routing layer, as read - set only for a ROUTING blockage", type="str", example="M1", is_optional=True),
                Field(name="placement", description="The placed instance this blockage is scoped to, if any", type="Placement"),
                Field(name="spacing", description="Minimum spacing override, in database units (DEF BLOCKAGES SPACING) - ROUTING only, mutually exclusive with design_rule_width", type="dbu", is_optional=True),
                Field(name="design_rule_width", description="Effective width for design rule checks, in database units (DEF BLOCKAGES DESIGNRULEWIDTH) - ROUTING only, mutually exclusive with spacing", type="dbu", is_optional=True),
                Field(name="is_soft", description="PLACEMENT ... SOFT - PLACEMENT only", type="bool", example=False),
                Field(name="placement_max_density", description="PLACEMENT ... PARTIAL maxDensity (0-100)", type="double", is_optional=True),
                Field(name="shapes", description="The blockage's geometry", type="Shape", is_list=True, is_child=True),
            ],
        ),
        Klass(
            name="LayoutVia",
            description="A design-scoped named via (DEF VIAS) - distinct from the Technology-level Via since these are per-design and may even shadow a Technology-level via name",
            fields=[
                Field(name="layout", description="Parent layout", type="Layout", parent="vias"),
                Field(name="name", description="The name of the via", type="str", example="VIA1_0", index=True, unique_per_parent=True),
                Field(name="foreign", description="Foreign cell reference (DEF VIAS FOREIGN)", type="Foreign", is_optional=True, is_child=True),
                Field(name="layers", description="Per-layer geometry", type="ViaLayer", is_list=True, is_child=True),
                Field(name="via_rule", description="Reference to a VIARULE with explicit cut geometry (DEF VIAS VIARULE)", type="ViaRuleReference", is_optional=True, is_child=True),
            ],
        ),
        Klass(
            # Named Route rather than Net to reserve the Net name for the
            # future Schematic/SystemVerilog netlist connectivity klass -
            # this klass only holds physical routed geometry, not
            # connectivity. `name` stays the raw string as read; `net` is
            # the resolved link `link` populates against it - unlike
            # PhysicalPort.net, an unresolved Route.net is always an
            # ERROR, since a Route with no logical Net is orphaned
            # physical routing geometry with no meaning.
            name="Route",
            description="The routing geometry of a regular or special net (DEF NETS/SPECIALNETS).",
            fields=[
                Field(name="layout", description="Parent layout", type="Layout", parent="routes"),
                Field(name="name", description="The name of the net this routes, as read - unique within its layout. See net for the resolved link", type="str", example="clk", index=True, unique_per_parent=True),
                Field(name="net", description="The logical Net this routes, resolved by `link` against `name`", type="Net", is_optional=True),
                Field(name="is_special", description="Whether this came from SPECIALNETS rather than NETS", type="bool", example=False),
                Field(name="width", description="Routing width override, in database units (DEF SPECIALNETS WIDTH) - SPECIALNETS only", type="dbu", is_optional=True),
                Field(name="voltage", description="Net voltage (DEF SPECIALNETS VOLTAGE) - SPECIALNETS only", type="double", is_optional=True),
                Field(name="use", description="SIGNAL, POWER, GROUND, CLOCK, ... (DEF NETS/SPECIALNETS USE)", type="str", example="POWER", is_optional=True),
                Field(name="shapes", description="Routed geometry (DEF NETS/SPECIALNETS ROUTED/NEW)", type="Shape", is_list=True, is_child=True),
            ],
        ),
        Klass(
            name="Region",
            description="A placement region (DEF REGIONS)",
            fields=[
                Field(name="layout", description="Parent layout", type="Layout", parent="regions"),
                Field(name="name", description="The name of the region - unique within its layout", type="str", example="region1", index=True, unique_per_parent=True),
                Field(name="region_type", description="FENCE or GUIDE (DEF REGIONS TYPE)", type="str", example="FENCE", is_optional=True),
                Field(name="rects", description="The region's rects (a region can be a multi-rect rectilinear area)", type="Rect", is_list=True),
            ],
        ),
    ],
    # What the renderer draws, each toggled as one column across every layer
    # (codegen --target render). Declaration order is the enum order.
    # has_selectable_objects must match what hit-testing walks (api/hit_test.hpp,
    # api.cpp's placement/row hit-tests); vias follow their owning Shape's purpose.
    purposes=[
        Purpose(name="TERMINAL", label="terminal", description="Pin shapes of cells, and the design's port shapes", has_selectable_objects=True),
        Purpose(name="OBSTRUCTION", label="obstruction", description="Obstruction (OBS) shapes of cells", has_selectable_objects=True),
        Purpose(name="BOUNDARY", label="boundary", description="Cell boundaries and the design's die area", under_placements=True),
        Purpose(name="TRACK_PREFERRED", label="trackPreferred", description="Routing tracks in their layer's preferred direction", visible_by_default=False, selectable_by_default=False, under_placements=True),
        Purpose(name="TRACK_NON_PREFERRED", label="trackNonPreferred", description="Routing tracks against their layer's preferred direction", visible_by_default=False, selectable_by_default=False, under_placements=True),
        Purpose(name="ROUTING_BLOCKAGE", label="routingBlockage", description="Routing blockages on a layer"),
        Purpose(name="ROW", label="row", description="Placement rows", visible_by_default=False, has_selectable_objects=True, under_placements=True),
        Purpose(name="GCELLGRID", label="gcellgrid", description="The global-routing cell grid", visible_by_default=False, selectable_by_default=False, under_placements=True),
        Purpose(name="PLACEMENT_BLOCKAGE", label="placementBlockage", description="Placement blockages", under_placements=True),
        Purpose(name="ROUTE", label="route", description="Routed wires and vias of nets and special nets", has_selectable_objects=True),
        Purpose(name="REGION", label="region", description="Placement regions", under_placements=True),
        Purpose(name="PLACEMENT", label="placement", description="Placed cell outlines and their instance names", has_selectable_objects=True, under_placements=True),
        Purpose(name="CUSTOM_SHAPE", label="customShape", description="Free-standing shapes, such as the results of shape commands"),
        Purpose(name="DEBUG", label="debug", description="Debug shapes, drawn on top of everything in a high-contrast color"),
        Purpose(name="FLIGHTLINE", label="flightline", description="Net connections between the selected cells' pins", visible_by_default=False, selectable_by_default=False),
        Purpose(name="PORT_MARKER", label="portMarker", description="Direction arrows beside each design port"),
    ],
)
