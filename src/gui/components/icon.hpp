#pragma once

#include "generated/gui/icons.hpp"
#include "imgui.h"

#include <cmath>

namespace le::gui
{
    /// @brief Draws `icon` into the `size` x `size` square at `min` in `color`;
    /// its faint parts take a fraction of `color`'s alpha.
    inline void draw_icon(ImDrawList *draw_list, Icon icon, ImVec2 min, float size, ImU32 color)
    {
        // Whole-pixel origin: the hairline strokes blur less.
        min = ImVec2(std::floor(min.x), std::floor(min.y));
        const icon_data::IconEntry &entry = icon_data::entry(icon);
        const float scale = size / icon_data::kViewBoxSize;
        const float thickness = entry.stroke_width * scale;
        const ImU32 alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
        for (uint16_t i = 0; i < entry.polyline_count; ++i)
        {
            const icon_data::Polyline &polyline = icon_data::kPolylines[entry.first_polyline + i];
            const ImU32 polyline_alpha = static_cast<ImU32>(static_cast<float>(alpha) * polyline.opacity + 0.5f);
            const ImU32 polyline_color = (color & ~IM_COL32_A_MASK) | (polyline_alpha << IM_COL32_A_SHIFT);
            for (uint16_t p = 0; p < polyline.point_count; ++p)
            {
                const icon_data::Point &point = icon_data::kPoints[polyline.first_point + p];
                draw_list->PathLineTo(ImVec2(min.x + point.x * scale, min.y + point.y * scale));
            }
            if (polyline.filled)
                draw_list->PathFillConvex(polyline_color);
            else
                draw_list->PathStroke(polyline_color, polyline.closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
        }
    }
}
