TEMPLATE = """
#pragma once
#include "ids.hpp"
#include "pool.hpp"
#include "index.hpp"
{%- for klass in schema.classes %}
#include "{{klass.to_snake_case()}}.hpp"
{%- endfor %}
#include <algorithm>
#include <cassert>
#include <string>

namespace {{schema.namespace}} {
    class Root {
    public:
        /// @brief Monotonic counter bumped by every bump_mutation_version()
        /// call - a cheap way for a caller (e.g. a render pipeline's own
        /// cache key) to tell whether *any* database content has changed
        /// since it last checked, without needing per-field/per-class
        /// change tracking. Mirrors Scene::selection_version()'s existing
        /// pattern (see scene.hpp) for the same reason: a hand-written,
        /// domain-specific mutation site (e.g. api.cpp's own CRUD
        /// functions - see TCL_EXPLORATION.md) calls bump_mutation_version()
        /// explicitly, since not every mutation goes through a generated
        /// create_x/delete_x/set_x_<field> (e.g. appending to a plain list
        /// field via a mutable get_x() pointer never does) - this Root
        /// method only tracks the counter itself, not when to bump it.
        uint64_t mutation_version() const { return mutation_version_; }

        /// @brief Bump the counter returned by mutation_version(). Call
        /// once per logical mutation (not once per internal step of a
        /// multi-step one) after the database content actually changed.
        void bump_mutation_version() { ++mutation_version_; }

    {%- for klass in schema.get_pool_classes() %}
        /// @brief Create a {{klass.name}} object
        {%- for field in klass.get_ordered_fields() %}
            {%- if field.unique_per_parent %}
        ///
        /// Fallible: returns an invalid {{klass.name}}Id (rather than
        /// inserting) if a sibling {{klass.name}} sharing the same
        /// {{klass.get_parent_field().name}} already has this {{field.name}}
        /// (unique_per_parent) - matches this codebase's existing
        /// Id::valid() sentinel convention for "no" rather than adding a
        /// new failure signal.
            {%- endif %}
        {%- endfor %}
        {{klass.name}}Id create_{{klass.to_snake_case()}}({{klass.name}}Data data) {
        {%- for field in klass.get_ordered_fields() %}
            {%- if field.unique_per_parent %}
            if (data.{{klass.get_parent_field().name}}.valid()) {
                auto& siblings_{{field.name}} = index_.{{klass.to_snake_case()}}_by_{{field.name}}[data.{{klass.get_parent_field().name}}];
                if (siblings_{{field.name}}.find(data.{{field.name}}) != siblings_{{field.name}}.end())
                    return {{klass.name}}Id{};
            }
            {%- endif %}
        {%- endfor %}
            {{klass.name}}Id id = {{klass.to_snake_case()}}_.create(std::move(data));

        {%- if klass.has_indecies() %}
            const auto& d = *{{klass.to_snake_case()}}_.get(id);
        {%- endif %}

        {%- for field in klass.get_ordered_fields() %}
            {%- if field.parent %}
                {%- if field._parent_field.is_list %}
            if (d.{{field.name}}.valid())
                index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}[d.{{field.name}}].push_back(id);
                {%- else %}
            if (d.{{field.name}}.valid())
                index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}[d.{{field.name}}] = id;
                {%- endif %}
            {%- elif field.index and field.unique_per_parent %}
            if (d.{{klass.get_parent_field().name}}.valid())
                index_.{{klass.to_snake_case()}}_by_{{field.name}}[d.{{klass.get_parent_field().name}}][d.{{field.name}}] = id;
            {%- elif field.index %}
            index_.{{klass.to_snake_case()}}_by_{{field.name}}[d.{{field.name}}] = id;
            {%- endif %}
        {%- endfor %}
            return id;
        }

        /// @brief Update an existing {{klass.name}} in place - the *only*
        /// way any {{klass.name}} field is ever mutated after creation (no
        /// generated or hand-written per-field setter exists for this
        /// class - see this round's own plan/commit message). Every
        /// parameter beyond `id`{% if klass.get_parent_fields() | length == 1 %} (including the parent){% endif %} is a
        /// std::optional<T> whose has_value() means "apply this field";
        /// omitted (nullopt) means "leave unchanged", the opposite of
        /// create_{{klass.to_snake_case()}}()'s own "omitted means unset"
        /// convention. False (no-op, nothing applied) if `id` doesn't
        /// exist{% if klass.get_parent_fields() | length == 1 and klass.get_unique_per_parent_fields() %}, if the new parent already has a sibling {{klass.name}} sharing the current {{klass.get_unique_per_parent_fields()[0].name}} (reparenting), or if a sibling under the (possibly just-reassigned) parent already has the requested {{klass.get_unique_per_parent_fields()[0].name}} (renaming){% elif klass.get_unique_per_parent_fields() %}, or if a sibling {{klass.name}} sharing the same {{klass.get_parent_field().name}} already has the requested {{klass.get_unique_per_parent_fields()[0].name}}{% endif %}.
        {%- if klass.get_parent_fields() | length > 1 %}
        /// {{klass.name}} has multiple parent fields ({%- for pf in klass.get_parent_fields() -%}{{pf.name}}{% if not loop.last %}, {% endif %}{%- endfor -%}) -
        /// no parent flag is generated at all here, since reassigning one
        /// alone would violate the "exactly one parent set" invariant
        /// create_{{klass.to_snake_case()}}() itself enforces.
        {%- endif %}
        bool update_{{klass.to_snake_case()}}({{klass.update_root_params()}}) {
{{klass.update_root_body()}}
        }

        /// @brief Erase the {{klass.name}} at {{klass.name}}Id, cleaning up
        /// any parent/index bookkeeping that referenced it. Does not
        /// cascade to children referencing this id - a child holding a
        /// now-dangling {{klass.name}}Id degrades gracefully the same way
        /// any other not-found lookup already does (see get_{{klass.to_snake_case()}}()),
        /// rather than being eagerly deleted itself. False (no-op) if id
        /// doesn't exist.
        bool delete_{{klass.to_snake_case()}}({{klass.name}}Id id) {
            const auto* existing = {{klass.to_snake_case()}}_.get(id);
            if (!existing) return false;

        {%- if klass.has_indecies() %}
            const auto& d = *existing;
        {%- for field in klass.get_ordered_fields() %}
            {%- if field.parent %}
                {%- if field._parent_field.is_list %}
            {
                auto it = index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.find(d.{{field.name}});
                if (it != index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.end()) {
                    it->second.erase(std::remove(it->second.begin(), it->second.end(), id), it->second.end());
                }
            }
                {%- else %}
            index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.erase(d.{{field.name}});
                {%- endif %}
            {%- elif field.index and field.unique_per_parent %}
            {
                auto it = index_.{{klass.to_snake_case()}}_by_{{field.name}}.find(d.{{klass.get_parent_field().name}});
                if (it != index_.{{klass.to_snake_case()}}_by_{{field.name}}.end())
                    it->second.erase(d.{{field.name}});
            }
            {%- elif field.index %}
            index_.{{klass.to_snake_case()}}_by_{{field.name}}.erase(d.{{field.name}});
            {%- endif %}
        {%- endfor %}
        {%- endif %}
            return {{klass.to_snake_case()}}_.erase(id);
        }

        {%- for field in klass.get_ordered_fields() %}
            {%- if field.parent or field.index %}
        /// @brief Set {{klass.name}}'s {{field.name}}, keeping the relevant
        /// Root index in sync (unlike assigning through
        /// get_{{klass.to_snake_case()}}() directly, which would leave a
        /// stale index entry behind). False (no-op) if id doesn't exist{% if field.unique_per_parent %}, or if a sibling {{klass.name}} sharing the current {{klass.get_parent_field().name}} already has this {{field.name}} (unique_per_parent){% endif %}.
                {%- if field.parent and klass.fields | selectattr("unique_per_parent") | list %}
        /// NOTE: this Klass has a unique_per_parent field
        /// ({%- for f in klass.fields | selectattr("unique_per_parent") -%}{{f.name}}{% if not loop.last %}, {% endif %}{%- endfor -%}) -
        /// reparenting via this setter does NOT move that field's
        /// sibling-index entries to the new parent's bucket (no generated
        /// or hand-written caller reparents a {{klass.name}} today, so
        /// this is a documented, currently-unreachable gap rather than a fix).
                {%- endif %}
        bool set_{{klass.to_snake_case()}}_{{field.name}}({{klass.name}}Id id, {{field.get_cpp_type()}} value) {
            auto* existing = {{klass.to_snake_case()}}_.get(id);
            if (!existing) return false;
            if (existing->{{field.name}} == value) return true;

            {%- if field.unique_per_parent %}
            {
                auto& siblings = index_.{{klass.to_snake_case()}}_by_{{field.name}}[existing->{{klass.get_parent_field().name}}];
                if (siblings.find(value) != siblings.end())
                    return false;
                siblings.erase(existing->{{field.name}});
                existing->{{field.name}} = value;
                siblings[value] = id;
            }
            return true;
            {%- elif field.parent %}
                {%- if field._parent_field.is_list %}
            {
                auto& old_siblings = index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}[existing->{{field.name}}];
                old_siblings.erase(std::remove(old_siblings.begin(), old_siblings.end(), id), old_siblings.end());
            }
                {%- else %}
            index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.erase(existing->{{field.name}});
                {%- endif %}

            existing->{{field.name}} = value;

                {%- if field._parent_field.is_list %}
            {
                auto& new_siblings = index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}[value];
                if (std::find(new_siblings.begin(), new_siblings.end(), id) == new_siblings.end())
                    new_siblings.push_back(id);
            }
                {%- else %}
            index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}[value] = id;
                {%- endif %}
            return true;
            {%- else %}
            index_.{{klass.to_snake_case()}}_by_{{field.name}}.erase(existing->{{field.name}});

            existing->{{field.name}} = value;

            index_.{{klass.to_snake_case()}}_by_{{field.name}}[value] = id;
            return true;
            {%- endif %}
        }
            {%- endif %}
        {%- endfor %}

        /// @brief Get mutable {{klass.name}}Data from a {{klass.name}}Id
        {{klass.name}}Data* get_{{klass.to_snake_case()}}({{klass.name}}Id id) { return {{klass.to_snake_case()}}_.get(id); }

        /// @brief Get immutable {{klass.name}}Data from a {{klass.name}}Id
        const {{klass.name}}Data* get_{{klass.to_snake_case()}}({{klass.name}}Id id) const { return {{klass.to_snake_case()}}_.get(id); }

        /// @brief Get all {{klass.name}}Ids
        const std::vector<{{klass.name}}Id> get_{{klass.to_snake_case()}}_ids() const { return {{klass.to_snake_case()}}_.ids(); }

        /// @brief Test if {{klass.name}} pool is empty
        bool is_{{klass.to_snake_case()}}_empty() { return {{klass.to_snake_case()}}_.is_empty(); }

        /// @brief Clear all data from {{klass.name}} pool
        void clear_{{klass.to_snake_case()}}() { return {{klass.to_snake_case()}}_.clear(); }

        /// @brief Get size of {{klass.name}} pool
        uint64_t get_{{klass.to_snake_case()}}_size() { return {{klass.to_snake_case()}}_.size(); }

        /// @brief Iterate through {{klass.name}}Ids
        template <typename Fn>
        void for_each_{{klass.to_snake_case()}}_id(Fn&& fn) const {
            return {{klass.to_snake_case()}}_.for_each_id(fn);
        }

        /// @brief Collect every {{klass.name}}Id whose data satisfies
        /// `predicate(root, id, data) -> bool`. A linear scan - no
        /// index-fast-path, correctness first (see TCL_EXPLORATION.md's
        /// "Filter-expression architecture" for why). Not domain-specific:
        /// filter-expression parsing/evaluation is hand-written elsewhere
        /// (get_{{klass.to_snake_case()}}_field()/match_{{klass.to_snake_case()}}_hop() in
        /// {{klass.to_snake_case()}}.hpp supply the per-field metadata that
        /// evaluator needs) - this just wraps for_each_{{klass.to_snake_case()}}_id()
        /// with a generic predicate callback.
        template <typename Predicate>
        std::vector<{{klass.name}}Id> search_{{klass.to_snake_case()}}(Predicate&& predicate) const {
            std::vector<{{klass.name}}Id> results;
            for_each_{{klass.to_snake_case()}}_id([&]({{klass.name}}Id id) {
                if (predicate(*this, id, *{{klass.to_snake_case()}}_.get(id))) {
                    results.push_back(id);
                }
            });
            return results;
        }

        {%- for field in klass.get_ordered_fields() %}
            {%- if field.is_child and field.is_reference() %}
                {%- if field.is_list %}
        /// @brief Get {{field.name}} {{field.type}}Ids for the specified {{klass.name}}Id
        const std::vector<{{field.type}}Id>& get_{{klass.to_snake_case()}}_{{field.name}}({{klass.name}}Id id) const {
            static const std::vector<{{field.type}}Id> empty;
            auto it = index_.{{klass.to_snake_case()}}_{{field.name}}.find(id);
            return it == index_.{{klass.to_snake_case()}}_{{field.name}}.end() ? empty : it->second;
        }
                {%- else %}
        /// @brief Get {{field.name}} {{field.type}}Id for the specified {{klass.name}}Id
        const {{field.type}}Id get_{{klass.to_snake_case()}}_{{field.name}}({{klass.name}}Id id) const {
            static const {{field.type}}Id empty;
            auto entry = index_.{{klass.to_snake_case()}}_{{field.name}}.find(id);
            if (entry == index_.{{klass.to_snake_case()}}_{{field.name}}.end())
                return empty;
            return entry->second;
        }
                {%- endif %}
            {%- elif field.index and field.unique_per_parent %}
        /// @brief Get the {{klass.name}}Id for the specified {{field.name}},
        /// scoped to one {{klass.get_parent_field().type}}Id sibling group
        /// (unique_per_parent - {{field.name}} is only unique within one
        /// {{klass.get_parent_field().type}}, not globally).
        {{klass.name}}Id get_{{klass.to_snake_case()}}_by_{{field.name}}({{klass.get_parent_field().type}}Id {{klass.get_parent_field().name}}, const {{field.get_cpp_type()}}& {{field.name}}) const {
            auto parent_it = index_.{{klass.to_snake_case()}}_by_{{field.name}}.find({{klass.get_parent_field().name}});
            if (parent_it == index_.{{klass.to_snake_case()}}_by_{{field.name}}.end())
                return {{klass.name}}Id{};
            auto it = parent_it->second.find({{field.name}});
            return it == parent_it->second.end() ? {{klass.name}}Id{} : it->second;
        }
            {%- elif field.index %}
        /// @brief Get {{klass.name}}Ids for the specified {{field.name}}
        {{klass.name}}Id get_{{klass.to_snake_case()}}_by_{{field.name}}(const {{field.get_cpp_type()}}& {{field.name}}) const {
            auto it = index_.{{klass.to_snake_case()}}_by_{{field.name}}.find({{field.name}});
            return it == index_.{{klass.to_snake_case()}}_by_{{field.name}}.end() ? {{klass.name}}Id{} : it->second;
        }
            {%- endif %}
        {%- endfor %}
    {%- endfor %}

    private:
        uint64_t mutation_version_ = 0;
    {%- for klass in schema.get_pool_classes() %}
        Pool<{{klass.name}}Data, {{klass.name}}Id> {{klass.to_snake_case()}}_;
    {%- endfor %}
        Index index_;
    };
}
"""
