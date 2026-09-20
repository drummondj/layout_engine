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
        // field from layout_engine_plugin.dart's Dart FFI implementation.
        //
        // Every case but LE_OBJECT_KIND_DESIGN uses the generated
        // is_child-list-field accessors (le_<owner>_<field>_count/_at,
        // api/generated_tcl/property_accessors_public.inc - regen-tcl
        // skill) instead of the generated search surface
        // (le_get_<type>/le_search_result_<type>_at) this used to call
        // uniformly - a deliberate change, not a stylistic one: the
        // search surface unconditionally *rewrites* a shared per-class
        // search-result cache on every call (a fresh query, by design),
        // so it needs handle->mutex_'s unique_lock every time and can't
        // run concurrently with an in-progress render, unlike the
        // count/_at pair here, which are plain Root reads (shared_lock -
        // le_handle.hpp's own mutex_ doc comment) - this is what lets
        // draw_object_detail below stay live and unblocked while a
        // render is in flight, matching every other panel. The DESIGN
        // case is the one exception left on the old search surface: a
        // Design's abstract is a single reference field, not a list
        // field, so it has no generated le_design_abstracts_count/_at
        // counterpart to switch to - see this file's own le_gui.cpp
        // caller for how that one remaining gap is handled (still gated
        // on is_rendering). Shape (the hierarchy's own leaf) and
        // Row/Placement/Region/Layout (no exposed accessor to enumerate
        // their own children through yet - see layout_engine_plugin.dart's
        // own comment on this) fall through to the empty default.
        std::vector<LeObjectRef> object_children(LeHandle *handle, const LeObjectRef &ref)
        {
            std::vector<LeObjectRef> children;
            switch (ref.kind)
            {
            case LE_OBJECT_KIND_LIBRARY:
            {
                const LeLibraryId library_id = ref_to_id<LeLibraryId>(ref);
                const int32_t count = le_library_designs_count(handle, library_id);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeDesignId id = le_library_designs_at(handle, library_id, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_DESIGN, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_DESIGN:
            {
                // Still the generated search surface (le_get_abstracts,
                // handle->mutex_-locked with a unique_lock - see this
                // function's own doc comment above for why), so skip it
                // outright while a render is in flight rather than call
                // it and block for however long that takes - the one
                // remaining case where this whole function can't just
                // stay unconditionally live the way every other case
                // now does. A Design directly selected mid-render simply
                // shows no children for that one frame-or-so instead;
                // le_is_rendering is lock-free (le_handle.hpp's own
                // is_rendering_ doc comment), safe to check here with no
                // extra cost.
                if (le_is_rendering(handle))
                    break;
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
                const int32_t terminal_count = le_abstract_terminals_count(handle, abstract_id);
                for (int32_t i = 0; i < terminal_count; ++i)
                {
                    const LeTerminalId id = le_abstract_terminals_at(handle, abstract_id, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_TERMINAL, id.index, id.generation));
                }
                const int32_t obstruction_count = le_abstract_obstructions_count(handle, abstract_id);
                for (int32_t i = 0; i < obstruction_count; ++i)
                {
                    const LeObstructionId id = le_abstract_obstructions_at(handle, abstract_id, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_OBSTRUCTION, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_TERMINAL:
            {
                const LeTerminalId terminal_id = ref_to_id<LeTerminalId>(ref);
                const int32_t count = le_terminal_ports_count(handle, terminal_id);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeTerminalPortId id = le_terminal_ports_at(handle, terminal_id, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_TERMINAL_PORT, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_TERMINAL_PORT:
            {
                const LeTerminalPortId port_id = ref_to_id<LeTerminalPortId>(ref);
                const int32_t count = le_terminal_port_shapes_count(handle, port_id);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeShapeId id = le_terminal_port_shapes_at(handle, port_id, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_OBSTRUCTION:
            {
                const LeObstructionId obstruction_id = ref_to_id<LeObstructionId>(ref);
                const int32_t count = le_obstruction_shapes_count(handle, obstruction_id);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeShapeId id = le_obstruction_shapes_at(handle, obstruction_id, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_BLOCKAGE:
            {
                const LeBlockageId blockage_id = ref_to_id<LeBlockageId>(ref);
                const int32_t count = le_blockage_shapes_count(handle, blockage_id);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeShapeId id = le_blockage_shapes_at(handle, blockage_id, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_ROUTE:
            {
                const LeRouteId route_id = ref_to_id<LeRouteId>(ref);
                const int32_t count = le_route_shapes_count(handle, route_id);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeShapeId id = le_route_shapes_at(handle, route_id, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
            {
                const LePhysicalPortSegmentId segment_id = ref_to_id<LePhysicalPortSegmentId>(ref);
                const int32_t count = le_physical_port_segment_shapes_count(handle, segment_id);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LeShapeId id = le_physical_port_segment_shapes_at(handle, segment_id, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_SHAPE, id.index, id.generation));
                }
                break;
            }
            case LE_OBJECT_KIND_PHYSICAL_PORT:
            {
                const LePhysicalPortId port_id = ref_to_id<LePhysicalPortId>(ref);
                const int32_t count = le_physical_port_segments_count(handle, port_id);
                for (int32_t i = 0; i < count; ++i)
                {
                    const LePhysicalPortSegmentId id = le_physical_port_segments_at(handle, port_id, i);
                    children.push_back(make_ref(LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT, id.index, id.generation));
                }
                break;
            }
            default:
                break;
            }
            return children;
        }

        // A read-only, click-and-drag-selectable, Ctrl+C-copyable text
        // field - ImGui's own idiom for this (there's no plain-text
        // "selectable label" widget), matching property_viewer.dart's
        // own SelectableText for each property name/value cell. `id`
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
            // through instead, closer to how SelectableText looked
            // against DataTable's own row background in property_viewer.dart.
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::InputText(id, buf.data(), buf.size(), ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor(3);
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

        // Whether calling token_for(handle, ref) (or le_object_property_count/_at
        // directly) right now risks blocking this whole GUI thread behind
        // an in-progress render - true only when BOTH a render is
        // actually in flight (le_is_rendering, lock-free) AND `ref` isn't
        // already what le_object_property_count/_at's own single-slot
        // cache holds (le_object_property_cache_current, shared_lock-only,
        // itself never blocks - api.hpp's own doc comment on both). A
        // stable selection whose ref is already cached stays resolved
        // every frame regardless of how long a *viewport-only* render
        // (zoom/pan/fit with no selection change) takes - only a
        // genuinely fresh ref (a new selection, or a different breadcrumb/
        // child-link entry never resolved before) can land on the slow,
        // write-lock-escalating path this guards.
        bool would_block_property_lookup(LeHandle *handle, const LeObjectRef &ref)
        {
            return le_is_rendering(handle) && !le_object_property_cache_current(handle, ref);
        }

        // The ancestor-chain box above the properties table - always
        // fully expanded (a single chain, never branching, so nothing to
        // collapse), root at the top. Clicking a row only changes which
        // node ObjectDetail shows properties for, never the chain itself.
        //
        // token_for (and the name_of it calls) goes through
        // le_object_property_count/_at's own single-slot cache
        // (le_handle.hpp's cached_object_property_ref/cached_object_properties) -
        // a real hazard here specifically, since this loop calls it once
        // per *different* ref in the chain every single frame, thrashing
        // that one-slot cache on every call rather than hitting its
        // shared_lock fast path the way a stable, repeated same-ref query
        // does elsewhere. Each thrash escalates to a real HandleWriteLock
        // rebuild (api.cpp's own le_object_property_count comment) - cheap
        // on its own, but a real, reproduced freeze when it lands while a
        // render is holding the shared lock for a long stretch (a real
        // design's own full-viewport fit can take seconds): the write-lock
        // attempt blocks this whole GUI thread frame in place, well before
        // it ever reaches the status bar/spinner draw or SwapBuffers later
        // in the same frame, so nothing else about the window can update
        // either - not just this panel. would_block_property_lookup above
        // narrows the skip to only the ref(s) that would actually risk
        // this, not every ref whenever a render merely happens to be
        // running - mirrors object_children's own LE_OBJECT_KIND_DESIGN
        // case (this file, above) for the same underlying reason, just
        // checked per-ref here since this loop's own refs vary by depth.
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
                label += would_block_property_lookup(handle, ref) ? "..." : token_for(handle, ref);
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

            // le_object_property_count/_at's own single-slot cache
            // (draw_hierarchy_tree's own comment above, on the same
            // hazard) only escalates to a real write lock when current_ref
            // differs from whatever it last cached - the common case
            // (browsing the same object's properties frame after frame,
            // including across a viewport-only render with no selection
            // change) stays on the shared_lock fast path and is safe
            // unconditionally, so would_block_property_lookup only skips
            // when current_ref itself just changed *and* a render happens
            // to be in flight right then - not every frame a render merely
            // happens to be running. The table still draws (headers,
            // children links below), just with no property rows for that
            // one frame-or-so.
            if (!would_block_property_lookup(handle, current_ref))
            {
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
                    ImGui::PushID(i);
                    ImGui::TableSetColumnIndex(0);
                    draw_selectable_text("##prop_name", property.name);
                    ImGui::TableSetColumnIndex(1);
                    draw_selectable_text("##prop_value", value);
                    ImGui::PopID();
                }
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
            // Each token_for(handle, child) call below queries a
            // *different* ref through the same single-slot property cache
            // draw_hierarchy_tree's own comment above describes - the same
            // thrash-then-possibly-block hazard, multiplied by however many
            // child links are shown this frame - would_block_property_lookup
            // (checked per-child below, since each has its own ref) only
            // skips the ones that would actually risk it.
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
                    const std::string label = would_block_property_lookup(handle, child) ? "..." : token_for(handle, child);
                    if (ImGui::SmallButton(label.c_str()))
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
