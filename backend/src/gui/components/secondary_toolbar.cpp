#include "secondary_toolbar.hpp"

#include "IconsLucide.h"
#include "compact_button.hpp"
#include "gui_provider.hpp"
#include "imgui.h"

#include <cstdint>
#include <iterator>

namespace le::gui
{
    namespace
    {
        // Smaller than ModeToolbar's own 48px buttons - this row is an
        // overlay on the design view, so it's kept compact.
        constexpr float kButtonSize = 36.0f;

        struct SnapChoice
        {
            int32_t mode;
            const char *label;
            const char *tooltip;
        };

        constexpr SnapChoice kSnapChoices[] = {
            {LE_PLACEMENT_SNAP_SITE, "Site", "Snap core cells to the nearest row's site grid, in an orientation the row allows"},
            {LE_PLACEMENT_SNAP_FIN_GRID, "FinFET", "Snap to the FinFET grid (LEF58_FINFET, or update_technology -fin_pitch)"},
            {LE_PLACEMENT_SNAP_MANUFACTURING_GRID, "Mfg grid", "Snap to the manufacturing grid"},
            {LE_PLACEMENT_SNAP_NONE, "None", "No snapping"},
        };

        // `disabled_reason` is shown in the tooltip (and the button
        // disabled) when non-null.
        bool draw_icon_action(const char *icon, const char *id, const char *tooltip, const char *disabled_reason)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::BeginDisabled(disabled_reason != nullptr);
            const bool clicked = icon_button(icon, id, kButtonSize);
            ImGui::EndDisabled();
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (disabled_reason)
                    ImGui::SetTooltip("%s - %s", tooltip, disabled_reason);
                else
                    ImGui::SetTooltip("%s", tooltip);
            }
            return clicked;
        }

        void draw_placement_toolbar(GuiProvider &provider)
        {
            const GuiProvider::State::PlacementMove &state = provider.state().placement_move;

            // Same "optimistic until confirmed" reasoning as
            // mode_selector.cpp's own draw_mode_selector -
            // set_placement_snap_mode is enqueued, so the backend value
            // lags a frame or more behind a click.
            static bool has_pending = false;
            static int32_t pending = LE_PLACEMENT_SNAP_SITE;
            if (has_pending && state.snap_mode == pending)
                has_pending = false;
            const int32_t display_mode = has_pending ? pending : state.snap_mode;

            // Every item is pinned to the row's own top y explicitly -
            // after SameLine(), ImGui's own text-baseline bookkeeping for
            // the vertically-centered "Snap:" label otherwise pushes each
            // following item down (a real reported misalignment: only the
            // first button after the label sat at the right height).
            const float row_y = ImGui::GetCursorPosY();
            const auto same_line = [row_y](float spacing = -1.0f)
            {
                ImGui::SameLine(0.0f, spacing);
                ImGui::SetCursorPosY(row_y);
            };

            ImGui::SetCursorPosY(row_y + (kButtonSize - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::TextUnformatted("Snap:");
            same_line();

            for (const SnapChoice &choice : kSnapChoices)
            {
                const bool selected = display_mode == choice.mode;
                const bool available = state.snap_available[choice.mode];
                ImGui::PushStyleColor(ImGuiCol_Button, selected ? kSelectedIconButtonColor : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                // A mode with nothing to snap to (no rows, no fin grid,
                // no MANUFACTURINGGRID) is disabled - BeginDisabled's own
                // fade is exactly the right cue here, unlike the
                // "already selected" case mode_selector.cpp avoids it for.
                ImGui::BeginDisabled(!available);
                const bool clicked = ImGui::Button(choice.label, ImVec2(0.0f, kButtonSize));
                ImGui::EndDisabled();
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s%s", choice.tooltip, available ? "" : " - not available for this design");
                if (clicked && !selected)
                {
                    provider.set_placement_snap_mode(choice.mode);
                    has_pending = true;
                    pending = choice.mode;
                }
                if (&choice != &kSnapChoices[std::size(kSnapChoices) - 1])
                    same_line();
            }

            // Rotate/flip commit immediately (le_apply_placement_orientation_op)
            // - disabled while a Move is under way (after its first click),
            // or when site snapping is on
            // and the row's Site symmetry doesn't permit the op.
            const auto disabled_reason = [&](int32_t op) -> const char *
            {
                if (state.orientation_ops_enabled & (1 << op))
                    return nullptr;
                if (state.is_move_anchored)
                    return "not available while moving (Esc to cancel the move)";
                return "not permitted by the row's site symmetry while snapping to sites";
            };
            same_line(16.0f);
            if (draw_icon_action(ICON_LC_ROTATE_CCW, "placement_rotate", "Rotate 90 degrees counterclockwise", disabled_reason(LE_ORIENTATION_OP_ROTATE_CCW)))
                provider.rotate_placement();
            same_line();
            if (draw_icon_action(ICON_LC_FLIP_HORIZONTAL_2, "placement_flip_h", "Flip horizontally", disabled_reason(LE_ORIENTATION_OP_FLIP_HORIZONTAL)))
                provider.flip_placement_horizontal();
            same_line();
            if (draw_icon_action(ICON_LC_FLIP_VERTICAL_2, "placement_flip_v", "Flip vertically", disabled_reason(LE_ORIENTATION_OP_FLIP_VERTICAL)))
                provider.flip_placement_vertical();
        }
    }

    bool has_secondary_toolbar(const GuiProvider &provider)
    {
        const GuiProvider::State &state = provider.state();
        return state.mode == LE_MODE_EDIT && state.placement_move.selected_count > 0;
    }

    void draw_secondary_toolbar(GuiProvider &provider)
    {
        if (provider.state().placement_move.selected_count > 0)
            draw_placement_toolbar(provider);
    }
}
