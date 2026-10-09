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
#include <string_view>
#include <vector>

namespace {{schema.namespace}} {
    /// @brief Which pool-backed class a ChangeLogEntry names.
    enum class ChangeKlass : uint16_t {
    {%- for klass in schema.get_pool_classes() %}
        {{klass.name}},
    {%- endfor %}
        None,
    };

    /// @brief What happened to the object a ChangeLogEntry names. NOTE
    /// marks an in-place edit made through a mutable get_<klass>()
    /// pointer, reported by its caller via note_<klass>_changed().
    enum class ChangeOp : uint8_t { CREATE, UPDATE, DELETE, NOTE };

    /// @brief An object's owner at the time a ChangeLogEntry was
    /// recorded: its first set parent field - `slot` is that field's index
    /// among the class's parent fields (Root::change_parent_field_name
    /// names it) - or ChangeKlass::None for a parentless object.
    struct ChangeParent {
        ChangeKlass klass = ChangeKlass::None;
        uint8_t slot = 0;
        uint32_t index = 0;
        uint32_t generation = 0;

        friend bool operator==(const ChangeParent &, const ChangeParent &) = default;
    };

    /// @brief One database mutation, recorded by every generated create_/
    /// update_/delete_/set_ call and by note_<klass>_changed(). An update
    /// records the owner before the change and, if it moved, a second
    /// entry with the new one - so a consumer can find everything an edit
    /// affected even after the object (or its owner) is gone.
    struct ChangeLogEntry {
        ChangeKlass klass = ChangeKlass::None;
        ChangeOp op = ChangeOp::UPDATE;
        uint32_t index = 0;
        uint32_t generation = 0;
        ChangeParent parent;
    };

    /// @brief A fixed-capacity ring of the most recent ChangeLogEntries,
    /// addressed by an ever-increasing sequence number - lets a consumer
    /// (e.g. a render stage) update incrementally instead of recomputing
    /// everything after an edit. A consumer remembers end_sequence() and
    /// later asks covers(that) before reading for_each_since(that); if
    /// the ring wrapped past it (a bulk load logs millions of entries) or
    /// saturate() was called, covers() is false and the consumer must
    /// treat everything as changed. Allocated on first use.
    class ChangeLog {
    public:
        static constexpr uint64_t kCapacity = 1u << 16;

        uint64_t end_sequence() const { return end_; }

        bool covers(uint64_t since) const {
            return since >= barrier_ && since <= end_ && end_ - since <= kCapacity;
        }

        template <typename Fn>
        void for_each_since(uint64_t since, Fn &&fn) const {
            for (uint64_t sequence = since; sequence < end_; ++sequence)
                fn(ring_[sequence % kCapacity]);
        }

        void append(const ChangeLogEntry &entry) {
            if (ring_.empty())
                ring_.resize(kCapacity);
            ring_[end_ % kCapacity] = entry;
            ++end_;
        }

        /// @brief Invalidate every earlier sequence number: covers() is
        /// false for all of them from now on.
        void saturate() {
            ++end_;
            barrier_ = end_;
        }

    private:
        std::vector<ChangeLogEntry> ring_;
        uint64_t end_ = 0;
        uint64_t barrier_ = 0;
    };

    class Root {
    public:
        /// @brief Monotonic counter bumped by every bump_mutation_version()
        /// call - a cheap way for a caller (e.g. a render pipeline's own
        /// cache key) to tell whether *any* database content has changed
        /// since it last checked, without needing per-field/per-class
        /// change tracking. Mirrors LeHandle::selection_version()'s existing
        /// pattern (see api/le_handle.hpp) for the same reason: a hand-written,
        /// domain-specific mutation site (e.g. api.cpp's own CRUD
        /// functions) calls bump_mutation_version()
        /// explicitly, since not every mutation goes through a generated
        /// create_x/delete_x/set_x_<field> (e.g. appending to a plain list
        /// field via a mutable get_x() pointer never does) - this Root
        /// method only tracks the counter itself, not when to bump it.
        uint64_t mutation_version() const { return mutation_version_; }

        /// @brief Bump the counter returned by mutation_version(). Call
        /// once per logical mutation (not once per internal step of a
        /// multi-step one) after the database content actually changed.
        void bump_mutation_version() { ++mutation_version_; }

        /// @brief Every recent mutation (ChangeLog's own doc comment).
        const ChangeLog &change_log() const { return change_log_; }

        /// @brief Mark everything as changed - for a bulk mutation that
        /// bypasses the generated create_/update_/delete_ calls.
        void saturate_change_log() { change_log_.saturate(); }

