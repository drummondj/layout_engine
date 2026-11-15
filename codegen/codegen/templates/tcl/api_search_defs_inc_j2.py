TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside namespace le::edit in
// api/api.cpp: each class's search. The default scope, when no `of_`
// parent is valid, comes from codegen/codegen/tcl_scope.py.
{% for klass in classes %}
{% set scope = search_scopes[klass.name] %}
{% set id_field = klass.tcl_friendly_id_field() %}
std::expected<std::vector<{{klass.name}}Id>, std::string> get_{{klass.tcl_plural_snake_case()}}(const LeHandle &handle_ref, const {{klass.name}}Query &query)
{
    const LeHandle *handle = &handle_ref;
    std::expected<std::optional<FilterExpr>, std::string> expr = api_detail::parse_search_filter("{{klass.name}}", query.filter);
    if (!expr)
        return std::unexpected(expr.error());

    {{ render_of_check_cpp_query(scope) | indent(4) }}

    std::vector<{{klass.name}}Id> found;
    // -filter compares lengths/areas in microns.
    const double filter_dbu_per_um = api_detail::display_dbu_per_um(handle->root);
    for (const {{klass.name}}Id id : candidates)
    {
        const {{klass.name}}Data *data = handle->root.get_{{klass.to_snake_case()}}(id);
        if (!data)
            continue;
{% if id_field %}
        if (!query.name.empty() && !filter_detail::glob_match(query.name, data->{{id_field.name}}))
            continue;
{% endif %}
        if (*expr && !evaluate_filter(**expr, handle->root, id, *data, filter_dbu_per_um))
            continue;
        found.push_back(id);
    }
    return found;
}

{% endfor %}
"""
