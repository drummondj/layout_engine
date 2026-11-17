#include "property_viewer.hpp"

#include "gui_provider.hpp"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <type_traits>
#include <variant>
#include <string>
#include <vector>

namespace le::gui
{
    namespace
    {
        // The label of a child kind's row: what the parent calls its list.
        const char *child_label(const ObjectId &id)
        {
            return std::visit([](const auto typed) -> const char *
                              {
                                  using T = std::decay_t<decltype(typed)>;
                                  if constexpr (std::is_same_v<T, TerminalPortId>)
                                      return "ports";
                                  else if constexpr (std::is_same_v<T, DesignId>)
                                      return "designs";
                                  else if constexpr (std::is_same_v<T, AbstractId>)
                                      return "abstracts";
                                  else if constexpr (std::is_same_v<T, TerminalId>)
                                      return "terminals";
                                  else if constexpr (std::is_same_v<T, ObstructionId>)
                                      return "obstructions";
                                  else if constexpr (std::is_same_v<T, ShapeId>)
                                      return "shapes";
                                  else if constexpr (std::is_same_v<T, WireId>)
                                      return "wires";
                                  else if constexpr (std::is_same_v<T, PhysicalPortSegmentId>)
                                      return "segments";
                                  else
                                      return "?"; },
                              id);
        }

        std::string format_value(const PropertyValue &property)
        {
            switch (property.type)
            {
            case PropertyValue::Type::DOUBLE:
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.3f", property.double_value);
                return buf;
            }
            case PropertyValue::Type::STRING:
                return property.string_value;
            case PropertyValue::Type::INT:
                return std::to_string(property.int_value);
            }
            return "";
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

        // The ancestor-chain box above the properties table - always
        // fully expanded (a single chain, never branching, so nothing to
        // collapse), root at the top. Clicking a row only changes which
        // node ObjectDetail shows properties for, never the chain itself.
        void draw_hierarchy_tree(const GuiProvider &provider, const std::vector<ObjectId> &hierarchy, std::optional<ObjectId> &current)
        {
            const std::vector<std::string> tokens = provider.tokens(hierarchy);
            const float row_height = ImGui::GetTextLineHeightWithSpacing();
            const float child_height = row_height * static_cast<float>(hierarchy.size()) + ImGui::GetStyle().WindowPadding.y * 2.0f;
            ImGui::BeginChild("property_viewer_hierarchy", ImVec2(0.0f, child_height), ImGuiChildFlags_Borders);
            for (size_t depth = 0; depth < hierarchy.size(); ++depth)
            {
                std::string label(depth * 2, ' ');
                if (depth > 0)
                {
                    label += "> ";
                }
                label += tokens[depth];
                ImGui::PushID(static_cast<int>(depth));
                if (ImGui::Selectable(label.c_str(), current == hierarchy[depth]))
                {
                    current = hierarchy[depth];
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }

        // `current`'s own property table, plus one extra row per child kind
        // at the bottom - clicking a child token re-anchors the hierarchy
        // tree above onto it.
        void draw_object_detail(
            GuiProvider &provider, std::optional<ObjectId> &current, const char *filter, bool show_hidden,
            std::vector<ObjectId> &hierarchy)
        {
            if (!current)
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

            const std::vector<PropertyValue> properties = provider.properties(*current);
            for (size_t i = 0; i < properties.size(); ++i)
            {
                const PropertyValue &property = properties[i];
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
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableSetColumnIndex(0);
                draw_selectable_text("##prop_name", property.name);
                ImGui::TableSetColumnIndex(1);
                draw_selectable_text("##prop_value", value);
                ImGui::PopID();
            }

            // children() can mix kinds in one list (an Abstract's Terminals
            // and Obstructions together) - grouped by kind so each gets its
            // own correctly-labeled row, at most kMaxChildLinks links each.
            constexpr size_t kMaxChildLinks = 10;
            std::map<size_t, std::vector<ObjectId>> children_by_kind;
            std::map<size_t, size_t> hidden_by_kind;
            for (const ObjectId &child : provider.children(*current))
            {
                std::vector<ObjectId> &group = children_by_kind[child.index()];
                if (group.size() < kMaxChildLinks)
                    group.push_back(child);
                else
                    ++hidden_by_kind[child.index()];
            }
            for (const auto &[kind, group] : children_by_kind)
            {
                const std::vector<std::string> labels = provider.tokens(group);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(child_label(group.front()));
                ImGui::TableSetColumnIndex(1);
                ImGui::PushID(static_cast<int>(kind));
                for (size_t shown = 0; shown < group.size(); ++shown)
                {
                    if (shown > 0)
                    {
                        ImGui::SameLine();
                    }
                    ImGui::PushID(static_cast<int>(shown));
                    if (ImGui::SmallButton(labels[shown].c_str()))
                    {
                        hierarchy = provider.ancestry(group[shown]);
                        current = group[shown];
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
                if (const auto hidden = hidden_by_kind.find(kind); hidden != hidden_by_kind.end())
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("+%zu more", hidden->second);
                }
            }

            ImGui::EndTable();
        }
    }

    void draw_property_viewer(GuiProvider &provider)
    {
        static size_t current_index = 0;
        static std::vector<ObjectId> hierarchy;
        static std::optional<ObjectId> current;
        static std::vector<ObjectId> last_selected;
        static std::optional<size_t> last_index;
        static char filter_buf[256] = "";
        static bool show_hidden = false;

        const size_t selection_count = provider.state().status_bar.selection_count;
        if (selection_count == 0)
        {
            ImGui::TextDisabled("No selection");
            // Reset so a later selection starts fresh instead of showing
            // a stale hierarchy from before everything was deselected.
            hierarchy.clear();
            current.reset();
            last_selected.clear();
            last_index.reset();
            return;
        }
        if (current_index >= selection_count)
        {
            current_index = 0;
        }

        std::vector<ObjectId> selected;
        selected.reserve(selection_count);
        for (size_t i = 0; i < selection_count; ++i)
        {
            if (const std::optional<ObjectId> id = provider.selected_object(i))
                selected.push_back(*id);
        }
        if (selected.empty())
            return;
        current_index = std::min(current_index, selected.size() - 1);

        const bool selection_changed = selected != last_selected;
        const bool index_changed = current_index != last_index;
        last_selected = selected;
        last_index = current_index;

        if (selection_changed || index_changed || hierarchy.empty())
        {
            hierarchy = provider.ancestry(selected[current_index]);
            current = selected[current_index];
        }

        ImGui::Checkbox("Show hidden properties", &show_hidden);

        ImGui::BeginDisabled(current_index == 0);
        if (ImGui::ArrowButton("##property_viewer_prev", ImGuiDir_Left))
        {
            --current_index;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("%zu / %zu", current_index + 1, selected.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(current_index + 1 >= selected.size());
        if (ImGui::ArrowButton("##property_viewer_next", ImGuiDir_Right))
        {
            ++current_index;
        }
        ImGui::EndDisabled();

        draw_hierarchy_tree(provider, hierarchy, current);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##property_viewer_filter", "Filter properties", filter_buf, sizeof(filter_buf));

        draw_object_detail(provider, current, filter_buf, show_hidden, hierarchy);
    }
}
