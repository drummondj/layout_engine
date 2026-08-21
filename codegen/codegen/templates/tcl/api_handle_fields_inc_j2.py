TEMPLATE = """// GENERATED - do not edit by hand. Regenerate via the regen-tcl skill
// (codegen --target tcl). #include'd once inside struct LeHandle's body
// (api.cpp), right before its closing brace.
//
// Single-slot property-table caches, same pattern as the hand-written
// cached_terminal_property_id/cached_terminal_properties pair (see that
// pair's own comment in api.cpp) - one pair per generated class.
{% for klass in classes %}
le::{{klass.name}}Id cached_{{klass.to_snake_case()}}_property_id{};
std::vector<le::PropertyValue> cached_{{klass.to_snake_case()}}_properties;
{% endfor %}

// Search-result caches, same "valid until the next call" convention as
// the hand-written terminal_search_results/library_search_results/etc.
// this generalizes - one per class, backing le_get_X/le_search_result_X_at.
{% for klass in classes %}
std::vector<le::{{klass.name}}Id> {{klass.to_snake_case()}}_search_results;
{% endfor %}

// Current-instance state for has_current_access classes - see
// codegen/codegen/tcl_scope.py's own module docstring for how every
// other readable class's get_<type> default scope derives from these.
// Independent of any hand-written "current view" state elsewhere (e.g.
// Scene::current_abstract(), which drives GUI rendering) - deliberately
// not bridged, see backend/CLAUDE.md's TCL codegen section.
{% for klass in current_access_classes %}
le::{{klass.name}}Id current_{{klass.to_snake_case()}}_id{};
{% endfor %}
"""
