TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd once in api.cpp's anonymous
// namespace, after the generated build_<x>_properties it calls.

// Per ObjectId alternative, in order: its snake_case class name (the Tcl
// friendly-id prefix) and whether its friendly id is a name
// ("library:lib1") rather than a packed numeric id ("shape:4294967296").
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

{% for klass in classes %}
static_assert(std::is_same_v<std::variant_alternative_t<{{loop.index0}}, le::ObjectId>, le::{{klass.name}}Id>);
{% endfor %}

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

// `path` (parse_property_path's segments) read off `id`, in microns:
// nullopt if `id` doesn't exist or the path leads nowhere (e.g. an unset
// reference); an error if a segment names no field of its class.
std::expected<std::optional<le::PropertyValue>, std::string> object_id_property_path(const le::Root &root, const le::ObjectId &id,
                                                                                   const std::vector<std::string> &path)
{
    switch (id.index())
    {
{% for klass in classes %}
    case {{loop.index0}}:
    {
        const le::{{klass.name}}Id typed_id = std::get<{{loop.index0}}>(id);
        const le::{{klass.name}}Data *data = root.get_{{klass.to_snake_case()}}(typed_id);
        if (!data)
            return std::nullopt;
        if (path.size() == 1)
        {
            if (std::optional<le::PropertyValue> found = find_property_by_name(build_{{klass.to_snake_case()}}_properties(root, typed_id), path[0]))
                return found;
            return std::unexpected("unknown field '" + path[0] + "' on {{klass.name}}");
        }
        if (std::optional<std::string> error = validate_filter_path("{{klass.name}}", path))
            return std::unexpected(std::move(*error));
        std::optional<le::PropertyValue> value = le::resolve_property_path(root, typed_id, *data, path);
        if (!value)
            return std::nullopt;
        return display_path_value(root, std::move(*value));
    }
{% endfor %}
    }
    return std::nullopt;
}
"""
