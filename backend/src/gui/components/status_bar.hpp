#pragma once

struct LeHandle;

namespace le::gui
{
    // ImGui port of frontend/lib/components/status_bar.dart - the bottom
    // status row showing the current interaction mode, a render-in-
    // progress indicator, the current tooltip message, the snapped mouse
    // position, and the current selection count. Draws directly into
    // whatever ImGui window is currently active (le_gui.cpp's own "Layout
    // Engine" window) at the current cursor position - call once per
    // frame, right where the row should appear (bottom of the window,
    // after everything drawn above it), matching how status_bar.dart
    // itself sits directly below LayoutEngine in home.dart's own layout.
    // `width` is the full row width available (the window's own content
    // width) - used to right-align the coordinates/selection text and to
    // wrap the tooltip text between the mode and that right-aligned
    // group, since ImGui has no layout-constraint system of its own to
    // derive this from automatically the way Flutter's LayoutBuilder did.
    void draw_status_bar(LeHandle *handle, float width);
}
