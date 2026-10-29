#pragma once

#include "generated/database/root.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

// The data steps a migration's RunCode op names: hand-written changes to
// an older file's data that renaming can't express. The loader runs each
// one past the file's version, in migration order, on the decoded objects
// before their indexes are rebuilt.

namespace le::persistence::migration_steps
{
    /// @brief What a step works on: an older file's objects, decoded but
    /// not yet in the database. Stored row r is the object with id {r, 0}.
    struct StepData
    {
        Root &root;
        /// Each row's value of the stored bool column `klass`.`field`, which
        /// the current schema no longer has; empty if the file has none.
        std::function<std::vector<uint8_t>(std::string_view klass, std::string_view field)> dropped_bools;
    };

    struct Step
    {
        std::string_view name; // the RunCode op's
        void (*run)(StepData &);
        /// The dropped columns it reads and the members it fills, as
        /// "Class.field": the loader doesn't warn that these were dropped
        /// or left at their default.
        std::array<std::string_view, 4> reads{};
        std::array<std::string_view, 4> writes{};
    };

    /// @brief 0.54.0: Route.is_special moves to each of its Wires and
    /// route Shapes.
    void copy_route_is_special_to_wiring(StepData &data);

    inline constexpr std::array kSteps{
        Step{"copy_route_is_special_to_wiring", &copy_route_is_special_to_wiring, {"Route.is_special"}, {"Wire.is_special", "Shape.is_special"}},
    };

    /// @brief The step named `name`, or null.
    constexpr const Step *find(std::string_view name)
    {
        for (const Step &step : kSteps)
            if (step.name == name)
                return &step;
        return nullptr;
    }
}
