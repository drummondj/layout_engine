#pragma once

namespace le::gui
{
    class GuiProvider;

    // The Info panel - the current mode's instructions
    // (le_tooltip_message), wrapped to the panel's width. Docked at the
    // bottom of the right sidebar by default.
    // Draws directly into whatever ImGui window is currently active - call
    // once per frame from within that window.
    void draw_info_panel(GuiProvider &provider);
}
