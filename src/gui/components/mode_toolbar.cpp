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
        // Labeled icons; le_gui.cpp's mode_toolbar_row is sized for
        // kLabeledIconButtonHeight plus its padding.
        constexpr float kButtonMinWidth = 56.0f;

        // Hover shows `shortcut` (none -> no tooltip), or `disabled_reason`
        // in its place while the button is disabled.
        void draw_tooltip(const char *shortcut, const char *disabled_reason = nullptr)
        {
            if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                return;
            if (disabled_reason != nullptr)
                ImGui::SetTooltip("%s", disabled_reason);
            else if (shortcut != nullptr && shortcut[0] != '\0')
                ImGui::SetTooltip("%s", shortcut);
        }

        // `icon` is one of the ICON_LC_* constants (IconsLucide.h). No
        // resting background (matching mode_selector.cpp's unselected
        // buttons) - every button here is a momentary action, so
        // ButtonHovered/ButtonActive alone give it a press/hover cue.
        // `disabled_reason` non-null greys it out and replaces its tooltip.
        bool draw_button(const char *icon, const char *label, const char *shortcut, const char *disabled_reason = nullptr)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::BeginDisabled(disabled_reason != nullptr);
            const bool clicked = labeled_icon_button(icon, label, label, kButtonMinWidth);
            ImGui::EndDisabled();
            ImGui::PopStyleColor();
            draw_tooltip(shortcut, disabled_reason);
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
        // than BeginDisabled(armed), which would also fade the glyph.
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
        void draw_tool_button(const char *icon, const char *label, const char *shortcut, bool backend_armed, ToolButtonState &state, OnArm on_arm,
                              const char *disabled_reason = nullptr)
        {
            if (state.has_pending && (backend_armed || ImGui::GetFrameCount() - state.pending_since_frame > kPendingArmFrames))
                state.has_pending = false;
            const bool armed = state.has_pending || backend_armed;

            ImGui::PushStyleColor(ImGuiCol_Button, armed ? kSelectedIconButtonColor : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::BeginDisabled(disabled_reason != nullptr);
            const bool clicked = labeled_icon_button(icon, label, label, kButtonMinWidth);
            ImGui::EndDisabled();
            ImGui::PopStyleColor();
            draw_tooltip(shortcut, disabled_reason);
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
            // le_arm_move refuses placements selected alongside anything
            // else - they snap differently.
            const int32_t placement_count = provider.state().placement_move.selected_count;
            const bool mixed_selection = placement_count > 0 && provider.state().status_bar.selection_count > placement_count;
            draw_tool_button(ICON_LC_MOVE, "Move", "ctrl-m", provider.state().is_move_armed, move_state,
                             [&]
                             { provider.arm_move(); },
                             mixed_selection ? "Not available with placements and other objects selected together" : nullptr);
            ImGui::SameLine();
            // Resize: click an edge of a selected shape, then click again
            // to place it; its snap options appear in the secondary
            // toolbar while armed.
            static ToolButtonState resize_state;
            draw_tool_button(ICON_LC_SCALING, "Resize", "ctrl-r", provider.state().is_resize_armed, resize_state,
                             [&]
                             { provider.arm_resize(); },
                             // le_arm_resize refuses a selection with a
                             // placement in it.
                             provider.state().placement_move.selected_count > 0 ? "Not available while a placement is selected" : nullptr);

            // Delete: removes the selected shape pieces (and any shape left
            // empty), never their owners.
            ImGui::SameLine();
            if (draw_button(ICON_LC_TRASH_2, "Delete", "Del",
                            provider.state().selected_shape_piece_count == 0 ? "Select shape parts to delete first" : nullptr))
                provider.delete_selected_pieces();

            // No Rotate/Align buttons until those features exist.
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
