#include "panels.hpp"

#include "gui_provider.hpp"
#include "components/design_saver.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <filesystem>
#include <unistd.h>
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
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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

TEST(Panels, APanelTheSavedLayoutHasNeverSeenDocksBesideItsSlot)
{
    constexpr ImGuiID kDockSpace = 0x1234;
    const auto saved_dock = [](const char *name)
    {
        const ImGuiWindowSettings *settings = ImGui::FindWindowSettingsByID(ImHashStr(name));
        return settings != nullptr ? settings->DockId : 0u;
    };

    // A layout saved by a build without the Hello and Strip panels.
    std::string ini;
    ImGuiID right = 0;
    ImGuiID center = 0;
    {
        HeadlessImGui imgui;
        imgui.frame([&]
                    {
                        ImGui::DockBuilderAddNode(kDockSpace, ImGuiDockNodeFlags_DockSpace);
                        ImGui::DockBuilderSetNodeSize(kDockSpace, ImVec2(800, 600));
                        center = kDockSpace;
                        right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.3f, nullptr, &center);
                        ImGui::DockBuilderDockWindow("Properties", right);
                        ImGui::DockBuilderDockWindow("Layout", center);
                        ImGui::DockBuilderFinish(kDockSpace); });
        for (int i = 0; i < 3; ++i)
            imgui.frame([]
                        {
                            ImGui::DockSpaceOverViewport(kDockSpace);
                            ImGui::Begin("Properties");
                            ImGui::End();
                            ImGui::Begin("Layout");
                            ImGui::End(); });
        ini = ImGui::SaveIniSettingsToMemory();
    }

    HeadlessImGui imgui;
    ImGui::LoadIniSettingsFromMemory(ini.c_str());
    PanelList panels;
    panels.add({"Properties", "Properties", DockSlot::RIGHT, nullptr});
    panels.add({"Hello", "ext.hello_ext.Hello", DockSlot::RIGHT, nullptr});
    panels.add({"Strip", "ext.acme.Strip", DockSlot::BOTTOM, nullptr});
    imgui.frame([&]
                {
                    panels.dock_panels_missing_from_saved_layout("Layout");
                    EXPECT_EQ(saved_dock("Hello###ext.hello_ext.Hello"), right);
                    EXPECT_EQ(saved_dock("Strip###ext.acme.Strip"), center);
                    EXPECT_EQ(saved_dock("Properties"), right); });
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

TEST(ExtensionGui, MenuItemsAreGroupedPerExtensionInRegistrationOrder)
{
    le::gui::clear_extension_gui_registrations();
    le::ext::GuiRegistry acme("acme");
    le::ext::GuiRegistry beta("beta");
    acme.add_menu_item({.label = "Route", .action = [](le::ext::ExtGuiContext &) {}});
    beta.add_menu_item({.label = "Check", .action = [](le::ext::ExtGuiContext &) {}});
    acme.add_menu_item({.label = "Unroute", .action = [](le::ext::ExtGuiContext &) {}});
    const std::vector<le::gui::ExtensionMenu> menus = le::gui::extension_menus();
    ASSERT_EQ(menus.size(), 2u);
    EXPECT_EQ(menus[0].extension, "acme");
    ASSERT_EQ(menus[0].items.size(), 2u);
    EXPECT_EQ(menus[0].items[0]->label, "Route");
    EXPECT_EQ(menus[0].items[1]->label, "Unroute");
    EXPECT_EQ(menus[1].extension, "beta");
    ASSERT_EQ(menus[1].items.size(), 1u);
    le::gui::clear_extension_gui_registrations();
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

// DesignSaver: Save asks before replacing an existing file unless the
// confirm_overwrite setting is off, and writes straight away otherwise.
class DesignSaverTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        le_create_library(session.handle, "lib");
        path = (std::filesystem::temp_directory_path() / ("design_saver_" + std::to_string(::getpid()) + ".led")).string();
        std::filesystem::remove(path);
    }
    void TearDown() override { std::filesystem::remove(path); }

    void refresh() { provider.refresh(); }

    Session session;
    le::gui::GuiProvider provider{session.handle};
    HeadlessImGui imgui;
    std::string path;
};

TEST_F(DesignSaverTest, SaveAsksBeforeOverwritingAnExistingFile)
{
    ASSERT_EQ(le_write_db(session.handle, path.c_str(), 1), 0); // the design's file now exists
    le_create_library(session.handle, "edited");
    refresh();
    le::gui::DesignSaver saver(true);
    imgui.frame([&] {
        saver.save(provider);
        saver.draw(provider);
    });
    EXPECT_TRUE(saver.busy()) << "waiting for the overwrite answer";
    EXPECT_TRUE(saver.status().empty()) << "nothing written yet";
    EXPECT_TRUE(provider.has_unsaved_design());
}

