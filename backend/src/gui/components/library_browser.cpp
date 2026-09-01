#include "library_browser.hpp"

#include "api.hpp"
#include "imgui.h"

#include <cctype>
#include <cstdint>
#include <cstring>

namespace le::gui
{
    namespace
    {
        bool contains_ignore_case(const char *haystack, const char *needle)
        {
            if (needle == nullptr || needle[0] == '\0')
            {
                return true;
            }
            if (haystack == nullptr)
            {
                return false;
            }
            const size_t haystack_len = std::strlen(haystack);
            const size_t needle_len = std::strlen(needle);
            if (needle_len > haystack_len)
            {
                return false;
            }
            for (size_t offset = 0; offset + needle_len <= haystack_len; ++offset)
            {
                bool matched = true;
                for (size_t i = 0; i < needle_len; ++i)
                {
                    if (std::tolower(static_cast<unsigned char>(haystack[offset + i])) !=
                        std::tolower(static_cast<unsigned char>(needle[i])))
                    {
                        matched = false;
                        break;
                    }
                }
                if (matched)
                {
                    return true;
                }
            }
            return false;
        }

        // Padding to fit_to_content's own margin, matching
        // le_provider.dart's own openDesign/openDesignLayout - both call
        // fitScene(10) right after switching, so a freshly opened design
        // is immediately framed instead of showing whatever pan/scale
        // the previously viewed one happened to leave behind.
        constexpr int32_t kFitScenePaddingPx = 10;

        void open_abstract(LeHandle *handle, LeDesignId design_id)
        {
            le_set_current_design_abstract_by_id(handle, design_id);
            le_fit_scene(handle, kFitScenePaddingPx);
        }

        void open_layout(LeHandle *handle, LeDesignId design_id)
        {
            le_set_current_design_layout_by_id(handle, design_id);
            le_fit_scene(handle, kFitScenePaddingPx);
        }

        // Whether `design` should show under the current filter - its
        // own name, or (so a search like "layout" surfaces every design
        // that has one) one of its real leaf labels. Empty filter always
        // matches, same as contains_ignore_case's own "no needle" case.
        bool design_matches_filter(const LeDesignInfo &design, const char *filter, bool has_abstract, bool has_layout)
        {
            if (filter[0] == '\0')
            {
                return true;
            }
            if (contains_ignore_case(design.name, filter))
            {
                return true;
            }
            if (has_abstract && contains_ignore_case("Abstract", filter))
            {
                return true;
            }
            if (has_layout && contains_ignore_case("Layout", filter))
            {
                return true;
            }
            return false;
        }

        // One Design row - a tree node (if it has at least one real leaf
        // to show) or a plain leaf itself otherwise, matching
        // library_browser.dart's own designNode.children shape (0, 1, or
        // both of "Abstract"/"Layout" - one leaf per view the Design
        // actually has, see LeDesignInfo's own abstract_id/layout_id doc
        // comments for when either can be invalid). Clicking the design
        // row itself only expands/collapses its own view list - opening
        // a view is unambiguous only for its "Abstract"/"Layout" leaves
        // below, each its own click target.
        void draw_design_node(LeHandle *handle, const LeDesignInfo &design, bool has_abstract, bool has_layout)
        {
            if (!has_abstract && !has_layout)
            {
                ImGui::TreeNodeEx(design.name, ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                return;
            }

            if (!ImGui::TreeNode(design.name))
            {
                return;
            }
            if (has_abstract)
            {
                ImGui::TreeNodeEx("Abstract", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                if (ImGui::IsItemClicked())
                {
                    open_abstract(handle, design.id);
                }
            }
            if (has_layout)
            {
                ImGui::TreeNodeEx("Layout", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                if (ImGui::IsItemClicked())
                {
                    open_layout(handle, design.id);
                }
            }
            ImGui::TreePop();
        }
    }

    void draw_library_browser(LeHandle *handle)
    {
        static char filter_buf[256] = "";
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##library_browser_filter", "filter", filter_buf, sizeof(filter_buf));

        const int32_t library_count = le_library_count(handle);
        if (library_count == 0)
        {
            ImGui::TextDisabled("Nothing loaded yet - read_lef from the console.");
            return;
        }

        for (int32_t library_index = 0; library_index < library_count; ++library_index)
        {
            const LeLibraryInfo library = le_library_at(handle, library_index);
            if (library.name == nullptr)
            {
                continue;
            }
            const int32_t design_count = le_library_design_count(handle, library_index);
            const bool library_name_matches = contains_ignore_case(library.name, filter_buf);

            // A library is shown if its own name matches, or at least
            // one of its designs does - matching library_browser.dart's
            // own getChildren() filtering (an ancestor stays reachable
            // whenever any descendant still matches, even if the
            // ancestor's own title doesn't).
            bool library_visible = library_name_matches;
            if (!library_visible)
            {
                for (int32_t design_index = 0; design_index < design_count; ++design_index)
                {
                    const LeDesignInfo design = le_library_design_at(handle, library_index, design_index);
                    if (design.name == nullptr)
                    {
                        continue;
                    }
                    const bool has_abstract = design.abstract_id.index != UINT32_MAX;
                    const bool has_layout = design.layout_id.index != UINT32_MAX;
                    if (design_matches_filter(design, filter_buf, has_abstract, has_layout))
                    {
                        library_visible = true;
                        break;
                    }
                }
            }
            if (!library_visible)
            {
                continue;
            }

            if (!ImGui::TreeNodeEx(library.name, ImGuiTreeNodeFlags_DefaultOpen))
            {
                continue;
            }
            for (int32_t design_index = 0; design_index < design_count; ++design_index)
            {
                const LeDesignInfo design = le_library_design_at(handle, library_index, design_index);
                if (design.name == nullptr)
                {
                    continue;
                }
                const bool has_abstract = design.abstract_id.index != UINT32_MAX;
                const bool has_layout = design.layout_id.index != UINT32_MAX;
                // The library itself matching its own name (not a
                // descendant match) still shows every one of its
                // designs unfiltered - only a library shown *because* a
                // descendant matched needs its own designs individually
                // filtered.
                if (!library_name_matches && !design_matches_filter(design, filter_buf, has_abstract, has_layout))
                {
                    continue;
                }
                draw_design_node(handle, design, has_abstract, has_layout);
            }
            ImGui::TreePop();
        }
    }
}
