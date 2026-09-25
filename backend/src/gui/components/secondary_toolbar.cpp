#include "secondary_toolbar.hpp"

#include "IconsLucide.h"
#include "compact_button.hpp"
#include "gui_provider.hpp"
#include "imgui.h"

#include <cstdint>
#include <functional>
#include <span>

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

        constexpr SnapChoice kPlacementSnapChoices[] = {
            {LE_PLACEMENT_SNAP_SITE, "Site", "Snap core cells to the nearest row's site grid, in an orientation the row allows"},
            {LE_PLACEMENT_SNAP_FIN_GRID, "FinFET", "Snap to the FinFET grid (LEF58_FINFET, or update_technology -fin_pitch)"},
            {LE_PLACEMENT_SNAP_MANUFACTURING_GRID, "Mfg grid", "Snap to the manufacturing grid"},
            {LE_PLACEMENT_SNAP_NONE, "None", "No snapping"},
        };

        // NEW_FEATURES_SEPT_2026.md item 3's own per-kind option lists.
        constexpr SnapChoice kRectPolygonSnapChoices[] = {
            {LE_SHAPE_SNAP_USER_GRID, "User grid", "Snap the dragged edge to the user grid"},
            {LE_SHAPE_SNAP_MANUFACTURING_GRID, "Mfg grid", "Snap the dragged edge to the manufacturing grid"},
            {LE_SHAPE_SNAP_FIN_GRID, "FinFET", "Snap to the FinFET grid across the fins, the manufacturing grid along them"},
            {LE_SHAPE_SNAP_NONE, "None", "No snapping"},
        };
        constexpr SnapChoice kPathSnapChoices[] = {
            {LE_SHAPE_SNAP_TRACKS, "Tracks", "Snap the segment's centerline to a routing track of its layer"},
            {LE_SHAPE_SNAP_MANUFACTURING_GRID, "Mfg edges", "Snap the path's edges to the manufacturing grid"},
            {LE_SHAPE_SNAP_USER_GRID, "User grid", "Snap the segment's centerline to the user grid"},
            {LE_SHAPE_SNAP_NONE, "None", "No snapping"},
        };
        // Item 13 - a moved via's (or via array's) origin.
        constexpr SnapChoice kViaSnapChoices[] = {
            {LE_SHAPE_SNAP_TRACKS, "Tracks", "Snap the via's origin to a routing track intersection of its layer"},
            {LE_SHAPE_SNAP_MANUFACTURING_GRID, "Mfg grid", "Snap the via's origin to the manufacturing grid"},
            {LE_SHAPE_SNAP_USER_GRID, "User grid", "Snap the via's origin to the user grid"},
            {LE_SHAPE_SNAP_NONE, "None", "No snapping"},
        };

        // Every item in the row is pinned to the row's own top y
        // explicitly - after SameLine(), ImGui's own text-baseline
        // bookkeeping for a vertically-centered label otherwise pushes
        // each following item down (a real reported misalignment).
        class Row
        {
        public:
            Row() : y_(ImGui::GetCursorPosY()) {}

            // Moves every following item onto a fresh line below this one.
            void wrap()
            {
                y_ += kButtonSize + ImGui::GetStyle().ItemSpacing.y;
                ImGui::SetCursorPosY(y_);
                first_ = true;
            }

            void same_line(float spacing = -1.0f)
            {
                if (first_)
                {
                    first_ = false;
                    return;
                }
                ImGui::SameLine(0.0f, spacing);
                ImGui::SetCursorPosY(y_);
            }

            void label(const char *text, float spacing = -1.0f)
            {
                same_line(spacing);
                ImGui::SetCursorPosY(y_ + (kButtonSize - ImGui::GetTextLineHeight()) * 0.5f);
                ImGui::TextUnformatted(text);
            }

        private:
            float y_;
            bool first_ = true;
        };

        // Same "optimistic until confirmed" reasoning as
        // mode_selector.cpp's own draw_mode_selector - a snap-mode change is
        // enqueued as a Tcl command, so the backend value lags a frame or
        // more behind a click. One per snap group.
        struct PendingChoice
        {
            bool has_pending = false;
            int32_t pending = 0;

            int32_t display(int32_t backend)
            {
                if (has_pending && backend == pending)
                    has_pending = false;
                return has_pending ? pending : backend;
            }
        };

        // One labeled group of mutually exclusive snap buttons - a mode
        // with nothing to snap to is disabled (BeginDisabled's fade is the
        // right cue here, unlike mode_selector.cpp's "already selected").
        void draw_snap_group(Row &row, const char *label, float label_spacing, std::span<const SnapChoice> choices, PendingChoice &pending,
                             int32_t backend_mode, const std::function<bool(int32_t)> &available, const std::function<void(int32_t)> &choose)
        {
            const int32_t display_mode = pending.display(backend_mode);
            row.label(label, label_spacing);
            for (const SnapChoice &choice : choices)
            {
                row.same_line();
                const bool selected = display_mode == choice.mode;
                const bool is_available = available(choice.mode);
                ImGui::PushID(label);
                ImGui::PushStyleColor(ImGuiCol_Button, selected ? kSelectedIconButtonColor : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::BeginDisabled(!is_available);
                const bool clicked = ImGui::Button(choice.label, ImVec2(0.0f, kButtonSize));
                ImGui::EndDisabled();
                ImGui::PopStyleColor();
                ImGui::PopID();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s%s", choice.tooltip, is_available ? "" : " - not available for this design");
                if (clicked && !selected)
                {
                    choose(choice.mode);
                    pending.has_pending = true;
                    pending.pending = choice.mode;
                }
            }
        }

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
            Row row;

            static PendingChoice pending{.pending = LE_PLACEMENT_SNAP_SITE};
            draw_snap_group(
                row, "Snap:", -1.0f, kPlacementSnapChoices, pending, state.snap_mode,
                [&](int32_t mode)
                { return state.snap_available[mode]; },
                [&](int32_t mode)
                { provider.set_placement_snap_mode(mode); });

            // Rotate/flip commit immediately (le_apply_placement_orientation_op)
            // - disabled while a Move is under way (after its first click),
            // or when site snapping is on and the row's Site symmetry
            // doesn't permit the op.
            const auto disabled_reason = [&](int32_t op) -> const char *
            {
                if (state.orientation_ops_enabled & (1 << op))
                    return nullptr;
                if (state.is_move_anchored)
                    return "not available while moving (Esc to cancel the move)";
                return "not permitted by the row's site symmetry while snapping to sites";
            };
            row.same_line(16.0f);
            if (draw_icon_action(ICON_LC_ROTATE_CCW, "placement_rotate", "Rotate 90 degrees counterclockwise", disabled_reason(LE_ORIENTATION_OP_ROTATE_CCW)))
                provider.rotate_placement();
            row.same_line();
            if (draw_icon_action(ICON_LC_FLIP_HORIZONTAL_2, "placement_flip_h", "Flip horizontally", disabled_reason(LE_ORIENTATION_OP_FLIP_HORIZONTAL)))
                provider.flip_placement_horizontal();
            row.same_line();
            if (draw_icon_action(ICON_LC_FLIP_VERTICAL_2, "placement_flip_v", "Flip vertically", disabled_reason(LE_ORIENTATION_OP_FLIP_VERTICAL)))
                provider.flip_placement_vertical();
        }

        // One snap group per kind of shape piece in `kinds` (a 1 <<
        // LePieceKind mask) - Resize's rects/polygons/paths (item 3), or
        // Move's paths/vias (item 13). Paths share one setting between the
        // two tools, so one pending state per kind serves both.
        void draw_shape_snap_toolbar(GuiProvider &provider, int32_t kinds)
        {
            const GuiProvider::State::Resize &state = provider.state().resize;
            Row row;

            struct Group
            {
                int32_t kind;
                const char *label;
                std::span<const SnapChoice> choices;
            };
            static const Group groups[] = {
                {LE_PIECE_KIND_RECT, "Rects:", kRectPolygonSnapChoices},
                {LE_PIECE_KIND_POLYGON, "Polygons:", kRectPolygonSnapChoices},
                {LE_PIECE_KIND_PATH, "Paths:", kPathSnapChoices},
                {LE_PIECE_KIND_VIA, "Vias:", kViaSnapChoices},
            };
            static PendingChoice pending[4] = {{.pending = LE_SHAPE_SNAP_USER_GRID}, {.pending = LE_SHAPE_SNAP_USER_GRID}, {.pending = LE_SHAPE_SNAP_USER_GRID}, {.pending = LE_SHAPE_SNAP_USER_GRID}};

            // A group that won't fit on the current line starts a new one
            // (the row's child window grows to fit - le_gui.cpp).
            const ImGuiStyle &style = ImGui::GetStyle();
            const auto group_width = [&](const Group &group)
            {
                float width = ImGui::CalcTextSize(group.label).x;
                for (const SnapChoice &choice : group.choices)
                    width += style.ItemSpacing.x + ImGui::CalcTextSize(choice.label).x + style.FramePadding.x * 2.0f;
                return width;
            };
            const float line_width = ImGui::GetContentRegionAvail().x;
            float used = 0.0f;

            bool first_group = true;
            for (const Group &group : groups)
            {
                if (!(kinds & (1 << group.kind)))
                    continue;
                const float width = group_width(group);
                if (!first_group && used + 16.0f + width > line_width)
                {
                    row.wrap();
                    first_group = true;
                    used = 0.0f;
                }
                used += (first_group ? 0.0f : 16.0f) + width;
                draw_snap_group(
                    row, group.label, first_group ? -1.0f : 16.0f, group.choices, pending[group.kind], state.snap_modes[group.kind],
                    [&](int32_t mode)
                    { return state.snap_available[group.kind][mode]; },
                    [&](int32_t mode)
                    { provider.set_shape_snap_mode(group.kind, mode); });
                first_group = false;
            }
        }
    }

    bool has_secondary_toolbar(const GuiProvider &provider)
    {
        const GuiProvider::State &state = provider.state();
        if (state.mode != LE_MODE_EDIT)
            return false;
        return (state.is_resize_armed && state.resize.selected_piece_kinds != 0) || state.placement_move.selected_count > 0 ||
               (state.is_move_armed && state.resize.move_snap_piece_kinds != 0);
    }

    void draw_secondary_toolbar(GuiProvider &provider)
    {
        // Resize, while armed, owns the row; otherwise placements' options;
        // otherwise, while Move is armed, its path/via snapping (item 13).
        const GuiProvider::State &state = provider.state();
        if (state.is_resize_armed && state.resize.selected_piece_kinds != 0)
            draw_shape_snap_toolbar(provider, state.resize.selected_piece_kinds);
        else if (state.placement_move.selected_count > 0)
            draw_placement_toolbar(provider);
        else if (state.is_move_armed && state.resize.move_snap_piece_kinds != 0)
            draw_shape_snap_toolbar(provider, state.resize.move_snap_piece_kinds);
    }
}
