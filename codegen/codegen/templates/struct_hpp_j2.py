TEMPLATE = """
#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <vector>

#include "ids.hpp"
#include "property.hpp"
{%- for include in klass.get_forward_includes(has_pool=False) %}
#include "{{include}}.hpp"
{%- endfor %}

namespace {{schema.namespace}}
{
    /**
        @brief {{klass.description}}

    {%- for field in klass.get_struct_fields() %}
        @param {{field.name}} ({{field.get_cpp_type()}}) {{field.description}}.{% if field.has_default() %} Defaults to {{field.default}}.{% endif %}
    {%- endfor %}
    */
    struct {{klass.name}}{{"Data" if klass.has_pool}} {
    {%- for field in klass.get_struct_fields() %}
        {{field.get_cpp_type()}} {{field.name}};
    {%- endfor %}
    };

    inline std::string to_string(const {{schema.namespace}}::{{klass.name}}{{"Data" if klass.has_pool}} &value)
    {
        std::string output = "{{klass.name}}{";
        {%- for field in klass.get_struct_fields() %}
        output += "{{field.name}}=" + {{field.wrap_with_to_string('value.' + field.name, schema.namespace)}} + " ";
        {%- endfor %}
        output += "}";
        return output;
    }

    /**
        @brief Fully-expanded (no list ever collapsed to a count), Tcl-
        list-friendly string form of this value, used by to_properties()
        (via Field::wrap_with_to_display_property()/
        wrap_with_to_property_string()) to serialize an embedded
        (non-pooled) Klass list item or scalar reference field for
        get_properties() - the get_properties()-facing analog of
        to_string(), which deliberately collapses list fields to a bare
        count instead (and keeps "ClassName{field=value}" debug
        labeling) for compact debug logging. This one recurses fully and
        drops all labeling - just this klass's own field values, bare
        space-joined (no enclosing brace of its own; a scalar reference
        to an embedded klass, or a list element, gets exactly one
        wrapping "{...}" at the *splice* site instead - see
        wrap_with_to_property_string()'s own docstring for the full
        recursive-bracing rule and why). Never called directly - only
        from generated to_properties()/to_property_list_string() bodies,
        same as to_string() itself.
    */
    inline std::string to_property_string(const {{schema.namespace}}::{{klass.name}}{{"Data" if klass.has_pool}} &value, double dbu_per_um)
    {
        std::string output;
        {%- for field in klass.get_struct_fields() %}
        if (!output.empty()) output += " ";
        output += {{field.wrap_with_to_property_string('value.' + field.name, schema.namespace)}};
        {%- endfor %}
        return output;
    }

    inline std::vector<{{schema.namespace}}::PropertyValue> to_properties(const {{schema.namespace}}::{{klass.name}}{{"Data" if klass.has_pool}} &value, double dbu_per_um)
    {
        std::vector<{{schema.namespace}}::PropertyValue> properties;
        {%- for field in klass.get_property_fields() %}
        properties.push_back({{field.wrap_with_to_display_property('value.' + field.name, schema.namespace)}});
        {%- endfor %}
        return properties;
    }

    inline std::ostream& operator<<(std::ostream& os, const {{schema.namespace}}::{{klass.name}}{{"Data" if klass.has_pool}} &value) {
        os << to_string(value);
        return os;
    }

    /**
        @brief Filter-expression leaf lookup: the named field's value as a
        PropertyValue, or nullopt if `name` isn't one of this class's
        filterable scalar fields (see get_filterable_scalar_fields() -
        list and non-enum-reference fields aren't leaves, use match_hop()
        for those instead). Generated boilerplate, not domain-specific -
        consumed by the hand-written filter-expression evaluator, not
        called directly. Overloaded (same name, every generated class) so
        a generic recursive evaluator can call get_field(data, name)
        without knowing which concrete class `data` is - overload
        resolution picks the right one from `data`'s type alone.
    */
    inline std::optional<{{schema.namespace}}::PropertyValue> get_field(const {{schema.namespace}}::{{klass.name}}{{"Data" if klass.has_pool}} &value, std::string_view name)
    {
    {%- for field in klass.get_filterable_scalar_fields() %}
        if (name == "{{field.name}}") return {{field.wrap_with_to_property('value.' + field.name, schema.namespace)}};
    {%- endfor %}
        return std::nullopt;
    }

    /**
        @brief Filter-expression path-hop resolution: if `hop` names one of
        this class's relational fields (see get_filterable_hop_fields()),
        resolves it - a single object for a parent/scalar-reference hop,
        an existential "does any element match" scan for a list hop - and
        calls `matcher` on each resolved target, short-circuiting true on
        the first match. Returns false if `hop` doesn't name a relational
        field on this class, or nothing matched. `matcher` must be a
        generic callable (e.g. a templated lambda/functor) since which
        concrete type - and which call shape - it's invoked with depends
        on which hop matched:
          - a target belonging to a pooled class: `matcher(target_id, target_data)`
            (two args) - the id is always already in scope here (it's
            either the field's own stored value, for a parent/scalar-pool
            hop, or the loop/lookup variable, for a child hop), so it
            costs nothing to hand it onward, and it's what lets a caller
            continue resolving a further hop past this one (which needs an
            id, not just data, if that further hop is itself a child hop).
          - a target that's just an embedded value (a non-pooled klass, or
            a scalar/list field that isn't a reference to a pooled klass):
            `matcher(target)` (one arg) - no id concept applies.
        `matcher` therefore needs (at least) two overloaded/templated call
        shapes to compile against every hop a class might have - see
        get_field()'s comment for the same "must compile against every
        possible target type" property this shares. Generated boilerplate,
        not domain-specific - consumed by the hand-written
        filter-expression evaluator, not called directly. Overloaded (same
        name, every generated class - see get_field()'s comment for why) -
        pooled classes take (root, id, data, ...), non-pooled ones just
        (data, ...), and overload resolution picks the right one from
        argument count and `data`'s type together.
    */
    {%- if klass.has_pool %}
    template <typename RootT, typename Matcher>
    inline bool match_hop(const RootT& root, {{klass.name}}Id id, const {{klass.name}}Data& d, std::string_view hop, Matcher&& matcher)
    {
    {%- else %}
    template <typename Matcher>
    inline bool match_hop(const {{klass.name}}& d, std::string_view hop, Matcher&& matcher)
    {
    {%- endif %}
    {%- for field in klass.get_filterable_hop_fields() %}
        if (hop == "{{field.name}}") {
        {%- if field.has_parent() %}
            const auto* target = root.get_{{field._type_klass.to_snake_case()}}(d.{{field.name}});
            return target != nullptr && matcher(d.{{field.name}}, *target);
        {%- elif field.is_child and field.is_list %}
            for (const auto& child_id : root.get_{{klass.to_snake_case()}}_{{field.name}}(id)) {
                const auto* target = root.get_{{field._type_klass.to_snake_case()}}(child_id);
                if (target != nullptr && matcher(child_id, *target)) return true;
            }
            return false;
        {%- elif field.is_child %}
            auto child_id = root.get_{{klass.to_snake_case()}}_{{field.name}}(id);
            const auto* target = root.get_{{field._type_klass.to_snake_case()}}(child_id);
            return target != nullptr && matcher(child_id, *target);
        {%- elif field.is_list %}
            for (const auto& item : d.{{field.name}}) {
                if (matcher(item)) return true;
            }
            return false;
        {%- elif field._type_klass.has_pool %}
            const auto* target = root.get_{{field._type_klass.to_snake_case()}}(d.{{field.name}});
            return target != nullptr && matcher(d.{{field.name}}, *target);
        {%- elif field.is_optional %}
            return d.{{field.name}}.has_value() && matcher(*d.{{field.name}});
        {%- else %}
            return matcher(d.{{field.name}});
        {%- endif %}
        }
    {%- endfor %}
        return false;
    }
}

"""
