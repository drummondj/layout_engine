#include "panels.hpp"

#include "gui_provider.hpp"
#include "components/design_saver.hpp"
#include "components/icon.hpp"
#include "components/library_browser.hpp"
#include "components/settings_panel.hpp"
#include "theme.hpp"

#include <gtest/gtest.h>
#include "../../test_support/scratch_dir.hpp"
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
        le::Session session;
        LeHandle *handle = session.handle();

        void create_library(const std::string &name)
        {
            ASSERT_TRUE(session.write().create_library(le::LibraryData{.name = name}).has_value());
        }
        void write_db(const std::string &path)
        {
            ASSERT_TRUE(session.write().write_db(path, true).has_value());
        }
        std::string queued() { return session.requests().take_next_tcl_command().value_or(""); }
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

    session.create_library("lib1");
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
        session.create_library("lib");
        path = le::test::scratch_path("design_saver.led");
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
    session.write_db(path); // the design's file now exists
    session.create_library("edited");
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
    session.write_db(path);
    session.create_library("edited");
    session.session.write().set_confirm_overwrite(false);
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
    session.write_db(path);
    std::filesystem::remove(path); // nothing to overwrite now
    refresh();
    le::gui::DesignSaver saver(false);
    imgui.frame([&] { saver.save(provider); });
    const std::string command = session.queued();
    EXPECT_EQ(command.rfind("write_db ", 0), 0u) << command;
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
    acme.add_toolbar_button({.icon = le::Icon::Move, .label = "Acme", .modes = le::ext::TOOLBAR_EDIT, .action = [](le::ext::ExtGuiContext &) {}});
    acme.add_settings_panel([](le::ext::ExtGuiContext &) {});
    ASSERT_EQ(le::gui::extension_toolbar_buttons().size(), 1u);
    EXPECT_EQ(le::gui::extension_toolbar_buttons()[0].extension, "acme");
    EXPECT_EQ(le::gui::extension_toolbar_buttons()[0].button.modes, static_cast<uint32_t>(le::ext::TOOLBAR_EDIT));
    EXPECT_EQ(le::gui::extension_toolbar_buttons()[0].button.icon, le::Icon::Move);
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

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFontConfig base_config;
    base_config.SizePixels = 13.0f;
    ImFont *base = io.Fonts->AddFontDefault(&base_config);
    const int fonts_before = io.Fonts->Fonts.Size;
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

// Every icon has geometry inside its viewBox, so it draws within its square.
TEST(Icons, EveryIconHasGeometryInsideItsViewBox)
{
    for (int i = 0; i < le::kIconCount; ++i)
    {
        const le::icon_data::IconEntry &entry = le::icon_data::entry(static_cast<le::Icon>(i));
        SCOPED_TRACE(std::string(entry.name));
        ASSERT_GT(entry.polyline_count, 0);
        for (uint16_t p = 0; p < entry.polyline_count; ++p)
        {
            const le::icon_data::Polyline &polyline = le::icon_data::kPolylines[entry.first_polyline + p];
            EXPECT_GE(polyline.point_count, polyline.filled ? 3 : 2);
            for (uint16_t k = 0; k < polyline.point_count; ++k)
            {
                const le::icon_data::Point &point = le::icon_data::kPoints[polyline.first_point + k];
                EXPECT_TRUE(point.x >= 0.0f && point.x <= le::icon_data::kViewBoxSize && point.y >= 0.0f && point.y <= le::icon_data::kViewBoxSize);
            }
        }
    }
}

// le::ext::icon takes a square item of the requested size (the font size by
// default) and draws the icon's faint parts at a fraction of the text alpha.
TEST(Icons, ExtIconIsAnItemThatDrawsTheIcon)
{
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(200, 200);
    unsigned char *pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame();
    ImGui::Begin("icons");
    ImDrawList *draw_list = ImGui::GetWindowDrawList();

    const int vertices_before = draw_list->VtxBuffer.Size;
    le::ext::icon(le::Icon::ZoomFit, 24.0f);
    EXPECT_EQ(ImGui::GetItemRectSize().x, 24.0f);
    EXPECT_EQ(ImGui::GetItemRectSize().y, 24.0f);
    EXPECT_GT(draw_list->VtxBuffer.Size, vertices_before);

    le::ext::icon(le::Icon::Move);
    EXPECT_EQ(ImGui::GetItemRectSize().x, ImGui::GetFontSize());

    // ZoomFit's frame is opaque and its inner square faint.
    draw_list->VtxBuffer.clear();
    le::gui::draw_icon(draw_list, le::Icon::ZoomFit, ImVec2(0, 0), 32.0f, IM_COL32(255, 255, 255, 200));
    ImU32 max_alpha = 0, min_alpha = 255;
    for (const ImDrawVert &vertex : draw_list->VtxBuffer)
    {
        const ImU32 alpha = (vertex.col >> IM_COL32_A_SHIFT) & 0xFF;
        if (alpha == 0)
            continue; // anti-aliasing fringe
        max_alpha = std::max(max_alpha, alpha);
        min_alpha = std::min(min_alpha, alpha);
    }
    EXPECT_EQ(max_alpha, 200u);
    EXPECT_EQ(min_alpha, 80u);

    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
}

