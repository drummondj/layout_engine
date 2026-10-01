#include "info_panel.hpp"

#include "gui_provider.hpp"
#include "imgui.h"

namespace le::gui
{
    void draw_info_panel(GuiProvider &provider)
    {
        const std::string &message = provider.state().status_bar.tooltip_message;
        if (!message.empty())
            ImGui::TextWrapped("%s", message.c_str());
    }
}
