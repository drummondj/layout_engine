#include "design_saver.hpp"

#include "gui_provider.hpp"
#include "imgui.h"

#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

namespace le::gui
{
    namespace
    {
        const std::vector<std::string> kDbFilters = {"Layout Engine databases (*.led)", "*.led", "All files", "*"};
        constexpr const char *kConfirmPopup = "Overwrite design file?###confirm_overwrite";
    }

    void DesignSaver::save(GuiProvider &provider)
    {
        const std::string path = provider.db_path();
        if (path.empty())
            save_as(provider);
        else
            write_or_confirm(provider, path, false);
    }

    void DesignSaver::save_as(GuiProvider &provider)
    {
        const std::string path = provider.db_path();
        dialog_.start(FileDialog::Mode::SAVE, "Save design as", path.empty() ? std::string("design.led") : path, kDbFilters,
                      provider.state().settings.confirm_overwrite);
    }

    void DesignSaver::write_or_confirm(GuiProvider &provider, const std::string &path, bool already_confirmed)
    {
        std::error_code ec;
        if (!already_confirmed && provider.state().settings.confirm_overwrite && std::filesystem::exists(path, ec))
        {
            pending_path_ = path;
            open_confirmation_ = true;
            return;
        }
        write(provider, path);
    }

    void DesignSaver::write(GuiProvider &provider, const std::string &path)
    {
        if (!write_now_)
        {
            provider.write_db(path);
            return;
        }
        status_ = provider.write_db_now(path) ? "Saved to " + path : "Couldn't save to " + path + " - see the terminal";
    }

    void DesignSaver::draw(GuiProvider &provider)
    {
        if (const std::optional<std::string> path = dialog_.poll())
            write_or_confirm(provider, *path, dialog_.overwrite_confirmed());

        ImGui::PushID(this);
        if (open_confirmation_)
        {
            ImGui::OpenPopup(kConfirmPopup);
            open_confirmation_ = false;
        }
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal(kConfirmPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("%s already exists.", pending_path_.c_str());
            ImGui::TextUnformatted("Replace it with the current design?");
            ImGui::TextDisabled("(Settings > Saving designs turns this question off)");
            ImGui::Spacing();
            if (ImGui::Button("Overwrite"))
            {
                const std::string path = pending_path_;
                pending_path_.clear();
                ImGui::CloseCurrentPopup();
                write(provider, path);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                pending_path_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}
