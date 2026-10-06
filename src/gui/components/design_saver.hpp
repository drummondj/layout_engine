#pragma once

#include "file_dialog.hpp"

#include <string>

namespace le::gui
{
    class GuiProvider;

    /// @brief Saving the design from the GUI. Save writes to the file the
    /// design was last read from or written to (or acts as Save As if
    /// there's none); Save As asks for a path. Either asks before
    /// overwriting an existing file unless the confirm_overwrite setting is
    /// off. The File menu's saver queues write_db (so it's in the console
    /// history); the exit dialog's writes at once (`write_now`), since the
    /// dialog needs the result while it's open.
    class DesignSaver
    {
    public:
        explicit DesignSaver(bool write_now) : write_now_(write_now) {}

        void save(GuiProvider &provider);
        void save_as(GuiProvider &provider);
        /// @brief A file dialog or overwrite confirmation is open.
        bool busy() const { return dialog_.active() || !pending_path_.empty(); }
        /// @brief Call every frame - inside the popup that started a save,
        /// if any, so its dialogs open on top of it.
        void draw(GuiProvider &provider);
        /// @brief What the last immediate write did ("" if none yet).
        const std::string &status() const { return status_; }

    private:
        void write_or_confirm(GuiProvider &provider, const std::string &path, bool already_confirmed);
        void write(GuiProvider &provider, const std::string &path);

        bool write_now_;
        FileDialog dialog_;
        std::string pending_path_; // waiting for the overwrite confirmation
        bool open_confirmation_ = false;
        std::string status_;
    };
}
