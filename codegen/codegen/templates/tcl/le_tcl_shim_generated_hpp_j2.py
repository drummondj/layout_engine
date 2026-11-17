TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd once from le_tcl_shim.hpp.
//
// The generated Tcl-facing commands for every readable class: property
// tables, child lists, current_<type>, get_<type> and create_/update_/
// delete_<type> (le_tcl_shim.hpp's "IDs", "get_<type>" and "Property
// tables" comments).

{% for klass in classes %}
int {{klass.to_snake_case()}}_token_resolves(const char *id);
int {{klass.to_snake_case()}}_property_count(const char *id);
const char *{{klass.to_snake_case()}}_property_name(const char *id, int index);
const char *{{klass.to_snake_case()}}_property_value(const char *id, int index);
const char *{{klass.to_snake_case()}}_property_path(const char *id, const char *path);
{% endfor %}
// --- is_child list fields: space-separated friendly-id lists. ---
{% for klass in classes %}
{%- for child_field in klass.tcl_child_list_fields() %}
const char *{{klass.to_snake_case()}}_{{child_field.name}}(const char *id);
{% endfor -%}
{% endfor %}

// --- Current-instance access (has_current_access classes only) ---
{% for klass in current_access_classes %}
const char *current_{{klass.to_snake_case()}}_cmd();
int set_current_{{klass.to_snake_case()}}_cmd(const char *id);
{% endfor %}

// --- get_<type> search - positional forms behind `get_<type>
// [<name-expr>...] [-of <parent-token>...] [-filter <expr>] [-help]` (see
// le_tcl_procs.tcl's own parse_get_args) ---
{% for klass in classes %}
{%- set scope = search_scopes[klass.name] %}
{%- set id_field = klass.tcl_friendly_id_field() %}
int get_{{klass.tcl_plural_snake_case()}}_cmd({% for op in scope.of_params %}const char *of_{{op.parent_field.name}}, {% endfor %}{% if id_field %}const char *name_expression, {% endif %}const char *filter_expression);
const char *get_{{klass.tcl_plural_snake_case()}}_at(int index);
{% endfor %}

// --- create_<type> - positional forms behind `create_<type> [-flag
// value...]` (le_tcl_procs.tcl's own generated create_<type> proc parses
// the flags then calls this). Each parent field takes a friendly-id
// token (resolved via resolve_<parent_klass>_id inside the .cpp
// definition), same as every other resolve_X_id-taking shim function.
// Lengths are microns; an optional string or enum is omitted when empty,
// any other optional field when its has_<field> flag is 0. ---
{% for klass in classes %}
const char *create_{{klass.to_snake_case()}}_cmd({{klass.create_shim_params()}});
{% endfor %}

// --- update_<type> - positional forms behind `update_<type> <id>
// [-flag value...]` (le_tcl_procs.tcl's own generated update_<type>
// proc parses the flags then calls this). `id` and (for a single-
// parent class) the parent are friendly-id tokens, resolved the same
// way create_<type>_cmd's own parent tokens are. An omitted field (an
// empty string or enum, or a 0 has_<field> flag) is left unchanged. Returns
// the (possibly-changed, e.g. after a rename) friendly id token on
// success, "" on failure. ---
{% for klass in classes %}
const char *update_{{klass.to_snake_case()}}_cmd({{klass.update_shim_params()}});
{% endfor %}

// --- delete_<type> - behind `delete_<type> <ids> [-help]`: `ids` is a
// space-separated list of one or more id tokens, deleted together with
// everything they own. Returns 0 on success, nonzero (nothing deleted)
// if the list is empty or any id is unknown. ---
{% for klass in classes %}
int delete_{{klass.to_snake_case()}}_cmd(const char *ids);
{% endfor %}
"""
