#include "panels.hpp"

#include "gui_provider.hpp"

#include <gtest/gtest.h>
#include <imgui.h>

#include <atomic>
#include <thread>

namespace
{
    using le::gui::DockSlot;
    using le::gui::Panel;
    using le::gui::PanelList;

    // A Dear ImGui context with no backend: frames run, nothing is shown.
    struct HeadlessImGui
    {
        HeadlessImGui()
        {
            ImGui::CreateContext();
            ImGuiIO &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(800, 600);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char *pixels = nullptr;
            int width = 0;
            int height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }
        ~HeadlessImGui() { ImGui::DestroyContext(); }

        template <class F>
        void frame(F &&body)
        {
            ImGui::NewFrame();
            body();
            ImGui::Render();
        }
    };

    struct Session
    {
        LeHandle *handle = le_create();
        ~Session() { le_destroy(handle); }
    };

    struct Counter
    {
        int frames = 0;
    };
}

TEST(Panels, ImGuiNameAddsTheIdOnlyWhenItDiffers)
{
    EXPECT_EQ((Panel{"Browser", "Browser"}.imgui_name()), "Browser");
    EXPECT_EQ((Panel{"Hello", "ext.hello_ext.Hello"}.imgui_name()), "Hello###ext.hello_ext.Hello");
}

TEST(Panels, OnlyOpenPanelsAreDrawn)
{
    HeadlessImGui imgui;
    int a = 0;
    int b = 0;
    PanelList panels;
    panels.add({"A", "A", DockSlot::LEFT, [&] { ++a; }});
    panels.add({"B", "B", DockSlot::RIGHT, [&] { ++b; }, false});
    imgui.frame([&] { panels.draw(); });
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 0);

    panels.find("A")->open = false;
    panels.find("B")->open = true;
    imgui.frame([&] { panels.draw(); });
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);
    EXPECT_TRUE(panels.any_in(DockSlot::RIGHT));
    EXPECT_FALSE(panels.any_in(DockSlot::BOTTOM));
}

TEST(Panels, OpenStateRoundTripsThroughTheIniAndKeepsUnknownPanels)
{
    PanelList panels;
    panels.add({"A", "A", DockSlot::LEFT, nullptr});
    panels.add({"B", "B", DockSlot::RIGHT, nullptr});
    panels.read_ini_line("A=0");
    panels.read_ini_line("ext.gone.Window=1");
    panels.read_ini_line("garbage");
    EXPECT_FALSE(panels.find("A")->open);
    EXPECT_TRUE(panels.find("B")->open);
    EXPECT_EQ(panels.ini_section(), "[LayoutEngine][Panels]\nA=0\nB=1\next.gone.Window=1\n\n");
}

TEST(ExtensionGui, RegistrationsCarryTheirExtension)
{
    le::gui::clear_extension_gui_registrations();
    le::ext::GuiRegistry registry("acme");
    registry.add_window({.title = "Router", .dock = le::ext::Dock::BOTTOM, .draw = [](le::ext::ExtGuiContext &) {}});
    registry.add_menu_item({.label = "Route", .action = [](le::ext::ExtGuiContext &) {}});
    ASSERT_EQ(le::gui::extension_windows().size(), 1u);
    EXPECT_EQ(le::gui::extension_windows()[0].extension, "acme");
    EXPECT_EQ(le::gui::extension_windows()[0].window.title, "Router");
    EXPECT_EQ(le::gui::dock_slot(le::gui::extension_windows()[0].window.dock), DockSlot::BOTTOM);
    ASSERT_EQ(le::gui::extension_menu_items().size(), 1u);
    EXPECT_EQ(le::gui::extension_menu_items()[0].extension, "acme");
    le::gui::clear_extension_gui_registrations();
    EXPECT_TRUE(le::gui::extension_windows().empty());
}

TEST(ExtensionGui, ContextSharesStateWithTheExtensionAndNeverWaitsToRead)
{
    Session session;
    le::gui::GuiProvider provider(session.handle);
    provider.refresh();
    le::ext::ExtGuiContext gui(provider, session.handle, "acme");
    le::ext::ExtensionContext core(session.handle, "acme");
    core.data<Counter>().frames = 3;
    EXPECT_EQ(gui.data<Counter>().frames, 3);
    EXPECT_FALSE(gui.is_busy());
    EXPECT_EQ(gui.selection_count(), 0);

    le_create_library(session.handle, "lib1");
    {
        const le::ext::ReadView view = gui.read();
        ASSERT_TRUE(view.valid());
        EXPECT_EQ(view.root().get_library_ids().size(), 1u);
    }

    // Another thread holds the database for an edit: read() gives up at once.
    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
    std::thread writer([&]
                       {
                           le::ext::WriteView view = core.write();
                           holding = true;
                           while (!release)
                               std::this_thread::yield(); });
    while (!holding)
        std::this_thread::yield();
    EXPECT_FALSE(gui.read().valid());
    release = true;
    writer.join();
    EXPECT_TRUE(gui.read().valid());
}
