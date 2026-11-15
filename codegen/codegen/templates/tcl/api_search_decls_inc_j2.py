TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside namespace le::edit in
// api/edit_ops.hpp: each class's search, under whichever lock the caller holds.
{% for klass in classes %}
/// @brief The {{klass.name}}s `query` finds; an error for a bad filter.
std::expected<std::vector<{{klass.name}}Id>, std::string> get_{{klass.tcl_plural_snake_case()}}(const LeHandle &handle, const {{klass.name}}Query &query);
{% endfor %}
"""
