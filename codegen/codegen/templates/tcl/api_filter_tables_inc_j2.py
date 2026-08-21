TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once in api.cpp, replacing the body
// of the hand-written filter_field_tables() (still hand-written: the
// FilterFieldTable struct itself, validate_filter_path/
// validate_filter_expr/parse_and_validate_filter which consume this
// table - only the table's own contents are generated).
//
// One entry per TCL-readable class, mechanically derived from
// Klass.get_filterable_scalar_fields()/get_filterable_hop_fields() -
// already the exact same source struct_hpp_j2.py's generated get_field()/
// match_hop() use, so this table can never drift out of sync with what
// those functions actually recognize.
{
{%- for klass in readable_classes %}
    {"{{klass.name}}", {{'{'}}{ {%- for f in klass.get_filterable_scalar_fields() %}"{{f.name}}"{% if not loop.last %}, {% endif %}{% endfor -%} }, { {%- for f in klass.get_filterable_hop_fields() %}{"{{f.name}}", "{{f.type}}"}{% if not loop.last %}, {% endif %}{% endfor -%} }{{'}'}}},
{%- endfor %}
};
"""
