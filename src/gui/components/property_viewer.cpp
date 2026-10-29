#include "property_viewer.hpp"

#include "gui_provider.hpp"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace le::gui
{
    namespace
    {
        bool refs_equal(const LeObjectRef &a, const LeObjectRef &b)
        {
            return a.kind == b.kind && a.index == b.index && a.generation == b.generation;
        }

        bool ref_is_valid(const LeObjectRef &ref)
        {
            return ref.index != UINT32_MAX;
        }

        std::string name_of(const GuiProvider &provider, const LeObjectRef &ref)
        {
            const int32_t count = provider.object_property_count(ref);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeProperty property = provider.object_property_at(ref, i);
                if (property.name != nullptr && std::strcmp(property.name, "name") == 0)
                {
                    return property.string_value != nullptr ? property.string_value : "";
                }
            }
            return "";
        }

        std::string packed_token(const char *prefix, const LeObjectRef &ref)
        {
            const uint64_t packed = (static_cast<uint64_t>(ref.generation) << 32) | ref.index;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(packed));
            return std::string(prefix) + ":" + buf;
        }

        // [ref]'s TCL-style friendly-id token - name-keyed for the classes
        // that have a name field, numeric-packed (generation<<32 | index)
        // for the ones that don't. Purely a display string - never
        // round-tripped through TCL, since LeObjectRef already carries the
        // same index/generation shape.
        std::string token_for(const GuiProvider &provider, const LeObjectRef &ref)
        {
            const char *kind_name = GuiProvider::object_kind_name(ref.kind);
            if (!ref_is_valid(ref) || !kind_name)
            {
                return "?";
            }
            if (GuiProvider::object_kind_is_named(ref.kind))
            {
                return std::string(kind_name) + ":" + name_of(provider, ref);
            }
            return packed_token(kind_name, ref);
        }

        const char *child_label(int32_t kind)
        {
            switch (kind)
            {
            case LE_OBJECT_KIND_TERMINAL_PORT:
                return "ports";
            case LE_OBJECT_KIND_LIBRARY:
                return "libraries";
            case LE_OBJECT_KIND_DESIGN:
                return "designs";
            case LE_OBJECT_KIND_ABSTRACT:
                return "abstracts";
            case LE_OBJECT_KIND_TERMINAL:
                return "terminals";
            case LE_OBJECT_KIND_OBSTRUCTION:
                return "obstructions";
            case LE_OBJECT_KIND_SHAPE:
            case LE_OBJECT_KIND_BLOCKAGE:
            case LE_OBJECT_KIND_ROUTE:
            case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
                return "shapes";
            case LE_OBJECT_KIND_PHYSICAL_PORT:
                return "segments";
            case LE_OBJECT_KIND_ROW:
                return "rows";
            case LE_OBJECT_KIND_PLACEMENT:
                return "placements";
            case LE_OBJECT_KIND_REGION:
                return "regions";
            case LE_OBJECT_KIND_LAYOUT:
                return "layouts";
            default:
                return "?";
            }
        }

        std::string format_value(const LeProperty &property)
        {
            switch (property.type)
            {
            case LE_PROPERTY_TYPE_DOUBLE:
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.3f", property.double_value);
                return buf;
            }
            case LE_PROPERTY_TYPE_STRING:
                return property.string_value != nullptr ? property.string_value : "";
            case LE_PROPERTY_TYPE_INT:
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(property.int_value));
                return buf;
            }
            default:
                return "";
            }
        }

        // A read-only, click-and-drag-selectable, Ctrl+C-copyable text
        // field - ImGui's own idiom for this (there's no plain-text
        // "selectable label" widget), for each property name/value cell. `id`
        // must be unique per call (e.g. "##prop_name_3") - ImGui widgets
        // are identified by id, not position. A `std::vector<char>`
        // sized to `text`'s own length (not a fixed-size stack buffer)
        // since a property value has no fixed bound worth guessing at.
        void draw_selectable_text(const char *id, const std::string &text)
        {
            std::vector<char> buf(text.begin(), text.end());
            buf.push_back('\0');
            ImGui::SetNextItemWidth(-1.0f);
            // Transparent frame background - an ordinary input field's
            // own filled/bordered look (blue by default) reads as an
            // editable field sitting inside a property table, not
            // selectable text; the table's own row striping shows
            // through instead.
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::InputText(id, buf.data(), buf.size(), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor(3);
        }

        // Rebuilds `hierarchy` as `ref`'s own ancestor chain (root first)
        // and makes `ref` the currently-displayed node - called both when
        // the outer selection/pager changes
        // and from a "children" row's own click (re-anchoring onto that
        // child without touching canvas selection at all).
        void jump_to(const GuiProvider &provider, const LeObjectRef &ref, std::vector<LeObjectRef> &hierarchy, LeObjectRef &current_ref)
        {
            std::vector<LeObjectRef> chain;
            chain.push_back(ref);
            LeObjectRef parent = provider.object_parent(ref);
            while (ref_is_valid(parent))
            {
                chain.push_back(parent);
                parent = provider.object_parent(parent);
            }
            hierarchy.assign(chain.rbegin(), chain.rend());
            current_ref = ref;
        }

        // The ancestor-chain box above the properties table - always
        // fully expanded (a single chain, never branching, so nothing to
        // collapse), root at the top. Clicking a row only changes which
        // node ObjectDetail shows properties for, never the chain itself.
        void draw_hierarchy_tree(const GuiProvider &provider, const std::vector<LeObjectRef> &hierarchy, LeObjectRef &current_ref)
        {
            const float row_height = ImGui::GetTextLineHeightWithSpacing();
            const float child_height = row_height * static_cast<float>(hierarchy.size()) + ImGui::GetStyle().WindowPadding.y * 2.0f;
            ImGui::BeginChild("property_viewer_hierarchy", ImVec2(0.0f, child_height), ImGuiChildFlags_Borders);
            for (size_t depth = 0; depth < hierarchy.size(); ++depth)
            {
                const LeObjectRef &ref = hierarchy[depth];
                std::string label(depth * 2, ' ');
                if (depth > 0)
                {
                    label += "> ";
                }
                label += token_for(provider, ref);
                ImGui::PushID(static_cast<int>(depth));
                if (ImGui::Selectable(label.c_str(), refs_equal(ref, current_ref)))
                {
                    current_ref = ref;
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }

        // `current_ref`'s own property table, plus one extra row per
        // child kind at the bottom - clicking a child token re-anchors the
        // hierarchy tree above onto it via jump_to.
        void draw_object_detail(
            GuiProvider &provider, LeObjectRef &current_ref, const char *filter, bool show_hidden,
            std::vector<LeObjectRef> &hierarchy)
        {
            if (!ref_is_valid(current_ref))
            {
                return;
            }

            std::string normalized_filter = filter;
            std::transform(normalized_filter.begin(), normalized_filter.end(), normalized_filter.begin(),
                            [](unsigned char c)
                            { return static_cast<char>(std::tolower(c)); });

            if (!ImGui::BeginTable(
                    "property_viewer_table", 2,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable))
            {
                return;
            }
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            const int32_t property_count = provider.object_property_count(current_ref);
            for (int32_t i = 0; i < property_count; ++i)
            {
                const LeProperty property = provider.object_property_at(current_ref, i);
                if (property.name == nullptr)
                {
                    continue;
                }
                const std::string value = format_value(property);
                if (!show_hidden && value.empty())
                {
                    continue;
                }
                if (!normalized_filter.empty())
                {
                    std::string name_lower = property.name;
                    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                                    [](unsigned char c)
                                    { return static_cast<char>(std::tolower(c)); });
                    if (name_lower.find(normalized_filter) == std::string::npos)
                    {
                        continue;
                    }
                }
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableSetColumnIndex(0);
                draw_selectable_text("##prop_name", property.name);
                ImGui::TableSetColumnIndex(1);
                draw_selectable_text("##prop_value", value);
                ImGui::PopID();
            }

            // object_children can mix kinds in one list (an Abstract's
            // own Terminals and Obstructions together) - grouped by kind
            // so each gets its own correctly-labeled row, matching
            // ObjectDetail's own childrenByKind grouping.
            std::map<int32_t, std::vector<LeObjectRef>> children_by_kind;
            for (const LeObjectRef &child : provider.object_children(current_ref))
            {
                children_by_kind[child.kind].push_back(child);
            }
            constexpr size_t kMaxChildLinks = 10;
            for (const auto &[kind, group] : children_by_kind)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(child_label(kind));
                ImGui::TableSetColumnIndex(1);
                size_t shown = 0;
                for (const LeObjectRef &child : group)
                {
                    if (shown >= kMaxChildLinks)
                    {
                        break;
                    }
                    if (shown > 0)
                    {
                        ImGui::SameLine();
                    }
                    ImGui::PushID(static_cast<int>(child.index));
                    ImGui::PushID(kind);
                    const std::string label = token_for(provider, child);
                    if (ImGui::SmallButton(label.c_str()))
                    {
                        jump_to(provider, child, hierarchy, current_ref);
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                    ++shown;
                }
                if (group.size() > kMaxChildLinks)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("+%zu more", group.size() - kMaxChildLinks);
                }
            }

            ImGui::EndTable();
        }
    }

    void draw_property_viewer(GuiProvider &provider)
    {
        static int current_index = 0;
        static std::vector<LeObjectRef> hierarchy;
        static LeObjectRef current_ref = provider.invalid_ref();
        static std::vector<LeObjectRef> last_selected;
        static int last_index = -1;
        static char filter_buf[256] = "";
        static bool show_hidden = false;

        const int32_t selection_count = provider.state().status_bar.selection_count;
        if (selection_count == 0)
        {
            ImGui::TextDisabled("No selection");
            // Reset so a later selection starts fresh instead of showing
            // a stale hierarchy from before everything was deselected.
            hierarchy.clear();
            current_ref = provider.invalid_ref();
            last_selected.clear();
            last_index = -1;
            return;
        }
        if (current_index >= selection_count)
        {
            current_index = 0;
        }

        std::vector<LeObjectRef> selected;
        selected.reserve(static_cast<size_t>(selection_count));
        for (int32_t i = 0; i < selection_count; ++i)
        {
            selected.push_back(provider.selected_object_ref(i));
        }

        const bool selection_changed =
            selected.size() != last_selected.size() ||
            !std::equal(selected.begin(), selected.end(), last_selected.begin(), refs_equal);
        const bool index_changed = current_index != last_index;
        last_selected = selected;
        last_index = current_index;

        if (selection_changed || index_changed || hierarchy.empty())
        {
            jump_to(provider, selected[static_cast<size_t>(current_index)], hierarchy, current_ref);
        }

        ImGui::Checkbox("Show hidden properties", &show_hidden);

        ImGui::BeginDisabled(current_index <= 0);
        if (ImGui::ArrowButton("##property_viewer_prev", ImGuiDir_Left))
        {
            --current_index;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("%d / %d", current_index + 1, selection_count);
        ImGui::SameLine();
        ImGui::BeginDisabled(current_index >= selection_count - 1);
        if (ImGui::ArrowButton("##property_viewer_next", ImGuiDir_Right))
        {
            ++current_index;
        }
        ImGui::EndDisabled();

        draw_hierarchy_tree(provider, hierarchy, current_ref);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##property_viewer_filter", "Filter properties", filter_buf, sizeof(filter_buf));

        draw_object_detail(provider, current_ref, filter_buf, show_hidden, hierarchy);
    }
}
