#pragma once

struct LeHandle;

namespace le::gui
{
    // ImGui port of frontend/lib/components/toolbars/mode_selector.dart -
    // a vertical Select/Edit/Ruler button column. Meant to be drawn
    // inline alongside the design view (le_gui.cpp's own "Layout" dock
    // panel), not as its own separate dock panel - mirrors
    // home.dart's own layout, where ModeSelector is a plain child widget
    // of the same DockingItem as LayoutEngine/StatusBar, not a panel of
    // its own. Draws directly into whatever ImGui window is currently
    // active - call once per frame from within that window.
    void draw_mode_selector(LeHandle *handle);
}
