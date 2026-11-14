#pragma once

#include "../core/themes.hpp"

struct ImGuiStyle;

namespace le::gui
{
    /// @brief Sets every themed color in `style` from `theme`: backgrounds
    /// from its background, and buttons, headers, tabs, frames and
    /// scrollbars from its accent. Text stays near-white whatever the theme.
    void apply_theme_colors(ImGuiStyle &style, const Theme &theme);
}
