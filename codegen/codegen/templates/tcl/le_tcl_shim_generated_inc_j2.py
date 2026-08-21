TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once in le_tcl_shim.cpp, right
// after the file's own anonymous namespace closes (so session()/pack/
// unpack/return_string/format_property_value/
// resolve_numeric_friendly_id/format_numeric_friendly_id are all already
// in scope - unqualified name lookup finds them regardless of being
// outside that namespace, same translation unit).
//
// Mirrors the hand-written resolve_obstruction_id/format_obstruction_id
// (numeric friendly ids) and resolve_library_id/format_library_id (name-
// based friendly ids) plus terminal_property_count/_at/_path-style bare
// shim functions (le_tcl_shim.cpp) exactly, generated instead of
// duplicated per class.

// --- Friendly-id resolve/format (emitted before anything that calls
// them, e.g. an is_child enumeration function formatting a child's id
// with a different class's own format_X_id). ---
{% for klass in classes %}
{%- set id_field = klass.tcl_indexed_id_field() %}
{%- set custom_id_field = klass.tcl_friendly_id_field() if id_field is none else none %}
{%- if custom_id_field %}
// {{klass.name}}'s friendly id uses `{{custom_id_field.name}}`, but that field isn't
// globally indexed (see its own comment in backend/src/database/schema.py) -
// there's no Root-level by-name lookup to generate a resolve/format pair
// from, so k{{klass.name}}Prefix/resolve_{{klass.to_snake_case()}}_id/format_{{klass.to_snake_case()}}_id stay
// hand-written (le_tcl_shim.cpp) instead.
{% else -%}
constexpr std::string_view k{{klass.name}}Prefix = "{{klass.to_snake_case()}}:";
{% if id_field -%}
std::string format_{{klass.to_snake_case()}}_id(const char *{{id_field.name}}) { return std::string(k{{klass.name}}Prefix) + ({{id_field.name}} ? {{id_field.name}} : ""); }

// Overload taking the Id directly (not just its {{id_field.name}}) - needed to
// format a friendly id when only an Id is in hand, e.g. formatting each
// element of a parent's child-enumeration list below.
std::string format_{{klass.to_snake_case()}}_id(Le{{klass.name}}Id id) { return format_{{klass.to_snake_case()}}_id(le_{{klass.to_snake_case()}}_{{id_field.name}}_by_id(session(), id)); }

// Fixed-prefix compare, empty/malformed input degrades to the same
// invalid sentinel as "no such object" - see resolve_library_id's own
// comment (le_tcl_shim.cpp) for why.
Le{{klass.name}}Id resolve_{{klass.to_snake_case()}}_id(const char *s)
{
    const Le{{klass.name}}Id invalid{.index = UINT32_MAX, .generation = 0};
    if (!s)
        return invalid;
    std::string_view sv(s);
    if (sv.substr(0, k{{klass.name}}Prefix.size()) != k{{klass.name}}Prefix)
        return invalid;
    return le_{{klass.to_snake_case()}}_by_{{id_field.name}}(session(), std::string(sv.substr(k{{klass.name}}Prefix.size())).c_str());
}
{% else -%}
Le{{klass.name}}Id resolve_{{klass.to_snake_case()}}_id(const char *s) { return resolve_numeric_friendly_id<Le{{klass.name}}Id>(s, k{{klass.name}}Prefix); }
std::string format_{{klass.to_snake_case()}}_id(Le{{klass.name}}Id id) { return format_numeric_friendly_id(id, k{{klass.name}}Prefix); }
{% endif %}
{% endif %}
{% endfor %}

// --- Property tables ---
{% for klass in classes %}
int {{klass.to_snake_case()}}_property_count(const char *id)
{
    return le_{{klass.to_snake_case()}}_property_count(session(), resolve_{{klass.to_snake_case()}}_id(id));
}

const char *{{klass.to_snake_case()}}_property_name(const char *id, int index)
{
    return le_{{klass.to_snake_case()}}_property_at(session(), resolve_{{klass.to_snake_case()}}_id(id), index).name;
}

const char *{{klass.to_snake_case()}}_property_value(const char *id, int index)
{
    return return_string(format_property_value(le_{{klass.to_snake_case()}}_property_at(session(), resolve_{{klass.to_snake_case()}}_id(id), index)));
}

const char *{{klass.to_snake_case()}}_property_path(const char *id, const char *path)
{
    return return_string(format_property_value(le_{{klass.to_snake_case()}}_property_path(session(), resolve_{{klass.to_snake_case()}}_id(id), path)));
}
{% endfor %}

