#pragma once

namespace le::gui
{
    class GuiProvider;

    // The bottom status row showing the current interaction mode, the
    // snapped mouse position, and the current selection count. Draws directly into
    // whatever ImGui window is currently active (le_gui.cpp's own "Layout
    // Engine" window) at the current cursor position - call once per
    // frame, right where the row should appear (bottom of the window,
    // after everything drawn above it). `width` is the full row width
    // available (the window's own content width) - used to right-align
    // the coordinates/selection text, since ImGui has no
    // layout-constraint system of its own to derive this from.
    void draw_status_bar(GuiProvider &provider, float width);
}
