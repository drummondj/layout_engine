TEMPLATE = """
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace {{schema.namespace}}
{
    /**
        @brief One name/value row of an object's schema-derived property
        table (see each generated to_properties() function).
    */
    struct PropertyValue
    {
        enum class Type
        {
            STRING,
            INT,
            DOUBLE,
        };

        // What an INT value measures - DBU (a length in database units) or
        // DBU2 (an area in database units squared) marks a raw dbu value
        // from get_field(), so a consumer working in microns (-filter,
        // chained property paths) can convert it (NEW_FEATURES_SEPT_2026.md
        // item 27). NONE for everything else.
        enum class Unit
        {
            NONE,
            DBU,
            DBU2,
        };

        std::string name;
        Type type;
        std::string string_value;
        int64_t int_value = 0;
        double double_value = 0.0;
        Unit unit = Unit::NONE;

        static PropertyValue make_string(std::string name, std::string value)
        {
            return PropertyValue{.name = std::move(name), .type = Type::STRING, .string_value = std::move(value)};
        }

        static PropertyValue make_int(std::string name, int64_t value, Unit unit = Unit::NONE)
        {
            return PropertyValue{.name = std::move(name), .type = Type::INT, .int_value = value, .unit = unit};
        }

        static PropertyValue make_double(std::string name, double value)
        {
            return PropertyValue{.name = std::move(name), .type = Type::DOUBLE, .double_value = value};
        }
    };

    /**
        @brief Converts a raw `dbu`-typed value (database units) to
        microns, given the owning Technology's own dbu-per-micron ratio.
        Every `dbu` field (schema.py TYPEMAP) is a raw int64_t with no
        unit-conversion context of its own - to_properties()/
        to_property_string() take `dbu_per_um` as a parameter for exactly
        this reason.
    */
    inline double to_um(int64_t value_dbu, double dbu_per_um)
    {
        return static_cast<double>(value_dbu) / dbu_per_um;
    }

    /**
        @brief to_um() for a `dbu2` value - an area in database units
        squared, converted to square microns.
    */
    inline double to_um2(int64_t value_dbu2, double dbu_per_um)
    {
        return static_cast<double>(value_dbu2) / (dbu_per_um * dbu_per_um);
    }

    /**
        @brief A PropertyValue's number in microns (DBU) or square microns
        (DBU2) - its raw number for any other unit.
    */
    inline double property_value_in_um(const PropertyValue &value, double dbu_per_um)
    {
        if (value.unit == PropertyValue::Unit::DBU)
            return to_um(value.int_value, dbu_per_um);
        if (value.unit == PropertyValue::Unit::DBU2)
            return to_um2(value.int_value, dbu_per_um);
        return value.type == PropertyValue::Type::DOUBLE ? value.double_value : static_cast<double>(value.int_value);
    }

    /**
        @brief Formats a microns value for display, trimming trailing
        zero decimal digits in groups of three - this project's own
        dbu/um convention: a database-microns value like 1000 gives
        exactly 3 significant decimal digits, so a *partial* trim (e.g.
        "0.34" instead of "0.340") would misrepresent that precision as
        coarser than it is.
    */
    inline std::string format_coordinate_um(double value)
    {
        std::string formatted = std::to_string(value); // always exactly 6 decimal digits
        const size_t dot = formatted.find('.');
        if (dot == std::string::npos)
            return formatted;

        size_t end = formatted.size();
        while (end - dot - 1 >= 3 && formatted.compare(end - 3, 3, "000") == 0)
            end -= 3;

        if (end == dot + 1) // stripped every decimal digit - drop the bare "." too
            end = dot;

        return formatted.substr(0, end);
    }

    /**
        @brief Joins a list of embedded (non-pooled) Klass values into one
        Tcl-list-friendly string for to_properties() (INDEXED_POOLS export
        style) - each item wrapped in its own "{...}" so a Tcl foreach/
        lindex can walk the list, with that item's own to_property_string()
        (already brace-balanced - every generated to_property_string()
        opens "Name{" and closes with a matching " }", and - unlike
        to_string() - recurses fully into any list/reference fields that
        item itself has, e.g. Polygon's own points list) supplying the
        full contents in place of a bare "<field>_count". Relies on
        argument-dependent lookup to find each element type's own
        to_property_string() overload, so this only compiles at the call
        site in a generated struct header that has already #include'd it
        - never called directly, only from generated to_properties()/
        to_property_string() bodies. `dbu_per_um` threads through to each
        element's own to_property_string() so a `dbu` field nested inside
        the list's element type still converts to microns.
    */
    template <typename T>
    inline std::string to_property_list_string(const std::vector<T> &items, double dbu_per_um)
    {
        std::string result;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            if (i != 0)
                result += " ";
            result += "{" + to_property_string(items[i], dbu_per_um) + "}";
        }
        return result;
    }
}
"""
