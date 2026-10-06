#pragma once

#include <functional>

namespace le::gui
{
    class GuiProvider;

    // A horizontal row of buttons whose contents depend on the current
    // mode (Select: Deselect All; Edit: Move/Resize/Delete/
    // Undo/Redo; Ruler: Clear Rulers). Meant to be drawn inline alongside
    // the design view (le_gui.cpp's own "Layout" dock panel), not as its
    // own separate dock panel. Draws directly
    // into whatever ImGui window is currently active - call once per
    // frame from within that window.
    // `extra` draws more buttons after the mode's own (extensions').
    void draw_mode_toolbar(GuiProvider &provider, const std::function<void()> &extra = {});

    // One momentary button in the toolbar's style: `icon` over `label`,
    // `tooltip` on hover (null or empty: none). True when clicked.
    bool draw_toolbar_button(const char *icon, const char *label, const char *tooltip);
}
