TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once from le_tcl_shim.hpp.
//
// Bare, handle-free TCL-facing declarations for the generated property-
// reading/search surface - same shape as the hand-written
// terminal_property_count/_at/_path/get_terminals_cmd/get_terminals_at
// etc. (le_tcl_shim.hpp's own "Property tables and search results"
// comment) for every TCL-readable class.

{% for klass in classes %}
int {{klass.to_snake_case()}}_property_count(const char *id);
const char *{{klass.to_snake_case()}}_property_name(const char *id, int index);
const char *{{klass.to_snake_case()}}_property_value(const char *id, int index);
const char *{{klass.to_snake_case()}}_property_path(const char *id, const char *path);
{% endfor %}
// --- is_child list field enumeration - space-separated friendly-id
// lists, same shape as the hand-written terminal_port_shapes/
// obstruction_shapes. ---
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
// definition), same as every other resolve_X_id-taking shim function -
// see api_declarations_inc_j2's own comment for the rest of the field
// conventions this follows uniformly. ---
{% for klass in classes %}
const char *create_{{klass.to_snake_case()}}_cmd({{klass.create_shim_params()}});
{% endfor %}

// --- update_<type> - positional forms behind `update_<type> <id>
// [-flag value...]` (le_tcl_procs.tcl's own generated update_<type>
// proc parses the flags then calls this). `id` and (for a single-
// parent class) the parent are friendly-id tokens, resolved the same
// way create_<type>_cmd's own parent tokens are - see
// api_declarations_inc_j2's own comment for the "omitted means leave
// unchanged" convention every other field follows uniformly. Returns
// the (possibly-changed, e.g. after a rename) friendly id token on
// success, "" on failure. ---
{% for klass in classes %}
const char *update_{{klass.to_snake_case()}}_cmd({{klass.update_shim_params()}});
{% endfor %}

// --- delete_<type> - positional form behind `delete_<type> <id>
// [-help]` (le_tcl_procs.tcl's own generated delete_<type> proc resolves
// no flags of its own, just the one positional id token, then calls this
// - see api_declarations_inc_j2's own comment for the cascade-to-owned-
// children convention every one of these follows uniformly). Returns 0
// on success, nonzero on failure (unknown id). ---
{% for klass in classes %}
int delete_{{klass.to_snake_case()}}_cmd(const char *id);
{% endfor %}
"""
