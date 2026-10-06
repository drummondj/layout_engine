#pragma once
// The extension SDK's GUI side, for an extension's GUI_SOURCES (linked into
// le_shell only - an extension's core library has no GUI dependency). Draw
// with Dear ImGui (<imgui.h>) inside a window's draw function; change the
// design by running a Tcl command, so the change lands in the console's
// history and in undo like every core GUI action.

#include "le/extension.hpp"

#include <imgui.h>

#include <cstdint>
#include <string>
#include <vector>

namespace le::gui
{
    class GuiProvider;
}

namespace le::ext
{
    /// @brief An extension's access to the GUI's session during one draw or
    /// menu callback. Valid only for that call.
    class ExtGuiContext
    {
    public:
        ExtGuiContext(gui::GuiProvider &provider, LeHandle *handle, std::string_view extension_name);

        /// @brief A render or Tcl command is in progress, so the design may
        /// be about to change. Renders happen on every mouse move and are
        /// usually over within a frame or two, so don't flip visible UI on
        /// it (disabling a button would make it flicker).
        bool is_busy() const;

        /// @brief How many objects are selected, and the `index`th of them
        /// (le_selected_object_ref).
        int32_t selection_count() const;
        LeObjectRef selected_object(int32_t index) const;

        /// @brief Queues `script` for the Tcl console to run, as if typed:
        /// the way to change the design from the GUI.
        void run_tcl_command(const std::string &script);

        /// @brief A read-only view of the database, or an invalid one
        /// (`!view.valid()`) while an edit holds it: the GUI thread never
        /// waits, so draw what you had last frame instead.
        ReadView read() const;

        /// @brief This extension's per-session state, shared with its Tcl
        /// commands (ExtensionContext::data).
        template <class T>
        T &data()
        {
            return extension_.data<T>();
        }

        /// @brief A font this extension registered with add_font, or the
        /// default font if it isn't loaded (its file is missing).
        ImFont *font(const std::string &name) const;

    private:
        gui::GuiProvider &provider_;
        ExtensionContext extension_;
        std::string extension_name_;
    };

    /// @brief Where a window docks in the default layout.
    enum class Dock
    {
        LEFT,   // with Browser
        RIGHT,  // a tab beside Properties/Layers/Settings
        BOTTOM, // a strip below the design view
        CENTER, // a tab beside the design view
    };

    /// @brief A dockable panel, listed in the Window menu. Its open/closed
    /// state is saved with the window layout.
    struct GuiWindow
    {
        std::string title;
        Dock dock = Dock::RIGHT;
        void (*draw)(ExtGuiContext &) = nullptr; // between ImGui::Begin and End
        bool open_by_default = true;
    };

    /// @brief An item in the Extensions menu.
    struct GuiMenuItem
    {
        std::string label;
        void (*action)(ExtGuiContext &) = nullptr;
    };

    /// @brief The design view's modes, for GuiToolbarButton::modes.
    enum ToolbarModes : uint32_t
    {
        TOOLBAR_SELECT = 1u << 0,
        TOOLBAR_EDIT = 1u << 1,
        TOOLBAR_RULER = 1u << 2,
        TOOLBAR_ALL_MODES = TOOLBAR_SELECT | TOOLBAR_EDIT | TOOLBAR_RULER,
    };

    /// @brief A button in the design view's toolbar, after the core ones.
    struct GuiToolbarButton
    {
        std::string icon;    // UTF-8: text, or a glyph from add_icon_glyphs
        std::string label;   // under the icon
        std::string tooltip; // on hover (empty: none)
        uint32_t modes = TOOLBAR_ALL_MODES;
        void (*action)(ExtGuiContext &) = nullptr;
    };

    /// @brief A shortcut, live while the mouse is over the design view (and
    /// no text field has focus). Keys the core uses - Z F D S E R M 0-9,
    /// the arrows, Escape and Delete, with any modifiers - are refused.
    struct GuiKeyBinding
    {
        ImGuiKey key = ImGuiKey_None;
        bool ctrl = false;
        bool shift = false;
        bool alt = false;
        void (*action)(ExtGuiContext &) = nullptr;
    };

    /// @brief One extension's GUI registrations, passed to its
    /// `le_ext_<name>_register_gui(GuiRegistry &)`.
    class GuiRegistry
    {
    public:
        explicit GuiRegistry(std::string extension_name) : extension_name_(std::move(extension_name)) {}

        void add_window(GuiWindow window);
        void add_menu_item(GuiMenuItem item);
        void add_toolbar_button(GuiToolbarButton button);
        void add_key_binding(GuiKeyBinding binding);
        /// @brief A collapsible section of the Settings panel - for this
        /// extension's settings (Registry::add_settings saves them).
        void add_settings_panel(void (*draw)(ExtGuiContext &));
        /// @brief Merges glyphs (`ranges`: ImGui pairs, 0-terminated) from a
        /// font file into the GUI's icon fonts, for toolbar icons and text.
        /// `file` is relative to the extension's directory (a resource).
        void add_icon_glyphs(std::string file, std::vector<ImWchar> ranges);
        /// @brief A font for this extension's windows, fetched with
        /// ExtGuiContext::font(name). `file` as for add_icon_glyphs.
        void add_font(std::string name, std::string file, float size_px);

    private:
        std::string extension_name_;
    };

    /// @brief Calls each extension's `le_ext_<name>_register_gui` (those
    /// with GUI_SOURCES), in dependency order - generated; le_shell calls it
    /// once at startup. Idempotent.
    void register_all_gui();
}
