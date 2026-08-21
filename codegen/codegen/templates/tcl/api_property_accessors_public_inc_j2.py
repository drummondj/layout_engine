TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once in api.cpp INSIDE the file's
// `extern "C" { ... }` block (right after it opens, before le_create) -
// NOT inside the anonymous namespace api_property_accessors_internal_inc_j2
// lives in. These functions are declared in api.hpp (generated_tcl/
// declarations.inc) and called from other translation units (le_tcl_shim.cpp)
// via SWIG - a function defined inside an unnamed namespace has internal
// linkage regardless of any `extern "C"` wrapping (extern "C" only
// affects name mangling/calling convention, not visibility), so putting
// these there would silently produce unresolved-symbol link errors in
// every other translation unit that calls them. Still in the same
// translation unit as api_property_accessors_internal_inc_j2's helpers
// (build_X_properties, to_c/from_c, display_dbu_per_um,
// find_property_by_name, validate_filter_path), so ordinary unqualified
// name lookup finds them regardless of anonymous-namespace boundaries.
//
// Mirrors the hand-written le_terminal_property_count/_at/_path/
// le_library_by_name functions (api.cpp) exactly, generated instead of
// duplicated per class - see codegen/codegen/tcl_generator.py's
// HAND_WRITTEN_CLASSES for which classes already have hand-written
// versions of this and are excluded here.

// --- Friendly-id-by-name lookups (name-indexed classes only) ---
{% for klass in classes %}
{%- set id_field = klass.tcl_indexed_id_field() %}
{%- if id_field %}
Le{{klass.name}}Id le_{{klass.to_snake_case()}}_by_{{id_field.name}}(LeHandle *handle, const char *{{id_field.name}})
{
    const Le{{klass.name}}Id invalid{.index = UINT32_MAX, .generation = 0};
    if (!handle || !{{id_field.name}})
        return invalid;
    std::lock_guard<std::mutex> lock(handle->mutex_);

    return to_c(handle->root.get_{{klass.to_snake_case()}}_by_{{id_field.name}}({{id_field.name}}));
}

const char *le_{{klass.to_snake_case()}}_{{id_field.name}}_by_id(LeHandle *handle, Le{{klass.name}}Id id)
{
    if (!handle)
        return nullptr;
    std::lock_guard<std::mutex> lock(handle->mutex_);

    const le::{{klass.name}}Data *data = handle->root.get_{{klass.to_snake_case()}}(from_c(id));
    return data ? data->{{id_field.name}}.c_str() : nullptr;
}
{% endif -%}
{% endfor %}

// --- Property tables ---
{% for klass in classes %}
int32_t le_{{klass.to_snake_case()}}_property_count(LeHandle *handle, Le{{klass.name}}Id id)
{
    if (!handle)
        return 0;
    std::lock_guard<std::mutex> lock(handle->mutex_);

    const le::{{klass.name}}Id typed_id = from_c(id);
    handle->cached_{{klass.to_snake_case()}}_properties = build_{{klass.to_snake_case()}}_properties(handle->root, typed_id);
    handle->cached_{{klass.to_snake_case()}}_property_id = typed_id;
    return static_cast<int32_t>(handle->cached_{{klass.to_snake_case()}}_properties.size());
}

LeProperty le_{{klass.to_snake_case()}}_property_at(LeHandle *handle, Le{{klass.name}}Id id, int32_t index)
{
    const LeProperty invalid{.name = nullptr, .type = LE_PROPERTY_TYPE_STRING, .string_value = nullptr, .int_value = 0, .double_value = 0.0};
    if (!handle || index < 0)
        return invalid;
    std::lock_guard<std::mutex> lock(handle->mutex_);

    const le::{{klass.name}}Id typed_id = from_c(id);
    if (handle->cached_{{klass.to_snake_case()}}_property_id != typed_id)
    {
        handle->cached_{{klass.to_snake_case()}}_properties = build_{{klass.to_snake_case()}}_properties(handle->root, typed_id);
        handle->cached_{{klass.to_snake_case()}}_property_id = typed_id;
    }

    if (static_cast<size_t>(index) >= handle->cached_{{klass.to_snake_case()}}_properties.size())
        return invalid;
    return to_c(handle->cached_{{klass.to_snake_case()}}_properties[static_cast<size_t>(index)]);
}

