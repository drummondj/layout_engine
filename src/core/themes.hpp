#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace le
{
    /// @brief An sRGB color, each channel 0..1.
    struct ThemeColor
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
    };

    /// @brief One GUI color theme: the window background and the accent
    /// that buttons, headers, tabs and frames are derived from. Both are
    /// dark enough for the GUI's near-white text.
    struct Theme
    {
        std::string_view name;  // settings.json and Tcl
        std::string_view label; // the Settings panel
        ThemeColor background;
        ThemeColor accent;
    };

    // The first is the default. Names are persisted: never rename one.
    inline constexpr std::array<Theme, 8> kThemes = {{
        {"gray", "Dark gray", {0.12f, 0.13f, 0.15f}, {0.28f, 0.34f, 0.48f}},
        {"blue", "Blue", {0.07f, 0.10f, 0.19f}, {0.18f, 0.32f, 0.58f}},
        {"teal", "Teal", {0.05f, 0.14f, 0.15f}, {0.12f, 0.40f, 0.42f}},
        {"green", "Green", {0.06f, 0.14f, 0.08f}, {0.18f, 0.40f, 0.24f}},
        {"brown", "Brown", {0.16f, 0.12f, 0.06f}, {0.46f, 0.34f, 0.14f}},
        {"red", "Red", {0.18f, 0.06f, 0.07f}, {0.52f, 0.18f, 0.22f}},
        {"purple", "Purple", {0.12f, 0.07f, 0.19f}, {0.36f, 0.24f, 0.56f}},
        {"plum", "Plum", {0.17f, 0.06f, 0.14f}, {0.48f, 0.18f, 0.40f}},
    }};

    /// @brief The index in kThemes of the theme called `name`, if any.
    constexpr std::optional<size_t> find_theme(std::string_view name)
    {
        for (size_t i = 0; i < kThemes.size(); ++i)
            if (kThemes[i].name == name)
                return i;
        return std::nullopt;
    }
}
