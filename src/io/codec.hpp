#pragma once
#include "byte_io.hpp"
#include "generated/database/native_tables.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Value encoding for the native file format (docs/NATIVE_FILE_FORMAT_RESEARCH.md
// §3 "Value encoding"). One column per stored field; within a column, one
// value per row, each encoded as:
//
//   reference (parent or plain)  varint: dense row index + 1, 0 = unset
//   integer (int/dbu/dbu2)       zig-zag varint
//   floating point               8-byte IEEE double
//   bool                         1 byte
//   string                       varint index into the file's string table
//   enum                         varint string-table index of the value's *name*
//   presence-flagged value       1 byte (0/1), then the value if 1
//   list                         varint count, then the elements
//   embedded struct              its stored fields, in the file's declared order
//   owner (<Klass>Owner)         1 byte (0/1); if 1, the option's name as a
//                                string-table index, then varint parent row
//
// Encoding is driven by the current C++ types. Decoding is driven by the
// *file's* schema descriptor: fields and struct members are matched by
// name, so fields added, removed or reordered since the file was written
// need no code (§4 "automatic name matching").

namespace le::persistence
{
    namespace nt = le::native_tables;

    inline constexpr uint32_t kNoRow = std::numeric_limits<uint32_t>::max();

    template <class T>
    struct IsOptional : std::false_type
    {
    };
    template <class T>
    struct IsOptional<std::optional<T>> : std::true_type
    {
        using value_type = T;
    };
    template <class T>
    struct IsVector : std::false_type
    {
    };
    template <class T>
    struct IsVector<std::vector<T>> : std::true_type
    {
        using value_type = T;
    };
    template <class T>
    struct IsVector<le::CompactVector<T>> : std::true_type
    {
        using value_type = T;
    };
    template <class T>
    struct IsId : std::false_type
    {
    };
    template <class Tag>
    struct IsId<le::Id<Tag>> : std::true_type
    {
        using tag = Tag;
    };
    template <class T>
    concept HasFields = requires { nt::Fields<T>::members; };
    template <class T>
    concept IsOwner = requires { nt::OwnerInfo<T>::name; };

    // --- Encoding ------------------------------------------------------------

    struct EncodeContext
    {
        const Root *root = nullptr;
        /// Per pooled class: slot index -> dense row index (kNoRow if dead).
        std::array<std::vector<uint32_t>, nt::kPooledClassCount> dense;
        /// References to an object that no longer exists, written as unset.
        uint64_t dangling_references = 0;

        uint32_t intern(std::string_view value)
        {
            auto it = string_ids_.find(value);
            if (it != string_ids_.end())
                return it->second;
            const auto id = static_cast<uint32_t>(strings_.size());
            strings_.push_back(std::make_unique<std::string>(value));
            string_ids_.emplace(*strings_.back(), id);
            return id;
        }

        const std::vector<std::unique_ptr<std::string>> &strings() const { return strings_; }

    private:
        std::vector<std::unique_ptr<std::string>> strings_;
        std::unordered_map<std::string_view, uint32_t> string_ids_;
    };

