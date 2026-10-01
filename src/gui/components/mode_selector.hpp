#pragma once

namespace le::gui
{
    class GuiProvider;

    // A vertical Select/Edit/Ruler button column. Meant to be drawn
    // inline alongside the design view (le_gui.cpp's own "Layout" dock
    // panel), not as its own separate dock panel. Draws directly into
    // whatever ImGui window is currently
    // active - call once per frame from within that window.
    void draw_mode_selector(GuiProvider &provider);
}