        /// @brief The current owner of the (live) object `klass`/`index`/
        /// `generation` names - ChangeKlass::None if it's gone or has none.
        ChangeParent change_parent_of(ChangeKlass klass, uint32_t index, uint32_t generation) const {
            switch (klass) {
        {%- for klass in schema.get_pool_classes() %}
            case ChangeKlass::{{klass.name}}:
                if (const auto *d = {{klass.to_snake_case()}}_.get({{klass.name}}Id{index, generation}))
                    return change_parent_({{klass.name}}Id{}, *d);
                return {};
        {%- endfor %}
            case ChangeKlass::None:
                return {};
            }
            return {};
        }

        /// @brief The name of `klass`'s parent field number `slot`
        /// (ChangeParent::slot) - empty if out of range.
        static std::string_view change_parent_field_name(ChangeKlass klass, uint8_t slot) {
            switch (klass) {
        {%- for klass in schema.get_pool_classes() %}
            case ChangeKlass::{{klass.name}}: {
                {%- if klass.get_parent_fields() %}
                static constexpr std::string_view names[] = {
                {%- for field in klass.get_parent_fields() %}"{{field.name}}"{% if not loop.last %}, {% endif %}{%- endfor -%}
                };
                return slot < std::size(names) ? names[slot] : std::string_view{};
                {%- else %}
                return {};
                {%- endif %}
            }
        {%- endfor %}
            case ChangeKlass::None:
                return {};
            }
            return {};
        }

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
        {%- for field in klass.get_singular_parent_fields() %}
        ///
        /// Fallible: returns an invalid {{klass.name}}Id if the
        /// {{field.name}}'s {{field.parent}} is already set (one per
        /// {{field.type}}).
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
        {%- for field in klass.get_singular_parent_fields() %}
            if (data.{{field.accessor}}.valid() && index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.contains(data.{{field.accessor}}))
                return {{klass.name}}Id{};
        {%- endfor %}
            {{klass.name}}Id id = {{klass.to_snake_case()}}_.create(std::move(data));
            log_change_(ChangeKlass::{{klass.name}}, ChangeOp::CREATE, id, change_parent_(id, *{{klass.to_snake_case()}}_.get(id)));
            index_insert_{{klass.to_snake_case()}}_(id, *{{klass.to_snake_case()}}_.get(id));
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
        {%- if klass.get_parent_fields() | length == 1 and klass.get_singular_parent_fields() %}
        /// Also false if the new {{klass.get_parent_fields()[0].name}} already has a {{klass.get_parent_fields()[0].parent}}.
        {%- endif %}
        {%- if klass.get_parent_fields() | length > 1 %}
        /// {{klass.name}} has multiple parent fields ({%- for pf in klass.get_parent_fields() -%}{{pf.name}}{% if not loop.last %}, {% endif %}{%- endfor -%}) -
        /// no parent flag is generated at all here, since reassigning one
        /// alone would violate the "exactly one parent set" invariant
        /// create_{{klass.to_snake_case()}}() itself enforces.
        {%- endif %}
        bool update_{{klass.to_snake_case()}}({{klass.update_root_params()}}) {
            const auto *before = {{klass.to_snake_case()}}_.get(id);
            if (!before) return false;
            const ChangeParent parent_before = change_parent_(id, *before);
            const bool applied = [&]() -> bool {
{{klass.update_root_body()}}
            }();
            if (applied) {
                log_change_(ChangeKlass::{{klass.name}}, ChangeOp::UPDATE, id, parent_before);
                const ChangeParent parent_after = change_parent_(id, *{{klass.to_snake_case()}}_.get(id));
                if (!(parent_after == parent_before))
                    log_change_(ChangeKlass::{{klass.name}}, ChangeOp::UPDATE, id, parent_after);
            }
            return applied;
        }

