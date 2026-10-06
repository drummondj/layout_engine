#include "panels.hpp"

#include "gui_provider.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>

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

        std::vector<ExtensionToolbarButton> &toolbar_storage()
        {
            static std::vector<ExtensionToolbarButton> buttons;
            return buttons;
        }

        std::vector<ExtensionKeyBinding> &key_storage()
        {
            static std::vector<ExtensionKeyBinding> bindings;
            return bindings;
        }

        std::vector<ExtensionSettingsPanel> &settings_storage()
        {
            static std::vector<ExtensionSettingsPanel> panels;
            return panels;
        }

        struct IconGlyphs
        {
            std::string extension;
            std::string file;
            std::vector<ImWchar> ranges;
        };
        std::vector<IconGlyphs> &icon_storage()
        {
            static std::vector<IconGlyphs> glyphs;
            return glyphs;
        }

        struct NamedFont
        {
            std::string extension;
            std::string name;
            std::string file;
            float size_px;
            ImFont *loaded = nullptr; // in the current atlas
        };
        std::vector<NamedFont> &font_storage()
        {
            static std::vector<NamedFont> fonts;
            return fonts;
        }

        // An extension's font file, or "" (with a warning) if it isn't there.
        std::string font_path(const std::string &extension, const std::string &file)
        {
            const std::string path = ext::registry().resource(extension, file);
            std::error_code ec;
            if (path.empty() || !std::filesystem::exists(path, ec))
            {
                spdlog::warn("gui: extension {}'s font {} wasn't found{}", extension, file, path.empty() ? " (its directory isn't known)" : " at " + path);
                return {};
            }
            return path;
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

    const std::vector<ExtensionToolbarButton> &extension_toolbar_buttons() { return toolbar_storage(); }
    const std::vector<ExtensionKeyBinding> &extension_key_bindings() { return key_storage(); }
    const std::vector<ExtensionSettingsPanel> &extension_settings_panels() { return settings_storage(); }

    bool is_core_key(ImGuiKey key)
    {
        static constexpr ImGuiKey kCoreKeys[] = {
            ImGuiKey_Z, ImGuiKey_F, ImGuiKey_D, ImGuiKey_S, ImGuiKey_E, ImGuiKey_R, ImGuiKey_M, ImGuiKey_0, ImGuiKey_1, ImGuiKey_2,
            ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
            ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Escape, ImGuiKey_Delete,
        };
        return std::ranges::find(kCoreKeys, key) != std::end(kCoreKeys);
    }

    void merge_extension_icon_glyphs(ImFontAtlas *atlas, float size_px)
    {
        for (const IconGlyphs &glyphs : icon_storage())
        {
            const std::string path = font_path(glyphs.extension, glyphs.file);
            if (path.empty())
                continue;
            ImFontConfig config;
            config.MergeMode = true;
            config.PixelSnapH = true;
            atlas->AddFontFromFileTTF(path.c_str(), size_px, &config, glyphs.ranges.data());
        }
    }

    void add_extension_fonts(ImFontAtlas *atlas)
    {
        for (NamedFont &font : font_storage())
        {
            const std::string path = font_path(font.extension, font.file);
            font.loaded = path.empty() ? nullptr : atlas->AddFontFromFileTTF(path.c_str(), font.size_px);
        }
    }

    void clear_extension_gui_registrations()
    {
        windows_storage().clear();
        menu_items_storage().clear();
        toolbar_storage().clear();
        key_storage().clear();
        settings_storage().clear();
        icon_storage().clear();
        font_storage().clear();
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

    void GuiRegistry::add_toolbar_button(GuiToolbarButton button)
    {
        gui::toolbar_storage().push_back({extension_name_, std::move(button)});
    }

    void GuiRegistry::add_key_binding(GuiKeyBinding binding)
    {
        if (gui::is_core_key(binding.key))
        {
            spdlog::warn("gui: extension {}'s shortcut on {} is ignored - Layout Engine uses that key", extension_name_, ImGui::GetKeyName(binding.key));
            return;
        }
        for (const gui::ExtensionKeyBinding &other : gui::key_storage())
            if (other.binding.key == binding.key && other.binding.ctrl == binding.ctrl && other.binding.shift == binding.shift && other.binding.alt == binding.alt)
            {
                spdlog::warn("gui: extension {}'s shortcut on {} is ignored - extension {} already uses it", extension_name_, ImGui::GetKeyName(binding.key),
                             other.extension);
                return;
            }
        gui::key_storage().push_back({extension_name_, binding});
    }

    void GuiRegistry::add_settings_panel(void (*draw)(ExtGuiContext &))
    {
        gui::settings_storage().push_back({extension_name_, draw});
    }

    void GuiRegistry::add_icon_glyphs(std::string file, std::vector<ImWchar> ranges)
    {
        if (ranges.empty() || ranges.back() != 0)
            ranges.push_back(0);
        gui::icon_storage().push_back({extension_name_, std::move(file), std::move(ranges)});
    }

    void GuiRegistry::add_font(std::string name, std::string file, float size_px)
    {
        gui::font_storage().push_back({extension_name_, std::move(name), std::move(file), size_px});
    }

    ImFont *ExtGuiContext::font(const std::string &name) const
    {
        for (const gui::NamedFont &font : gui::font_storage())
            if (font.extension == extension_name_ && font.name == name && font.loaded)
                return font.loaded;
        return ImGui::GetFont();
    }

    ExtGuiContext::ExtGuiContext(gui::GuiProvider &provider, LeHandle *handle, std::string_view extension_name)
        : provider_(provider), extension_(handle, extension_name), extension_name_(extension_name)
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
