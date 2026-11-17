TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd once in le_tcl_shim.cpp after its
// anonymous namespace, whose helpers (read_view/write_view, Lookup,
// strip_prefix, the numeric friendly-id pair, fail/fail_text/status,
// properties_of and the search results) it uses. Each command opens one
// view and reports a failure's reason through last_error_cmd.

// --- Friendly-id resolve/format, emitted before anything formatting
// another class's ids (e.g. a child list). A name-based id looks the name
// up; a numeric one only parses, and an id naming nothing is invalid. ---
{% for klass in classes %}
{%- set snake = klass.to_snake_case() %}
{%- set id_field = klass.tcl_indexed_id_field() %}
{%- set custom_id_field = klass.tcl_friendly_id_field() if id_field is none else none %}
{%- if custom_id_field %}
// {{klass.name}}'s friendly id uses `{{custom_id_field.name}}`, which is only unique
// within its parent, so its resolve/format pair is hand-written
// (le_tcl_shim.cpp), scoped by the current objects.
{% else -%}
constexpr std::string_view k{{klass.name}}Prefix = "{{snake}}:";
{% if id_field -%}
std::string format_{{snake}}_id(const Lookup &lk, le::{{klass.name}}Id id)
{
    const le::{{klass.name}}Data *data = lk.root.get_{{snake}}(id);
    return std::string(k{{klass.name}}Prefix) + (data ? data->{{id_field.name}} : "");
}

le::{{klass.name}}Id resolve_{{snake}}_id(const Lookup &lk, const char *s)
{
    const std::optional<std::string> value = strip_prefix(s, k{{klass.name}}Prefix);
    return value ? lk.root.get_{{snake}}_by_{{id_field.name}}(*value) : le::{{klass.name}}Id{};
}
{% else -%}
le::{{klass.name}}Id resolve_{{snake}}_id(const Lookup &, const char *s) { return resolve_numeric_friendly_id<le::{{klass.name}}Id>(s, k{{klass.name}}Prefix); }
std::string format_{{snake}}_id(const Lookup &, le::{{klass.name}}Id id) { return format_numeric_friendly_id(id, k{{klass.name}}Prefix); }
{% endif %}
{% endif %}
{% endfor %}

// --- Property tables ---
{% for klass in classes %}
{%- set snake = klass.to_snake_case() %}
// 1 if `id` is a well-formed token naming an object that resolves (a
// numeric one only has its form checked; create_/update_<type> still
// reject a stale id). Lets a create_/update_<type> proc reject a
// reference flag that doesn't resolve instead of leaving it unset.
int {{snake}}_token_resolves(const char *id)
{
    const le::ReadView view = read_view();
    return resolve_{{snake}}_id(lookup(view), id).valid() ? 1 : 0;
}

int {{snake}}_property_count(const char *id)
{
    const le::ReadView view = read_view();
    return static_cast<int>(properties_of(view, le::ObjectId(resolve_{{snake}}_id(lookup(view), id))).size());
}

const char *{{snake}}_property_name(const char *id, int index)
{
    const le::ReadView view = read_view();
    return property_name_at(properties_of(view, le::ObjectId(resolve_{{snake}}_id(lookup(view), id))), index);
}

const char *{{snake}}_property_value(const char *id, int index)
{
    const le::ReadView view = read_view();
    return property_value_at(properties_of(view, le::ObjectId(resolve_{{snake}}_id(lookup(view), id))), index);
}

const char *{{snake}}_property_path(const char *id, const char *path)
{
    const le::ReadView view = read_view();
    return property_at_path(view, le::ObjectId(resolve_{{snake}}_id(lookup(view), id)), path);
}
{% endfor %}

// --- is_child list fields: space-separated friendly-id lists. ---
{% for klass in classes %}
{%- for child_field in klass.tcl_child_list_fields() %}
const char *{{klass.to_snake_case()}}_{{child_field.name}}(const char *id)
{
    const le::ReadView view = read_view();
    const Lookup lk = lookup(view);
    return return_string(join_ids(lk.root.get_{{klass.to_snake_case()}}_{{child_field.name}}(resolve_{{klass.to_snake_case()}}_id(lk, id)),
                                  [&](le::{{child_field._type_klass.name}}Id child) { return format_{{child_field._type_klass.to_snake_case()}}_id(lk, child); }));
}
{% endfor -%}
{% endfor %}

// --- current_<type>_cmd()/set_current_<type>_cmd(), behind the one
// current_<type> ?id? proc. ---
{% for klass in current_access_classes %}
{%- set snake = klass.to_snake_case() %}
const char *current_{{snake}}_cmd()
{
    const le::ReadView view = read_view();
    const le::{{klass.name}}Id id = view.current_{{snake}}();
    return return_string(id.valid() ? format_{{snake}}_id(lookup(view), id) : std::string());
}

int set_current_{{snake}}_cmd(const char *id)
{
    le::WriteView view = write_view();
    return status(view.set_current_{{snake}}(resolve_{{snake}}_id(lookup(view), id)));
}
{% endfor %}

