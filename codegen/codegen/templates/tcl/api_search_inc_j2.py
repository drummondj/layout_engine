TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once in api.cpp INSIDE the
// `extern "C" { ... }` block, alongside property_accessors_public.inc
// (same external-linkage reasoning - le_get_X/le_search_result_X_at are
// called from le_tcl_shim.cpp, another translation unit).
//
// Mirrors the hand-written le_get_terminals/le_search_result_terminal_at
// (api.cpp) exactly in overall shape - `-of` handling, name-glob,
// -filter - generated instead of duplicated per class. The one part that
// isn't a direct transcription is the *default* scope used when every
// `-of` was omitted/invalid: that's derived generically from schema
// structure by codegen/codegen/tcl_scope.py (see its own module
// docstring), not hand-picked per class.
{% for klass in classes %}
{%- set scope = search_scopes[klass.name] %}
{%- set id_field = klass.tcl_friendly_id_field() %}
int32_t le_get_{{klass.tcl_plural_snake_case()}}(LeHandle *handle{% for op in scope.of_params %}, Le{{op.parent_klass.name}}Id of_{{op.parent_field.name}}{% endfor %}{% if id_field %}, const char *name_expression{% endif %}, const char *filter_expression)
{
    if (!handle)
        return 0;
    std::lock_guard<std::mutex> lock(handle->mutex_);

    bool ok = true;
    auto expr = parse_and_validate_filter(handle, "le_get_{{klass.tcl_plural_snake_case()}}", "{{klass.name}}", filter_expression, ok);
    if (!ok)
        return -1;

    {{ render_of_check_cpp(scope) | indent(4) }}

    handle->{{klass.to_snake_case()}}_search_results.clear();
    for (const le::{{klass.name}}Id id : candidates)
    {
        const le::{{klass.name}}Data *data = handle->root.get_{{klass.to_snake_case()}}(id);
        if (!data)
            continue;
        {%- if id_field %}
        if (name_expression && name_expression[0] != '\\0' && !le::filter_detail::glob_match(name_expression, data->{{id_field.name}}))
            continue;
        {%- endif %}
        if (expr && !le::evaluate_filter(*expr, handle->root, id, *data))
            continue;
        handle->{{klass.to_snake_case()}}_search_results.push_back(id);
    }
    return static_cast<int32_t>(handle->{{klass.to_snake_case()}}_search_results.size());
}

Le{{klass.name}}Id le_search_result_{{klass.to_snake_case()}}_at(LeHandle *handle, int32_t index)
{
    const Le{{klass.name}}Id invalid{.index = UINT32_MAX, .generation = 0};
    if (!handle || index < 0)
        return invalid;
    std::lock_guard<std::mutex> lock(handle->mutex_);

    if (static_cast<size_t>(index) >= handle->{{klass.to_snake_case()}}_search_results.size())
        return invalid;
    return to_c(handle->{{klass.to_snake_case()}}_search_results[static_cast<size_t>(index)]);
}
{% endfor %}
"""
