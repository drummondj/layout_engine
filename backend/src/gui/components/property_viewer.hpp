#pragma once

struct LeHandle;

namespace le::gui
{
    // ImGui port of frontend/lib/components/property_viewer.dart - shows
    // every property of whichever database object is currently selected
    // on the canvas, with a pager for a multi-object selection, an
    // ancestor-chain hierarchy tree (Library -> ... -> the selected
    // object) a user can click to inspect any ancestor instead, and a
    // "children" row per kind (e.g. an Abstract's own Terminals/
    // Obstructions) that re-anchors the hierarchy tree when clicked.
    // Backs the "Properties" dock panel (le_gui.cpp). Draws directly
    // into whatever ImGui window is currently active - call once per
    // frame from within the "Properties" window.
    void draw_property_viewer(LeHandle *handle);
}
