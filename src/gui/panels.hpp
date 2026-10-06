#pragma once
// The dockable panels (every window but the design view): the core ones and
// those extensions register, drawn the same way, toggled from the Window
// menu, with their open/closed state saved in the window layout file. Uses
// Dear ImGui only (no GLFW), so it runs headless in tests.

#include "le/extension_gui.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace le::gui
{
    /// @brief Where a panel goes in the default dock layout.
    enum class DockSlot
    {
        LEFT,
        LEFT_BOTTOM, // below LEFT
        RIGHT,
        BOTTOM, // below the design view; split off only if a panel asks for it
        CENTER,
    };

    DockSlot dock_slot(ext::Dock dock);

    struct Panel
    {
        std::string title; // the tab and Window menu text
        std::string id;    // unique; the saved open state's key and the ImGui ID
        DockSlot slot = DockSlot::RIGHT;
        std::function<void()> draw;
        bool open = true;

        /// @brief The ImGui window name: the title, plus "###id" when they differ.
        std::string imgui_name() const { return title == id ? title : title + "###" + id; }
    };

    class PanelList
    {
    public:
        void add(Panel panel) { panels_.push_back(std::move(panel)); }
        const std::vector<Panel> &panels() const { return panels_; }
        Panel *find(const std::string &id);
        bool any_in(DockSlot slot) const;

        /// @brief One checkable item per panel; call inside a BeginMenu.
        void draw_window_menu_items();
        /// @brief Begin/draw/End for each open panel, each with a close button.
        void draw();

        /// @brief After a saved dock layout loads (before the first Begin):
        /// docks each panel the layout has never seen beside a panel of the
        /// same slot that it has, else beside `center_window` (the design
        /// view) - without this a newly added panel would open floating.
        void dock_panels_missing_from_saved_layout(const char *center_window) const;

        /// @brief The [LayoutEngine][Panels] ini section: one `id=0|1` line
        /// per panel. Lines for panels this build doesn't have are kept
        /// and written back, so a build without an extension doesn't
        /// forget its windows.
        void read_ini_line(const char *line);
        std::string ini_section() const;

    private:
        std::vector<Panel> panels_;
        std::map<std::string, bool> unknown_;
    };

    /// @brief What extensions registered (register_all_gui), in order.
    struct ExtensionWindow
    {
        std::string extension;
        ext::GuiWindow window;
    };
    struct ExtensionMenuItem
    {
        std::string extension;
        ext::GuiMenuItem item;
    };
    const std::vector<ExtensionWindow> &extension_windows();
    const std::vector<ExtensionMenuItem> &extension_menu_items();

    /// @brief The Extensions menu's submenus: one per extension with menu
    /// items, in registration order, each with its items in order.
    struct ExtensionMenu
    {
        std::string extension;
        std::vector<const ext::GuiMenuItem *> items;
    };
    std::vector<ExtensionMenu> extension_menus();
    /// @brief Forgets every registration - for tests.
    void clear_extension_gui_registrations();
}
