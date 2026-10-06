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
    };

    void draw_hello_window(le::ext::ExtGuiContext &ctx)
    {
        // The read view can't be had while an edit holds the database; show
        // the last count until it can.
        WindowState &state = ctx.data<WindowState>();
        if (const le::ext::ReadView view = ctx.read(); view.valid())
            state.library_count = static_cast<int>(view.root().get_library_ids().size());
        ImGui::Text("Libraries: %d", state.library_count);
        ImGui::Text("Greeting libraries added this session: %d", ctx.data<hello::State>().greeting_libraries_added);
        ImGui::Text("Selected objects: %d", ctx.selection_count());
        ImGui::BeginDisabled(ctx.is_busy());
        if (ImGui::Button("Add greeting library"))
            ctx.run_tcl_command("hello_add_greeting_library gui");
        ImGui::EndDisabled();
    }
}

void le_ext_hello_ext_register_gui(le::ext::GuiRegistry &registry)
{
    registry.add_window({.title = "Hello", .dock = le::ext::Dock::RIGHT, .draw = draw_hello_window});
    registry.add_menu_item({.label = "Add a greeting library",
                            .action = [](le::ext::ExtGuiContext &ctx) { ctx.run_tcl_command("hello_add_greeting_library menu"); }});
}
