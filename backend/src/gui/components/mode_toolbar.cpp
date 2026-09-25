#include "mode_toolbar.hpp"

#include "IconsLucide.h"
#include "compact_button.hpp"
#include "gui_provider.hpp"
#include "imgui.h"

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

        // An Edit-mode tool button (Move, Resize) - highlighted while its
        // tool is armed. Same "optimistic until confirmed" reasoning as
        // mode_selector.cpp's own draw_mode_button: arming is enqueued, not
        // applied synchronously, so re-reading the armed state on the very
        // next frame would otherwise flicker the button back to unarmed
        // until the queued command lands. Armed keeps a permanent
        // highlighted background (compact_button.hpp's own
        // kSelectedIconButtonColor); unarmed draws only the icon glyph.
        // Clicking while armed is a no-op - a plain `!armed` guard rather
        // than BeginDisabled(armed), which would also fade the glyph (a
        // real reported bug).
        // A pending arm the backend refuses (nothing suitable selected)
        // never shows up as armed - it expires after this many frames
        // rather than leaving the button highlighted forever.
        constexpr int kPendingArmFrames = 30;

        struct ToolButtonState
        {
            bool has_pending = false;
            int pending_since_frame = 0;
        };

        template <typename OnArm>
        void draw_tool_button(const char *icon, const char *id, const char *tooltip, bool backend_armed, ToolButtonState &state, OnArm on_arm)
        {
            if (state.has_pending && (backend_armed || ImGui::GetFrameCount() - state.pending_since_frame > kPendingArmFrames))
                state.has_pending = false;
            const bool armed = state.has_pending || backend_armed;

            ImGui::PushStyleColor(ImGuiCol_Button, armed ? kSelectedIconButtonColor : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            // icon_button - see draw_button's own comment above.
            const bool clicked = icon_button(icon, id, kIconButtonSize);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltip);
            if (clicked && !armed)
            {
                on_arm();
                state.has_pending = true;
                state.pending_since_frame = ImGui::GetFrameCount();
            }
        }
    }

    void draw_mode_toolbar(GuiProvider &provider)
    {
        const int32_t mode = provider.state().mode;
        switch (mode)
        {
        case LE_MODE_SELECT:
            if (draw_button(ICON_LC_BOX_SELECT, "Select All", "ctrl-a"))
                provider.select_all();
            ImGui::SameLine();
            if (draw_button(ICON_LC_CIRCLE_X, "Deselect All", "ctrl-d"))
                provider.deselect_all();
            break;

        case LE_MODE_EDIT:
        {
            static ToolButtonState move_state;
            draw_tool_button(ICON_LC_MOVE, "move", "Move (ctrl-m)", provider.state().is_move_armed, move_state,
                             [&]
                             { provider.arm_move(); });
            ImGui::SameLine();
            // NEW_FEATURES_SEPT_2026.md item 3 - drag a selected shape's
            // edges/segments; its snap options appear in the secondary
            // toolbar while armed.
            static ToolButtonState resize_state;
            draw_tool_button(ICON_LC_SCALING, "resize", "Resize (ctrl-r) - click an edge of a selected shape, then click again to place it", provider.state().is_resize_armed, resize_state,
                             [&]
                             { provider.arm_resize(); });

            // mode_toolbar.dart's own Rotate/Align */Delete buttons are
            // all still no-ops there too (`onPressed: () => {}`) - left
            // unported here rather than wiring up dead buttons; add them
            // once the underlying feature exists.
            ImGui::SameLine();
            if (draw_button(ICON_LC_UNDO_2, "Undo", "ctrl-z"))
                provider.undo();
            ImGui::SameLine();
            if (draw_button(ICON_LC_REDO_2, "Redo", "shift-ctrl-z"))
                provider.redo();
            break;
        }

        case LE_MODE_RULER:
            if (draw_button(ICON_LC_CIRCLE_X, "Clear Rulers", nullptr))
                provider.clear_rulers();
            break;

        default:
            break;
        }
    }
}
