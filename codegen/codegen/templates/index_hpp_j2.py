TEMPLATE = """
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace {{schema.namespace}} {
    /// Contains indexes for specified index fields along with parent/child relationships
    struct Index {

    {%- for klass in schema.get_pool_classes() %}
        {%- for field in klass.get_ordered_fields() %}
            {%- if field.is_reference() and field._type_klass.has_pool %}
                {%- if field.is_list %}
        std::unordered_map<{{klass.name}}Id, std::vector<{{field.type}}Id>> {{klass.to_snake_case()}}_{{field.name}};
                {%- else %}
        std::unordered_map<{{klass.name}}Id, {{field.type}}Id> {{klass.to_snake_case()}}_{{field.name}};
                {%- endif %}
            {%- elif field.index and field.unique_per_parent %}
        std::unordered_map<{{klass.get_parent_field().type}}Id, std::unordered_map<{{field.get_cpp_type()}}, {{klass.name}}Id>> {{klass.to_snake_case()}}_by_{{field.name}};
            {%- elif field.index %}
        std::unordered_map<{{field.get_cpp_type()}}, {{klass.name}}Id> {{klass.to_snake_case()}}_by_{{field.name}};
            {%- endif %}
        {%- endfor %}
    {%- endfor %}
    };
}
"""
