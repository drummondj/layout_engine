#include "panels.hpp"

#include "gui_provider.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>

namespace le::gui
{
    namespace
    {
        std::vector<ExtensionWindow> &windows_storage()
        {
            static std::vector<ExtensionWindow> windows;
            return windows;
        }

        std::vector<ExtensionMenuItem> &menu_items_storage()
        {
            static std::vector<ExtensionMenuItem> items;
            return items;
        }
    }

    DockSlot dock_slot(ext::Dock dock)
    {
        switch (dock)
        {
        case ext::Dock::LEFT:
            return DockSlot::LEFT;
        case ext::Dock::BOTTOM:
            return DockSlot::BOTTOM;
        case ext::Dock::CENTER:
            return DockSlot::CENTER;
        case ext::Dock::RIGHT:
            break;
        }
        return DockSlot::RIGHT;
    }

    Panel *PanelList::find(const std::string &id)
    {
        for (Panel &panel : panels_)
            if (panel.id == id)
                return &panel;
        return nullptr;
    }

    bool PanelList::any_in(DockSlot slot) const
    {
        for (const Panel &panel : panels_)
            if (panel.slot == slot)
                return true;
        return false;
    }

    void PanelList::dock_panels_missing_from_saved_layout(const char *center_window) const
    {
        const auto saved_dock = [](const std::string &name) -> ImGuiID
        {
            const ImGuiWindowSettings *settings = ImGui::FindWindowSettingsByID(ImHashStr(name.c_str()));
            return settings != nullptr ? settings->DockId : 0;
        };
        const auto is_saved = [](const std::string &name) { return ImGui::FindWindowSettingsByID(ImHashStr(name.c_str())) != nullptr; };
        const ImGuiID center = saved_dock(center_window);
        for (const Panel &panel : panels_)
        {
            if (is_saved(panel.imgui_name()))
                continue;
            ImGuiID target = 0;
            for (const Panel &other : panels_)
                if (other.slot == panel.slot && is_saved(other.imgui_name()) && (target = saved_dock(other.imgui_name())) != 0)
                    break;
            if (target == 0)
                target = center;
            if (target != 0)
                ImGui::DockBuilderDockWindow(panel.imgui_name().c_str(), target);
        }
    }

    void PanelList::draw_window_menu_items()
    {
        for (Panel &panel : panels_)
            ImGui::MenuItem(panel.imgui_name().c_str(), nullptr, &panel.open);
    }

    void PanelList::draw()
    {
        for (Panel &panel : panels_)
        {
            if (!panel.open)
                continue;
            // A docked tab that isn't selected is "collapsed": End without drawing.
            if (ImGui::Begin(panel.imgui_name().c_str(), &panel.open) && panel.draw)
                panel.draw();
            ImGui::End();
        }
    }

    void PanelList::read_ini_line(const char *line)
    {
        const char *equals = std::strrchr(line, '=');
        if (equals == nullptr || equals == line)
            return;
        const std::string id(line, equals);
        const bool open = std::strcmp(equals + 1, "0") != 0;
        if (Panel *panel = find(id))
            panel->open = open;
        else
            unknown_[id] = open;
    }

    std::string PanelList::ini_section() const
    {
        std::string out = "[LayoutEngine][Panels]\n";
        for (const Panel &panel : panels_)
            out += panel.id + (panel.open ? "=1\n" : "=0\n");
        for (const auto &[id, open] : unknown_)
            out += id + (open ? "=1\n" : "=0\n");
        return out + "\n";
    }

    const std::vector<ExtensionWindow> &extension_windows()
    {
        return windows_storage();
    }

    const std::vector<ExtensionMenuItem> &extension_menu_items()
    {
        return menu_items_storage();
    }

    std::vector<ExtensionMenu> extension_menus()
    {
        std::vector<ExtensionMenu> menus;
        for (const ExtensionMenuItem &registered : menu_items_storage())
        {
            auto menu = std::find_if(menus.begin(), menus.end(), [&](const ExtensionMenu &m) { return m.extension == registered.extension; });
            if (menu == menus.end())
                menu = menus.insert(menus.end(), {registered.extension, {}});
            menu->items.push_back(&registered.item);
        }
        return menus;
    }

    void clear_extension_gui_registrations()
    {
        windows_storage().clear();
        menu_items_storage().clear();
    }
}

namespace le::ext
{
    void GuiRegistry::add_window(GuiWindow window)
    {
        gui::windows_storage().push_back({extension_name_, std::move(window)});
    }

    void GuiRegistry::add_menu_item(GuiMenuItem item)
    {
        gui::menu_items_storage().push_back({extension_name_, std::move(item)});
    }

    ExtGuiContext::ExtGuiContext(gui::GuiProvider &provider, LeHandle *handle, std::string_view extension_name)
        : provider_(provider), extension_(handle, extension_name)
    {
    }

    bool ExtGuiContext::is_busy() const
    {
        return provider_.state().is_rendering || provider_.state().is_command_running;
    }

    int32_t ExtGuiContext::selection_count() const
    {
        return provider_.state().status_bar.selection_count;
    }

    LeObjectRef ExtGuiContext::selected_object(int32_t index) const
    {
        return provider_.selected_object_ref(index);
    }

    void ExtGuiContext::run_tcl_command(const std::string &script)
    {
        provider_.run_tcl_command(script);
    }

    ReadView ExtGuiContext::read() const
    {
        return ReadView(extension_.handle(), std::try_to_lock);
    }
}
