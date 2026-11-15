TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside namespace le::ext in
// api/extension.cpp. ExtensionContext's accessors take the session's lock
// shared for the one read.
{% for klass in current_access_classes %}
{% set snake = klass.to_snake_case() %}
{{klass.name}}Id ExtensionContext::current_{{snake}}() const
{
    std::shared_lock<std::shared_mutex> lock(handle_->mutex_);
    return handle_->current_{{snake}}_id;
}

{% endfor %}
"""
