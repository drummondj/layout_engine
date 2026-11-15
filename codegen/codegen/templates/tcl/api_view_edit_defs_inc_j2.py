TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside namespace le in api/views.cpp:
// WriteView's create/update/delete, each forwarding to
// le::edit under the lock the view holds.
{% for klass in classes %}
{% set snake = klass.to_snake_case() %}
std::expected<{{klass.name}}Id, std::string> WriteView::create_{{snake}}({{klass.name}}Data data) { return edit::create_{{snake}}(*handle_, std::move(data)); }
std::expected<void, std::string> WriteView::update_{{snake}}({{klass.name}}Id id, {{klass.name}}Changes changes) { return edit::update_{{snake}}(*handle_, id, std::move(changes)); }
std::expected<void, std::string> WriteView::delete_{{snake}}({{klass.name}}Id id) { return edit::delete_{{snake}}(*handle_, id); }
std::expected<void, std::string> WriteView::delete_{{snake}}_batch(std::span<const {{klass.name}}Id> ids) { return edit::delete_{{snake}}_batch(*handle_, ids); }
{% endfor %}
{% for klass in current_access_classes %}
{% set snake = klass.to_snake_case() %}
std::expected<void, std::string> WriteView::set_current_{{snake}}({{klass.name}}Id id)
{
    if (!root_->get_{{snake}}(id))
        return std::unexpected("no such {{klass.name}}");
    handle_->current_{{snake}}_id = id;
    return {};
}
{% endfor %}
"""
