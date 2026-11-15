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

// LeObjectKind's values are ObjectId's alternative indexes.
{% for klass in classes %}
static_assert(LE_OBJECT_KIND_{{klass.to_snake_case()|upper}} == {{loop.index0}} && std::is_same_v<std::variant_alternative_t<{{loop.index0}}, le::ObjectId>, le::{{klass.name}}Id>);
{% endfor %}

// The ObjectId `ref` names; nullopt for an unknown kind.
std::optional<le::ObjectId> object_id_from_ref(LeObjectRef ref)
{
    switch (static_cast<LeObjectKind>(ref.kind))
    {
{% for klass in classes %}
    case LE_OBJECT_KIND_{{klass.to_snake_case()|upper}}:
        return le::ObjectId(std::in_place_index<{{loop.index0}}>, id_from_ref<le::{{klass.name}}Id>(ref));
{% endfor %}
    }
    return std::nullopt;
}

LeObjectRef ref_from_object_id(const le::ObjectId &id)
{
    return std::visit([&](auto typed)
                      { return ref_from_id(static_cast<LeObjectKind>(id.index()), typed); },
                      id);
}

// Each class's property table (build_<x>_properties), in microns.
std::vector<le::PropertyValue> build_object_id_properties(const le::Root &root, const le::ObjectId &id)
{
    switch (id.index())
    {
{% for klass in classes %}
    case {{loop.index0}}:
        return build_{{klass.to_snake_case()}}_properties(root, std::get<{{loop.index0}}>(id));
{% endfor %}
    }
    return {};
}

std::vector<le::PropertyValue> build_object_properties(const le::Root &root, LeObjectRef ref)
{
    const std::optional<le::ObjectId> id = object_id_from_ref(ref);
    return id ? build_object_id_properties(root, *id) : std::vector<le::PropertyValue>{};
}

// `id`'s immediate parent, read off the class's parent field(s) in
// schema.py; a class with several (mutually exclusive) parent fields
// returns whichever is set. nullopt for a class with no parent, or an id
// that doesn't resolve.
std::optional<le::ObjectId> object_id_parent(const le::Root &root, const le::ObjectId &id)
{
    switch (id.index())
    {
{% for klass in classes %}
    case {{loop.index0}}:
{% set parents = parent_fields[klass.name] %}
{% if not parents %}
        return std::nullopt;
{% else %}
    {
        const le::{{klass.name}}Data *object = root.get_{{klass.to_snake_case()}}(std::get<{{loop.index0}}>(id));
        if (!object)
            return std::nullopt;
{% for field, parent in parents %}
        if (object->{{field.accessor}}.valid())
            return le::ObjectId(object->{{field.accessor}});
{% endfor %}
        return std::nullopt;
    }
{% endif %}
{% endfor %}
    }
    return std::nullopt;
}

LeObjectRef object_ref_parent(const le::Root &root, LeObjectRef ref)
{
    const std::optional<le::ObjectId> id = object_id_from_ref(ref);
    const std::optional<le::ObjectId> parent = id ? object_id_parent(root, *id) : std::nullopt;
    return parent ? ref_from_object_id(*parent) : invalid_object_ref();
}
"""
