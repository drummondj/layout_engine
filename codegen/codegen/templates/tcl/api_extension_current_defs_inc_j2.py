TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside namespace le::ext in
// api/extension.cpp. A view already holds the session's lock, so its
// accessors read the handle directly; ExtensionContext's take the lock
// shared for the one read.
{% for klass in current_access_classes %}
{% set snake = klass.to_snake_case() %}
{{klass.name}}Id ReadView::current_{{snake}}() const { return handle_->current_{{snake}}_id; }

{{klass.name}}Id WriteView::current_{{snake}}() const { return handle_->current_{{snake}}_id; }

{{klass.name}}Id ExtensionContext::current_{{snake}}() const
{
    std::shared_lock<std::shared_mutex> lock(handle_->mutex_);
    return handle_->current_{{snake}}_id;
}

{% endfor %}
"""
