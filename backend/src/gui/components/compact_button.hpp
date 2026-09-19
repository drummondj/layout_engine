#pragma once

#include "icon_font.hpp"
#include "imgui.h"

#include <string>

namespace le::gui
{
    // Background for a "currently selected/armed" icon button
    // (mode_selector.cpp's own selected mode, mode_toolbar.cpp's armed
    // Move button) - a neutral dark gray rather than
    // ImGuiCol_ButtonActive's own themed blue (set_dark_pastel_imgui_style,
    // le_gui.cpp), which read as too similar to the rest of the theme's
    // blue-tinted chrome to stand out as a distinct "this one is
    // active" state. Deliberately a plain literal, not derived from any
    // themed color, so a future theme change can't silently turn it
    // blue again.
    constexpr ImVec4 kSelectedIconButtonColor = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

    // Draws a single icon glyph (large_icon_font(), icon_font.hpp) in a
    // button of exactly `size` x `size` - safe from the "glyph renders
    // off-center" bug a plain ImGui::Button(icon, ImVec2(size,size))
    // hits otherwise: the themed FramePadding (set_dark_pastel_imgui_style,
    // le_gui.cpp) shrinks RenderTextClipped's own interior clip rect
    // below the glyph's own advance width once a button this small
    // takes an *explicit* fixed size (a normally-sized, auto-sized
    // button never hits this - ImGui grows it to fit label + 2x
    // padding on its own). A too-small clip rect trips Dear ImGui's
    // own "never start left of the clip rect" clamp inside
    // RenderTextClipped, which silently left-aligns instead of
    // centering - measured as a real ~2.5px rightward bias (this is
    // what mode_selector.cpp's draw_mode_button hand-rolled a fix for
    // before this helper existed; extracted here so every future
    // icon-only button gets it for free rather than each call site
    // repeating the same PushFont/PushStyleVar dance). Zero
    // FramePadding keeps the whole button as the clip rect, always
    // wider than a single glyph, so true centering always applies.
    //
    // Caller still owns background color (Button/ButtonHovered/
    // ButtonActive) and BeginDisabled/EndDisabled around this call -
    // both vary per call site (selected/armed state) and don't belong
    // in a shared helper. `id` disambiguates this button's ImGui id
    // from any other button using the same icon glyph elsewhere (goes
    // after "##", so it's never actually rendered - see mode_toolbar.cpp's
    // own draw_button for the established convention this replaces).
    inline bool icon_button(const char *icon, const std::string &id, float size)
    {
        ImGui::PushFont(large_icon_font());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        const std::string label = std::string(icon) + "##" + id;
        const bool clicked = ImGui::Button(label.c_str(), ImVec2(size, size));
        ImGui::PopStyleVar();
        ImGui::PopFont();
        return clicked;
    }

    // Same underlying fix as icon_button above, for a small *fixed-width*
    // plain-text button (e.g. a single "+"/"-" character in the default
    // font) instead of an icon-font glyph - horizontal padding only, so
    // the button keeps its normal themed height and lines up with
    // whatever it's drawn next to on the same line (e.g. an adjacent
    // InputInt). Same measured bug (a real ~1.5px rightward bias),
    // same root cause - see icon_button's own comment above.
    inline bool compact_button(const char *label, float width)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        const bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));
        ImGui::PopStyleVar();
        return clicked;
    }
}
