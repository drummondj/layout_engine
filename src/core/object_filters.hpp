#pragma once
#include "../database/database.hpp"
#include <algorithm>
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

    /// @brief Placement.type and Route.use values to filter out, upper-case.
    /// The Layers panel's per-value visibility (rendering and selection)
    /// and selectability (selection only) each use one of these. Empty
    /// sets filter nothing.
    struct ObjectFilterSets
    {
        std::set<std::string> placement_types;
        std::set<std::string> route_uses;

        bool empty() const { return placement_types.empty() && route_uses.empty(); }
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
