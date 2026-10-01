#pragma once
#include "../database/database.hpp"
#include <cctype>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace le
{
    /// @brief A FinFET placement grid (NEW_FEATURES_SEPT_2026.md item 2) -
    /// grid lines at `offset + k * pitch` along Y for horizontal fins, or
    /// along X for vertical ones.
    struct FinGrid
    {
        int64_t pitch = 0;
        int64_t offset = 0;
        bool horizontal = true;

        friend bool operator==(const FinGrid &, const FinGrid &) = default;
    };

    namespace fin_grid_detail
    {
        inline bool iequals(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        inline int64_t microns_to_dbu(double microns, double dbu_per_um)
        {
            return static_cast<int64_t>(std::llround(microns * dbu_per_um));
        }
    }

    /// @brief Parses a LEF58_FINFET property value -
    /// `FINFET PITCH <p> [OFFSET <o>] [HORIZONTAL|VERTICAL] ;` (microns,
    /// case-insensitive, surrounding quotes/whitespace ignored) - into a
    /// dbu-space FinGrid. nullopt if there's no FINFET keyword, no
    /// PITCH, a non-positive pitch, or `dbu_per_um` isn't positive.
    inline std::optional<FinGrid> parse_lef58_finfet(std::string_view text, double dbu_per_um)
    {
        using fin_grid_detail::iequals;
        if (dbu_per_um <= 0.0)
            return std::nullopt;

        std::string cleaned(text);
        for (char &c : cleaned)
            if (c == '"' || c == ';')
                c = ' ';

        std::istringstream in(cleaned);
        std::string token;
        bool seen_finfet = false;
        std::optional<double> pitch;
        double offset = 0.0;
        bool horizontal = true;
        while (in >> token)
        {
            if (iequals(token, "FINFET"))
                seen_finfet = true;
            else if (iequals(token, "PITCH"))
            {
                double value = 0.0;
                if (!(in >> value))
                    return std::nullopt;
                pitch = value;
            }
            else if (iequals(token, "OFFSET"))
            {
                if (!(in >> offset))
                    return std::nullopt;
            }
            else if (iequals(token, "HORIZONTAL"))
                horizontal = true;
            else if (iequals(token, "VERTICAL"))
                horizontal = false;
        }

        if (!seen_finfet || !pitch || *pitch <= 0.0)
            return std::nullopt;

        const int64_t pitch_dbu = fin_grid_detail::microns_to_dbu(*pitch, dbu_per_um);
        if (pitch_dbu <= 0)
            return std::nullopt;
        return FinGrid{.pitch = pitch_dbu, .offset = fin_grid_detail::microns_to_dbu(offset, dbu_per_um), .horizontal = horizontal};
    }

    /// @brief The effective FinFET grid of `root`'s (first) Technology: a
    /// LIBRARY `LEF58_FINFET` PROPERTYDEFINITIONS entry's own string value
    /// (LEF58's convention for a library-level property - `LIBRARY
    /// LEF58_FINFET STRING "FINFET PITCH ..." ;`), with any of
    /// Technology.fin_pitch/fin_offset/fin_direction that are set
    /// (update_technology) overriding the matching part. nullopt if
    /// neither source yields a positive pitch.
    inline std::optional<FinGrid> technology_fin_grid(const Root &root)
    {
        const auto technology_ids = root.get_technology_ids();
        if (technology_ids.empty())
            return std::nullopt;
        const TechnologyId technology_id = technology_ids.front();
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return std::nullopt;

        std::optional<FinGrid> grid;
        for (const PropertyDefinitionId id : root.get_technology_property_definitions(technology_id))
        {
            const PropertyDefinitionData *def = root.get_property_definition(id);
            if (def && def->default_string && fin_grid_detail::iequals(def->owner_type, "LIBRARY") &&
                fin_grid_detail::iequals(def->name, "LEF58_FINFET"))
            {
                grid = parse_lef58_finfet(*def->default_string, technology->database_units_microns);
                if (grid)
                    break;
            }
        }

        if (technology->fin_pitch)
        {
            if (!grid)
                grid = FinGrid{};
            grid->pitch = *technology->fin_pitch;
        }
        if (!grid)
            return std::nullopt;
        if (technology->fin_offset)
            grid->offset = *technology->fin_offset;
        if (technology->fin_direction)
            grid->horizontal = *technology->fin_direction != RoutingDirection::V;
        if (grid->pitch <= 0)
            return std::nullopt;
        return grid;
    }

    /// @brief `root`'s (first) Technology's MANUFACTURINGGRID, in dbu -
    /// nullopt if unset, or it rounds to less than 1 dbu.
    inline std::optional<int64_t> technology_manufacturing_grid(const Root &root)
    {
        const auto technology_ids = root.get_technology_ids();
        if (technology_ids.empty())
            return std::nullopt;
        const TechnologyData *technology = root.get_technology(technology_ids.front());
        if (!technology || !technology->manufacturing_grid || technology->database_units_microns <= 0.0)
            return std::nullopt;
        const int64_t grid = fin_grid_detail::microns_to_dbu(*technology->manufacturing_grid, technology->database_units_microns);
        if (grid <= 0)
            return std::nullopt;
        return grid;
    }
}
