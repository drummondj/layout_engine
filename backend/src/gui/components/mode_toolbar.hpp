#pragma once

struct LeHandle;

namespace le::gui
{
    // ImGui port of frontend/lib/components/toolbars/mode_toolbar.dart -
    // a horizontal row of buttons whose contents depend on the current
    // mode (Select: Select All/Deselect All; Edit: Move/Undo/Redo;
    // Ruler: Clear Rulers). The Dart original's own Edit-mode
    // Resize/Rotate/Align */Delete buttons are all still no-ops there
    // too (`onPressed: () => {}`) - left unported here rather than
    // wiring up dead buttons; add them once the underlying feature
    // actually exists. Meant to be drawn inline alongside the design
    // view (le_gui.cpp's own "Layout" dock panel), not as its own
    // separate dock panel - mirrors home.dart's own layout, where
    // ModeToolbar is a plain child widget of the same DockingItem as
    // LayoutEngine/StatusBar, not a panel of its own. Draws directly
    // into whatever ImGui window is currently active - call once per
    // frame from within that window.
    void draw_mode_toolbar(LeHandle *handle);
}
