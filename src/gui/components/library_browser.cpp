#include "library_browser.hpp"

#include "gui_provider.hpp"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace le::gui
{
    namespace
    {
        bool contains_ignore_case(const std::string &haystack_text, const char *needle)
        {
            if (needle == nullptr || needle[0] == '\0')
            {
                return true;
            }
            const char *haystack = haystack_text.c_str();
            const size_t haystack_len = haystack_text.size();
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

        // Whether `design` should show under the current filter - its
        // own name, or (so a search like "layout" surfaces every design
        // that has one) one of its real leaf labels. Empty filter always
        // matches, same as contains_ignore_case's own "no needle" case.
        bool design_matches_filter(const GuiProvider::DesignEntry &design, const char *filter, bool has_abstract, bool has_layout)
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
        // to show) or a plain leaf itself otherwise - 0, 1, or
        // both of "Abstract"/"Layout" (one leaf per view the Design
        // actually has). Clicking the design
        // row itself only expands/collapses its own view list - opening
        // a view is unambiguous only for its "Abstract"/"Layout" leaves
        // below, each its own click target.
        void draw_design_node(GuiProvider &provider, const GuiProvider::DesignEntry &design, bool has_abstract, bool has_layout)
        {
            if (!has_abstract && !has_layout)
            {
                ImGui::TreeNodeEx(design.name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                return;
            }

            if (!ImGui::TreeNode(design.name.c_str()))
            {
                return;
            }
            if (has_abstract)
            {
                ImGui::TreeNodeEx("Abstract", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                if (ImGui::IsItemClicked())
                {
                    provider.open_design_abstract(design.id);
                }
            }
            if (has_layout)
            {
                ImGui::TreeNodeEx("Layout", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                if (ImGui::IsItemClicked())
                {
                    provider.open_design_layout(design.id);
                }
            }
            ImGui::TreePop();
        }
    }

    bool design_shown_in_browser(const GuiProvider::DesignEntry &design, const char *filter, bool show_designs_without_views)
    {
        if (!design.has_abstract && !design.has_layout && !show_designs_without_views)
            return false;
        return design_matches_filter(design, filter, design.has_abstract, design.has_layout);
    }

    void draw_library_browser(GuiProvider &provider)
    {
        static char filter_buf[256] = "";
        // Off by default: a netlist read leaves a Design per module, most
        // with only a Schematic, which would bury the ones that can open.
        static bool show_designs_without_views = false;
        ImGui::Checkbox("Show designs without physical views", &show_designs_without_views);
        ImGui::SetItemTooltip("Designs with neither an Abstract nor a Layout, e.g. modules read only from a netlist");
        ImGui::SetNextItemWidth(-1.0f);
        // Libraries start collapsed.
        // Editing the filter opens every library still shown (so its
        // matches are visible) or, once cleared, collapses them again -
        // only on the frame the text changes, so a library can still be
        // collapsed/expanded by hand while a filter is active.
        const bool filter_changed = ImGui::InputTextWithHint("##library_browser_filter", "filter", filter_buf, sizeof(filter_buf));

        const std::vector<GuiProvider::LibraryEntry> &libraries = provider.libraries();
        if (libraries.empty())
        {
            ImGui::TextDisabled("Nothing loaded yet");
            return;
        }

        for (const GuiProvider::LibraryEntry &library : libraries)
        {
            const bool library_name_matches = contains_ignore_case(library.name, filter_buf);

            // A library is shown if its own name matches, or at least
            // one of its designs does (an ancestor stays reachable
            // whenever any descendant still matches, even if the
            // ancestor's own title doesn't).
            const bool library_visible = library_name_matches || std::ranges::any_of(library.designs, [&](const GuiProvider::DesignEntry &design)
                                                                                     { return design_shown_in_browser(design, filter_buf, show_designs_without_views); });
            if (!library_visible)
            {
                continue;
            }

            if (filter_changed)
                ImGui::SetNextItemOpen(filter_buf[0] != '\0', ImGuiCond_Always);
            if (!ImGui::TreeNode(library.name.c_str()))
            {
                continue;
            }
            for (const GuiProvider::DesignEntry &design : library.designs)
            {
                // The library itself matching its own name (not a
                // descendant match) still shows every one of its
                // designs unfiltered - only a library shown *because* a
                // descendant matched needs its own designs individually
                // filtered.
                if (!design.has_abstract && !design.has_layout && !show_designs_without_views)
                {
                    continue;
                }
                if (!library_name_matches && !design_matches_filter(design, filter_buf, design.has_abstract, design.has_layout))
                {
                    continue;
                }
                draw_design_node(provider, design, design.has_abstract, design.has_layout);
            }
            ImGui::TreePop();
        }
    }
}
