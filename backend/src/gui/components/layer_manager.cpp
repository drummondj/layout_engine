#include "layer_manager.hpp"

#include "api.hpp"
#include "imgui.h"

#include <cstdint>
#include <string>
#include <vector>

namespace le::gui
{
    namespace
    {
        struct LayerEntry
        {
            LeLayerRow row;
            bool visible;
            bool selectable;
        };

        struct PurposeEntry
        {
            int32_t ordinal;
            bool visible;
            bool selectable;
        };

        // le::ViewLayerPurpose's own declaration order (api.hpp's own
        // le_purpose_at doc comment has the authoritative list) - purely
        // a display label here, unlike layout_engine_plugin.dart's own
        // hand-synced LeLayerPurpose enum, which also has to carry this
        // spelling through to a Tcl set_purpose_visible/_selectable
        // command string; le_set_purpose_visible/_selectable below take
        // the raw ordinal directly; there's no name-string round trip to
        // keep in sync with le_tcl_procs.tcl's own ::purpose_names dict
        // here at all.
        constexpr const char *kPurposeNames[] = {
            "terminal", "obstruction", "boundary", "trackPreferred",
            "trackNonPreferred", "routingBlockage", "row", "gcellgrid",
            "placementBlockage", "route", "region", "placementName",
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
        // routes through a Tcl command instead of a direct FFI call (see
        // le_provider.dart's own runTclCommand call sites) - purely so
        // the action leaves a command-history entry a user can scroll
        // back through (`history`/`command_history` in this shell),
        // matching what a typed `set_layer_visible ...` line would.
        // This GUI has no Tcl interpreter of its own to evaluate one
        // directly (src/gui/'s own no-Tcl-dependency design - see
        // le_gui.hpp), so it queues the command text for le_shell.cpp's
        // own console thread to evaluate shortly after instead (see
        // le_enqueue_tcl_command's own doc comment, api.hpp) - real
        // per-frame interaction (mouse/keyboard) stays a direct le_*
        // call, unaffected.
        void enqueue(LeHandle *handle, const std::string &command)
        {
            le_enqueue_tcl_command(handle, command.c_str());
        }

        // A checkbox bound to a value this GUI doesn't own the truth
        // for - `backend_value` is only current as of the *last* frame's
        // own le_is_layer_name_visible()/etc. read, and a click here
        // enqueues a Tcl command (see enqueue() above) that won't
        // actually land on the backend for up to ~100ms (le_shell.cpp's
        // own readline event-hook poll interval) rather than applying
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

    void draw_layer_manager(LeHandle *handle)
    {
        // Hierarchy depth - a plain "commit on Enter" field
        // (ImGuiInputTextFlags_EnterReturnsTrue), matching
        // HierarchyRow's own TextField(onSubmitted:) in layer_manager.dart
        // rather than live-updating per keystroke. Submitting a valid
        // (non-negative) value shows *only* that value - ignoring
        // le_hierarchy_depth()'s own still-stale read - until the
        // backend actually catches up to it (same "optimistic until
        // confirmed" reasoning as draw_optimistic_checkbox above, and
        // the same flicker it avoids: pressing Enter defocuses the
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
        const int32_t backend_depth = le_hierarchy_depth(handle);
        if (has_pending_depth && backend_depth == pending_depth_value)
        {
            has_pending_depth = false;
        }
        if (!depth_field_was_active && !has_pending_depth)
        {
            depth_buf = backend_depth;
        }
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::InputInt("Hierarchy Depth", &depth_buf, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (depth_buf >= 0)
            {
                enqueue(handle, "set_hierarchy_depth " + std::to_string(depth_buf));
                has_pending_depth = true;
                pending_depth_value = depth_buf;
            }
            else
            {
                // le_set_hierarchy_depth itself rejects a negative value
                // (left unchanged) - reset the field back to the real
                // current value immediately, matching HierarchyRow's own
                // _submit() in layer_manager.dart, rather than showing a
                // value that was never (and never will be) applied.
                depth_buf = backend_depth;
                has_pending_depth = false;
            }
        }
        depth_field_was_active = ImGui::IsItemActive();

        ImGui::Separator();

        // Pseudo-rows with no physical Technology Layer of their own
        // (ROW/BOUNDARY/GCELLGRID/PLACEMENT_BLOCKAGE/REGION) are skipped
        // here - each already has its own single-purpose entry below,
        // showing it again as if it were a whole extra layer would be a
        // redundant, confusing duplicate (BUGS_AND_ENHANCEMENTS.md E12) -
        // matching refreshLayers' own has_physical_layer filter.
        std::vector<LayerEntry> layers;
        bool all_layers_visible = true;
        bool all_layers_selectable = true;
        const int32_t layer_count = le_layer_count(handle);
        for (int32_t i = 0; i < layer_count; ++i)
        {
            const LeLayerRow row = le_layer_at(handle, i);
            if (row.name == nullptr || !row.has_physical_layer)
            {
                continue;
            }
            const bool visible = le_is_layer_name_visible(handle, row.name);
            const bool selectable = le_is_layer_name_selectable(handle, row.name) != 0;
            layers.push_back(LayerEntry{row, visible, selectable});
            all_layers_visible = all_layers_visible && visible;
            all_layers_selectable = all_layers_selectable && selectable;
        }

        std::vector<PurposeEntry> purposes;
        bool all_purposes_visible = true;
        bool all_purposes_selectable = true;
        const int32_t purpose_count = le_purpose_count(handle);
        for (int32_t i = 0; i < purpose_count; ++i)
        {
            const int32_t ordinal = le_purpose_at(handle, i);
            if (ordinal < 0)
            {
                continue;
            }
            const bool visible = le_is_purpose_visible(handle, ordinal) != 0;
            const bool selectable = le_is_purpose_selectable(handle, ordinal) != 0;
            purposes.push_back(PurposeEntry{ordinal, visible, selectable});
            all_purposes_visible = all_purposes_visible && visible;
            all_purposes_selectable = all_purposes_selectable && selectable;
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
        // Tcl command when it covers more than one row - le_provider.dart's
        // own setAllLayersVisible/etc. batch the exact same way (one
        // command-history entry per user action, not one per row/dozens
        // for a big design's own "All" click - see their own comment).
        draw_toggle_row(
            "all", [] { ImGui::TextUnformatted("All"); }, all_layers_visible && all_purposes_visible,
            all_layers_selectable && all_purposes_selectable,
            [&](bool value)
            {
                std::string script;
                for (const LayerEntry &layer : layers)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_layer_visible {") + layer.row.name + "} " + tcl_bool(value);
                }
                for (const PurposeEntry &purpose : purposes)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_purpose_visible ") + purpose_name(purpose.ordinal) + " " + tcl_bool(value);
                }
                if (!script.empty())
                    enqueue(handle, script);
            },
            [&](bool value)
            {
                std::string script;
                for (const LayerEntry &layer : layers)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_layer_selectable {") + layer.row.name + "} " + tcl_bool(value);
                }
                for (const PurposeEntry &purpose : purposes)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_purpose_selectable ") + purpose_name(purpose.ordinal) + " " + tcl_bool(value);
                }
                if (!script.empty())
                    enqueue(handle, script);
            });

