#pragma once
#include "database.hpp"
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <vector>

namespace le
{
    /// @brief The Library named `name`, created if none exists yet - the
    /// read_lef/read_def/read_verilog `-library` target.
    inline LibraryId get_or_create_library(Root &root, const std::string &name)
    {
        const LibraryId existing = root.get_library_by_name(name);
        if (existing.valid())
            return existing;
        return root.create_library(LibraryData{.name = name});
    }

    /// @brief Every Design named `name`, in any Library (a Design's name
    /// is only unique within its Library).
    inline std::vector<DesignId> designs_named(const Root &root, const std::string &name)
    {
        std::vector<DesignId> found;
        root.for_each_library_id([&](LibraryId library)
                                 {
            const DesignId design = root.get_design_by_name(library, name);
            if (design.valid())
                found.push_back(design); });
        return found;
    }

    /// @brief Resolves a Design referenced by name alone (a DEF COMPONENT's
    /// macro, a Verilog instance's module, `design:NAME`): the one in
    /// `preferred` if it has one, else the only Design of that name in any
    /// Library. Invalid if there's none, or several and none in
    /// `preferred` - `matches`, if given, receives every candidate so the
    /// caller can report the ambiguity.
    inline DesignId find_design_by_name(const Root &root, const std::string &name, LibraryId preferred = {},
                                        std::vector<DesignId> *matches = nullptr)
    {
        if (preferred.valid())
        {
            const DesignId design = root.get_design_by_name(preferred, name);
            if (design.valid())
                return design;
        }
        std::vector<DesignId> found = designs_named(root, name);
        const DesignId result = found.size() == 1 ? found.front() : DesignId{};
        if (matches)
            *matches = std::move(found);
        return result;
    }

    /// @brief The Design a user-typed reference names: `NAME` if only one
    /// Library has a Design called that, else `LIB/NAME` (tried at every
    /// '/', so either name may contain one). Invalid if neither resolves.
    inline DesignId resolve_design_reference(const Root &root, const std::string &text)
    {
        const DesignId by_name = find_design_by_name(root, text);
        if (by_name.valid())
            return by_name;
        for (size_t slash = text.find('/'); slash != std::string::npos; slash = text.find('/', slash + 1))
        {
            const LibraryId library = root.get_library_by_name(text.substr(0, slash));
            if (!library.valid())
                continue;
            const DesignId design = root.get_design_by_name(library, text.substr(slash + 1));
            if (design.valid())
                return design;
        }
        return {};
    }

    /// @brief How resolve_design_reference() would name `design`: its name
    /// if that's unambiguous, else `LIB/NAME`. Empty if it doesn't exist.
    inline std::string design_reference(const Root &root, DesignId design)
    {
        const DesignData *data = root.get_design(design);
        if (!data)
            return {};
        if (find_design_by_name(root, data->name) == design)
            return data->name;
        const LibraryData *library = root.get_library(data->library);
        return (library ? library->name : std::string()) + "/" + data->name;
    }

    /// @brief "lib_a, lib_b" - the Libraries of `designs`, for an
    /// ambiguous-name message.
    inline std::string library_names_of(const Root &root, const std::vector<DesignId> &designs)
    {
        std::string names;
        for (const DesignId design : designs)
        {
            const DesignData *data = root.get_design(design);
            const LibraryData *library = data ? root.get_library(data->library) : nullptr;
            names += (names.empty() ? "" : ", ") + (library ? library->name : std::string("?"));
        }
        return names;
    }

    /// @brief A Design's views, as get_or_create_design's `view` argument.
    enum class DesignView
    {
        Abstract,
        Schematic,
        Layout,
    };

    /// @brief True if `design` already has `view`.
    inline bool design_has_view(const Root &root, DesignId design, DesignView view)
    {
        switch (view)
        {
        case DesignView::Abstract:
            return root.get_design_abstract(design).valid();
        case DesignView::Schematic:
            return root.get_design_schematic(design).valid();
        case DesignView::Layout:
            return root.get_design_layout(design).valid();
        }
        return false;
    }

    /// @brief The Design a read of `view` named `name` goes into: the one
    /// in `library_id` if it exists (the caller rejects it if it already
    /// has `view`); else the only same-named Design in another Library, if
    /// it lacks `view` - so a LEF read gives an Abstract to a Design a DEF
    /// or Verilog read made, and a netlist's stub modules attach to LEF
    /// cells; else a new Design in `library_id`. A second library's cell
    /// of the same name (INV in two standard-cell libraries) thus gets its
    /// own Design. Logs a warning naming both libraries when it reuses one
    /// from another library and `caller` is non-empty (read_verilog passes
    /// empty: attaching its stubs to LEF cells is the point).
    inline DesignId get_or_create_design(Root &root, LibraryId library_id, const std::string &name, DesignView view,
                                         std::string_view caller)
    {
        const DesignId own = root.get_design_by_name(library_id, name);
        if (own.valid())
            return own;

        const std::vector<DesignId> elsewhere = designs_named(root, name);
        if (elsewhere.size() == 1 && !design_has_view(root, elsewhere.front(), view))
        {
            if (!caller.empty())
            {
                const DesignData *design = root.get_design(elsewhere.front());
                const LibraryData *existing_library = design ? root.get_library(design->library) : nullptr;
                const LibraryData *requested_library = root.get_library(library_id);
                spdlog::warn("{}: design {} already exists in library {} - adding this view to it there, not to library {}",
                             caller, name, existing_library ? existing_library->name : "?",
                             requested_library ? requested_library->name : "?");
            }
            return elsewhere.front();
        }
        return root.create_design(DesignData{.library = library_id, .name = name});
    }
}
