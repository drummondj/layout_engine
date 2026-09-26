#pragma once

namespace le::gui
{
    class GuiProvider;

    // The Settings panel (NEW_FEATURES_SEPT_2026.md item 9) - grid spacing,
    // ruler and label font sizes, hierarchy depth and the flightline fanout
    // limit (the last two moved here from layer_manager.hpp), plus saving
    // them to / loading them from a JSON settings file: the default one
    // (le_default_settings_path - le_shell loads it at startup) or one
    // picked with the system file dialog. Draws directly into whatever
    // ImGui window is currently active - call once per frame from within
    // that window.
    void draw_settings_panel(GuiProvider &provider);
}
