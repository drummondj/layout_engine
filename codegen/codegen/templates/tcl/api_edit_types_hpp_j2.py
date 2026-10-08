TEMPLATE = """#pragma once
// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). One <Klass>Changes per class: what
// update_<type> changes (le::ext::WriteView, le::edit). A set member is
// applied, an unset one left as it is; lengths are in dbu.
#include "database.hpp"

#include <optional>

namespace le
{
{% for klass in classes %}
    /// @brief The {{klass.name}} fields update_{{klass.to_snake_case()}} changes; unset ones stay as they are.
    struct {{klass.name}}Changes
    {
{% for type, name in klass.edit_changes_members() %}
        std::optional<{{type}}> {{name}};
{% endfor %}
    };

{% endfor %}
}
"""