        draw_spacer_row();

        draw_toggle_row(
            "all_purposes", [] { ImGui::TextUnformatted("Purposes"); }, all_purposes_visible, all_purposes_selectable,
            [&](bool value)
            {
                std::string script;
                for (const PurposeEntry &purpose : purposes)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_purpose_visible ") + purpose_name(purpose.ordinal) + " " + tcl_bool(value);
                }
                if (!script.empty())
                    enqueue(handle, script);
            },
            [&](bool value)
            {
                std::string script;
                for (const PurposeEntry &purpose : purposes)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_purpose_selectable ") + purpose_name(purpose.ordinal) + " " + tcl_bool(value);
                }
                if (!script.empty())
                    enqueue(handle, script);
            });
        for (const PurposeEntry &purpose : purposes)
        {
            draw_toggle_row(
                purpose_name(purpose.ordinal), [&] { ImGui::TextUnformatted(purpose_name(purpose.ordinal)); },
                purpose.visible, purpose.selectable,
                [&](bool value)
                {
                    enqueue(handle, std::string("set_purpose_visible ") + purpose_name(purpose.ordinal) + " " + tcl_bool(value));
                },
                [&](bool value)
                {
                    enqueue(
                        handle, std::string("set_purpose_selectable ") + purpose_name(purpose.ordinal) + " " + tcl_bool(value));
                });
        }

        draw_spacer_row();

        draw_toggle_row(
            "all_layers", [] { ImGui::TextUnformatted("Layers"); }, all_layers_visible, all_layers_selectable,
            [&](bool value)
            {
                std::string script;
                for (const LayerEntry &layer : layers)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_layer_visible {") + layer.row.name + "} " + tcl_bool(value);
                }
                if (!script.empty())
                    enqueue(handle, script);
            },
            [&](bool value)
            {
                std::string script;
                for (const LayerEntry &layer : layers)
                {
                    if (!script.empty())
                        script += "; ";
                    script += std::string("set_layer_selectable {") + layer.row.name + "} " + tcl_bool(value);
                }
                if (!script.empty())
                    enqueue(handle, script);
            });
        for (const LayerEntry &layer : layers)
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
                { enqueue(handle, std::string("set_layer_visible {") + layer.row.name + "} " + tcl_bool(value)); },
                [&](bool value)
                { enqueue(handle, std::string("set_layer_selectable {") + layer.row.name + "} " + tcl_bool(value)); });
        }

        ImGui::EndTable();
    }
}
