TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside the bodies of le::ext::ReadView,
// WriteView and ExtensionContext (<le/extension.hpp>): one accessor per
// class with a current instance (`current_<type>` in Tcl).
{% for klass in current_access_classes %}
/// @brief The current {{klass.name}}; invalid if none is set.
{{klass.name}}Id current_{{klass.to_snake_case()}}() const;
{% endfor %}
"""