// --- get_<type>: the count found (-1 for a bad -filter, its reason in
// last_error_cmd), each read back by get_<type>_at. ---
{% for klass in classes %}
{%- set scope = search_scopes[klass.name] %}
{%- set id_field = klass.tcl_friendly_id_field() %}
{%- set snake = klass.to_snake_case() %}
{%- set plural = klass.tcl_plural_snake_case() %}
int get_{{plural}}_cmd({% for op in scope.of_params %}const char *of_{{op.parent_field.name}}, {% endfor %}{% if id_field %}const char *name_expression, {% endif %}const char *filter_expression)
{
    const le::ReadView view = read_view();
    const Lookup lk = lookup(view);
    le::{{klass.name}}Query query;
    {%- for op in scope.of_params %}
    query.of_{{op.parent_field.name}} = resolve_{{op.parent_klass.to_snake_case()}}_id(lk, of_{{op.parent_field.name}});
    {%- endfor %}
    {%- if id_field %}
    query.name = name_expression ? name_expression : "";
    {%- endif %}
    query.filter = filter_expression ? filter_expression : "";
    const std::expected<std::vector<le::{{klass.name}}Id>, std::string> found = view.get_{{plural}}(query);
    if (!found)
        return fail("get_{{plural}}: invalid -filter expression: " + found.error(), -1);
    return keep_search_results(*found, [&](le::{{klass.name}}Id id) { return format_{{snake}}_id(lk, id); });
}

const char *get_{{plural}}_at(int index)
{
    return search_result_at(index);
}
{% endfor %}

// --- create_<type>: each parent token resolved, the arguments converted
// (create_<type>_native), the new object's token back, or "" with the
// reason in last_error_cmd. ---
{% for klass in classes if klass.get_owner_fields() %}
// A {{klass.name}} owner from its option's name (the Tcl flag, e.g. "route")
// and that owner's token; unset if either is unknown.
le::{{klass.owner_type_name()}} resolve_{{klass.to_snake_case()}}_owner(const Lookup &lk, const char *kind, const char *id)
{
    const std::string name = kind ? kind : "";
{%- for f in klass.get_owner_fields() %}
    if (name == "{{f.name}}")
        return le::{{klass.owner_type_name()}}::{{f.name}}(resolve_{{f._parent_klass.to_snake_case()}}_id(lk, id));
{%- endfor %}
    return le::{{klass.owner_type_name()}}{};
}
{% endfor %}
{% for klass in classes %}
{%- set snake = klass.to_snake_case() %}
std::expected<le::{{klass.name}}Id, std::string> create_{{snake}}_native(le::WriteView &view{% if klass.create_native_params() %}, {{klass.create_native_params()}}{% endif %})
{
{{emit.tcl_shim.create_native_body(klass)}}
}

const char *create_{{snake}}_cmd({{klass.create_shim_params()}})
{
    le::WriteView view = write_view();
    [[maybe_unused]] const Lookup lk = lookup(view);
    const std::expected<le::{{klass.name}}Id, std::string> created = create_{{snake}}_native(view{% if klass.create_shim_forward_args() %}, {{klass.create_shim_forward_args()}}{% endif %});
    if (!created)
        return fail_text("create_{{snake}}: " + created.error());
    return return_string(format_{{snake}}_id(lookup(view), *created));
}
{% endfor %}

// --- update_<type>: formats the id resolved before the update, so a
// rename (Terminal's name-based token) still returns the object's token. ---
{% for klass in classes %}
{%- set snake = klass.to_snake_case() %}
std::expected<void, std::string> update_{{snake}}_native(le::WriteView &view, {{klass.update_native_params()}})
{
{{emit.tcl_shim.update_native_body(klass)}}
}

const char *update_{{snake}}_cmd({{klass.update_shim_params()}})
{
    le::WriteView view = write_view();
    const Lookup lk = lookup(view);
    const le::{{klass.name}}Id typed_id = resolve_{{snake}}_id(lk, id);
    const std::expected<void, std::string> updated = update_{{snake}}_native(view, typed_id{% if klass.update_shim_forward_args() %}, {{klass.update_shim_forward_args()}}{% endif %});
    if (!updated)
        return fail_text("update_{{snake}}: " + updated.error());
    return return_string(format_{{snake}}_id(lookup(view), typed_id));
}
{% endfor %}

// --- delete_<type>: `ids` is a space-separated list of tokens (none
// contains whitespace), deleted together: all or nothing, one undo step. ---
{% for klass in classes %}
{%- set snake = klass.to_snake_case() %}
int delete_{{snake}}_cmd(const char *ids)
{
    le::WriteView view = write_view();
    const Lookup lk = lookup(view);
    std::vector<le::{{klass.name}}Id> resolved;
    std::istringstream stream(ids ? ids : "");
    std::string token;
    while (stream >> token)
        resolved.push_back(resolve_{{snake}}_id(lk, token.c_str()));
    if (resolved.empty())
        return fail("delete_{{snake}}: no ids given");
    const std::expected<void, std::string> deleted = view.delete_{{snake}}_batch(resolved);
    return deleted ? 0 : fail("delete_{{snake}}: " + deleted.error());
}
{% endfor %}
"""
