TEMPLATE = """
#pragma once
#include <compare>
#include <cstdint>
#include <limits>
#include <functional>
#include <iostream>

namespace {{schema.namespace}} {


    template <class Tag>
    struct Id
    {
        uint32_t index = std::numeric_limits<uint32_t>::max();
        uint32_t generation = 0;

        constexpr bool valid() const noexcept
        {
            return index != std::numeric_limits<uint32_t>::max();
        }

        friend constexpr auto operator<=>(const Id&, const Id&) = default;
    };

    template <class Tag>
    inline std::string to_string(Id<Tag> id)
    {
        if (!id.valid())
            return "Id{invalid}";

        return "Id{index=" + std::to_string(id.index) + ", generation=" + std::to_string(id.generation) + "}";
    };

    
    template <class Tag>
    std::ostream& operator<<(std::ostream& os, const Id<Tag>& id)
    {
        return os << to_string(id);
    }

    {%- for klass in schema.get_pool_classes() %}
    struct {{ klass.name }}Tag
    {
    };

    using {{ klass.name }}Id = Id<{{ klass.name }}Tag>;
    {%- endfor %}
}

namespace std
{
    template <class Tag>
    struct hash<{{schema.namespace}}::Id<Tag>>
    {
        size_t operator()(const {{schema.namespace}}::Id<Tag> &id) const noexcept
        {
            size_t h1 = std::hash<uint32_t>{}(id.index);
            size_t h2 = std::hash<uint32_t>{}(id.generation);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };
}


"""
