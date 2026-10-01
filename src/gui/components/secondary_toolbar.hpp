#pragma once

namespace le::gui
{
    class GuiProvider;

    // The secondary toolbar (NEW_FEATURES_SEPT_2026.md item 2): a
    // tool-specific row of options shown directly under ModeToolbar only
    // while a tool that has options is active - currently just the
    // placement toolbar (Edit mode with placements selected: the Move
    // snap mode, plus rotate/flip, which commit immediately). Future tools (e.g. shape
    // resize, item 3) add their own row to draw_secondary_toolbar's
    // dispatch and to has_secondary_toolbar.
    //
    // le_gui.cpp overlays it on the top edge of the design view rather
    // than inserting a row that pushes the view down: showing/hiding it
    // then never resizes the viewport (which would force a full
    // re-render and shift the design under the cursor mid-move).
    bool has_secondary_toolbar(const GuiProvider &provider);

    // Draws into whatever ImGui window is current - call only when
    // has_secondary_toolbar() is true.
    void draw_secondary_toolbar(GuiProvider &provider);
}
