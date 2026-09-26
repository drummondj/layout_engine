#pragma once

namespace le::gui
{
    class GuiProvider;

    // The Info panel (NEW_FEATURES_SEPT_2026.md item 15) - the current
    // mode's instructions (le_tooltip_message), wrapped to the panel's
    // width. Docked at the bottom of the right sidebar by default; replaces
    // the status bar's old middle column, where a long message was clipped.
    // Draws directly into whatever ImGui window is currently active - call
    // once per frame from within that window.
    void draw_info_panel(GuiProvider &provider);
}
