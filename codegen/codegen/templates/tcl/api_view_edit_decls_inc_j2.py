TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside the body of le::WriteView
// (api/views.hpp): each class's create/update/delete, recorded into
// the open undo step. Lengths are in dbu; errors come back as values.
{% for klass in classes %}
{% set snake = klass.to_snake_case() %}
/// @brief Creates a {{klass.name}} from `data` (its parent, owner and references must exist).
std::expected<{{klass.name}}Id, std::string> create_{{snake}}({{klass.name}}Data data);
/// @brief Applies the set members of `changes` to {{klass.name}} `id`.
std::expected<void, std::string> update_{{snake}}({{klass.name}}Id id, {{klass.name}}Changes changes);
/// @brief Deletes {{klass.name}} `id` and everything it owns.
std::expected<void, std::string> delete_{{snake}}({{klass.name}}Id id);
/// @brief Deletes every {{klass.name}} in `ids` and everything they own, as one edit: all or nothing.
std::expected<void, std::string> delete_{{snake}}_batch(std::span<const {{klass.name}}Id> ids);
{% endfor %}
{% for klass in current_access_classes %}
/// @brief Makes {{klass.name}} `id` the current one (`current_{{klass.to_snake_case()}}` in Tcl); an error if it doesn't exist.
std::expected<void, std::string> set_current_{{klass.to_snake_case()}}({{klass.name}}Id id);
{% endfor %}
{% for klass in current_access_classes %}
/// @brief Makes {{klass.name}} `id` the current one (`current_{{klass.to_snake_case()}}` in Tcl); an error if it doesn't exist.
std::expected<void, std::string> set_current_{{klass.to_snake_case()}}({{klass.name}}Id id);
{% endfor %}
"""
