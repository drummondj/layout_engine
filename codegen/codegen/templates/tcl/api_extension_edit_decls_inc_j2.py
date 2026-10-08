TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside the body of le::ext::WriteView
// (<le/extension.hpp>): each class's create/update/delete, recorded into
// the open undo step. Lengths are in dbu; errors come back as values.
{% for klass in classes %}
{% set snake = klass.to_snake_case() %}
/// @brief Creates a {{klass.name}} from `data` (its parent, owner and references must exist).
std::expected<{{klass.name}}Id, std::string> create_{{snake}}({{klass.name}}Data data);
/// @brief Applies the set members of `changes` to {{klass.name}} `id`.
std::expected<void, std::string> update_{{snake}}({{klass.name}}Id id, {{klass.name}}Changes changes);
/// @brief Deletes {{klass.name}} `id` and everything it owns.
std::expected<void, std::string> delete_{{snake}}({{klass.name}}Id id);
{% endfor %}
"""
