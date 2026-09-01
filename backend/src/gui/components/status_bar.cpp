#include "status_bar.hpp"

#include "api.hpp"
#include "imgui.h"

#include <cstdio>

namespace le::gui
{
    namespace
    {
        const char *mode_label(int32_t mode)
        {
            switch (mode)
            {
            case LE_MODE_SELECT:
                return "Select";
            case LE_MODE_EDIT:
                return "Edit";
            case LE_MODE_RULER:
                return "Ruler";
            default:
                return "?";
            }
        }
    }

    // A 3-column table (mode+spinner | tooltip | coordinates+selection),
    // rather than status_bar.dart's own width-cutoff Row/Column switch -
    // ImGui tables clip an oversized middle cell automatically instead of
    // wrapping/eliding it, so there's no equivalent narrow-width fallback
    // layout needed here; the fixed-width outer columns naturally push
    // the last one flush against the table's own right edge (`width`,
    // matching the outer table size passed below) the same way
    // status_bar.dart's own trailing Row items sit at its right edge.
    void draw_status_bar(LeHandle *handle, float width)
    {
        ImGui::Separator();

        // A small left/right inset - the caller's own window has zero
        // WindowPadding (le_gui.cpp's own comment on why), so without
        // this the row's own text would sit flush against the window's
        // physical edges.
        constexpr float kHorizontalInset = 8.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kHorizontalInset);
        const float table_width = width - (2.0f * kHorizontalInset);

        if (!ImGui::BeginTable(
                "status_bar_row", 3,
                ImGuiTableFlags_SizingFixedFit,
                ImVec2(table_width, 0.0f)))
        {
            return;
        }
        ImGui::TableSetupColumn("mode", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("tooltip", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("coords", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Mode: %s", mode_label(le_get_mode(handle)));
        // status_bar.dart's own isRunning||isRendering spinner has no
        // ImGui equivalent here - a text marker toggling in and out
        // every render (rather than a real animated spinner) read as
        // distracting flicker rather than useful feedback, so this was
        // dropped rather than kept as a text stand-in; le_gui.cpp's own
        // "rendering..." corner overlay on the design view itself still
        // covers the same signal.

        ImGui::TableSetColumnIndex(1);
        const char *tooltip = le_tooltip_message(handle);
        if (tooltip != nullptr && tooltip[0] != '\0')
        {
            ImGui::TextUnformatted(tooltip);
        }

        ImGui::TableSetColumnIndex(2);
        const LeSnappedMousePosition pos = le_snapped_mouse_position(handle);
        char coords_text[64];
        if (pos.has_position)
        {
            std::snprintf(coords_text, sizeof(coords_text), "X: %.3f Y: %.3f", pos.x_um, pos.y_um);
        }
        else
        {
            std::snprintf(coords_text, sizeof(coords_text), "X: - Y: -");
        }
        ImGui::Text("%s   Selected: %d", coords_text, le_selection_count(handle));

        ImGui::EndTable();
    }
}
