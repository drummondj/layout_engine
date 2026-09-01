#include "mode_selector.hpp"

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
        const char *mode_keyword(int32_t mode)
        {
            switch (mode)
            {
            case LE_MODE_SELECT:
                return "select";
            case LE_MODE_EDIT:
                return "edit";
            case LE_MODE_RULER:
                return "ruler";
            default:
                return "select";
            }
        }

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

        // Lucide equivalents of mode_selector.dart's own HugeIcons -
        // ICON_LC_MOUSE_POINTER_SQUARE_DASHED (a marquee-selection
        // cursor) for strokeRoundedCursorRectangleSelection01,
        // ICON_LC_PENCIL for strokeRoundedCursorEdit01, ICON_LC_RULER
        // for strokeRoundedRuler - Lucide has no exact 1:1 match for any
        // of these (a different icon set entirely), picked for closest
        // visual/semantic fit.
        const char *mode_icon(int32_t mode)
        {
            switch (mode)
            {
            case LE_MODE_SELECT:
                return ICON_LC_MOUSE_POINTER_SQUARE_DASHED;
            case LE_MODE_EDIT:
                return ICON_LC_PENCIL;
            case LE_MODE_RULER:
                return ICON_LC_RULER;
            default:
                return "";
            }
        }

        const char *mode_shortcut(int32_t mode)
        {
            switch (mode)
            {
            case LE_MODE_SELECT:
                return "s";
            case LE_MODE_EDIT:
                return "e";
            case LE_MODE_RULER:
                return "r";
            default:
                return "";
            }
        }

        // Same "optimistic until confirmed" reasoning as
        // layer_manager.cpp's own draw_optimistic_checkbox/hierarchy
        // depth field - set_mode is enqueued (evaluated on le_shell's
        // own console thread up to ~100ms later), so re-reading
        // le_get_mode() on the very next frame would otherwise flicker
        // the highlighted button back to the old mode until that lands.
        void draw_mode_button(LeHandle *handle, int32_t mode, int32_t display_mode, bool &has_pending, int32_t &pending)
        {
            const bool selected = display_mode == mode;
            if (selected)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            // Disabled while already selected, matching ModeButton's own
            // `onPressed: selected ? null : onPressed` in mode_selector.dart -
            // clicking the already-active mode is a no-op there too.
            ImGui::BeginDisabled(selected);
            const std::string face = std::string(mode_icon(mode)) + " " + mode_label(mode);
            if (ImGui::Button(face.c_str(), ImVec2(64.0f, 40.0f)))
            {
                enqueue_tcl_command(handle, std::string("set_mode ") + mode_keyword(mode));
                has_pending = true;
                pending = mode;
            }
            ImGui::EndDisabled();
            if (selected)
            {
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s (%s)", mode_label(mode), mode_shortcut(mode));
            }
        }
    }

    void draw_mode_selector(LeHandle *handle)
    {
        static bool has_pending_mode = false;
        static int32_t pending_mode = LE_MODE_SELECT;

        const int32_t backend_mode = le_get_mode(handle);
        if (has_pending_mode && backend_mode == pending_mode)
        {
            has_pending_mode = false;
        }
        const int32_t display_mode = has_pending_mode ? pending_mode : backend_mode;

        draw_mode_button(handle, LE_MODE_SELECT, display_mode, has_pending_mode, pending_mode);
        ImGui::Spacing();
        draw_mode_button(handle, LE_MODE_EDIT, display_mode, has_pending_mode, pending_mode);
        ImGui::Spacing();
        draw_mode_button(handle, LE_MODE_RULER, display_mode, has_pending_mode, pending_mode);
    }
}
