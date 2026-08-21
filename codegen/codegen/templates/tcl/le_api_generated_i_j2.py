TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). %include'd once from le_api.i.
//
// SWIG declarations for the generated property-reading surface - plain
// positional forms, same as every hand-written declaration in le_api.i
// (SWIG-wrapped C++ functions are always positional; no `-flag value`
// parsing happens here, that's le_tcl_procs.tcl's job).

{% for klass in classes %}
int {{klass.to_snake_case()}}_property_count(const char *id);
const char *{{klass.to_snake_case()}}_property_name(const char *id, int index);
const char *{{klass.to_snake_case()}}_property_value(const char *id, int index);
const char *{{klass.to_snake_case()}}_property_path(const char *id, const char *path);
{% endfor %}
{% for klass in classes %}
{%- for child_field in klass.tcl_child_list_fields() %}
const char *{{klass.to_snake_case()}}_{{child_field.name}}(const char *id);
{% endfor -%}
{% endfor %}

{% for klass in current_access_classes %}
const char *current_{{klass.to_snake_case()}}_cmd();
int set_current_{{klass.to_snake_case()}}_cmd(const char *id);
{% endfor %}

{% for klass in classes %}
{%- set scope = search_scopes[klass.name] %}
{%- set id_field = klass.tcl_friendly_id_field() %}
int get_{{klass.tcl_plural_snake_case()}}_cmd({% for op in scope.of_params %}const char *of_{{op.parent_field.name}}, {% endfor %}{% if id_field %}const char *name_expression, {% endif %}const char *filter_expression);
const char *get_{{klass.tcl_plural_snake_case()}}_at(int index);
{% endfor %}

// --- list_compound_kind() create fields (Shape.rects/polygons/paths) -
// %apply the shared POINTS_ARRAY_UM/POINTS_COORD_COUNT typemap (defined
// hand-written, above this %include, in le_api.i itself) to each such
// field's own flat parameter names, before any declaration below uses
// them (SWIG requires %apply to precede the declarations it covers). ---
{% for klass in classes %}
{{klass.list_compound_swig_applies()}}
{%- endfor %}

{% for klass in classes %}
const char *create_{{klass.to_snake_case()}}_cmd({{klass.create_shim_params()}});
{% endfor %}

{% for klass in classes %}
const char *update_{{klass.to_snake_case()}}_cmd({{klass.update_shim_params()}});
{% endfor %}

{% for klass in classes %}
int delete_{{klass.to_snake_case()}}_cmd(const char *id);
{% endfor %}
"""
