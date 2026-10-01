#pragma once

namespace le::gui
{
    class GuiProvider;

    // A horizontal row of buttons whose contents depend on the current
    // mode (Select: Select All/Deselect All; Edit: Move/Resize/Delete/
    // Undo/Redo; Ruler: Clear Rulers). Meant to be drawn inline alongside
    // the design view (le_gui.cpp's own "Layout" dock panel), not as its
    // own separate dock panel. Draws directly
    // into whatever ImGui window is currently active - call once per
    // frame from within that window.
    void draw_mode_toolbar(GuiProvider &provider);
}
