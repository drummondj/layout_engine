#pragma once

struct LeHandle;

namespace le::gui
{
    // ImGui port of frontend/lib/components/library_browser.dart - a
    // filterable Library -> Design -> {Abstract, Layout} tree, backing
    // the "Browser" dock panel (le_gui.cpp). Clicking a Design row only
    // expands/collapses its own view list (unlike the Dart tree, whose
    // design-node tap opens its Abstract view directly) - opening a view
    // is unambiguous only for its "Abstract"/"Layout" leaf, each its own
    // click target. Reads directly from the handle every frame (le_library_count/
    // le_library_at/le_library_design_count/le_library_design_at) rather
    // than caching a tree structure of its own - immediate-mode's own
    // natural fit, unlike library_browser.dart's own Node tree +
    // TreeController (needed there to give Flutter's retained-mode
    // AnimatedTreeView something stable to diff and animate against).
    // Draws directly into whatever ImGui window is currently active -
    // call once per frame from within the "Browser" window.
    void draw_library_browser(LeHandle *handle);
}
