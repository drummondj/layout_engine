TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once from api.hpp - see this
// project's own "generated code, never hand-edited" convention
// (backend/CLAUDE.md's Database codegen section; this is the TCL-surface
// analog of it).
//
// Plain-C declarations for the generated TCL property-reading/search
// surface - one Id typedef, friendly-id-by-name lookup (name-indexed
// classes only), property table accessors, is_child-field enumeration
// pairs, get_<type> search, current-instance access (has_current_access
// classes only), create_<type>, update_<type>, and delete_<type>, per
// class. Covers every TCL-readable class uniformly - the only CRUD-ish
// surface still hand-written elsewhere is Shape's own
// remove_shape_rect/_polygon/_path (removing one geometry entry by index,
// not per-class flag-driven CRUD - unrelated to this file).

// --- Friendly-id-by-name lookups (name-indexed classes only) ---
{% for klass in classes %}
{%- set id_field = klass.tcl_indexed_id_field() %}
{%- if id_field %}
/// @brief The {{klass.name}}Id whose {{id_field.name}} matches `{{id_field.name}}`
/// (Root::get_{{klass.to_snake_case()}}_by_{{id_field.name}}, a real global cmg
/// index=True lookup). Returns an invalid id (index == UINT32_MAX) if
/// handle/{{id_field.name}} is null or nothing matches.
Le{{klass.name}}Id le_{{klass.to_snake_case()}}_by_{{id_field.name}}(LeHandle *handle, const char *{{id_field.name}});

/// @brief The {{id_field.name}} of the {{klass.name}} at `id` - the reverse of
/// le_{{klass.to_snake_case()}}_by_{{id_field.name}} above, needed to format a friendly id
/// from an Id alone (e.g. when enumerating a parent's children). Named
/// with a `_by_id` suffix (unlike le_terminal_name/le_library_name's own
/// hand-written precedent) so it can never collide with an unrelated,
/// differently-shaped accessor of the bare name for some other class
/// (e.g. le_design_name(LeHandle*, int32_t index), a flat-index
/// enumeration accessor predating this generator). Owned by the handle's
/// Root - valid until the handle is destroyed. Returns nullptr if handle
/// is null or id doesn't name a {{klass.name}} on this handle.
const char *le_{{klass.to_snake_case()}}_{{id_field.name}}_by_id(LeHandle *handle, Le{{klass.name}}Id id);
{% endif -%}
{% endfor %}

// --- Property tables (count/at/path - see le_terminal_property_count's
// own api.hpp comment for the general by-id property-table contract
// every one of these mirrors) ---
{% for klass in classes %}
int32_t le_{{klass.to_snake_case()}}_property_count(LeHandle *handle, Le{{klass.name}}Id id);
LeProperty le_{{klass.to_snake_case()}}_property_at(LeHandle *handle, Le{{klass.name}}Id id, int32_t index);
LeProperty le_{{klass.to_snake_case()}}_property_path(LeHandle *handle, Le{{klass.name}}Id id, const char *path);
{% endfor %}

// --- is_child list field enumeration (count/at) - to_properties() never
// includes these (structural, not struct fields), so a script needs a
// dedicated way to reach e.g. Technology's own Layers - same shape as
// the existing hand-written le_terminal_port_shape_count/_at. ---
{% for klass in classes %}
{%- for child_field in klass.tcl_child_list_fields() %}
int32_t le_{{klass.to_snake_case()}}_{{child_field.name}}_count(LeHandle *handle, Le{{klass.name}}Id id);
Le{{child_field.type}}Id le_{{klass.to_snake_case()}}_{{child_field.name}}_at(LeHandle *handle, Le{{klass.name}}Id id, int32_t index);
{% endfor -%}
{% endfor %}

// --- Current-instance access (has_current_access classes only) - see
// codegen/codegen/tcl_scope.py's own module docstring for how every
// other readable class's get_<type> default scope derives from these. ---
{% for klass in current_access_classes %}
/// @brief The current {{klass.name}} (invalid id, index == UINT32_MAX, if unset).
Le{{klass.name}}Id le_current_{{klass.to_snake_case()}}(LeHandle *handle);
/// @brief Sets the current {{klass.name}}. Returns 0 on success, nonzero if
/// handle is null or id doesn't name a {{klass.name}} on this handle (the
/// current value is left unchanged on failure).
int le_set_current_{{klass.to_snake_case()}}(LeHandle *handle, Le{{klass.name}}Id id);
{% endfor %}

