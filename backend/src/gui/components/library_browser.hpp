#pragma once

namespace le::gui
{
    class GuiProvider;

    // ImGui port of frontend/lib/components/library_browser.dart - a
    // filterable Library -> Design -> {Abstract, Layout} tree, backing
    // the "Browser" dock panel (le_gui.cpp). Clicking a Design row only
    // expands/collapses its own view list (unlike the Dart tree, whose
    // design-node tap opens its Abstract view directly) - opening a view
    // is unambiguous only for its "Abstract"/"Layout" leaf, each its own
    // click target. Reads via GuiProvider::library_count/_at/
    // library_design_count/_at every frame rather than caching a tree
    // structure of its own - immediate-mode's own natural fit, unlike
    // library_browser.dart's own Node tree + TreeController (needed there
    // to give Flutter's retained-mode AnimatedTreeView something stable
    // to diff and animate against). This tree isn't part of
    // GuiProvider::State - only walked for an *expanded* TreeNode, so
    // eagerly fetching every library's full design list every frame
    // regardless of expand state would be strictly more work than this.
    // Draws directly into whatever ImGui window is currently active -
    // call once per frame from within the "Browser" window.
    void draw_library_browser(GuiProvider &provider);
}