        /// @brief Record an in-place edit of a {{klass.name}} made through
        /// the mutable get_{{klass.to_snake_case()}}() pointer, which the
        /// change log can't see on its own. Call once per edited object.
        void note_{{klass.to_snake_case()}}_changed({{klass.name}}Id id) {
            if (const auto *d = {{klass.to_snake_case()}}_.get(id))
                log_change_(ChangeKlass::{{klass.name}}, ChangeOp::NOTE, id, change_parent_(id, *d));
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
            log_change_(ChangeKlass::{{klass.name}}, ChangeOp::DELETE, id, change_parent_(id, *existing));

        {%- if klass.has_indecies() %}
            const auto& d = *existing;
        {%- for field in klass.get_ordered_fields() %}
            {%- if field.parent %}
                {%- if field._parent_field.is_list %}
            {
                auto it = index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.find(d.{{field.accessor}});
                if (it != index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.end()) {
                    it->second.erase(std::remove(it->second.begin(), it->second.end(), id), it->second.end());
                }
            }
                {%- else %}
            {
                auto it = index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.find(d.{{field.accessor}});
                if (it != index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.end() && it->second == id)
                    index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.erase(it);
            }
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
        {%- if klass.get_owner_fields() %}

        /// @brief Set {{klass.name}}'s owner (which owner field, and that
        /// parent), moving it from the old owner's child list to the new
        /// one's. False (no-op) if id doesn't exist{% if klass.get_singular_parent_fields() | selectattr("owner") | list %}, or if the new owner's
        /// single slot for it ({%- for field in klass.get_singular_parent_fields() | selectattr("owner") -%}{{field._parent_klass.name}}.{{field.parent}}{% if not loop.last %}, {% endif %}{%- endfor -%}) is already taken{% endif %}.
        bool set_{{klass.to_snake_case()}}_owner({{klass.name}}Id id, {{klass.owner_type_name()}} value) {
            auto* existing = {{klass.to_snake_case()}}_.get(id);
            if (!existing) return false;
            if (existing->owner == value) return true;
            {%- for field in klass.get_singular_parent_fields() | selectattr("owner") %}
            if (value.kind == {{klass.name}}OwnerKind::{{field.owner_kind()}} && index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.contains({{field.type}}Id{value.index, value.generation}))
                return false;
            {%- endfor %}
            const ChangeParent parent_before = change_parent_(id, *existing);
            log_change_(ChangeKlass::{{klass.name}}, ChangeOp::UPDATE, id, parent_before);
            switch (existing->owner.kind)
            {
            {%- for field in klass.get_owner_fields() %}
            case {{klass.name}}OwnerKind::{{field.owner_kind()}}:
                {%- if field._parent_field.is_list %}
            {
                auto& old_siblings = index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}[existing->{{field.accessor}}];
                old_siblings.erase(std::remove(old_siblings.begin(), old_siblings.end(), id), old_siblings.end());
                break;
            }
                {%- else %}
            {
                auto it = index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.find(existing->{{field.accessor}});
                if (it != index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.end() && it->second == id)
                    index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.erase(it);
                break;
            }
                {%- endif %}
            {%- endfor %}
            case {{klass.name}}OwnerKind::None:
                break;
            }
            existing->owner = value;
            index_insert_{{klass.to_snake_case()}}_owner_(id, *existing);
            const ChangeParent parent_after = change_parent_(id, *existing);
            if (!(parent_after == parent_before))
                log_change_(ChangeKlass::{{klass.name}}, ChangeOp::UPDATE, id, parent_after);
            return true;
        }
        {%- endif %}

        {%- for field in klass.get_ordered_fields() %}
            {%- if field.parent or field.index %}
        /// @brief Set {{klass.name}}'s {{field.name}}, keeping the relevant
        /// Root index in sync (unlike assigning through
        /// get_{{klass.to_snake_case()}}() directly, which would leave a
        /// stale index entry behind). False (no-op) if id doesn't exist{% if field.unique_per_parent %}, or if a sibling {{klass.name}} sharing the current {{klass.get_parent_field().name}} already has this {{field.name}} (unique_per_parent){% elif field.parent and not field.owner and not field._parent_field.is_list %}, or if the new {{field.name}} already has a {{field.parent}}{% endif %}.
                {%- if field.parent and klass.fields | selectattr("unique_per_parent") | list %}
        /// NOTE: this Klass has a unique_per_parent field
        /// ({%- for f in klass.fields | selectattr("unique_per_parent") -%}{{f.name}}{% if not loop.last %}, {% endif %}{%- endfor -%}) -
        /// reparenting via this setter does NOT move that field's
        /// sibling-index entries to the new parent's bucket (no generated
        /// or hand-written caller reparents a {{klass.name}} today, so
        /// this is a documented, currently-unreachable gap rather than a fix).
                {%- endif %}
        bool set_{{klass.to_snake_case()}}_{{field.name}}({{klass.name}}Id id, {{field.get_cpp_type()}} value) {
            {%- if field.owner %}
            const auto* existing = {{klass.to_snake_case()}}_.get(id);
            if (!existing) return false;
            // Clearing a field that isn't the current owner changes nothing.
            if (!value.valid() && existing->owner.kind != {{klass.name}}OwnerKind::{{field.owner_kind()}}) return true;
            return set_{{klass.to_snake_case()}}_owner(id, {{klass.owner_type_name()}}::{{field.name}}(value));
        }
            {%- else %}
            auto* existing = {{klass.to_snake_case()}}_.get(id);
            if (!existing) return false;
            if (existing->{{field.name}} == value) return true;
            {%- if field.parent and not field._parent_field.is_list %}
            if (value.valid() && index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.contains(value))
                return false;
            {%- endif %}
            log_change_(ChangeKlass::{{klass.name}}, ChangeOp::UPDATE, id, change_parent_(id, *existing));

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
            {
                auto it = index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.find(existing->{{field.name}});
                if (it != index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.end() && it->second == id)
                    index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}.erase(it);
            }
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
            {
                // Another object may hold the old name; leave its entry.
                auto old_it = index_.{{klass.to_snake_case()}}_by_{{field.name}}.find(existing->{{field.name}});
                if (old_it != index_.{{klass.to_snake_case()}}_by_{{field.name}}.end() && old_it->second == id)
                    index_.{{klass.to_snake_case()}}_by_{{field.name}}.erase(old_it);
            }

