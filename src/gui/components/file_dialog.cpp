#include "file_dialog.hpp"

#include "imgui.h"
#include "portable-file-dialogs.h"

#include <array>
#include <cstdlib>

namespace le::gui
{
    struct FileDialog::State
    {
        bool active = false;
        Mode mode = Mode::SAVE;
        std::string title;
        std::unique_ptr<pfd::save_file> save_dialog;
        std::unique_ptr<pfd::open_file> open_dialog;
        bool system_confirms = false; // the system save dialog asks before overwriting
        bool prompt_open = false;
        std::array<char, 1024> prompt_path{};
    };

    FileDialog::FileDialog() : state_(std::make_unique<State>()) {}
    FileDialog::~FileDialog() = default;

    void FileDialog::start(Mode mode, std::string title, const std::string &default_path, std::vector<std::string> filters, bool confirm_overwrite)
    {
        *state_ = State{};
        state_->active = true;
        state_->mode = mode;
        state_->title = std::move(title);
        if (pfd::settings::available())
        {
            // zenity 4 draws with GTK 4, whose default GPU renderer leaves its
            // window blank under WSLg and on GPU-less servers; the software
            // renderer always works. A user's own GSK_RENDERER wins.
            setenv("GSK_RENDERER", "cairo", 0);
            if (mode == Mode::SAVE)
                state_->save_dialog = std::make_unique<pfd::save_file>(state_->title, default_path, filters,
                                                                       confirm_overwrite ? pfd::opt::none : pfd::opt::force_overwrite);
            else
                state_->open_dialog = std::make_unique<pfd::open_file>(state_->title, default_path, filters, pfd::opt::none);
            state_->system_confirms = mode == Mode::SAVE && confirm_overwrite;
            return;
        }
        default_path.copy(state_->prompt_path.data(), state_->prompt_path.size() - 1);
        state_->prompt_open = true;
    }

    bool FileDialog::active() const { return state_->active; }

    std::optional<std::string> FileDialog::poll()
    {
        State &s = *state_;
        if (!s.active)
            return std::nullopt;
        const auto finish = [&](const std::string &path) -> std::optional<std::string> {
            overwrite_confirmed_ = s.system_confirms && (s.save_dialog != nullptr);
            s = State{};
            return path.empty() ? std::nullopt : std::optional<std::string>(path);
        };
        if (s.save_dialog && s.save_dialog->ready(0))
            return finish(s.save_dialog->result());
        if (s.open_dialog && s.open_dialog->ready(0))
        {
            const std::vector<std::string> paths = s.open_dialog->result();
            return finish(paths.empty() ? std::string() : paths.front());
        }

        // The in-app prompt, unique per dialog object.
        ImGui::PushID(this);
        const std::string popup = s.title + "###file_dialog_prompt";
        if (s.prompt_open)
        {
            ImGui::OpenPopup(popup.c_str());
            s.prompt_open = false;
        }
        std::optional<std::string> chosen;
        if (ImGui::BeginPopupModal(popup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const bool save = s.mode == Mode::SAVE;
            ImGui::TextUnformatted(save ? "Save to:" : "Open:");
            ImGui::TextDisabled("(no system file dialog found - install zenity or kdialog for one)");
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            const bool entered = ImGui::InputText("##path", s.prompt_path.data(), s.prompt_path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
            bool done = false;
            if (ImGui::Button(save ? "Save" : "Open") || entered)
            {
                chosen = std::string(s.prompt_path.data());
                done = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
                done = true;
            if (done)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            if (done)
            {
                ImGui::PopID();
                return finish(chosen.value_or(""));
            }
        }
        ImGui::PopID();
        return std::nullopt;
    }
}
