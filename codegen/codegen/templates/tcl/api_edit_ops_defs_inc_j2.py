TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside namespace le::edit in
// api/edit_ops.cpp. Bodies: codegen/emitters/edit_ops.py.
{% for klass in classes %}
{% set snake = klass.to_snake_case() %}
std::expected<{{klass.name}}Id, std::string> create_{{snake}}(LeHandle &handle, {{klass.name}}Data data)
{
{{emit.edit_ops.create_op_body(klass)}}
}

std::expected<void, std::string> update_{{snake}}(LeHandle &handle, {{klass.name}}Id id, {{klass.name}}Changes changes)
{
{{emit.edit_ops.update_op_body(klass)}}
}

std::expected<void, std::string> delete_{{snake}}(LeHandle &handle, {{klass.name}}Id id)
{
{{emit.edit_ops.delete_op_body(klass)}}
}

std::expected<void, std::string> delete_{{snake}}_batch(LeHandle &handle, std::span<const {{klass.name}}Id> ids)
{
{{emit.edit_ops.delete_batch_op_body(klass)}}
}

{% endfor %}
"""
