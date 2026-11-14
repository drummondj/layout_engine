#include "theme.hpp"

#include "imgui.h"

namespace le::gui
{
    namespace
    {
        ImVec4 rgba(ThemeColor c, float alpha = 1.0f) { return {c.r, c.g, c.b, alpha}; }

        // `t` of the way from `a` to `b`.
        ThemeColor mix(ThemeColor a, ThemeColor b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

        ThemeColor lighten(ThemeColor c, float t) { return mix(c, {1.0f, 1.0f, 1.0f}, t); }

        ThemeColor darken(ThemeColor c, float t) { return mix(c, {0.0f, 0.0f, 0.0f}, t); }
    }

    void apply_theme_colors(ImGuiStyle &style, const Theme &theme)
    {
        ImVec4 *colors = style.Colors;
        const ThemeColor bg = theme.background;
        const ThemeColor accent = theme.accent;

        // Backgrounds
        colors[ImGuiCol_WindowBg] = rgba(bg);
        colors[ImGuiCol_ChildBg] = rgba(lighten(bg, 0.02f));
        colors[ImGuiCol_PopupBg] = rgba(darken(bg, 0.15f), 0.95f);
        colors[ImGuiCol_MenuBarBg] = rgba(lighten(bg, 0.02f));
        colors[ImGuiCol_Border] = rgba(lighten(accent, 0.05f), 0.40f);

        // Text
        colors[ImGuiCol_Text] = ImVec4(0.90f, 0.93f, 0.95f, 1.00f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.65f, 0.70f, 1.00f);
        colors[ImGuiCol_TextSelectedBg] = rgba(lighten(accent, 0.15f), 0.60f);

        // Headers
        colors[ImGuiCol_Header] = rgba(lighten(accent, 0.12f), 0.60f);
        colors[ImGuiCol_HeaderHovered] = rgba(lighten(accent, 0.22f), 0.80f);
        colors[ImGuiCol_HeaderActive] = rgba(lighten(accent, 0.25f));

        // Buttons
        colors[ImGuiCol_Button] = rgba(accent, 0.70f);
        colors[ImGuiCol_ButtonHovered] = rgba(lighten(accent, 0.12f), 0.85f);
        colors[ImGuiCol_ButtonActive] = rgba(lighten(accent, 0.18f));

        // Frames
        colors[ImGuiCol_FrameBg] = rgba(mix(bg, accent, 0.45f));
        colors[ImGuiCol_FrameBgHovered] = rgba(mix(bg, accent, 0.80f));
        colors[ImGuiCol_FrameBgActive] = rgba(lighten(accent, 0.08f));

        // Tabs
        colors[ImGuiCol_Tab] = rgba(mix(bg, accent, 0.90f), 0.80f);
        colors[ImGuiCol_TabHovered] = rgba(lighten(accent, 0.12f));
        colors[ImGuiCol_TabSelected] = rgba(lighten(accent, 0.20f));
        colors[ImGuiCol_TabSelectedOverline] = rgba(lighten(accent, 0.40f));
        colors[ImGuiCol_TabDimmed] = rgba(mix(bg, accent, 0.55f), 0.80f);
        colors[ImGuiCol_TabDimmedSelected] = rgba(lighten(accent, 0.03f));

        // Titles
        colors[ImGuiCol_TitleBg] = rgba(mix(bg, accent, 0.50f));
        colors[ImGuiCol_TitleBgActive] = rgba(mix(bg, accent, 0.85f));
        colors[ImGuiCol_TitleBgCollapsed] = rgba(darken(bg, 0.20f), 0.75f);

        // Scrollbars
        colors[ImGuiCol_ScrollbarBg] = rgba(lighten(bg, 0.01f));
        colors[ImGuiCol_ScrollbarGrab] = rgba(accent, 0.60f);
        colors[ImGuiCol_ScrollbarGrabHovered] = rgba(lighten(accent, 0.10f), 0.80f);
        colors[ImGuiCol_ScrollbarGrabActive] = rgba(lighten(accent, 0.22f));

        // Checkboxes, radios, sliders
        colors[ImGuiCol_CheckMark] = rgba(lighten(accent, 0.70f));
        colors[ImGuiCol_SliderGrab] = rgba(lighten(accent, 0.35f));
        colors[ImGuiCol_SliderGrabActive] = rgba(lighten(accent, 0.50f));

        // Resize grip
        colors[ImGuiCol_ResizeGrip] = rgba(accent, 0.60f);
        colors[ImGuiCol_ResizeGripHovered] = rgba(lighten(accent, 0.12f), 0.80f);
        colors[ImGuiCol_ResizeGripActive] = rgba(lighten(accent, 0.30f));

        // Separators
        colors[ImGuiCol_Separator] = rgba(lighten(accent, 0.10f), 0.70f);
        colors[ImGuiCol_SeparatorHovered] = rgba(lighten(accent, 0.30f), 0.90f);
        colors[ImGuiCol_SeparatorActive] = rgba(lighten(accent, 0.45f));

        // Docking, drag & drop
        colors[ImGuiCol_DockingPreview] = rgba(lighten(accent, 0.30f), 0.70f);
        colors[ImGuiCol_DragDropTarget] = rgba(lighten(accent, 0.50f), 0.90f);
    }
}