// --- get_<type> search (name-glob + -of + -filter) - see
// codegen/codegen/tcl_scope.py's own module docstring for how the
// default (-of omitted) scope is computed. ---
{% for klass in classes %}
{%- set scope = search_scopes[klass.name] %}
{%- set id_field = klass.tcl_friendly_id_field() %}
int32_t le_get_{{klass.tcl_plural_snake_case()}}(LeHandle *handle{% for op in scope.of_params %}, Le{{op.parent_klass.name}}Id of_{{op.parent_field.name}}{% endfor %}{% if id_field %}, const char *name_expression{% endif %}, const char *filter_expression);
Le{{klass.name}}Id le_search_result_{{klass.to_snake_case()}}_at(LeHandle *handle, int32_t index);
{% endfor %}

// --- create_<type> - one flag per scalar field (str/int/double/dbu/bool/
// enum), required iff Field.create_required(); a flattenable embedded
// struct (Point/Rect/Symmetry/... - Field.compound_klass()) or a *list*
// of one (Field.list_compound_kind() - e.g. Shape.rects/polygons/paths)
// also gets its own flag, exploded into a fixed set of C slots or a
// single flat (const double*, int32_t count) pair respectively; a
// non-flattenable embedded struct or an is_child relationship stays out
// of scope (a future add_X/set_X round - see TCL_EXPLORATION.md). A dbu
// field (plain or nested inside a compound/list_compound one) crosses
// this C boundary in microns (`<field>_um`, converted via
// database_units_microns()/to_dbu()). An optional numeric field
// (int/double/dbu) gets a companion
// `has_<field>` int32_t so an omitted flag stores real std::nullopt, not
// a zero value that could collide with a genuine, meaningful 0 - unlike
// str/enum fields, which use nullptr for "omitted" directly (see
// Field.create_needs_has_flag()'s own docstring). An enum field crosses
// as `const char *` (its to_string()/from_string() spelling, e.g.
// "INPUT"), not a raw numeric code. A multi-parent class (e.g. Shape's
// terminal_port/obstruction) takes one Id parameter per parent field;
// le_create_<type> itself rejects zero or more than one resolving. ---
{% for klass in classes %}
Le{{klass.name}}Id le_create_{{klass.to_snake_case()}}(LeHandle *handle{% if klass.create_api_params() %}, {{klass.create_api_params()}}{% endif %});
{% endfor %}

// --- update_<type> - the *only* way any TCL-readable class's fields are
// ever mutated after creation (no per-field setter, generated or hand-
// written, exists anymore - see this round's own plan/commit message).
// Same flag set as create_<type> (same compound/enum/dbu conventions),
// but every field's own has_<field>/nullptr signal here means "leave
// unchanged" when omitted, the opposite of create_<type>'s "omitted
// means unset". A single-parent class also takes a has_<parent>/
// Le<Parent>Id pair to optionally reassign its parent; a multi-parent
// class (Shape/ViaLayer/Foreign/LayerDensityEntry) gets no parent
// parameter at all - reassigning one parent field alone would violate
// the "exactly one parent set" invariant create_<type> itself enforces,
// and there's no atomic "swap parent, clear siblings" primitive
// designed. Returns 0 on success, nonzero (with a message pushed to
// handle->messages) if id/the resolved parent doesn't exist, an enum
// flag is unrecognized, or Root::update_<klass>() itself failed (a
// rename/reparent collision - see Root::update_<klass>()'s own
// docstring in root.hpp). ---
{% for klass in classes %}
int le_update_{{klass.to_snake_case()}}(LeHandle *handle, {{klass.update_api_params()}});
{% endfor %}

// --- delete_<type> - cascades to every owned pool-backed child reachable
// through Klass.tcl_child_list_fields(), however many schema-graph levels
// deep that goes for this particular class (see Klass.delete_api_body()'s
// own docstring for the full recursive-at-codegen-time mechanism and the
// undo/redo ordering it depends on) - a class with no such fields (most
// of the ~35) gets a trivial, non-cascading delete instead. Returns 0 on
// success, nonzero if handle is null or id doesn't name a live object of
// this class on this handle. ---
{% for klass in classes %}
int le_delete_{{klass.to_snake_case()}}(LeHandle *handle, Le{{klass.name}}Id id);
{% endfor %}
"""
