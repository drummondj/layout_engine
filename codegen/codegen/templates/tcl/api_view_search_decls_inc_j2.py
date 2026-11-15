TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside the body of le::ReadView
// (api/views.hpp): each class's search (`get_<type>` in Tcl).
{% for klass in classes %}
/// @brief The {{klass.name}}s `query` finds; an error for a bad filter.
std::expected<std::vector<{{klass.name}}Id>, std::string> get_{{klass.tcl_plural_snake_case()}}(const {{klass.name}}Query &query = {}) const;
{% endfor %}
"""
