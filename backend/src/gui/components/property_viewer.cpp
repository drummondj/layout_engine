#include "property_viewer.hpp"

#include "api.hpp"
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

        template <typename IdT>
        IdT ref_to_id(const LeObjectRef &ref)
        {
            IdT id{};
            id.index = ref.index;
            id.generation = ref.generation;
            return id;
        }

        // le_get_shapes' own "exactly one real parent" contract (Shape
        // has 7 possible parent fields) needs an explicit invalid id for
        // every parent slot that doesn't apply to a given caller - same
        // convention layout_engine_plugin.dart's own _invalidXxxId
        // getters use.
        template <typename IdT>
        IdT invalid_id()
        {
            IdT id{};
            id.index = UINT32_MAX;
            id.generation = 0;
            return id;
        }

        LeObjectRef make_ref(int32_t kind, uint32_t index, uint32_t generation)
        {
            LeObjectRef ref;
            ref.kind = kind;
            ref.index = index;
            ref.generation = generation;
            return ref;
        }

        std::string name_of(LeHandle *handle, const LeObjectRef &ref)
        {
            const int32_t count = le_object_property_count(handle, ref);
            for (int32_t i = 0; i < count; ++i)
            {
                const LeProperty property = le_object_property_at(handle, ref, i);
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

        // [ref]'s own TCL-style friendly-id token (property_viewer.dart's
        // own _tokenFor) - name-keyed for the classes that have a real
        // name field, numeric-packed (generation<<32 | index) for the
        // ones that don't. Purely a display string here - never actually
        // round-tripped through TCL, matching the Dart original's own
        // "no TCL round trip needed, LeObjectRef already carries the same
        // index/generation shape" reasoning.
        std::string token_for(LeHandle *handle, const LeObjectRef &ref)
        {
            if (!ref_is_valid(ref))
            {
                return "?";
            }
            switch (ref.kind)
            {
            case LE_OBJECT_KIND_LIBRARY:
                return "library:" + name_of(handle, ref);
            case LE_OBJECT_KIND_DESIGN:
                return "design:" + name_of(handle, ref);
            case LE_OBJECT_KIND_TERMINAL:
                return "terminal:" + name_of(handle, ref);
            case LE_OBJECT_KIND_ROW:
                return "row:" + name_of(handle, ref);
            case LE_OBJECT_KIND_PLACEMENT:
                return "placement:" + name_of(handle, ref);
            case LE_OBJECT_KIND_ROUTE:
                return "route:" + name_of(handle, ref);
            case LE_OBJECT_KIND_PHYSICAL_PORT:
                return "physical_port:" + name_of(handle, ref);
            case LE_OBJECT_KIND_REGION:
                return "region:" + name_of(handle, ref);
            case LE_OBJECT_KIND_ABSTRACT:
                return packed_token("abstract", ref);
            case LE_OBJECT_KIND_TERMINAL_PORT:
                return packed_token("terminal_port", ref);
            case LE_OBJECT_KIND_OBSTRUCTION:
                return packed_token("obstruction", ref);
            case LE_OBJECT_KIND_SHAPE:
                return packed_token("shape", ref);
            case LE_OBJECT_KIND_BLOCKAGE:
                return packed_token("blockage", ref);
            case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
                return packed_token("physical_port_segment", ref);
            case LE_OBJECT_KIND_LAYOUT:
                return packed_token("layout", ref);
            default:
                return "?";
            }
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

        // `ref`'s own database children, one entry per real child object -
        // property_viewer.dart's own objectChildren, ported field-for-
        // field from layout_engine_plugin.dart's Dart FFI implementation
        // (there is no single le_object_children() in api.hpp - the Dart
        // plugin itself dispatches per LeObjectKind onto the matching
        // le_get_<type>/le_search_result_<type>_at pair, the same
        // generated search surface get_<type> uses over TCL). Shape (the
        // hierarchy's own leaf) and Row/Placement/Region/Layout (no
        // exposed search function to enumerate their own children through
        // yet - see layout_engine_plugin.dart's own comment on this) fall
        // through to the empty default.
        std::vector<LeObjectRef> object_children(LeHandle *handle, const LeObjectRef &ref)
        {
            std::vector<LeObjectRef> children;
            switch (ref.kind)
            {
            case LE_OBJECT_KIND_LIBRARY:
            {
                const LeLibraryId library_id = ref_to_id<LeLibraryId>(ref);
                const int32_t count = le_get_designs(handle, library_id, nullptr, nullptr);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeDesignId id = le_search_result_design_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_DESIGN, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_DESIGN:
            {
                const LeDesignId design_id = ref_to_id<LeDesignId>(ref);
                const int32_t count = le_get_abstracts(handle, design_id, nullptr);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeAbstractId id = le_search_result_abstract_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_ABSTRACT, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_ABSTRACT:
            {
                const LeAbstractId abstract_id = ref_to_id<LeAbstractId>(ref);
                const int32_t terminal_count = le_get_terminals(handle, abstract_id, nullptr, nullptr);
                for (int32_t i = 0; i < terminal_count; ++i)
                {
                    const LeTerminalId id = le_search_result_terminal_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_TERMINAL, id.index, id.generation));
                }
                const int32_t obstruction_count = le_get_obstructions(handle, abstract_id, nullptr);
                for (int32_t i = 0; i < obstruction_count; ++i)
                {
                    const LeObstructionId id = le_search_result_obstruction_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_OBSTRUCTION, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_TERMINAL:
            {
                const LeTerminalId terminal_id = ref_to_id<LeTerminalId>(ref);
                const int32_t count = le_get_terminal_ports(handle, terminal_id, nullptr);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeTerminalPortId id = le_search_result_terminal_port_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_TERMINAL_PORT, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_TERMINAL_PORT:
            {
                const LeTerminalPortId port_id = ref_to_id<LeTerminalPortId>(ref);
                const int32_t count = le_get_shapes(
                    handle, port_id, invalid_id<LeObstructionId>(), invalid_id<LePhysicalPortSegmentId>(),
                    invalid_id<LeBlockageId>(), invalid_id<LeRouteId>(), invalid_id<LeLayoutId>(),
                    invalid_id<LeAbstractId>(), nullptr);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeShapeId id = le_search_result_shape_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_OBSTRUCTION:
            {
                const LeObstructionId obstruction_id = ref_to_id<LeObstructionId>(ref);
                const int32_t count = le_get_shapes(
                    handle, invalid_id<LeTerminalPortId>(), obstruction_id, invalid_id<LePhysicalPortSegmentId>(),
                    invalid_id<LeBlockageId>(), invalid_id<LeRouteId>(), invalid_id<LeLayoutId>(),
                    invalid_id<LeAbstractId>(), nullptr);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeShapeId id = le_search_result_shape_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_BLOCKAGE:
            {
                const LeBlockageId blockage_id = ref_to_id<LeBlockageId>(ref);
                const int32_t count = le_get_shapes(
                    handle, invalid_id<LeTerminalPortId>(), invalid_id<LeObstructionId>(),
                    invalid_id<LePhysicalPortSegmentId>(), blockage_id, invalid_id<LeRouteId>(),
                    invalid_id<LeLayoutId>(), invalid_id<LeAbstractId>(), nullptr);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeShapeId id = le_search_result_shape_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_ROUTE:
            {
                const LeRouteId route_id = ref_to_id<LeRouteId>(ref);
                const int32_t count = le_get_shapes(
                    handle, invalid_id<LeTerminalPortId>(), invalid_id<LeObstructionId>(),
                    invalid_id<LePhysicalPortSegmentId>(), invalid_id<LeBlockageId>(), route_id,
                    invalid_id<LeLayoutId>(), invalid_id<LeAbstractId>(), nullptr);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeShapeId id = le_search_result_shape_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
            {
                const LePhysicalPortSegmentId segment_id = ref_to_id<LePhysicalPortSegmentId>(ref);
                const int32_t count = le_get_shapes(
                    handle, invalid_id<LeTerminalPortId>(), invalid_id<LeObstructionId>(), segment_id,
                    invalid_id<LeBlockageId>(), invalid_id<LeRouteId>(), invalid_id<LeLayoutId>(),
                    invalid_id<LeAbstractId>(), nullptr);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeShapeId id = le_search_result_shape_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_PHYSICAL_PORT:
            {
                const LePhysicalPortId port_id = ref_to_id<LePhysicalPortId>(ref);
                const int32_t count = le_get_physical_port_segments(handle, port_id, nullptr);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LePhysicalPortSegmentId id = le_search_result_physical_port_segment_at(handle, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT, id.index, id.generation));
                }
                break;
            }
            default:
                break;
            }
            return children;
        }

        // Rebuilds `hierarchy` as `ref`'s own ancestor chain (root first)
        // and makes `ref` the currently-displayed node - property_viewer.dart's
        // own _jumpTo, called both when the outer selection/pager changes
        // and from a "children" row's own click (re-anchoring onto that
        // child without touching canvas selection at all).
        void jump_to(LeHandle *handle, const LeObjectRef &ref, std::vector<LeObjectRef> &hierarchy, LeObjectRef &current_ref)
        {
            std::vector<LeObjectRef> chain;
            chain.push_back(ref);
            LeObjectRef parent = le_object_parent(handle, ref);
            while (ref_is_valid(parent))
            {
                chain.push_back(parent);
                parent = le_object_parent(handle, parent);
            }
            hierarchy.assign(chain.rbegin(), chain.rend());
            current_ref = ref;
        }

        // The ancestor-chain box above the properties table - always
        // fully expanded (a single chain, never branching, so nothing to
        // collapse), root at the top. Clicking a row only changes which
        // node ObjectDetail shows properties for, never the chain itself.
        void draw_hierarchy_tree(LeHandle *handle, const std::vector<LeObjectRef> &hierarchy, LeObjectRef &current_ref)
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
                label += token_for(handle, ref);
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
        // child kind at the bottom (property_viewer.dart's own
        // ObjectDetail) - clicking a child token re-anchors the
        // hierarchy tree above onto it via jump_to.
        void draw_object_detail(
            LeHandle *handle, LeObjectRef &current_ref, const char *filter, bool show_hidden,
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

            const int32_t property_count = le_object_property_count(handle, current_ref);
            for (int32_t i = 0; i < property_count; ++i)
            {
                const LeProperty property = le_object_property_at(handle, current_ref, i);
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
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(property.name);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(value.c_str());
            }

            // objectChildren can mix kinds in one list (an Abstract's own
            // Terminals and Obstructions together) - grouped by kind so
            // each gets its own correctly-labeled row, matching
            // ObjectDetail's own childrenByKind grouping.
            std::map<int32_t, std::vector<LeObjectRef>> children_by_kind;
            for (const LeObjectRef &child : object_children(handle, current_ref))
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
                    if (ImGui::SmallButton(token_for(handle, child).c_str()))
                    {
                        jump_to(handle, child, hierarchy, current_ref);
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

    void draw_property_viewer(LeHandle *handle)
    {
        static int current_index = 0;
        static std::vector<LeObjectRef> hierarchy;
        static LeObjectRef current_ref = le_object_invalid_ref();
        static std::vector<LeObjectRef> last_selected;
        static int last_index = -1;
        static char filter_buf[256] = "";
        static bool show_hidden = false;

        const int32_t selection_count = le_selection_count(handle);
        if (selection_count == 0)
        {
            ImGui::TextDisabled("No selection");
            // Reset so a later selection starts fresh instead of showing
            // a stale hierarchy from before everything was deselected -
            // mirrors _syncHierarchy's own reset in property_viewer.dart.
            hierarchy.clear();
            current_ref = le_object_invalid_ref();
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
            selected.push_back(le_selected_object_ref(handle, i));
        }

        const bool selection_changed =
            selected.size() != last_selected.size() ||
            !std::equal(selected.begin(), selected.end(), last_selected.begin(), refs_equal);
        const bool index_changed = current_index != last_index;
        last_selected = selected;
        last_index = current_index;

        if (selection_changed || index_changed || hierarchy.empty())
        {
            jump_to(handle, selected[static_cast<size_t>(current_index)], hierarchy, current_ref);
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

        draw_hierarchy_tree(handle, hierarchy, current_ref);

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##property_viewer_filter", "Filter properties", filter_buf, sizeof(filter_buf));

        draw_object_detail(handle, current_ref, filter_buf, show_hidden, hierarchy);
    }
}