            existing->{{field.name}} = value;

            index_.{{klass.to_snake_case()}}_by_{{field.name}}[value] = id;
            return true;
            {%- endif %}
        }
            {%- endif %}
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
        void clear_{{klass.to_snake_case()}}() { change_log_.saturate(); return {{klass.to_snake_case()}}_.clear(); }

        /// @brief Get size of {{klass.name}} pool
        uint64_t get_{{klass.to_snake_case()}}_size() { return {{klass.to_snake_case()}}_.size(); }

        /// @brief Iterate through {{klass.name}}Ids
        template <typename Fn>
        void for_each_{{klass.to_snake_case()}}_id(Fn&& fn) const {
            return {{klass.to_snake_case()}}_.for_each_id(fn);
        }

        /// @brief Collect every {{klass.name}}Id whose data satisfies
        /// `predicate(root, id, data) -> bool`. A linear scan - no
        /// index-fast-path, correctness first. Not domain-specific:
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

        /// @brief Direct pool access - for the native file format
        /// (src/io/native_format.cpp) only, which saves pools as-is and loads them
        /// with Pool::load_dense() followed by rebuild_indexes(). Anything
        /// else must go through the generated create_/update_/delete_
        /// calls, which keep the indexes and the change log in step.
    {%- for klass in schema.get_pool_classes() %}
        Pool<{{klass.name}}Data, {{klass.name}}Id>& pool_{{klass.to_snake_case()}}() { return {{klass.to_snake_case()}}_; }
        const Pool<{{klass.name}}Data, {{klass.name}}Id>& pool_{{klass.to_snake_case()}}() const { return {{klass.to_snake_case()}}_; }
    {%- endfor %}

        /// @brief Empty every pool and index, and saturate the change log
        /// (everything changed).
        void clear_all() {
        {%- for klass in schema.get_pool_classes() %}
            {{klass.to_snake_case()}}_.clear();
        {%- endfor %}
            index_ = Index{};
            change_log_.saturate();
        }

        /// @brief Take over every pool and index from `other` (which is
        /// left empty), saturating the change log - how the native file
        /// format's loader swaps in a fully built and validated Root only
        /// once nothing can fail any more. mutation_version() is kept
        /// (callers bump it), so a version-keyed cache can never mistake
        /// the new contents for an old version.
        void replace_contents_from(Root&& other) {
        {%- for klass in schema.get_pool_classes() %}
            {{klass.to_snake_case()}}_ = std::move(other.{{klass.to_snake_case()}}_);
        {%- endfor %}
            index_ = std::move(other.index_);
            other.clear_all();
            change_log_.saturate();
        }

        /// @brief Rebuild every index from the pools, visiting each pool's
        /// live slots in index order - what create_<klass>() would have
        /// built had each object been created in that order. For after
        /// Pool::load_dense(). Returns a description of every
        /// unique_per_parent violation found (the duplicate keeps the
        /// index entry of whichever came later, as create_ would never
        /// have allowed); empty means the data is consistent. Saturates
        /// the change log.
        std::vector<std::string> rebuild_indexes() {
            std::vector<std::string> problems;
            index_ = Index{};
        {%- for klass in schema.get_pool_classes() %}
            rebuild_{{klass.to_snake_case()}}_index(problems);
        {%- endfor %}
            change_log_.saturate();
            return problems;
        }

    {%- for klass in schema.get_pool_classes() %}
        /// @brief rebuild_indexes() for {{klass.name}} alone, into an index
        /// that has no {{klass.name}} entries yet. Each class fills only its
        /// own index maps (its by-field lookups, and the child lists its
        /// parent fields name), so different classes' calls may run
        /// concurrently - the native file loader does. Appends to
        /// `problems`; doesn't touch the change log.
        void rebuild_{{klass.to_snake_case()}}_index(std::vector<std::string>& problems) {
            {{klass.to_snake_case()}}_.for_each_id([&]({{klass.name}}Id id) {
                [[maybe_unused]] const auto& d = *{{klass.to_snake_case()}}_.get(id);
            {%- for field in klass.get_ordered_fields() %}
                {%- if field.unique_per_parent %}
                if (d.{{klass.get_parent_field().name}}.valid()) {
                    const auto& siblings = index_.{{klass.to_snake_case()}}_by_{{field.name}}[d.{{klass.get_parent_field().name}}];
                    if (siblings.find(d.{{field.name}}) != siblings.end())
                        problems.push_back("{{klass.name}} " + std::to_string(id.index) + ": duplicate {{field.name}} under the same {{klass.get_parent_field().name}}");
                }
                {%- endif %}
            {%- endfor %}
                index_insert_{{klass.to_snake_case()}}_(id, d);
            });
        }
    {%- endfor %}

    private:
    {%- for klass in schema.get_pool_classes() %}
        {%- if klass.get_owner_fields() %}
        void index_insert_{{klass.to_snake_case()}}_owner_({{klass.name}}Id id, const {{klass.name}}Data& d) {
            switch (d.owner.kind)
            {
            {%- for field in klass.get_owner_fields() %}
            case {{klass.name}}OwnerKind::{{field.owner_kind()}}:
                {%- if field._parent_field.is_list %}
                index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}[d.{{field.accessor}}].push_back(id);
                {%- else %}
                index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}[d.{{field.accessor}}] = id;
                {%- endif %}
                break;
            {%- endfor %}
            case {{klass.name}}OwnerKind::None:
                break;
            }
        }
        {%- endif %}
        void index_insert_{{klass.to_snake_case()}}_([[maybe_unused]] {{klass.name}}Id id, [[maybe_unused]] const {{klass.name}}Data& d) {
        {%- if klass.get_owner_fields() %}
            index_insert_{{klass.to_snake_case()}}_owner_(id, d);
        {%- endif %}
        {%- for field in klass.get_ordered_fields() %}
            {%- if field.parent and not field.owner %}
                {%- if field._parent_field.is_list %}
            if (d.{{field.accessor}}.valid())
                index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}[d.{{field.accessor}}].push_back(id);
                {%- else %}
            if (d.{{field.accessor}}.valid())
                index_.{{field._parent_klass.to_snake_case()}}_{{field.parent}}[d.{{field.accessor}}] = id;
                {%- endif %}
            {%- elif field.index and field.unique_per_parent %}
            if (d.{{klass.get_parent_field().name}}.valid())
                index_.{{klass.to_snake_case()}}_by_{{field.name}}[d.{{klass.get_parent_field().name}}][d.{{field.name}}] = id;
            {%- elif field.index %}
            index_.{{klass.to_snake_case()}}_by_{{field.name}}[d.{{field.name}}] = id;
            {%- endif %}
        {%- endfor %}
        }
    {%- endfor %}

        template <typename IdT>
        void log_change_(ChangeKlass klass, ChangeOp op, IdT id, ChangeParent parent) {
            change_log_.append(ChangeLogEntry{.klass = klass, .op = op, .index = id.index, .generation = id.generation, .parent = parent});
        }

    {%- for klass in schema.get_pool_classes() %}
        static ChangeParent change_parent_({{klass.name}}Id, [[maybe_unused]] const {{klass.name}}Data &d) {
        {%- for field in klass.get_parent_fields() %}
            if (d.{{field.accessor}}.valid())
                return ChangeParent{.klass = ChangeKlass::{{field.type}}, .slot = {{loop.index0}}, .index = d.{{field.accessor}}.index, .generation = d.{{field.accessor}}.generation};
        {%- endfor %}
            return {};
        }
    {%- endfor %}

        ChangeLog change_log_;
        uint64_t mutation_version_ = 0;
    {%- for klass in schema.get_pool_classes() %}
        Pool<{{klass.name}}Data, {{klass.name}}Id> {{klass.to_snake_case()}}_;
    {%- endfor %}
        Index index_;
    };

    /// @brief Saturates `root`'s change log when it goes out of scope -
    /// for a bulk operation (a file reader, say) that may write through
    /// mutable get_<klass>() pointers without note_<klass>_changed().
    struct SaturateChangeLogOnExit {
        Root &root;
        ~SaturateChangeLogOnExit() { root.saturate_change_log(); }
    };
}
"""