TEST_F(DesignSaverTest, SaveWritesStraightAwayWhenTheQuestionIsOff)
{
    ASSERT_EQ(le_write_db(session.handle, path.c_str(), 1), 0);
    le_create_library(session.handle, "edited");
    le_set_confirm_overwrite(session.handle, 0);
    refresh();
    le::gui::DesignSaver saver(true);
    imgui.frame([&] {
        saver.save(provider);
        saver.draw(provider);
    });
    EXPECT_FALSE(saver.busy());
    EXPECT_EQ(saver.status(), "Saved to " + path);
    EXPECT_FALSE(provider.has_unsaved_design());
}

TEST_F(DesignSaverTest, TheMenusSaverQueuesWriteDb)
{
    ASSERT_EQ(le_write_db(session.handle, path.c_str(), 1), 0);
    std::filesystem::remove(path); // nothing to overwrite now
    refresh();
    le::gui::DesignSaver saver(false);
    imgui.frame([&] { saver.save(provider); });
    const char *command = le_take_next_pending_tcl_command(session.handle);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(std::string(command).rfind("write_db ", 0), 0u) << command;
}

TEST(ExtensionGui, KeyBindingsOnCoreKeysOrTakenCombinationsAreRefused)
{
    le::gui::clear_extension_gui_registrations();
    le::ext::GuiRegistry acme("acme");
    le::ext::GuiRegistry beta("beta");
    acme.add_key_binding({.key = ImGuiKey_Z, .action = [](le::ext::ExtGuiContext &) {}});              // core key
    acme.add_key_binding({.key = ImGuiKey_M, .ctrl = true, .action = [](le::ext::ExtGuiContext &) {}}); // core, any modifiers
    acme.add_key_binding({.key = ImGuiKey_H, .action = [](le::ext::ExtGuiContext &) {}});
    beta.add_key_binding({.key = ImGuiKey_H, .action = [](le::ext::ExtGuiContext &) {}});              // taken by acme
    beta.add_key_binding({.key = ImGuiKey_H, .shift = true, .action = [](le::ext::ExtGuiContext &) {}});
    ASSERT_EQ(le::gui::extension_key_bindings().size(), 2u);
    EXPECT_EQ(le::gui::extension_key_bindings()[0].extension, "acme");
    EXPECT_EQ(le::gui::extension_key_bindings()[1].extension, "beta");
    EXPECT_TRUE(le::gui::extension_key_bindings()[1].binding.shift);
    le::gui::clear_extension_gui_registrations();
}

TEST(ExtensionGui, ToolbarButtonsAndSettingsPanelsCarryTheirExtension)
{
    le::gui::clear_extension_gui_registrations();
    le::ext::GuiRegistry acme("acme");
    acme.add_toolbar_button({.icon = "A", .label = "Acme", .modes = le::ext::TOOLBAR_EDIT, .action = [](le::ext::ExtGuiContext &) {}});
    acme.add_settings_panel([](le::ext::ExtGuiContext &) {});
    ASSERT_EQ(le::gui::extension_toolbar_buttons().size(), 1u);
    EXPECT_EQ(le::gui::extension_toolbar_buttons()[0].extension, "acme");
    EXPECT_EQ(le::gui::extension_toolbar_buttons()[0].button.modes, static_cast<uint32_t>(le::ext::TOOLBAR_EDIT));
    ASSERT_EQ(le::gui::extension_settings_panels().size(), 1u);
    EXPECT_EQ(le::gui::extension_settings_panels()[0].extension, "acme");
    le::gui::clear_extension_gui_registrations();
}

TEST(ExtensionGui, FontsLoadIntoTheAtlasAndAreFoundByName)
{
    le::gui::clear_extension_gui_registrations();
    const std::string font = std::string(LE_FONT_DIR) + "/Quicksand-Medium.ttf";
    le::ext::GuiRegistry acme("acme");
    acme.add_font("heading", font, 24.0f);
    acme.add_font("missing", "no/such/font.ttf", 12.0f);
    acme.add_icon_glyphs(font, {'A', 'A'});

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFontConfig base_config;
    base_config.SizePixels = 13.0f; // merging needs an explicitly sized font, as in le_gui.cpp
    ImFont *base = io.Fonts->AddFontDefault(&base_config);
    const int fonts_before = io.Fonts->Fonts.Size;
    le::gui::merge_extension_icon_glyphs(io.Fonts, 13.0f);
    EXPECT_EQ(io.Fonts->Fonts.Size, fonts_before) << "glyphs merge into the font added last";
    le::gui::add_extension_fonts(io.Fonts);
    EXPECT_EQ(io.Fonts->Fonts.Size, fonts_before + 1) << "a missing font file is skipped";
    unsigned char *pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    Session session;
    le::gui::GuiProvider provider(session.handle);
    le::ext::ExtGuiContext ctx(provider, session.handle, "acme");
    io.DisplaySize = ImVec2(100, 100);
    ImGui::NewFrame();
    EXPECT_NE(ctx.font("heading"), base);
    EXPECT_EQ(ctx.font("missing"), ImGui::GetFont()) << "falls back to the current font";
    ImGui::Render();
    ImGui::DestroyContext();
    le::gui::clear_extension_gui_registrations();
}
