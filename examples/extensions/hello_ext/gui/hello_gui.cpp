// hello_ext's GUI: a "Hello" window and an Extensions menu item. Edits go
// through Tcl commands, so they show in the console and undo like any other.

#include "hello_ext/hello.hpp"

#include <le/extension_gui.hpp>

#include <imgui.h>

namespace
{
    // What the window remembers between frames, per session.
    struct WindowState
    {
        int library_count = 0;
        int added = 0; // numbers the libraries the GUI adds
    };

    // Adds "hello_<library_name><n>", as a Tcl command (so it's in the
    // console history and undo).
    void add_library(le::ext::ExtGuiContext &ctx)
    {
        WindowState &state = ctx.data<WindowState>();
        ctx.run_tcl_command("hello_add_library " + ctx.data<hello::State>().library_name + std::to_string(++state.added));
    }

    void draw_hello_window(le::ext::ExtGuiContext &ctx)
    {
        // The read view can't be had while an edit holds the database; show
        // the last count until it can.
        WindowState &state = ctx.data<WindowState>();
        if (const le::ext::ReadView view = ctx.read(); view.valid())
            state.library_count = static_cast<int>(view.root().get_library_ids().size());
        ImGui::PushFont(ctx.font("heading"));
        ImGui::TextUnformatted("Hello");
        ImGui::PopFont();
        ImGui::Text("Libraries: %d", state.library_count);
        ImGui::Text("Libraries added this session: %d", ctx.data<hello::State>().libraries_added);
        ImGui::Text("Selected objects: %d", ctx.selection_count());
        // Queued, so it's safe to click mid-render: it runs when the console is free.
        if (ImGui::Button("Add library"))
            add_library(ctx);
    }

    // hello_ext's section of the Settings panel.
    void draw_settings(le::ext::ExtGuiContext &ctx)
    {
        std::string &name = ctx.data<hello::State>().library_name;
        char buffer[64] = {};
        name.copy(buffer, sizeof(buffer) - 1);
        if (ImGui::InputText("Library name", buffer, sizeof(buffer)))
            name = buffer;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Libraries added from the GUI are named hello_<this><n>");
    }
}

void le_ext_hello_ext_register_gui(le::ext::GuiRegistry &registry)
{
    registry.add_window({.title = "Hello", .dock = le::ext::Dock::RIGHT, .draw = draw_hello_window});
    registry.add_menu_item({.label = "Add a library", .action = add_library});
    registry.add_toolbar_button({.icon = "H", .label = "Hello", .tooltip = "Add a library (hello_ext) - H", .action = add_library});
    registry.add_key_binding({.key = ImGuiKey_H, .action = add_library});
    registry.add_settings_panel(draw_settings);
    // The toolbar icon "H" is a glyph merged into the icon fonts, from a
    // font shipped as a resource; the window heading uses the same font.
    registry.add_icon_glyphs("fonts/Quicksand-Medium.ttf", {'H', 'H', 0});
    registry.add_font("heading", "fonts/Quicksand-Medium.ttf", 24.0f);
}
