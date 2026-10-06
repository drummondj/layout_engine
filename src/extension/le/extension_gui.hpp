#pragma once
// The extension SDK's GUI side, for an extension's GUI_SOURCES (linked into
// le_shell only - an extension's core library has no GUI dependency). Draw
// with Dear ImGui (<imgui.h>) inside a window's draw function; change the
// design by running a Tcl command, so the change lands in the console's
// history and in undo like every core GUI action.

#include "le/extension.hpp"

#include <string>

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
        /// be about to change.
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

    private:
        gui::GuiProvider &provider_;
        ExtensionContext extension_;
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

    /// @brief One extension's GUI registrations, passed to its
    /// `le_ext_<name>_register_gui(GuiRegistry &)`.
    class GuiRegistry
    {
    public:
        explicit GuiRegistry(std::string extension_name) : extension_name_(std::move(extension_name)) {}

        void add_window(GuiWindow window);
        void add_menu_item(GuiMenuItem item);

    private:
        std::string extension_name_;
    };

    /// @brief Calls each extension's `le_ext_<name>_register_gui` (those
    /// with GUI_SOURCES), in dependency order - generated; le_shell calls it
    /// once at startup. Idempotent.
    void register_all_gui();
}
