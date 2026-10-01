#include "mode_selector.hpp"

#include "IconsLucide.h"
#include "compact_button.hpp"
#include "gui_provider.hpp"
#include "imgui.h"

#include <cstdint>
#include <string>

namespace le::gui
{
    namespace
    {
        const char *mode_keyword(int32_t mode)
        {
            switch (mode)
            {
            case LE_MODE_SELECT:
                return "select";
            case LE_MODE_EDIT:
                return "edit";
            case LE_MODE_RULER:
                return "ruler";
            default:
                return "select";
            }
        }

        const char *mode_label(int32_t mode)
        {
            switch (mode)
            {
            case LE_MODE_SELECT:
                return "Select";
            case LE_MODE_EDIT:
                return "Edit";
            case LE_MODE_RULER:
                return "Ruler";
            default:
                return "?";
            }
        }

        // Lucide equivalents of mode_selector.dart's own HugeIcons -
        // ICON_LC_MOUSE_POINTER_SQUARE_DASHED (a marquee-selection
        // cursor) for strokeRoundedCursorRectangleSelection01,
        // ICON_LC_PENCIL for strokeRoundedCursorEdit01, ICON_LC_RULER
        // for strokeRoundedRuler - Lucide has no exact 1:1 match for any
        // of these (a different icon set entirely), picked for closest
        // visual/semantic fit.
        const char *mode_icon(int32_t mode)
        {
            switch (mode)
            {
            case LE_MODE_SELECT:
                return ICON_LC_MOUSE_POINTER_SQUARE_DASHED;
            case LE_MODE_EDIT:
                return ICON_LC_PENCIL;
            case LE_MODE_RULER:
                return ICON_LC_RULER;
            default:
                return "";
            }
        }

        const char *mode_shortcut(int32_t mode)
        {
            switch (mode)
            {
            case LE_MODE_SELECT:
                return "s";
            case LE_MODE_EDIT:
                return "e";
            case LE_MODE_RULER:
                return "r";
            default:
                return "";
            }
        }

        // Icon-only now (label moved to a hover tooltip) - fits within
        // le_gui.cpp's own 64px-wide mode_selector_column child (48 +
        // its 8px WindowPadding on each side).
        constexpr float kIconButtonSize = 48.0f;

        // Same "optimistic until confirmed" reasoning as
        // layer_manager.cpp's own draw_optimistic_checkbox/hierarchy
        // depth field - set_mode is enqueued (evaluated on le_shell's
        // own console thread up to ~100ms later), so re-reading
        // le_get_mode() on the very next frame would otherwise flicker
        // the highlighted button back to the old mode until that lands.
        void draw_mode_button(GuiProvider &provider, int32_t mode, int32_t display_mode, bool &has_pending, int32_t &pending)
        {
            const bool selected = display_mode == mode;
            // Selected keeps a permanent highlighted background (a
            // neutral dark gray - compact_button.hpp's own
            // kSelectedIconButtonColor, not the theme's own blue
            // ButtonActive); unselected now draws no resting background
            // at all - only the icon glyph - so ButtonHovered/ButtonActive
            // stay themed for hover/press feedback but the idle state is
            // fully transparent.
            ImGui::PushStyleColor(ImGuiCol_Button,
                                   selected ? kSelectedIconButtonColor
                                            : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            // icon_button (compact_button.hpp) - not a plain
            // ImGui::Button(icon, ImVec2(size,size)) - see its own doc
            // comment for why: this button's small, explicit fixed size
            // would otherwise trip a real ImGui centering bug and render
            // the icon visibly right-of-center.
            const bool clicked = icon_button(mode_icon(mode), std::string("mode_") + mode_keyword(mode), kIconButtonSize);
            // Clicking the already-active mode is a no-op, matching
            // ModeButton's own `onPressed: selected ? null : onPressed`
            // in mode_selector.dart - guarded here with a plain `!selected`
            // check rather than wrapping the button in BeginDisabled(selected)
            // (the more obvious-looking way to express "already active,
            // ignore clicks"), because BeginDisabled also multiplies
            // everything drawn inside it by style.Alpha * DisabledAlpha
            // (60% by default) - it faded the icon glyph itself along
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
                ImGui::SetTooltip("%s (%s)", mode_label(mode), mode_shortcut(mode));
            }
        }
    }

    void draw_mode_selector(GuiProvider &provider)
    {
        static bool has_pending_mode = false;
        static int32_t pending_mode = LE_MODE_SELECT;

        const int32_t backend_mode = provider.state().mode;
        if (has_pending_mode && backend_mode == pending_mode)
        {
            has_pending_mode = false;
        }
        const int32_t display_mode = has_pending_mode ? pending_mode : backend_mode;

        draw_mode_button(provider, LE_MODE_SELECT, display_mode, has_pending_mode, pending_mode);
        ImGui::Spacing();
        draw_mode_button(provider, LE_MODE_EDIT, display_mode, has_pending_mode, pending_mode);
        ImGui::Spacing();
        draw_mode_button(provider, LE_MODE_RULER, display_mode, has_pending_mode, pending_mode);
    }
}
