#pragma once

struct LeHandle;

namespace le::gui
{
    // ImGui port of frontend/lib/components/layer_manager.dart - the
    // hierarchy-depth field plus the layer/purpose visibility+
    // selectability grid (row-per-layer, row-per-purpose, each with its
    // own "All ..." aggregate toggle row). Shares the "Properties" dock
    // panel with property_viewer.hpp (a separate section, not a
    // separate tab, for this pass). Draws directly into whatever ImGui
    // window is currently active - call once per frame from within that
    // window.
    void draw_layer_manager(LeHandle *handle);
}
