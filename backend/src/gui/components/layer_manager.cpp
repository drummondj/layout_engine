#include "layer_manager.hpp"

#include "compact_button.hpp"
#include "gui_provider.hpp"
#include "imgui.h"

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
        // (purpose_name's own out-of-range fallback below) - confirmed
        // the hard way: PLACEMENT_BOUNDARY was appended after this array
        // was last updated, and neither this file nor le_tcl_procs.tcl's
        // own dict ever picked it up until now.
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
            "placementName",
            "placementBoundary",
            "customShape",
            "debug",
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

        // One "name | V | S" row - draws two checkboxes (each acting
        // immediately on click, calling `on_visible`/`on_selectable`
        // with the new value) after whatever `draw_name` puts in the
        // first column (plain text for a purpose/aggregate row, a color
        // swatch + text for a layer row). `id` must be unique per row.
        template <typename DrawName, typename OnVisible, typename OnSelectable>
        void draw_toggle_row(
            const char *id, DrawName &&draw_name, bool visible, bool selectable, OnVisible &&on_visible,
            OnSelectable &&on_selectable)
        {
            ImGui::PushID(id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            draw_name();
            ImGui::TableSetColumnIndex(1);
            draw_optimistic_checkbox("##visible", visible, on_visible);
            ImGui::TableSetColumnIndex(2);
            draw_optimistic_checkbox("##selectable", selectable, on_selectable);
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

        // Hierarchy depth - a plain "commit on Enter" field
        // (ImGuiInputTextFlags_EnterReturnsTrue), matching
        // HierarchyRow's own TextField(onSubmitted:) in layer_manager.dart
        // rather than live-updating per keystroke. Submitting a valid
        // (non-negative) value shows *only* that value - ignoring
        // state().layer_manager.hierarchy_depth's own still-stale value -
        // until the backend actually catches up to it (same "optimistic
        // until confirmed" reasoning as draw_optimistic_checkbox above,
        // and the same flicker it avoids: pressing Enter defocuses the
        // field immediately, so without this, the very next frame's own
        // re-sync would snap the field back to the old value for the
        // ~100ms the queued command takes to land, then snap forward
        // again once it does - a real, reported bug). Still re-synced
        // whenever nothing is pending and the field isn't focused, so an
        // external change (e.g. from the Tcl console, running
        // concurrently with this GUI) shows up here too.
        static int depth_buf = 0;
        static bool depth_field_was_active = false;
        static bool has_pending_depth = false;
        static int pending_depth_value = 0;
        const int32_t backend_depth = state.layer_manager.hierarchy_depth;
        if (has_pending_depth && backend_depth == pending_depth_value)
        {
            has_pending_depth = false;
        }
        if (!depth_field_was_active && !has_pending_depth)
        {
            depth_buf = backend_depth;
        }
        // Submits `new_value` the same way pressing Enter in the field
        // itself does (below) - shared by the "-"/"+" buttons so they
        // stay in lockstep with the field's own optimistic-update/
        // negative-value handling instead of duplicating it. Clamped to
        // 0 rather than left as a no-op for a button click specifically
        // (unlike a typed negative value, which resets to whatever the
        // backend still reports - see the field's own submit branch
        // below) - a "-" press at 0 should visibly settle at 0, not
        // silently do nothing.
        auto submit_depth = [&](int new_value)
        {
            if (new_value < 0)
                new_value = 0;
            depth_buf = new_value;
            provider.set_hierarchy_depth(new_value);
            has_pending_depth = true;
            pending_depth_value = new_value;
        };

        // 3 characters wide (this field only ever holds a small
        // hierarchy-depth integer, never worth 100px) - the label moved
        // out to its own TextUnformatted below so the "-"/"+" buttons
        // can sit between the field and it.
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("000").x + ImGui::GetStyle().FramePadding.x * 2.0f);
        if (ImGui::InputInt("##hierarchy_depth", &depth_buf, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (depth_buf >= 0)
            {
                submit_depth(depth_buf);
            }
            else
            {
                // set_hierarchy_depth itself rejects a negative value
                // (left unchanged) - reset the field back to the real
                // current value immediately, matching HierarchyRow's own
                // _submit() in layer_manager.dart, rather than showing a
                // value that was never (and never will be) applied.
                depth_buf = backend_depth;
                has_pending_depth = false;
            }
        }
        depth_field_was_active = ImGui::IsItemActive();

        constexpr float kStepButtonWidth = 24.0f;
        // compact_button (compact_button.hpp), not a plain ImGui::Button -
        // see its own doc comment for the centering bug a button this
        // narrow would otherwise hit.
        ImGui::SameLine();
        if (compact_button("-", kStepButtonWidth))
            submit_depth(depth_buf - 1);
        ImGui::SameLine();
        if (compact_button("+", kStepButtonWidth))
            submit_depth(depth_buf + 1);
        ImGui::SameLine();
        ImGui::TextUnformatted("Hierarchy Depth");

        ImGui::Separator();

        // Pseudo-rows with no physical Technology Layer of their own
        // (ROW/BOUNDARY/GCELLGRID/PLACEMENT_BLOCKAGE/REGION) are already
        // filtered out of state().layer_manager.layers by
        // GuiProvider::refresh() (has_physical_layer) - each already has
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
                { provider.set_purpose_selectable(purpose_name(purpose.ordinal), value); });
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
                    ImGui::ColorButton(
                        "##swatch", color,
                        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_NoAlpha,
                        ImVec2(16.0f, 16.0f));
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
