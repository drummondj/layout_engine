TEMPLATE = """
#pragma once
#include <optional>
#include <string_view>

namespace {{schema.namespace}} {
    /// @brief {{klass.description}}
    enum class {{klass.name}} : uint8_t
    {
    {%- for field in klass.fields %}
        /// @brief {{field.description}}
        {{field.name}} = {{field.value}},
    {%- endfor %}
    };

    inline std::string to_string({{schema.namespace}}::{{klass.name}} value)
    {
        switch (value)
        {
        {%- for field in klass.fields %}
        case {{schema.namespace}}::{{klass.name}}::{{field.name}}:
            return "{{field.name}}";
        {%- endfor %}
        }
        return "UNKNOWN";
    }

    inline std::ostream& operator<<(std::ostream& os, {{schema.namespace}}::{{klass.name}} value) {
        os << to_string(value);
        return os;
    }

    /// @brief Parses the exact spelling to_string() produces back into a
    /// {{klass.name}} - std::nullopt (not a default enum value or an
    /// exception) for anything else, matching this codebase's "no
    /// exceptions for expected-missing-data" convention (see CLAUDE.md).
    inline std::optional<{{schema.namespace}}::{{klass.name}}> {{klass.to_snake_case()}}_from_string(std::string_view value)
    {
    {%- for field in klass.fields %}
        if (value == "{{field.name}}")
            return {{schema.namespace}}::{{klass.name}}::{{field.name}};
    {%- endfor %}
        return std::nullopt;
    }
}

"""
