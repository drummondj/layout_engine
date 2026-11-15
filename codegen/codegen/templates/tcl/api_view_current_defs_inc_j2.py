TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside namespace le in api/views.cpp.
// A view already holds the session's lock, so its accessors read the
// handle directly.
{% for klass in current_access_classes %}
{% set snake = klass.to_snake_case() %}
{{klass.name}}Id ReadView::current_{{snake}}() const { return handle_->current_{{snake}}_id; }

{{klass.name}}Id WriteView::current_{{snake}}() const { return handle_->current_{{snake}}_id; }

{% endfor %}
"""
