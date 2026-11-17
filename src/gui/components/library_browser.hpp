#pragma once

#include "gui_provider.hpp"

namespace le::gui
{
    /// @brief Whether the Browser lists `design` under `filter`: a design
    /// with neither an Abstract nor a Layout only when
    /// `show_designs_without_views`.
    bool design_shown_in_browser(const GuiProvider::DesignEntry &design, const char *filter, bool show_designs_without_views);

    // A filterable Library -> Design -> {Abstract, Layout} tree, backing
    // the "Browser" dock panel (le_gui.cpp). Clicking a Design row only
    // expands/collapses its own view list - opening a view
    // is unambiguous only for its "Abstract"/"Layout" leaf, each its own
    // click target. Draws GuiProvider::libraries() every frame, which
    // re-reads the tree only after the database changes.
    // Draws directly into whatever ImGui window is currently active -
    // call once per frame from within the "Browser" window.
    void draw_library_browser(GuiProvider &provider);
}
