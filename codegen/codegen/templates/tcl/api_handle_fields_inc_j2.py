TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd once inside struct LeHandle's body
// (api/le_handle.hpp), right before its closing brace.

// The current object of each has_current_access class: Tcl's
// current_<type>, and what every other class's get_<type> default scope
// derives from (codegen/codegen/tcl_scope.py). Opening a Design's view
// moves the matching one too.
{% for klass in current_access_classes %}
le::{{klass.name}}Id current_{{klass.to_snake_case()}}_id{};
{% endfor %}
"""