    template <class T>
    void encode_value(ByteWriter &w, const T &value, EncodeContext &ctx)
    {
        if constexpr (IsOwner<T>)
        {
            bool written = false;
            nt::OwnerInfo<T>::with_kind(value.kind, [&]<class P>(P, std::string_view option) {
                const auto &dense = ctx.dense[P::index];
                const typename P::Id parent{value.index, value.generation};
                if (value.index < dense.size() && dense[value.index] != kNoRow && P::pool(*ctx.root).contains(parent))
                {
                    w.u8(1);
                    w.varint(ctx.intern(option));
                    w.varint(dense[value.index]);
                    written = true;
                }
                else
                    ++ctx.dangling_references;
            });
            if (!written)
                w.u8(0);
        }
        else if constexpr (IsId<T>::value)
        {
            using P = nt::Pooled<typename IsId<T>::tag>;
            if (!value.valid())
            {
                w.varint(0);
                return;
            }
            const auto &dense = ctx.dense[P::index];
            if (value.index < dense.size() && dense[value.index] != kNoRow && P::pool(*ctx.root).contains(value))
                w.varint(uint64_t{dense[value.index]} + 1);
            else
            {
                ++ctx.dangling_references;
                w.varint(0);
            }
        }
        else if constexpr (IsOptional<T>::value)
        {
            w.u8(value.has_value() ? 1 : 0);
            if (value)
                encode_value(w, *value, ctx);
        }
        else if constexpr (IsVector<T>::value)
        {
            w.varint(value.size());
            for (const auto &element : value)
                encode_value(w, element, ctx);
        }
        else if constexpr (std::is_same_v<T, std::string>)
            w.varint(ctx.intern(value));
        else if constexpr (std::is_same_v<T, bool>)
            w.u8(value ? 1 : 0);
        else if constexpr (std::is_enum_v<T>)
            w.varint(ctx.intern(nt::EnumInfo<T>::to_name(value)));
        else if constexpr (std::is_integral_v<T>)
            w.svarint(static_cast<int64_t>(value));
        else if constexpr (std::is_floating_point_v<T>)
            w.fixed<double>(static_cast<double>(value));
        else
        {
            static_assert(HasFields<T>, "no native-format encoding for this type");
            std::apply([&](const auto &...member) { (encode_value(w, value.*(member.ptr), ctx), ...); }, nt::Fields<T>::members);
        }
    }

    // --- The file's schema, as the decoder sees it ----------------------------

    struct FileStruct;

    /// @brief One stored value's type in the file.
    struct FileType
    {
        enum class Kind
        {
            Int,
            Float,
            Bool,
            Str,
            Enum,
            Ref,
            Struct,
            Owner,
        };
        Kind kind = Kind::Int;
        std::string name; // Enum/Ref/Struct: the type's schema name; scalars: the schema type ("dbu", ...)
        bool presence = false;
        bool list = false;
        const FileStruct *structure = nullptr; // Kind::Struct only
        /// Kind::Owner only: each option's name and the class it refers to.
        std::vector<std::pair<std::string, std::string>> owner_options;
    };

    struct FileField
    {
        std::string name;
        FileType type;
    };

    /// @brief A pooled class or embedded struct as the file describes it
    /// (stored fields only - child fields are derived, never written).
    struct FileStruct
    {
        std::string name;
        bool pooled = false;
        std::vector<FileField> fields;

        /// For each field, which member of the current struct of the same
        /// name it maps to (-1: none, skip it), and whether that is simply
        /// field i -> member i for every member (the usual case - decoded
        /// with no per-field lookup). Computed once, on first use, by
        /// whichever decoding thread gets there first.
        mutable std::once_flag plan_once;
        mutable std::vector<int> plan;
        mutable bool identity = false;
    };

    // --- Compatibility --------------------------------------------------------

