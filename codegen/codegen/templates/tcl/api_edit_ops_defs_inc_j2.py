TEMPLATE = """// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). #include'd inside namespace le::edit in
// api/edit_ops.cpp. Bodies: Klass.create_op_body()/update_op_body()/
// delete_op_body() (schema.py).
{% for klass in classes %}
{% set snake = klass.to_snake_case() %}
std::expected<{{klass.name}}Id, std::string> create_{{snake}}(LeHandle &handle, {{klass.name}}Data data)
{
{{klass.create_op_body()}}
}

std::expected<void, std::string> update_{{snake}}(LeHandle &handle, {{klass.name}}Id id, {{klass.name}}Changes changes)
{
{{klass.update_op_body()}}
}

std::expected<void, std::string> delete_{{snake}}(LeHandle &handle, {{klass.name}}Id id)
{
{{klass.delete_op_body()}}
}

{% endfor %}
"""
