#include "settings_panel.hpp"

#include "committed_field.hpp"
#include "gui_provider.hpp"
#include "imgui.h"

#include "portable-file-dialogs.h"

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace le::gui
{
    namespace
    {
        const std::vector<std::string> kJsonFilters = {"JSON files (*.json)", "*.json", "All files", "*"};

        // A Save As/Load in progress. The system dialog (portable-file-dialogs
        // - zenity/kdialog on Linux, the native one on macOS/Windows) runs
        // asynchronously and is polled each frame so the GUI keeps drawing.
        // With no dialog helper installed, a small in-app path prompt
        // (a modal popup) stands in for it.
        struct FileRequest
        {
            enum class Kind
            {
                NONE,
                SAVE,
                LOAD,
            } kind = Kind::NONE;
            std::unique_ptr<pfd::save_file> save_dialog;
            std::unique_ptr<pfd::open_file> open_dialog;
            bool prompt_open = false;
            std::array<char, 1024> prompt_path{};
        };

        void section(const char *title)
        {
            ImGui::Spacing();
            ImGui::SeparatorText(title);
        }

        // Starts a Save As (`save` true) or Load - the system dialog when
        // one is available, else the in-app prompt, prefilled either way
        // with the default settings file.
        void start_file_request(FileRequest &request, bool save)
        {
            const std::string default_path = le_default_settings_path();
            request.kind = save ? FileRequest::Kind::SAVE : FileRequest::Kind::LOAD;
            if (pfd::settings::available())
            {
                if (save)
                    request.save_dialog = std::make_unique<pfd::save_file>("Save settings", default_path, kJsonFilters, pfd::opt::none);
                else
                    request.open_dialog = std::make_unique<pfd::open_file>("Load settings", default_path, kJsonFilters, pfd::opt::none);
                return;
            }
            request.prompt_path.fill('\0');
            default_path.copy(request.prompt_path.data(), request.prompt_path.size() - 1);
            request.prompt_open = true;
        }

        // Finishes a request whose path is now known ("" - cancelled).
        void finish_file_request(GuiProvider &provider, FileRequest &request, const std::string &path, std::string &status)
        {
            if (!path.empty())
            {
                if (request.kind == FileRequest::Kind::SAVE)
                {
                    provider.save_settings(path);
                    status = "Saved to " + path;
                }
                else
                {
                    provider.load_settings(path);
                    status = "Loaded " + path;
                }
            }
            request = FileRequest{};
        }

        void poll_file_request(GuiProvider &provider, FileRequest &request, std::string &status)
        {
            if (request.save_dialog && request.save_dialog->ready(0))
                finish_file_request(provider, request, request.save_dialog->result(), status);
            else if (request.open_dialog && request.open_dialog->ready(0))
            {
                const std::vector<std::string> paths = request.open_dialog->result();
                finish_file_request(provider, request, paths.empty() ? std::string() : paths.front(), status);
            }

            if (request.prompt_open)
            {
                ImGui::OpenPopup("Settings file###settings_path_prompt");
                request.prompt_open = false;
            }
            if (ImGui::BeginPopupModal("Settings file###settings_path_prompt", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                const bool save = request.kind == FileRequest::Kind::SAVE;
                ImGui::TextUnformatted(save ? "Save settings to:" : "Load settings from:");
                ImGui::TextDisabled("(no system file dialog found - install zenity or kdialog for one)");
                ImGui::SetNextItemWidth(420.0f);
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere();
                const bool entered = ImGui::InputText("##settings_path", request.prompt_path.data(), request.prompt_path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                if (ImGui::Button(save ? "Save" : "Load") || entered)
                {
                    finish_file_request(provider, request, request.prompt_path.data(), status);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    request = FileRequest{};
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    }

    void draw_settings_panel(GuiProvider &provider)
    {
        const GuiProvider::State::Settings &settings = provider.state().settings;
        static constexpr const char *kNoTechnology = "Read a technology LEF first - grid spacing is set in microns";

        section("Grid");
        static CommittedField<double> minor_field;
        draw_committed_double_field("##minor_grid", "Minor spacing (um)", "%.4g", settings.minor_grid_um, minor_field,
                                    [&](double um)
                                    { provider.set_grid_spacing_um(um, 0.0); }, kNoTechnology);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What drawing, Move and Resize snap to on the user grid");
        static CommittedField<double> major_field;
        draw_committed_double_field("##major_grid", "Major spacing (um)", "%.4g", settings.major_grid_um, major_field,
                                    [&](double um)
                                    { provider.set_grid_spacing_um(0.0, um); }, kNoTechnology);

        // Minor spacing to the manufacturing grid, major to 10x that.
        const double mfg_um = settings.manufacturing_grid_um;
        char mfg_label[64];
        if (mfg_um > 0.0)
            std::snprintf(mfg_label, sizeof(mfg_label), "Use manufacturing grid (%g um)###use_mfg_grid", mfg_um);
        else
            std::snprintf(mfg_label, sizeof(mfg_label), "Use manufacturing grid###use_mfg_grid");
        ImGui::BeginDisabled(mfg_um <= 0.0);
        if (ImGui::Button(mfg_label))
            provider.set_grid_spacing_um(mfg_um, mfg_um * 10.0);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", mfg_um > 0.0 ? "Sets the minor spacing to the manufacturing grid and the major spacing to 10x that"
                                                 : "The technology has no MANUFACTURINGGRID");

        section("Text");
        static CommittedField<double> ruler_field;
        draw_committed_double_field("##ruler_label_size", "Ruler font size (px)", "%.3g", settings.ruler_label_size_px, ruler_field,
                                    [&](double px)
                                    { provider.set_ruler_label_size(px); });
        // Labels scale with their shapes between the two (item 9).
        static CommittedField<double> label_min_field;
        draw_committed_double_field("##label_min_size", "Min label font size (px)", "%.3g", settings.label_min_size_px, label_min_field,
                                    [&](double px)
                                    { provider.set_label_min_size(px); });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Smallest size of pin, route and placement name labels - keeps them legible when zoomed out");
        static CommittedField<double> label_max_field;
        draw_committed_double_field("##label_max_size", "Max label font size (px)", "%.3g", settings.label_max_size_px, label_max_field,
                                    [&](double px)
                                    { provider.set_label_max_size(px); });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Largest size of pin, route and placement name labels - stops them growing without bound when zoomed in");

        section("Hierarchy");
        static CommittedField<int32_t> depth_field;
        draw_committed_int_field("##hierarchy_depth", "Hierarchy Depth", settings.hierarchy_depth, depth_field,
                                 [&](int32_t value)
                                 { provider.set_hierarchy_depth(value); });
        static CommittedField<int32_t> fanout_field;
        draw_committed_int_field("##flightline_max_fanout", "Flightline Max Fanout", settings.flightline_max_fanout, fanout_field,
                                 [&](int32_t value)
                                 { provider.set_flightline_max_fanout(value); });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Nets connecting more pins than this (besides the selected one) draw no flightlines - 0 for no limit");

        // NEW_FEATURES_SEPT_2026.md item 21 - set_max_concurrency.
        section("Performance");
        static CommittedField<int32_t> cpus_field;
        draw_committed_int_field("##max_concurrency", "CPUs", settings.max_concurrency, cpus_field,
                                 [&](int32_t value)
                                 { provider.set_max_concurrency(value); });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Most threads rendering may use at once - at least 2");

        section("Settings file");
        static FileRequest request;
        static std::string status;
        const bool busy = request.kind != FileRequest::Kind::NONE;
        const std::string default_path = le_default_settings_path();

        ImGui::BeginDisabled(busy || default_path.empty());
        if (ImGui::Button("Save"))
        {
            provider.save_settings("");
            status = "Saved to " + default_path;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Save to %s - loaded automatically when le_shell starts", default_path.empty() ? "(HOME isn't set)" : default_path.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Save As..."))
            start_file_request(request, true);
        ImGui::SameLine();
        if (ImGui::Button("Load..."))
            start_file_request(request, false);
        ImGui::EndDisabled();
        // NEW_FEATURES_SEPT_2026.md item 25.
        if (ImGui::Button("Reset window layout"))
            provider.request_window_layout_reset();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Put the panels back where they start - the layout (and window size) is saved automatically to window_layout.ini beside the settings file");
        // Settings from the Tcl console, the grid/snap toolbars and the
        // rest are saved too - this panel just shows the most common ones.
        ImGui::TextWrapped("Also saves the snap modes chosen in the toolbars. Details of a failed save or load are in the terminal.");
        if (!status.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", status.c_str());
            ImGui::PopStyleColor();
        }

        poll_file_request(provider, request, status);
    }
}
