TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd once from api.hpp. One kind per
// TCL-readable class, in schema order; ordinals are only meaningful within
// one process (le_object_kind_name gives the stable name).
typedef enum LeObjectKind
{
{% for klass in classes %}
    LE_OBJECT_KIND_{{klass.to_snake_case()|upper}} = {{loop.index0}},
{% endfor %}
} LeObjectKind;
"""