// The Browser hides a design with no Abstract or Layout unless asked to
// show it; the filter still applies either way.
TEST(LibraryBrowser, ADesignWithoutPhysicalViewsShowsOnlyWhenAskedFor)
{
    const le::gui::GuiProvider::DesignEntry schematic_only{.id = {}, .name = "fifo", .has_abstract = false, .has_layout = false};
    le::gui::GuiProvider::DesignEntry cell = schematic_only;
    cell.name = "INV_X1";
    cell.has_abstract = true;

    EXPECT_FALSE(le::gui::design_shown_in_browser(schematic_only, "", false));
    EXPECT_TRUE(le::gui::design_shown_in_browser(schematic_only, "", true));
    EXPECT_FALSE(le::gui::design_shown_in_browser(schematic_only, "inv", true)) << "filtered out by name";
    EXPECT_TRUE(le::gui::design_shown_in_browser(cell, "", false));
    EXPECT_TRUE(le::gui::design_shown_in_browser(cell, "abstract", false)) << "matched by its view";
}

// A layer or purpose name, put into a queued Tcl command, stays one word
// whatever it holds.
TEST(TclQuote, KeepsAnyNameOneWord)
{
    EXPECT_EQ(le::gui::tcl_quote("M1"), "\"M1\"");
    EXPECT_EQ(le::gui::tcl_quote("a}b{c"), "\"a}b{c\"");
    EXPECT_EQ(le::gui::tcl_quote("x [y] $z \"q\" \\"), "\"x \\[y\\] \\$z \\\"q\\\" \\\\\"");
}

namespace
{
    // WCAG 2 contrast ratio between two opaque sRGB colors.
    double contrast(ImVec4 a, ImVec4 b)
    {
        const auto luminance = [](ImVec4 c)
        {
            const auto channel = [](float v)
            { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
            return 0.2126 * channel(c.x) + 0.7152 * channel(c.y) + 0.0722 * channel(c.z);
        };
        const double la = luminance(a);
        const double lb = luminance(b);
        return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
    }

    // `c` drawn with its alpha over the opaque `under`.
    ImVec4 over(ImVec4 c, ImVec4 under)
    {
        return {under.x + (c.x - under.x) * c.w, under.y + (c.y - under.y) * c.w, under.z + (c.z - under.z) * c.w, 1.0f};
    }
}

// Every theme keeps the near-white text readable: AAA (7:1) on the panel
// backgrounds and input frames, AA (4.5:1) on buttons.
TEST(Theme, EveryThemeKeepsTheTextReadable)
{
    HeadlessImGui imgui;
    for (const le::Theme &theme : le::kThemes)
    {
        SCOPED_TRACE(std::string(theme.name));
        ImGuiStyle style;
        le::gui::apply_theme_colors(style, theme);
        const ImVec4 text = style.Colors[ImGuiCol_Text];
        const ImVec4 window = style.Colors[ImGuiCol_WindowBg];
        EXPECT_EQ(window.x, theme.background.r);
        for (ImGuiCol background : {ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_MenuBarBg, ImGuiCol_FrameBg})
            EXPECT_GE(contrast(text, over(style.Colors[background], window)), 7.0) << ImGui::GetStyleColorName(background);
        EXPECT_GE(contrast(text, over(style.Colors[ImGuiCol_PopupBg], window)), 7.0);
        for (ImGuiCol button : {ImGuiCol_Button, ImGuiCol_ButtonHovered})
            EXPECT_GE(contrast(text, over(style.Colors[button], window)), 4.5) << ImGui::GetStyleColorName(button);
    }
}

TEST(Theme, NamesAreUniqueAndTheDefaultIsGray)
{
    EXPECT_EQ(le::kThemes[0].name, "gray");
    for (size_t i = 0; i < le::kThemes.size(); ++i)
        EXPECT_EQ(le::find_theme(le::kThemes[i].name), i);
    EXPECT_FALSE(le::find_theme("orange"));
}

// The Settings panel shows the handle's theme and changes it through a
// queued set_theme, so the change lands in the command history.
TEST(Theme, TheSettingsPanelReadsAndQueuesTheTheme)
{
    Session session;
    le::gui::GuiProvider provider{session.handle};
    HeadlessImGui imgui;
    ASSERT_TRUE(session.session.write().set_theme("red"));
    provider.refresh();
    EXPECT_EQ(provider.state().settings.theme, *le::find_theme("red"));
    imgui.frame([&] {
        ImGui::Begin("Settings");
        le::gui::draw_settings_panel(provider);
        ImGui::End();
    });
    provider.set_theme(*le::find_theme("teal"));
    EXPECT_EQ(session.queued(), "set_theme teal");
}
