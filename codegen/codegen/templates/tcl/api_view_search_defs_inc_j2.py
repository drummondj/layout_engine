TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside namespace le in api/views.cpp.
{% for klass in classes %}
std::expected<std::vector<{{klass.name}}Id>, std::string> ReadView::get_{{klass.tcl_plural_snake_case()}}(const {{klass.name}}Query &query) const
{
    return edit::get_{{klass.tcl_plural_snake_case()}}(*handle_, query);
}
{% endfor %}
"""