    /// @brief Whether a value of the file's type `ft` can be read into a
    /// current `T` - empty if so, otherwise why not. `ft`'s own presence/
    /// list flags are the outer layer being matched against T.
    template <class T>
    std::string incompatibility(const FileType &ft, bool presence, bool list)
    {
        using K = FileType::Kind;
        if constexpr (IsOwner<T>)
        {
            if (ft.kind != K::Owner || list || presence)
                return "was not an owner, now " + std::string(nt::OwnerInfo<T>::name);
            for (const auto &[option, klass] : ft.owner_options)
            {
                std::string problem = "owner option " + option + " no longer exists";
                nt::OwnerInfo<T>::with_option(option, [&]<class P>(P, auto) {
                    problem = klass == P::name ? std::string{} : "owner option " + option + " referred to " + klass + ", now refers to " + std::string(P::name);
                });
                if (!problem.empty())
                    return problem;
            }
            return {};
        }
        else if constexpr (IsId<T>::value)
        {
            using P = nt::Pooled<typename IsId<T>::tag>;
            if (ft.kind != K::Ref || list)
                return "was not a reference, now a reference to " + std::string(P::name);
            if (ft.name != P::name)
                return "referred to " + ft.name + ", now refers to " + std::string(P::name);
            return {};
        }
        else if constexpr (IsOptional<T>::value)
        {
            if (list)
                return "was a list, now a single optional value";
            return incompatibility<typename IsOptional<T>::value_type>(ft, false, false);
        }
        else if constexpr (IsVector<T>::value)
            return incompatibility<typename IsVector<T>::value_type>(ft, false, false); // an absent optional value reads as an empty list
        else
        {
            if (list)
                return "was a list, now a single value";
            if (presence)
                return "was optional, now required";
            if constexpr (std::is_same_v<T, std::string>)
                return ft.kind == K::Str ? std::string{} : "was not a string, now a string";
            else if constexpr (std::is_same_v<T, bool>)
                return ft.kind == K::Bool ? std::string{} : "was not a bool, now a bool";
            else if constexpr (std::is_enum_v<T>)
            {
                if (ft.kind != K::Enum)
                    return "was not an enum, now " + std::string(nt::EnumInfo<T>::name);
                if (ft.name != nt::EnumInfo<T>::name)
                    return "was enum " + ft.name + ", now " + std::string(nt::EnumInfo<T>::name);
                return {};
            }
            else if constexpr (std::is_integral_v<T>)
                return ft.kind == K::Int ? std::string{} : "was not an integer, now an integer";
            else if constexpr (std::is_floating_point_v<T>)
                return ft.kind == K::Float || ft.kind == K::Int ? std::string{} : "was not a number, now floating point";
            else
            {
                static_assert(HasFields<T>);
                if (ft.kind != K::Struct || ft.name != nt::Fields<T>::name)
                    return "was not a " + std::string(nt::Fields<T>::name) + ", now one";
                std::string problem;
                for (const FileField &field : ft.structure->fields)
                {
                    std::apply(
                        [&](const auto &...member) {
                            ((member.name == field.name && problem.empty()
                                  ? (void)(problem = incompatibility<typename std::remove_cvref_t<decltype(member)>::type>(field.type, field.type.presence, field.type.list))
                                  : void()),
                             ...);
                        },
                        nt::Fields<T>::members);
                    if (!problem.empty())
                        return ft.name + "." + field.name + " " + problem;
                }
                return {};
            }
        }
    }

    // --- Decoding -------------------------------------------------------------

    struct DecodeContext
    {
        /// The file's string table (shared, read-only - one DecodeContext per
        /// decoding thread points at the same one).
        const std::vector<std::string_view> *strings = nullptr;
        /// Rows per pooled class (current dense index), for range-checking references.
        std::array<uint64_t, nt::kPooledClassCount> row_counts{};
        /// Enum values renamed by migrations since the file was written:
        /// enum name -> (stored name -> current name). Null when none.
        const std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> *enum_renames = nullptr;

        std::string_view string_at(ByteReader &r)
        {
            const uint64_t index = r.varint();
            if (!strings || index >= strings->size())
                r.fail("string index out of range");
            return (*strings)[index];
        }
    };

    /// @brief Skip one value of the file's type (a struct member the current
    /// schema no longer has).
    void skip_value(ByteReader &r, const FileType &ft, bool presence, bool list, DecodeContext &ctx);

