TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once in api.cpp INSIDE the file's
// own anonymous namespace, right after build_shape_properties() - these
// are internal helpers (no api.hpp declaration), same as the hand-
// written to_c(le::LibraryId)/build_terminal_properties this mirrors -
// never called from another translation unit, so they must stay inside
// the anonymous namespace (unlike api_property_accessors_public_inc_j2's
// le_X_property_count/_at/_path etc., which need real external C
// linkage and live in a separate generated fragment for exactly that
// reason - see this file's own header comment in
// codegen/codegen/templates/tcl/api_property_accessors_public_inc_j2.py).

// --- Id conversions ---
{% for klass in classes %}
Le{{klass.name}}Id to_c(le::{{klass.name}}Id id) { return Le{{klass.name}}Id{.index = id.index, .generation = id.generation}; }
le::{{klass.name}}Id from_c(Le{{klass.name}}Id id) { return le::{{klass.name}}Id{.index = id.index, .generation = id.generation}; }
{% endfor %}

// --- Property tables ---
{% for klass in classes %}
std::vector<le::PropertyValue> build_{{klass.to_snake_case()}}_properties(const le::Root &root, le::{{klass.name}}Id id)
{
    const le::{{klass.name}}Data *data = root.get_{{klass.to_snake_case()}}(id);
    if (!data)
        return {};
    std::vector<le::PropertyValue> properties = le::to_properties(*data, display_dbu_per_um(root));
    {%- for child_field in klass.tcl_child_list_fields() %}
    properties.push_back(le::PropertyValue::make_int("{{child_field.name}}_count", static_cast<int64_t>(root.get_{{klass.to_snake_case()}}_{{child_field.name}}(id).size())));
    {%- endfor %}
    {%- for rf in klass.get_reference_create_fields() %}
    // to_properties() has no Root to resolve {{rf.name}} against, so it
    // falls back to to_string(Le{{rf.type}}Id) - a bare "Id{index=.., generation=..}"
    // debug string (see Field.wrap_with_to_display_property()'s own
    // docstring). Overwrite it here with a friendly "{{rf._type_klass.to_snake_case()}}:<name>"
    // token instead, same spelling every other friendly id in this codebase
    // uses - a signal reference, not a navigable link (unlike a child-list
    // property, this field's value is never one of properties() own rows'
    // clickable targets).
    for (le::PropertyValue &property : properties)
    {
        if (property.name != "{{rf.name}}")
            continue;
        const le::{{rf.type}}Data *referenced = root.get_{{rf._type_klass.to_snake_case()}}(data->{{rf.name}});
        property = le::PropertyValue::make_string("{{rf.name}}", referenced ? "{{rf._type_klass.to_snake_case()}}:" + referenced->name : std::string());
        break;
    }
    {%- endfor %}
    return properties;
}
{% endfor %}
"""
