TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside namespace le::edit in
// api/edit_ops.hpp: each class's create/update/delete. The caller holds
// the handle's exclusive lock; each records itself into the open
// transaction, if any.
{% for klass in classes %}
{% set snake = klass.to_snake_case() %}
std::expected<{{klass.name}}Id, std::string> create_{{snake}}(LeHandle &handle, {{klass.name}}Data data);
std::expected<void, std::string> update_{{snake}}(LeHandle &handle, {{klass.name}}Id id, {{klass.name}}Changes changes);
std::expected<void, std::string> delete_{{snake}}(LeHandle &handle, {{klass.name}}Id id);
{% endfor %}
"""