    template <class T>
    void decode_value(ByteReader &r, const FileType &ft, bool presence, bool list, T &out, DecodeContext &ctx)
    {
        using K = FileType::Kind;
        if constexpr (IsOwner<T>)
        {
            const uint8_t present = r.u8();
            if (present > 1)
                r.fail("bad owner presence byte");
            out = T{};
            if (!present)
                return;
            const std::string_view option = ctx.string_at(r);
            const uint64_t row = r.varint();
            const bool known = nt::OwnerInfo<T>::with_option(option, [&]<class P>(P, auto kind) {
                if (row >= ctx.row_counts[P::index])
                    r.fail("owner " + std::string(option) + " row " + std::to_string(row) + " past the last " + std::string(P::name) + " row");
                out = T{kind, static_cast<uint32_t>(row), 0};
            });
            if (!known)
                r.fail("unknown owner option " + std::string(option));
        }
        else if constexpr (IsId<T>::value)
        {
            using P = nt::Pooled<typename IsId<T>::tag>;
            const uint64_t value = r.varint();
            if (value == 0)
                out = T{};
            else if (value - 1 >= ctx.row_counts[P::index])
                r.fail("reference to " + std::string(P::name) + " row " + std::to_string(value - 1) + " past the last row");
            else
                out = T{static_cast<uint32_t>(value - 1), 0};
        }
        else if constexpr (IsOptional<T>::value)
        {
            if (presence)
            {
                const uint8_t present = r.u8();
                if (present > 1)
                    r.fail("bad presence byte");
                if (!present)
                {
                    out.reset();
                    return;
                }
            }
            typename IsOptional<T>::value_type value{};
            decode_value(r, ft, false, false, value, ctx);
            out = std::move(value);
        }
        else if constexpr (IsVector<T>::value)
        {
            if (list)
            {
                const uint64_t n = r.count();
                out.resize(n);
                for (auto &element : out)
                    decode_value(r, ft, false, false, element, ctx);
            }
            else
            {
                // T -> list<T>: a single value becomes a one-element list,
                // an absent optional one an empty list.
                if (presence)
                {
                    const uint8_t present = r.u8();
                    if (present > 1)
                        r.fail("bad presence byte");
                    if (!present)
                    {
                        out.clear();
                        return;
                    }
                }
                out.resize(1);
                decode_value(r, ft, false, false, out[0], ctx);
            }
        }
        else if constexpr (std::is_same_v<T, std::string>)
            out.assign(ctx.string_at(r));
        else if constexpr (std::is_same_v<T, bool>)
        {
            const uint8_t value = r.u8();
            if (value > 1)
                r.fail("bad bool");
            out = value != 0;
        }
        else if constexpr (std::is_enum_v<T>)
        {
            std::string_view name = ctx.string_at(r);
            if (ctx.enum_renames)
                if (auto e = ctx.enum_renames->find(nt::EnumInfo<T>::name); e != ctx.enum_renames->end())
                    if (auto renamed = e->second.find(name); renamed != e->second.end())
                        name = renamed->second;
            const auto value = nt::EnumInfo<T>::from_name(name);
            if (!value)
                r.fail(std::string(nt::EnumInfo<T>::name) + " has no value " + std::string(name) + " any more (a migration is needed)");
            out = *value;
        }
        else if constexpr (std::is_integral_v<T>)
        {
            const int64_t value = r.svarint();
            bool in_range;
            if constexpr (std::is_signed_v<T>)
                in_range = value >= static_cast<int64_t>(std::numeric_limits<T>::min()) && value <= static_cast<int64_t>(std::numeric_limits<T>::max());
            else
                in_range = value >= 0 && static_cast<uint64_t>(value) <= static_cast<uint64_t>(std::numeric_limits<T>::max());
            if (!in_range)
                r.fail("integer " + std::to_string(value) + " out of range for its field");
            out = static_cast<T>(value);
        }
        else if constexpr (std::is_floating_point_v<T>)
            out = static_cast<T>(ft.kind == K::Int ? static_cast<double>(r.svarint()) : r.fixed<double>());
        else
        {
            static_assert(HasFields<T>);
            const FileStruct &file = *ft.structure;
            std::call_once(file.plan_once, [&] {
                constexpr size_t members = std::tuple_size_v<std::remove_cvref_t<decltype(nt::Fields<T>::members)>>;
                file.identity = file.fields.size() == members;
                for (size_t f = 0; f < file.fields.size(); ++f)
                {
                    int index = -1, i = 0;
                    std::apply([&](const auto &...member) { ((member.name == file.fields[f].name ? (void)(index = i) : void(), ++i), ...); }, nt::Fields<T>::members);
                    file.plan.push_back(index);
                    file.identity = file.identity && index == static_cast<int>(f);
                }
            });
            const auto &fields = file.fields;
            if (file.identity)
            {
                size_t f = 0;
                std::apply(
                    [&](const auto &...member) {
                        ((decode_value(r, fields[f].type, fields[f].type.presence, fields[f].type.list, out.*(member.ptr), ctx), ++f), ...);
                    },
                    nt::Fields<T>::members);
                return;
            }
            const auto &mapping = file.plan;
            for (size_t f = 0; f < fields.size(); ++f)
            {
                const FileType &field_type = fields[f].type;
                if (mapping[f] < 0)
                {
                    skip_value(r, field_type, field_type.presence, field_type.list, ctx);
                    continue;
                }
                int i = 0;
                std::apply(
                    [&](const auto &...member) {
                        ((i++ == mapping[f] ? decode_value(r, field_type, field_type.presence, field_type.list, out.*(member.ptr), ctx) : void()), ...);
                    },
                    nt::Fields<T>::members);
            }
        }
    }
}
