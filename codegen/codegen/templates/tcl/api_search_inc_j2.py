TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd once in api.cpp INSIDE the
// `extern "C" { ... }` block: each le_get_<type> runs the C++ search
// (search_defs.inc) and keeps its result for le_search_result_<type>_at.
{% for klass in classes %}
{%- set scope = search_scopes[klass.name] %}
{%- set id_field = klass.tcl_friendly_id_field() %}
int32_t le_get_{{klass.tcl_plural_snake_case()}}(LeHandle *handle{% for op in scope.of_params %}, Le{{op.parent_klass.name}}Id of_{{op.parent_field.name}}{% endfor %}{% if id_field %}, const char *name_expression{% endif %}, const char *filter_expression)
{
    if (!handle)
        return 0;
    // Exclusive: it refills the search-result cache, which
    // le_search_result_{{klass.to_snake_case()}}_at reads.
    HandleWriteLock lock(handle);
    le::{{klass.name}}Query query;
    {%- for op in scope.of_params %}
    query.of_{{op.parent_field.name}} = from_c(of_{{op.parent_field.name}});
    {%- endfor %}
    {%- if id_field %}
    query.name = name_expression ? name_expression : "";
    {%- endif %}
    query.filter = filter_expression ? filter_expression : "";
    std::expected<std::vector<le::{{klass.name}}Id>, std::string> found = le::edit::get_{{klass.tcl_plural_snake_case()}}(*handle, query);
    if (!found)
    {
        spdlog::error("le_get_{{klass.tcl_plural_snake_case()}}: {}", found.error());
        return -1;
    }
    handle->{{klass.to_snake_case()}}_search_results = std::move(*found);
    return static_cast<int32_t>(handle->{{klass.to_snake_case()}}_search_results.size());
}

Le{{klass.name}}Id le_search_result_{{klass.to_snake_case()}}_at(LeHandle *handle, int32_t index)
{
    const Le{{klass.name}}Id invalid{.index = UINT32_MAX, .generation = 0};
    if (!handle || index < 0)
        return invalid;
    std::shared_lock<std::shared_mutex> lock(handle->mutex_);
    if (static_cast<size_t>(index) >= handle->{{klass.to_snake_case()}}_search_results.size())
        return invalid;
    return to_c(handle->{{klass.to_snake_case()}}_search_results[static_cast<size_t>(index)]);
}
{% endfor %}
"""
