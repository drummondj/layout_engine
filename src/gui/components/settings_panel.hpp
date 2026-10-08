#pragma once

#include <functional>

namespace le::gui
{
    class GuiProvider;

    // The Settings panel - grid spacing, ruler and label font sizes,
    // hierarchy depth (for this session only) and the flightline fanout
    // limit, plus saving
    // them to / loading them from a JSON settings file: the default one
    // (le_default_settings_path - le_shell loads it at startup) or one
    // picked with the system file dialog. Draws directly into whatever
    // ImGui window is currently active - call once per frame from within
    // that window.
    // `extra` draws more sections at the end (extensions').
    void draw_settings_panel(GuiProvider &provider, const std::function<void()> &extra = {});
}
