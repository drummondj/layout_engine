#pragma once

#include "icon.hpp"
#include "imgui.h"

#include <algorithm>
#include <string>

namespace le::gui
{
    // Background for a "currently selected/armed" icon button
    // (mode_selector.cpp's own selected mode, mode_toolbar.cpp's armed
    // Move button) - a neutral dark gray rather than the theme's
    // ImGuiCol_ButtonActive (gui/theme.cpp), which reads as too similar
    // to the rest of the theme's accent-tinted chrome to stand out as a
    // distinct "this one is active" state. Deliberately a plain literal,
    // not derived from the theme, so it stays neutral under every theme.
    constexpr ImVec4 kSelectedIconButtonColor = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

    /// @brief A `size` x `size` button showing `icon` (`icon_size` px,
    /// centered) in the text color, so BeginDisabled fades it. `id` is its
    /// ImGui id. The caller owns the background colors and disabling.
    inline bool icon_button(Icon icon, const std::string &id, float size, float icon_size)
    {
        const bool clicked = ImGui::Button(("##" + id).c_str(), ImVec2(size, size));
        const ImVec2 rect_min = ImGui::GetItemRectMin();
        const float inset = (size - icon_size) * 0.5f;
        draw_icon(ImGui::GetWindowDrawList(), icon, ImVec2(rect_min.x + inset, rect_min.y + inset), icon_size, ImGui::GetColorU32(ImGuiCol_Text));
        return clicked;
    }

    // labeled_icon_button's geometry: a kLabeledIconSize icon centered in
    // the top kLabeledIconAreaHeight, the label (text font at
    // kIconLabelFontSize) centered in the line below it.
    constexpr float kIconLabelFontSize = 12.0f;
    constexpr float kLabeledIconSize = 32.0f;
    constexpr float kLabeledIconAreaHeight = 40.0f;
    constexpr float kLabeledIconButtonHeight = 56.0f;
    constexpr float kIconLabelPadding = 4.0f;

    /// @brief Width of a labeled_icon_button: `min_width`, or wider if the label needs it.
    inline float labeled_icon_button_width(const char *label, float min_width)
    {
        ImGui::PushFont(nullptr, kIconLabelFontSize);
        const float label_width = ImGui::CalcTextSize(label).x;
        ImGui::PopFont();
        return std::max(min_width, label_width + 2.0f * kIconLabelPadding);
    }

    // An icon with its label below, in one button - the whole area is
    // clickable and takes the themed hover/active background. Both are
    // drawn into the button's rect rather than as its ImGui label, so the
    // FramePadding clip rect can't push the label off center. Drawn with
    // ImGuiCol_Text, so BeginDisabled fades them like any other button
    // text. Same caller-owned background/disabled contract as icon_button.
    inline bool labeled_icon_button(Icon icon, const char *label, const std::string &id, float min_width)
    {
        const float width = labeled_icon_button_width(label, min_width);
        const bool clicked = ImGui::Button(("##" + id).c_str(), ImVec2(width, kLabeledIconButtonHeight));
        const ImVec2 rect_min = ImGui::GetItemRectMin();
        ImDrawList *draw_list = ImGui::GetWindowDrawList();
        const ImU32 text_color = ImGui::GetColorU32(ImGuiCol_Text);

        draw_icon(draw_list, icon, ImVec2(rect_min.x + (width - kLabeledIconSize) * 0.5f, rect_min.y + (kLabeledIconAreaHeight - kLabeledIconSize) * 0.5f),
                  kLabeledIconSize, text_color);

        ImGui::PushFont(nullptr, kIconLabelFontSize);
        const ImVec2 label_size = ImGui::CalcTextSize(label);
        const float label_area_height = kLabeledIconButtonHeight - kLabeledIconAreaHeight;
        draw_list->AddText(ImVec2(rect_min.x + (width - label_size.x) * 0.5f, rect_min.y + kLabeledIconAreaHeight + (label_area_height - label_size.y) * 0.5f),
                           text_color, label);
        ImGui::PopFont();
        return clicked;
    }

    // A small *fixed-width* plain-text button (e.g. a single "+"/"-"
    // character in the default font). The themed FramePadding shrinks
    // RenderTextClipped's clip rect below the text's width in a button
    // this narrow, and ImGui then left-aligns instead of centering (a
    // ~1.5px rightward bias); zero horizontal padding keeps it centered.
    // The vertical padding stays, so the button keeps its themed height
    // and lines up with what's beside it (e.g. an InputInt).
    inline bool compact_button(const char *label, float width)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        const bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));
        ImGui::PopStyleVar();
        return clicked;
    }
}
