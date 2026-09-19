#include "mode_toolbar.hpp"

#include "IconsLucide.h"
#include "api.hpp"
#include "compact_button.hpp"
#include "imgui.h"
#include "tcl_command_queue.hpp"

#include <cstdint>
#include <string>

namespace le::gui
{
    namespace
    {
        // Icon-only now (label moved to a hover tooltip) - fits within
        // le_gui.cpp's own 64px-tall mode_toolbar_row child (48 + its
        // 8px WindowPadding on each side).
        constexpr float kIconButtonSize = 48.0f;

        // `icon` is one of the ICON_LC_* constants (IconsLucide.h) -
        // see mode_selector.cpp's own comment on why Lucide, and that
        // there's no exact 1:1 match for every one of mode_toolbar.dart's
        // own HugeIcons. No resting background (matching mode_selector.cpp's
        // own unselected-button treatment) - every button here is a
        // momentary action, never a "currently selected" one, so
        // ButtonHovered/ButtonActive alone (still themed) give it a
        // press/hover cue.
        bool draw_button(const char *icon, const char *label, const char *shortcut)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            // icon_button (compact_button.hpp), not a plain ImGui::Button -
            // see its own doc comment for the centering bug this avoids.
            const bool clicked = icon_button(icon, label, kIconButtonSize);
            ImGui::PopStyleColor();
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

            // Armed keeps a permanent highlighted background (a neutral
            // dark gray - compact_button.hpp's own kSelectedIconButtonColor,
            // matching mode_selector.cpp's own selected-mode treatment,
            // not the theme's own blue ButtonActive); unarmed now draws
            // no resting background at all - only the icon glyph -
            // matching draw_button's own treatment above.
            ImGui::PushStyleColor(ImGuiCol_Button,
                                   armed ? kSelectedIconButtonColor
                                         : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            // See draw_button's own comment above.
            const bool move_clicked = icon_button(ICON_LC_MOVE, "move", kIconButtonSize);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Move (ctrl-m)");
            // Clicking while already armed is a no-op, matching
            // ToolbarButton's own `onPressed: selected ? null : onPressed`
            // in mode_toolbar.dart - see mode_selector.cpp's own
            // draw_mode_button for why this is a plain `!armed` guard
            // rather than BeginDisabled(armed) (the latter also fades
            // the icon glyph itself via DisabledAlpha, a real reported
            // bug, not just a duller background).
            if (move_clicked && !armed)
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