// --- is_child list field enumeration - space-separated friendly-id
// lists (same shape as the hand-written terminal_port_shapes/
// obstruction_shapes). ---
{% for klass in classes %}
{%- for child_field in klass.tcl_child_list_fields() %}
const char *{{klass.to_snake_case()}}_{{child_field.name}}(const char *id)
{
    Le{{klass.name}}Id parent_id = resolve_{{klass.to_snake_case()}}_id(id);
    int32_t count = le_{{klass.to_snake_case()}}_{{child_field.name}}_count(session(), parent_id);
    if (count <= 0)
        return return_string("");
    std::ostringstream out;
    for (int32_t i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            out << ' ';
        }
        out << format_{{child_field._type_klass.to_snake_case()}}_id(le_{{klass.to_snake_case()}}_{{child_field.name}}_at(session(), parent_id, i));
    }
    return return_string(out.str());
}
{% endfor -%}
{% endfor %}

// --- Current-instance access - current_<type>_cmd()/set_current_<type>_cmd()
// back the single Tcl-level current_<type> [<id>] wrapper proc
// (le_tcl_procs_generated_tcl_j2.py) - both suffixed _cmd for symmetry
// (there is no bare current_<type> at this layer or SWIG's, unlike
// before the two Tcl commands were folded into one - see that proc's
// own comment). ---
{% for klass in current_access_classes %}
const char *current_{{klass.to_snake_case()}}_cmd()
{
    Le{{klass.name}}Id id = le_current_{{klass.to_snake_case()}}(session());
    if (id.index == UINT32_MAX)
        return return_string("");
    return return_string(format_{{klass.to_snake_case()}}_id(id));
}

int set_current_{{klass.to_snake_case()}}_cmd(const char *id)
{
    return le_set_current_{{klass.to_snake_case()}}(session(), resolve_{{klass.to_snake_case()}}_id(id));
}
{% endfor %}

// --- get_<type> search ---
{% for klass in classes %}
{%- set scope = search_scopes[klass.name] %}
{%- set id_field = klass.tcl_friendly_id_field() %}
int get_{{klass.tcl_plural_snake_case()}}_cmd({% for op in scope.of_params %}const char *of_{{op.parent_field.name}}, {% endfor %}{% if id_field %}const char *name_expression, {% endif %}const char *filter_expression)
{
    return le_get_{{klass.tcl_plural_snake_case()}}(session(){% for op in scope.of_params %}, resolve_{{op.parent_klass.to_snake_case()}}_id(of_{{op.parent_field.name}}){% endfor %}{% if id_field %}, name_expression{% endif %}, filter_expression);
}

const char *get_{{klass.tcl_plural_snake_case()}}_at(int index)
{
    Le{{klass.name}}Id id = le_search_result_{{klass.to_snake_case()}}_at(session(), index);
    if (id.index == UINT32_MAX)
        return return_string("");
    return return_string(format_{{klass.to_snake_case()}}_id(id));
}
{% endfor %}

// --- create_<type> - mirrors create_terminal_port_cmd/create_obstruction_cmd's
// own shape exactly (resolve each parent token, forward, format the new
// id back into a friendly-id string on success, "" on failure - le_create_<type>
// itself pushes the actual error message onto handle->messages) - always
// uses the Id-taking format_<snake>_id() overload (present for every
// class, hand-written or generated - see that overload's own comment)
// rather than any class-specific field-taking overload some classes also
// happen to have. ---
{% for klass in classes %}
const char *create_{{klass.to_snake_case()}}_cmd({{klass.create_shim_params()}})
{
    Le{{klass.name}}Id id = le_create_{{klass.to_snake_case()}}(session(){% if klass.create_shim_forward_args() %}, {{klass.create_shim_forward_args()}}{% endif %});
    if (id.index == UINT32_MAX)
        return return_string("");
    return return_string(format_{{klass.to_snake_case()}}_id(id));
}
{% endfor %}

// --- update_<type> - resolves `id` to a real Le<Klass>Id *before*
// calling le_update_<type> and reuses that same resolved id to format
// the return value, rather than re-resolving the caller's original
// friendly-id string afterward - correct even for a class whose
// friendly id can change as a result of the update itself (e.g.
// Terminal's own name-based one after a rename), unlike create_<type>_cmd
// above (which formats the brand-new id le_create_<type> itself
// returns, nothing to re-resolve). ---
{% for klass in classes %}
const char *update_{{klass.to_snake_case()}}_cmd({{klass.update_shim_params()}})
{
    const Le{{klass.name}}Id typed_id = resolve_{{klass.to_snake_case()}}_id(id);
    int rc = le_update_{{klass.to_snake_case()}}(session(), typed_id{% if klass.update_shim_forward_args() %}, {{klass.update_shim_forward_args()}}{% endif %});
    if (rc != 0)
        return return_string("");
    return return_string(format_{{klass.to_snake_case()}}_id(typed_id));
}
{% endfor %}

// --- delete_<type> - resolves the friendly-id token the same way every
// other shim function does, then forwards straight to le_delete_<type>
// (api.hpp/api.cpp), which does the actual cascade - see that function's
// own comment for why the cascade lives at the api.cpp layer, not here. ---
{% for klass in classes %}
int delete_{{klass.to_snake_case()}}_cmd(const char *id)
{
    return le_delete_{{klass.to_snake_case()}}(session(), resolve_{{klass.to_snake_case()}}_id(id));
}
{% endfor %}
"""
