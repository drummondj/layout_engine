#include "mode_toolbar.hpp"

#include "IconsLucide.h"
#include "api.hpp"
#include "imgui.h"
#include "tcl_command_queue.hpp"

#include <cstdint>
#include <string>

namespace le::gui
{
    namespace
    {
        // `icon` is one of the ICON_LC_* constants (IconsLucide.h) -
        // see mode_selector.cpp's own comment on why Lucide, and that
        // there's no exact 1:1 match for every one of mode_toolbar.dart's
        // own HugeIcons.
        bool draw_button(const char *icon, const char *label, const char *shortcut)
        {
            const std::string face = std::string(icon) + " " + label;
            const bool clicked = ImGui::Button(face.c_str(), ImVec2(0.0f, 32.0f));
            if (ImGui::IsItemHovered())
            {
                if (shortcut != nullptr && shortcut[0] != '\0')
                    ImGui::SetTooltip("%s (%s)", label, shortcut);
                else
                    ImGui::SetTooltip("%s", label);
            }
            return clicked;
        }
    }

    void draw_mode_toolbar(LeHandle *handle)
    {
        const int32_t mode = le_get_mode(handle);
        switch (mode)
        {
        case LE_MODE_SELECT:
            if (draw_button(ICON_LC_BOX_SELECT, "Select All", "ctrl-a"))
                enqueue_tcl_command(handle, "select_all");
            ImGui::SameLine();
            if (draw_button(ICON_LC_CIRCLE_X, "Deselect All", "ctrl-d"))
                enqueue_tcl_command(handle, "deselect_all");
            break;

        case LE_MODE_EDIT:
        {
            // Same "optimistic until confirmed" reasoning as
            // mode_selector.cpp's own draw_mode_button - arm_move is
            // enqueued, not applied synchronously, so re-reading
            // le_is_move_armed() on the very next frame would otherwise
            // flicker the button back to unarmed until the queued
            // command lands.
            static bool has_pending_move = false;
            static bool pending_move_value = false;
            const bool backend_armed = le_is_move_armed(handle) != 0;
            if (has_pending_move && backend_armed == pending_move_value)
                has_pending_move = false;
            const bool armed = has_pending_move ? pending_move_value : backend_armed;

            if (armed)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            // Disabled while already armed, matching ToolbarButton's own
            // `onPressed: selected ? null : onPressed` in mode_toolbar.dart.
            ImGui::BeginDisabled(armed);
            const bool move_clicked = ImGui::Button(ICON_LC_MOVE " Move", ImVec2(0.0f, 32.0f));
            ImGui::EndDisabled();
            if (armed)
                ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Move (ctrl-m)");
            if (move_clicked)
            {
                enqueue_tcl_command(handle, "arm_move");
                has_pending_move = true;
                pending_move_value = true;
            }

            // mode_toolbar.dart's own Resize/Rotate/Align */Delete
            // buttons are all still no-ops there too (`onPressed: () =>
            // {}`) - left unported here rather than wiring up dead
            // buttons; add them once the underlying feature exists.
            ImGui::SameLine();
            if (draw_button(ICON_LC_UNDO_2, "Undo", "ctrl-z"))
                enqueue_tcl_command(handle, "undo");
            ImGui::SameLine();
            if (draw_button(ICON_LC_REDO_2, "Redo", "shift-ctrl-z"))
                enqueue_tcl_command(handle, "redo");
            break;
        }

        case LE_MODE_RULER:
            if (draw_button(ICON_LC_CIRCLE_X, "Clear Rulers", nullptr))
                enqueue_tcl_command(handle, "clear_rulers");
            break;

        default:
            break;
        }
    }
}