LeProperty le_{{klass.to_snake_case()}}_property_path(LeHandle *handle, Le{{klass.name}}Id id, const char *path)
{
    const LeProperty invalid{.name = nullptr, .type = LE_PROPERTY_TYPE_STRING, .string_value = nullptr, .int_value = 0, .double_value = 0.0};
    if (!handle || !path)
        return invalid;
    std::lock_guard<std::mutex> lock(handle->mutex_);

    auto parsed = le::parse_property_path(path);
    if (!parsed)
    {
        handle->messages.push_back(fmt::format("ERROR: le_{{klass.to_snake_case()}}_property_path: {}", parsed.error()));
        return invalid;
    }

    const le::{{klass.name}}Id typed_id = from_c(id);
    const le::{{klass.name}}Data *data = handle->root.get_{{klass.to_snake_case()}}(typed_id);
    if (!data)
        return invalid;

    if (parsed->size() == 1)
    {
        if (auto found = find_property_by_name(build_{{klass.to_snake_case()}}_properties(handle->root, typed_id), (*parsed)[0]))
        {
            handle->cached_property_path_value = std::move(*found);
            return to_c(handle->cached_property_path_value);
        }
        handle->messages.push_back(fmt::format("ERROR: le_{{klass.to_snake_case()}}_property_path: unknown field '{}' on {{klass.name}}", (*parsed)[0]));
        return invalid;
    }

    if (auto error = validate_filter_path("{{klass.name}}", *parsed))
    {
        handle->messages.push_back(fmt::format("ERROR: le_{{klass.to_snake_case()}}_property_path: {}", *error));
        return invalid;
    }

    auto value = le::resolve_property_path(handle->root, typed_id, *data, *parsed);
    if (!value)
        return invalid;
    handle->cached_property_path_value = std::move(*value);
    return to_c(handle->cached_property_path_value);
}
{% endfor %}

// --- is_child list field enumeration (count/at) ---
{% for klass in classes %}
{%- for child_field in klass.tcl_child_list_fields() %}
int32_t le_{{klass.to_snake_case()}}_{{child_field.name}}_count(LeHandle *handle, Le{{klass.name}}Id id)
{
    if (!handle)
        return 0;
    std::lock_guard<std::mutex> lock(handle->mutex_);

    return static_cast<int32_t>(handle->root.get_{{klass.to_snake_case()}}_{{child_field.name}}(from_c(id)).size());
}

Le{{child_field.type}}Id le_{{klass.to_snake_case()}}_{{child_field.name}}_at(LeHandle *handle, Le{{klass.name}}Id id, int32_t index)
{
    const Le{{child_field.type}}Id invalid{.index = UINT32_MAX, .generation = 0};
    if (!handle || index < 0)
        return invalid;
    std::lock_guard<std::mutex> lock(handle->mutex_);

    const auto &children = handle->root.get_{{klass.to_snake_case()}}_{{child_field.name}}(from_c(id));
    if (static_cast<size_t>(index) >= children.size())
        return invalid;
    return to_c(children[static_cast<size_t>(index)]);
}
{% endfor -%}
{% endfor %}

// --- Current-instance access ---
{% for klass in current_access_classes %}
Le{{klass.name}}Id le_current_{{klass.to_snake_case()}}(LeHandle *handle)
{
    const Le{{klass.name}}Id invalid{.index = UINT32_MAX, .generation = 0};
    if (!handle)
        return invalid;
    std::lock_guard<std::mutex> lock(handle->mutex_);
    return to_c(handle->current_{{klass.to_snake_case()}}_id);
}

int le_set_current_{{klass.to_snake_case()}}(LeHandle *handle, Le{{klass.name}}Id id)
{
    if (!handle)
        return 1;
    std::lock_guard<std::mutex> lock(handle->mutex_);
    const le::{{klass.name}}Id typed_id = from_c(id);
    if (!handle->root.get_{{klass.to_snake_case()}}(typed_id))
        return 1;
    handle->current_{{klass.to_snake_case()}}_id = typed_id;
    return 0;
}
{% endfor %}

// --- create_<type> - see api_declarations_inc_j2's own comment for the
// field-scope/optionality/dbu-microns/multi-parent conventions every one
// of these follows uniformly. Mirrors le_create_terminal_port/
// le_create_obstruction's own shape for the common case (one parent, no
// optional/enum/dbu fields) and le_create_terminal's own shape (str/enum
// fields, unique_per_parent handling) for the richer ones - generated
// instead of duplicated per class (see Klass.create_api_body() for how
// this body is actually built). ---
{% for klass in classes %}
Le{{klass.name}}Id le_create_{{klass.to_snake_case()}}(LeHandle *handle{% if klass.create_api_params() %}, {{klass.create_api_params()}}{% endif %})
{
{{klass.create_api_body()}}
}
{% endfor %}

// --- update_<type> - see api_declarations_inc_j2's own comment for the
// "omitted means leave unchanged" convention and the single-parent-only
// reparent flag every one of these follows uniformly (see
// Klass.update_api_body() for how this body is actually built). Ends
// with exactly one call into Root::update_<klass>(), the only place
// this class's fields are ever mutated (root.hpp). ---
{% for klass in classes %}
int le_update_{{klass.to_snake_case()}}(LeHandle *handle, {{klass.update_api_params()}})
{
{{klass.update_api_body()}}
}
{% endfor %}

// --- delete_<type> - see api_declarations_inc_j2's own comment for the
// cascade-to-owned-children convention every one of these follows
// uniformly (see Klass.delete_api_body() for how this body is actually
// built - the recursive cascade plan, the deepest-first/self-last
// Transaction::record_delete ordering, and why a leaf class's own delete
// needs none of that machinery). ---
{% for klass in classes %}
int le_delete_{{klass.to_snake_case()}}(LeHandle *handle, Le{{klass.name}}Id id)
{
{{klass.delete_api_body()}}
}
{% endfor %}
"""
