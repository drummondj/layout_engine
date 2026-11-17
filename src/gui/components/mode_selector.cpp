#include "mode_selector.hpp"

#include "compact_button.hpp"
#include "gui_provider.hpp"
#include "imgui.h"

#include <cstdint>
#include <string>

namespace le::gui
{
    namespace
    {
        const char *mode_keyword(InteractionMode mode)
        {
            switch (mode)
            {
            case InteractionMode::SELECT:
                return "select";
            case InteractionMode::EDIT:
                return "edit";
            case InteractionMode::RULER:
                return "ruler";
            default:
                return "select";
            }
        }

        const char *mode_label(InteractionMode mode)
        {
            switch (mode)
            {
            case InteractionMode::SELECT:
                return "Select";
            case InteractionMode::EDIT:
                return "Edit";
            case InteractionMode::RULER:
                return "Ruler";
            default:
                return "?";
            }
        }

        Icon mode_icon(InteractionMode mode)
        {
            switch (mode)
            {
            case InteractionMode::SELECT:
                return Icon::Select;
            case InteractionMode::EDIT:
                return Icon::Edit;
            case InteractionMode::RULER:
                return Icon::Ruler;
            default:
                return Icon::Select;
            }
        }

        const char *mode_shortcut(InteractionMode mode)
        {
            switch (mode)
            {
            case InteractionMode::SELECT:
                return "s";
            case InteractionMode::EDIT:
                return "e";
            case InteractionMode::RULER:
                return "r";
            default:
                return "";
            }
        }

        // Fills le_gui.cpp's 64px-wide mode_selector_column child (48 +
        // its 8px WindowPadding on each side); every mode label fits.
        constexpr float kButtonWidth = 48.0f;

        // Same "optimistic until confirmed" reasoning as
        // layer_manager.cpp's own draw_optimistic_checkbox/hierarchy
        // depth field - set_mode is enqueued (evaluated on le_shell's
        // own console thread up to ~100ms later), so re-reading
        // the mode on the very next frame would otherwise flicker
        // the highlighted button back to the old mode until that lands.
        void draw_mode_button(GuiProvider &provider, InteractionMode mode, InteractionMode display_mode, bool &has_pending, InteractionMode &pending)
        {
            const bool selected = display_mode == mode;
            // Selected keeps a permanent highlighted background (a
            // neutral dark gray - compact_button.hpp's own
            // kSelectedIconButtonColor, not the theme's own
            // ButtonActive); unselected now draws no resting background
            // at all - only the icon - so ButtonHovered/ButtonActive
            // stay themed for hover/press feedback but the idle state is
            // fully transparent.
            ImGui::PushStyleColor(ImGuiCol_Button,
                                   selected ? kSelectedIconButtonColor
                                            : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            const bool clicked = labeled_icon_button(mode_icon(mode), mode_label(mode), std::string("mode_") + mode_keyword(mode), kButtonWidth);
            // Clicking the already-active mode is a no-op - guarded with
            // a plain `!selected`
            // check rather than wrapping the button in BeginDisabled(selected)
            // (the more obvious-looking way to express "already active,
            // ignore clicks"), because BeginDisabled also multiplies
            // everything drawn inside it by style.Alpha * DisabledAlpha
            // (60% by default) - it faded the icon itself along
            // with disabling the click, a real reported bug (measured:
            // (156,160,163) selected vs (230,237,242) unselected, not
            // just a duller background).
            if (clicked && !selected)
            {
                provider.set_mode(mode);
                has_pending = true;
                pending = mode;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", mode_shortcut(mode));
            }
        }
    }

    void draw_mode_selector(GuiProvider &provider)
    {
        static bool has_pending_mode = false;
        static InteractionMode pending_mode = InteractionMode::SELECT;

        const InteractionMode backend_mode = provider.state().mode;
        if (has_pending_mode && backend_mode == pending_mode)
        {
            has_pending_mode = false;
        }
        const InteractionMode display_mode = has_pending_mode ? pending_mode : backend_mode;

        draw_mode_button(provider, InteractionMode::SELECT, display_mode, has_pending_mode, pending_mode);
        ImGui::Spacing();
        draw_mode_button(provider, InteractionMode::EDIT, display_mode, has_pending_mode, pending_mode);
        ImGui::Spacing();
        draw_mode_button(provider, InteractionMode::RULER, display_mode, has_pending_mode, pending_mode);
    }
}
