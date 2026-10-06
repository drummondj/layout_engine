#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace le::gui
{
    /// @brief Asks for a file path to save to or open: the system dialog
    /// (portable-file-dialogs - zenity/kdialog on Linux), run asynchronously
    /// and polled each frame so the GUI keeps drawing, or, with no dialog
    /// helper installed, a small in-app path prompt.
    class FileDialog
    {
    public:
        enum class Mode
        {
            SAVE,
            OPEN,
        };

        FileDialog();
        ~FileDialog();
        FileDialog(const FileDialog &) = delete;
        FileDialog &operator=(const FileDialog &) = delete;

        /// @brief Starts asking. `filters` are pfd's (label, pattern) pairs.
        /// A SAVE in the system dialog asks before overwriting a file if
        /// `confirm_overwrite`.
        void start(Mode mode, std::string title, const std::string &default_path, std::vector<std::string> filters, bool confirm_overwrite = true);
        /// @brief Whether the path poll() last returned has had its
        /// overwrite confirmed already (by the system dialog).
        bool overwrite_confirmed() const { return overwrite_confirmed_; }
        /// @brief Whether a request is in progress.
        bool active() const;
        /// @brief Call once per frame: the chosen path, once, when there is
        /// one (empty optional while waiting, and after a cancel).
        std::optional<std::string> poll();

    private:
        struct State;
        std::unique_ptr<State> state_;
        bool overwrite_confirmed_ = false;
    };
}
