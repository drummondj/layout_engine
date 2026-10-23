#pragma once
#include "../database/database.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace le
{
    /// @brief The filter value of a Placement whose reference design has no
    /// Abstract.type (or no Abstract at all).
    inline constexpr std::string_view kUnsetFilterValue = "UNSET";

    /// @brief Placement.type/placement_status and Route.use values, and a
    /// route Wire's or Shape's routing_status/route_shape values, to filter
    /// out, upper-case. The Layers panel's per-value visibility (rendering
    /// and selection) and selectability (selection only) each use one of
    /// these. Empty sets filter nothing.
    struct ObjectFilterSets
    {
        std::set<std::string> placement_types;
        std::set<std::string> route_uses;
        std::set<std::string> placement_statuses;
        std::set<std::string> routing_statuses;
        std::set<std::string> route_shapes;

        bool empty() const
        {
            return placement_types.empty() && route_uses.empty() && placement_statuses.empty() && routing_statuses.empty() &&
                   route_shapes.empty();
        }
        bool operator==(const ObjectFilterSets &) const = default;
    };

    /// @brief `value` upper-cased, or kUnsetFilterValue when unset or empty
    /// - LEF/DEF keywords are case-insensitive.
    inline std::string to_filter_value(const std::optional<std::string> &value)
    {
        if (!value || value->empty())
            return std::string(kUnsetFilterValue);
        std::string upper = *value;
        std::ranges::transform(upper, upper.begin(), [](unsigned char c)
                               { return static_cast<char>(std::toupper(c)); });
        return upper;
    }

    /// @brief A Placement's type: its reference design's Abstract.type (LEF
    /// MACRO CLASS, e.g. "CORE" or "CORE SPACER").
    inline std::string placement_type(const Root &root, DesignId reference_design)
    {
        const AbstractData *abstract = root.get_abstract(root.get_design_abstract(reference_design));
        return to_filter_value(abstract ? abstract->type : std::nullopt);
    }

    /// @brief A Route's use; unset means SIGNAL, DEF's default USE.
    inline std::string route_use(const RouteData &route)
    {
        return route.use && !route.use->empty() ? to_filter_value(route.use) : std::string("SIGNAL");
    }

    /// @brief Every Route.use value the Layers panel offers: DEF NETS/
    /// SPECIALNETS USE.
    inline const std::vector<std::string> &route_use_values()
    {
        static const std::vector<std::string> values{"SIGNAL", "POWER", "GROUND", "CLOCK", "TIEOFF", "ANALOG", "SCAN", "RESET"};
        return values;
    }

    /// @brief Every Placement.placement_status value the Layers panel offers.
    inline const std::vector<std::string> &placement_status_values()
    {
        static const std::vector<std::string> values{"UNPLACED", "PLACED", "FIXED", "COVER", "SOFTFIXED"};
        return values;
    }

    /// @brief Every routing_status value the Layers panel offers; an unset
    /// one is ROUTED.
    inline const std::vector<std::string> &routing_status_values()
    {
        static const std::vector<std::string> values{"ROUTED", "FIXED", "COVER", "NOSHIELD", "SHIELD"};
        return values;
    }

    /// @brief Every route_shape value the Layers panel offers (DEF
    /// SPECIALNETS SHAPE), kUnsetFilterValue last for wiring with none.
    inline const std::vector<std::string> &route_shape_values()
    {
        static const std::vector<std::string> values{"RING",      "PADRING",      "BLOCKRING", "STRIPE",   "FOLLOWPIN",
                                                     "IOWIRE",    "COREWIRE",     "BLOCKWIRE", "BLOCKAGEWIRE",
                                                     "FILLWIRE",  "FILLWIREOPC",  "DRCFILL",   std::string(kUnsetFilterValue)};
        return values;
    }

    /// @brief A set of filter values of one enum as a flag per enum value,
    /// plus one for unset - so a per-object check is one array lookup.
    template <typename Enum>
    struct EnumFilterMask
    {
        static constexpr std::size_t kUnset = 32;

        std::array<bool, kUnset + 1> hidden{};
        bool any = false;

        /// @brief The mask of `values`, each an enum name (as to_string
        /// spells it) or kUnsetFilterValue; other names are ignored.
        template <typename FromString>
        static EnumFilterMask from(const std::set<std::string> &values, FromString &&from_string)
        {
            EnumFilterMask mask;
            for (const std::string &value : values)
            {
                if (value == kUnsetFilterValue)
                    mask.hidden[kUnset] = mask.any = true;
                else if (const std::optional<Enum> parsed = from_string(value); parsed && static_cast<std::size_t>(*parsed) < kUnset)
                    mask.hidden[static_cast<std::size_t>(*parsed)] = mask.any = true;
            }
            return mask;
        }

        bool hides(std::optional<Enum> value) const
        {
            return any && hidden[value ? static_cast<std::size_t>(*value) : kUnset];
        }

        bool operator==(const EnumFilterMask &) const = default;
    };

    /// @brief The routing_status and route_shape filters, for a route's
    /// Wire or Shape.
    struct RouteWiringFilter
    {
        EnumFilterMask<RoutingStatus> statuses;
        EnumFilterMask<RouteShape> shapes;

        static RouteWiringFilter from(const ObjectFilterSets &sets)
        {
            return RouteWiringFilter{
                .statuses = EnumFilterMask<RoutingStatus>::from(sets.routing_statuses, [](std::string_view v)
                                                               { return routing_status_from_string(v); }),
                .shapes = EnumFilterMask<RouteShape>::from(sets.route_shapes, [](std::string_view v)
                                                           { return route_shape_from_string(v); }),
            };
        }

        bool any() const { return statuses.any || shapes.any; }

        /// @brief Whether `wiring` (a WireData or ShapeData) is filtered
        /// out; an unset routing_status is ROUTED.
        template <typename Wiring>
        bool hides(const Wiring &wiring) const
        {
            return statuses.hides(wiring.routing_status.value_or(RoutingStatus::ROUTED)) || shapes.hides(wiring.route_shape);
        }

        bool operator==(const RouteWiringFilter &) const = default;
    };

    /// @brief The placement_status filter.
    inline EnumFilterMask<PlacementStatus> placement_status_mask(const ObjectFilterSets &sets)
    {
        return EnumFilterMask<PlacementStatus>::from(sets.placement_statuses, [](std::string_view v)
                                                     { return placement_status_from_string(v); });
    }

    /// @brief Every distinct Placement.type a Design in `root` gives its
    /// placements, sorted, with kUnsetFilterValue last when any Design has
    /// no typed Abstract. Scans Designs (library cells), not placements.
    inline std::vector<std::string> placement_type_values(const Root &root)
    {
        std::set<std::string> types;
        bool any_unset = false;
        for (const DesignId design_id : root.get_design_ids())
        {
            std::string type = placement_type(root, design_id);
            if (type == kUnsetFilterValue)
                any_unset = true;
            else
                types.insert(std::move(type));
        }
        std::vector<std::string> values(types.begin(), types.end());
        if (any_unset)
            values.emplace_back(kUnsetFilterValue);
        return values;
    }

    /// @brief The Designs whose placements have a type in `types` - so a
    /// per-placement check is one hash lookup on reference_design.
    inline std::unordered_set<DesignId> designs_of_placement_types(const Root &root, const std::set<std::string> &types)
    {
        std::unordered_set<DesignId> designs;
        if (types.empty())
            return designs;
        for (const DesignId design_id : root.get_design_ids())
            if (types.contains(placement_type(root, design_id)))
                designs.insert(design_id);
        return designs;
    }
}
