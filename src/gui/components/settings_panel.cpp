#include "settings_panel.hpp"

#include "committed_field.hpp"
#include "file_dialog.hpp"
#include "gui_provider.hpp"
#include "imgui.h"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace le::gui
{
    namespace
    {
        const std::vector<std::string> kJsonFilters = {"JSON files (*.json)", "*.json", "All files", "*"};

        void section(const char *title)
        {
            ImGui::Spacing();
            ImGui::SeparatorText(title);
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
        // Labels scale with their shapes between the two.
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

        // set_max_concurrency.
        section("Performance");
        static CommittedField<int32_t> cpus_field;
        draw_committed_int_field("##max_concurrency", "CPUs", settings.max_concurrency, cpus_field,
                                 [&](int32_t value)
                                 { provider.set_max_concurrency(value); });
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Most threads rendering may use at once - at least 2");

        section("Settings file");
        static FileDialog dialog;
        static bool dialog_saves = false;
        static std::string status;
        const bool busy = dialog.active();
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
        {
            dialog.start(FileDialog::Mode::SAVE, "Save settings", default_path, kJsonFilters);
            dialog_saves = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Load..."))
        {
            dialog.start(FileDialog::Mode::OPEN, "Load settings", default_path, kJsonFilters);
            dialog_saves = false;
        }
        ImGui::EndDisabled();
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

        if (const std::optional<std::string> path = dialog.poll())
        {
            if (dialog_saves)
            {
                provider.save_settings(*path);
                status = "Saved to " + *path;
            }
            else
            {
                provider.load_settings(*path);
                status = "Loaded " + *path;
            }
        }
    }
}
