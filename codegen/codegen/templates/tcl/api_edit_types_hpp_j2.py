TEMPLATE = """#pragma once
// GENERATED - do not edit by hand. The build regenerates it
// (codegen --target tcl). One <Klass>Changes per class: what
// update_<type> changes (le::WriteView, le::edit). A set member is
// applied, an unset one left as it is; lengths are in dbu.
#include "database.hpp"

#include <optional>
#include <string>
#include <variant>

namespace le
{
    /// @brief Any database object the API can name: one of the TCL-readable
    /// classes' ids, in schema order.
    using ObjectId = std::variant<
{% for klass in classes %}
        {{klass.name}}Id{{ "," if not loop.last }}
{% endfor %}
        >;

{% for klass in classes %}
{% set scope = search_scopes[klass.name] %}
    /// @brief What get_{{klass.tcl_plural_snake_case()}} finds: those of the valid `of_` parents
    /// (together), else its default scope (the current objects'); then
    /// only those whose {{ "name matches the `name` glob and that " if klass.tcl_friendly_id_field() else "" }}pass `filter`. Empty strings match everything.
    struct {{klass.name}}Query
    {
{% for op in scope.of_params %}
        {{op.parent_klass.name}}Id of_{{op.parent_field.name}};
{% endfor %}
{% if klass.tcl_friendly_id_field() %}
        std::string name;
{% endif %}
        /// @brief A -filter expression (lengths in microns).
        std::string filter;
    };

{% endfor %}
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
