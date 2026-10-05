TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd once in api.cpp's anonymous
// namespace, after the generated build_<x>_properties and the
// id_from_ref/ref_from_id/invalid_object_ref helpers it calls.

// Per LeObjectKind: its snake_case class name (the TCL friendly-id prefix)
// and whether its friendly id is a name ("library:lib1") rather than a
// packed numeric id ("shape:4294967296").
struct ObjectKindInfo
{
    const char *name;
    bool named;
};
constexpr ObjectKindInfo kObjectKinds[] = {
{% for klass in classes %}
    {"{{klass.to_snake_case()}}", {{'true' if klass.tcl_friendly_id_field() else 'false'}}},
{% endfor %}
};

// Dispatches to the by-id property builder each class's
// le_<x>_property_count/_at already uses. Lock-free: the public
// le_object_property_* functions take handle->mutex_ once themselves.
std::vector<le::PropertyValue> build_object_properties(const le::Root &root, LeObjectRef ref)
{
    switch (static_cast<LeObjectKind>(ref.kind))
    {
{% for klass in classes %}
    case LE_OBJECT_KIND_{{klass.to_snake_case()|upper}}:
        return build_{{klass.to_snake_case()}}_properties(root, id_from_ref<le::{{klass.name}}Id>(ref));
{% endfor %}
    }
    return {};
}

// `ref`'s immediate parent, read off the class's parent field(s) in
// schema.py; a class with several (mutually exclusive) parent fields
// returns whichever is set. Invalid for a class with no parent, or a ref
// that doesn't resolve.
LeObjectRef object_ref_parent(const le::Root &root, LeObjectRef ref)
{
    switch (static_cast<LeObjectKind>(ref.kind))
    {
{% for klass in classes %}
    case LE_OBJECT_KIND_{{klass.to_snake_case()|upper}}:
{% set parents = parent_fields[klass.name] %}
{% if not parents %}
        return invalid_object_ref();
{% else %}
    {
        const le::{{klass.name}}Data *object = root.get_{{klass.to_snake_case()}}(id_from_ref<le::{{klass.name}}Id>(ref));
        if (!object)
            return invalid_object_ref();
{% for field, parent in parents %}
        if (object->{{field.accessor}}.valid())
            return ref_from_id(LE_OBJECT_KIND_{{parent.to_snake_case()|upper}}, object->{{field.accessor}});
{% endfor %}
        return invalid_object_ref();
    }
{% endif %}
{% endfor %}
    }
    return invalid_object_ref();
}
"""
