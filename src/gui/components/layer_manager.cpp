#include "layer_manager.hpp"

#include "gui_provider.hpp"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace le::gui
{
    namespace
    {
        // le::ViewLayerPurpose's own declaration order (api.hpp's own
        // le_purpose_at doc comment has the authoritative list) - purely
        // a display label here; le_set_purpose_visible/_selectable below
        // take the raw ordinal directly, there's no name-string round
        // trip. Still a separate hand-synced copy from le_tcl_procs.tcl's
        // own ::purpose_names dict, though - a purpose appended to
        // ViewLayerPurpose (view_style.hpp) must be added to both, or it
        // silently falls off the end of this array and displays as "?"
        // (purpose_name's own out-of-range fallback below).
        constexpr const char *kPurposeNames[] = {
            "terminal",
            "obstruction",
            "boundary",
            "trackPreferred",
            "trackNonPreferred",
            "routingBlockage",
            "row",
            "gcellgrid",
            "placementBlockage",
            "route",
            "region",
            "placement",
            "customShape",
            "debug",
            "flightline",
            "portMarker",
        };
        constexpr int32_t kPurposeNameCount = static_cast<int32_t>(sizeof(kPurposeNames) / sizeof(kPurposeNames[0]));

        const char *purpose_name(int32_t ordinal)
        {
            if (ordinal < 0 || ordinal >= kPurposeNameCount)
            {
                return "?";
            }
            return kPurposeNames[ordinal];
        }

        const char *tcl_bool(bool value)
        {
            return value ? "1" : "0";
        }

        // Layer/purpose visibility+selectability and hierarchy depth are
        // exactly the actions the Flutter frontend's own LeProvider
        // routes through a Tcl command instead of a direct FFI call -
        // see GuiProvider::run_tcl_command's own doc comment
        // (gui_provider.hpp).

        // A checkbox bound to a value this GUI doesn't own the truth
        // for - `backend_value` is only current as of the *last* frame's
        // own provider.refresh(), and a click here enqueues a Tcl
        // command (see GuiProvider::run_tcl_command) that won't actually
        // land on the backend for up to ~100ms (le_shell.cpp's own
        // readline event-hook poll interval) rather than applying
        // immediately. Without this, the checkbox would visibly toggle
        // on click, then snap back to the old value for the next few
        // frames once this function re-reads `backend_value` and finds
        // it still unchanged, then snap forward again once the queued
        // command finally lands - a real, reported "changes back to the
        // old value, then back to the new one" flicker. Instead, once
        // clicked, the checkbox shows *only* the just-clicked value
        // (ignoring backend_value entirely) until backend_value actually
        // catches up to it - tracked via ImGui's own per-widget
        // GetStateStorage() (keyed off `str_id`, scoped within the
        // caller's own PushID), not a variable this function could own
        // itself, since a fresh local `static` would be shared across
        // every row calling this same function rather than being
        // distinct per row/column.
        template <typename OnToggle>
        void draw_optimistic_checkbox(const char *str_id, bool backend_value, OnToggle &&on_toggle)
        {
            ImGuiStorage *storage = ImGui::GetStateStorage();
            ImGui::PushID(str_id);
            const ImGuiID has_pending_id = ImGui::GetID("has_pending");
            const ImGuiID pending_value_id = ImGui::GetID("pending_value");
            bool has_pending = storage->GetBool(has_pending_id, false);
            if (has_pending && backend_value == storage->GetBool(pending_value_id, false))
            {
                has_pending = false;
                storage->SetBool(has_pending_id, false);
            }

            bool display_value = has_pending ? storage->GetBool(pending_value_id, false) : backend_value;
            if (ImGui::Checkbox("##checkbox", &display_value))
            {
                on_toggle(display_value);
                storage->SetBool(has_pending_id, true);
                storage->SetBool(pending_value_id, display_value);
            }
            ImGui::PopID();
        }

        // The swatch's color picker popup (NEW_FEATURES_SEPT_2026.md item
        // 17), opened by a click on it - call right after the swatch, in
        // its row's ID scope. The picked color is applied when a drag or
        // the hex field is finished (applying every frame of a drag would
        // queue a Tcl command per frame); "Default" drops it again.
        void draw_layer_color_picker(GuiProvider &provider, const LeLayerRow &row)
        {
            if (!ImGui::BeginPopup("##color_picker"))
                return;
            static float edit[3] = {0.0f, 0.0f, 0.0f};
            if (ImGui::IsWindowAppearing())
            {
                edit[0] = static_cast<float>(row.color_r) / 255.0f;
                edit[1] = static_cast<float>(row.color_g) / 255.0f;
                edit[2] = static_cast<float>(row.color_b) / 255.0f;
            }
            ImGui::TextUnformatted(row.name);
            ImGui::ColorPicker3("##picker", edit, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_DisplayHex);
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                const auto channel = [](float v)
                { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
                provider.set_layer_color(row.name, channel(edit[0]), channel(edit[1]), channel(edit[2]));
            }
            if (ImGui::Button("Default"))
            {
                provider.reset_layer_color(row.name);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Go back to the default palette color");
            ImGui::EndPopup();
        }

        // One "name | V | S" row - draws two checkboxes (each acting
        // immediately on click, calling `on_visible`/`on_selectable`
        // with the new value) after whatever `draw_name` puts in the
        // first column (plain text for a purpose/aggregate row, a color
        // swatch + text for a layer row). `id` must be unique per row.
        // `has_selectable` false leaves the S cell empty - a purpose
        // nothing on which can ever be selected (NEW_FEATURES_SEPT_2026.md
        // item 8).
        template <typename DrawName, typename OnVisible, typename OnSelectable>
        void draw_toggle_row(
            const char *id, DrawName &&draw_name, bool visible, bool selectable, OnVisible &&on_visible,
            OnSelectable &&on_selectable, bool has_selectable = true)
        {
            ImGui::PushID(id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            draw_name();
            ImGui::TableSetColumnIndex(1);
            draw_optimistic_checkbox("##visible", visible, on_visible);
            if (has_selectable)
            {
                ImGui::TableSetColumnIndex(2);
                draw_optimistic_checkbox("##selectable", selectable, on_selectable);
            }
            ImGui::PopID();
        }

        // A blank spacer row - stands in for layer_manager.dart's own
        // Divider() between the "All"/Purposes/Layers sections. A real
        // separator line drawn *inside* one continuous table (needed so
        // every row's checkboxes still line up in the same two columns)
        // would need its own manual draw-list line rather than a plain
        // ImGui::Separator() (which assumes it owns a full ordinary row,
        // not a table cell) - not worth the extra complexity for a
        // cosmetic divider in a prototype.
        void draw_spacer_row()
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Spacing();
        }
    }

    void draw_layer_manager(GuiProvider &provider)
    {
        const GuiProvider::State &state = provider.state();

        // Pseudo-rows with no physical Technology Layer of their own
        // (ROW/BOUNDARY/GCELLGRID/PLACEMENT_BLOCKAGE/REGION/DEBUG/
        // FLIGHTLINE/...) are already filtered out of
        // state().layer_manager.layers by GuiProvider::refresh()
        // (has_physical_layer, gui_provider_test.cpp) - each already has
        // its own single-purpose entry below, showing it again as if it
        // were a whole extra layer would be a redundant, confusing
        // duplicate (BUGS_AND_ENHANCEMENTS.md E12).
        const std::vector<GuiProvider::LayerRow> &layers = state.layer_manager.layers;
        bool all_layers_visible = true;
        bool all_layers_selectable = true;
        for (const GuiProvider::LayerRow &layer : layers)
        {
            all_layers_visible = all_layers_visible && layer.visible;
            all_layers_selectable = all_layers_selectable && layer.selectable;
        }

        const std::vector<GuiProvider::PurposeRow> &purposes = state.layer_manager.purposes;
        bool all_purposes_visible = true;
        bool all_purposes_selectable = true;
        for (const GuiProvider::PurposeRow &purpose : purposes)
        {
            all_purposes_visible = all_purposes_visible && purpose.visible;
            // A purpose without a selectable checkbox can't hold the
            // "All"/"Purposes" aggregate unchecked, and isn't toggled by it.
            if (purpose.has_selectable_objects)
                all_purposes_selectable = all_purposes_selectable && purpose.selectable;
        }

        if (!ImGui::BeginTable("layer_manager_table", 3, ImGuiTableFlags_SizingFixedFit))
        {
            return;
        }
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("V", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableSetupColumn("S", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableHeadersRow();

        // Every row/aggregate below is queued as *one* semicolon-joined
        // Tcl command when it covers more than one row (via
        // provider.run_tcl_command) - le_provider.dart's own
        // setAllLayersVisible/etc. batch the exact same way (one
        // command-history entry per user action, not one per row/dozens
        // for a big design's own "All" click - see their own comment).
        draw_toggle_row(
            "all", []
            { ImGui::TextUnformatted("All"); }, all_layers_visible && all_purposes_visible,
            all_layers_selectable && all_purposes_selectable,
            [&](bool value)
            {
                std::string script;
                for (const GuiProvider::LayerRow &layer : layers)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_layer_visible {") + layer.row.name + "} " + tcl_bool(value);
                }
                for (const GuiProvider::PurposeRow &purpose : purposes)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_purpose_visible ") + purpose_name(purpose.ordinal) + " " + tcl_bool(value);
                }
                if (!script.empty())
                    provider.run_tcl_command(script);
            },
            [&](bool value)
            {
                std::string script;
                for (const GuiProvider::LayerRow &layer : layers)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_layer_selectable {") + layer.row.name + "} " + tcl_bool(value);
                }
                for (const GuiProvider::PurposeRow &purpose : purposes)
                {
                    if (!purpose.has_selectable_objects)
                        continue;
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_purpose_selectable ") + purpose_name(purpose.ordinal) + " " + tcl_bool(value);
                }
                if (!script.empty())
                    provider.run_tcl_command(script);
            });

        draw_spacer_row();

        draw_toggle_row(
            "all_purposes", []
            { ImGui::TextUnformatted("Purposes"); }, all_purposes_visible, all_purposes_selectable,
            [&](bool value)
            {
                std::string script;
                for (const GuiProvider::PurposeRow &purpose : purposes)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_purpose_visible ") + purpose_name(purpose.ordinal) + " " + tcl_bool(value);
                }
                if (!script.empty())
                    provider.run_tcl_command(script);
            },
            [&](bool value)
            {
                std::string script;
                for (const GuiProvider::PurposeRow &purpose : purposes)
                {
                    if (!purpose.has_selectable_objects)
                        continue;
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_purpose_selectable ") + purpose_name(purpose.ordinal) + " " + tcl_bool(value);
                }
                if (!script.empty())
                    provider.run_tcl_command(script);
            });
        for (const GuiProvider::PurposeRow &purpose : purposes)
        {
            draw_toggle_row(
                purpose_name(purpose.ordinal), [&]
                { ImGui::TextUnformatted(purpose_name(purpose.ordinal)); },
                purpose.visible, purpose.selectable,
                [&](bool value)
                { provider.set_purpose_visible(purpose_name(purpose.ordinal), value); },
                [&](bool value)
                { provider.set_purpose_selectable(purpose_name(purpose.ordinal), value); },
                purpose.has_selectable_objects);
        }

        draw_spacer_row();

        draw_toggle_row(
            "all_layers", []
            { ImGui::TextUnformatted("Layers"); }, all_layers_visible, all_layers_selectable,
            [&](bool value)
            {
                std::string script;
                for (const GuiProvider::LayerRow &layer : layers)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_layer_visible {") + layer.row.name + "} " + tcl_bool(value);
                }
                if (!script.empty())
                    provider.run_tcl_command(script);
            },
            [&](bool value)
            {
                std::string script;
                for (const GuiProvider::LayerRow &layer : layers)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_layer_selectable {") + layer.row.name + "} " + tcl_bool(value);
                }
                if (!script.empty())
                    provider.run_tcl_command(script);
            });
        for (const GuiProvider::LayerRow &layer : layers)
        {
            draw_toggle_row(
                layer.row.name,
                [&]
                {
                    const ImVec4 color(
                        static_cast<float>(layer.row.color_r) / 255.0f, static_cast<float>(layer.row.color_g) / 255.0f,
                        static_cast<float>(layer.row.color_b) / 255.0f, 1.0f);
                    // NEW_FEATURES_SEPT_2026.md item 17 - clicking the
                    // swatch opens a color picker; the choice is saved
                    // with the settings file.
                    if (ImGui::ColorButton(
                            "##swatch", color,
                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_NoAlpha,
                            ImVec2(16.0f, 16.0f)))
                        ImGui::OpenPopup("##color_picker");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Click to change %s's color", layer.row.name);
                    draw_layer_color_picker(provider, layer.row);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(layer.row.name);
                },
                layer.visible, layer.selectable,
                [&](bool value)
                { provider.set_layer_visible(layer.row.name, value); },
                [&](bool value)
                { provider.set_layer_selectable(layer.row.name, value); });
        }

        ImGui::EndTable();
    }
}
