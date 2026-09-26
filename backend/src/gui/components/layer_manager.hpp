#pragma once

namespace le::gui
{
    class GuiProvider;

    // ImGui port of frontend/lib/components/layer_manager.dart - the
    // layer/purpose visibility+
    // selectability grid (row-per-layer, row-per-purpose, each with its
    // own "All ..." aggregate toggle row). Hierarchy depth and the
    // flightline fanout limit moved to settings_panel.hpp (item 9). Shares the "Properties" dock
    // panel with property_viewer.hpp (a separate section, not a
    // separate tab, for this pass). Draws directly into whatever ImGui
    // window is currently active - call once per frame from within that
    // window.
    void draw_layer_manager(GuiProvider &provider);
}
