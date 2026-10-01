#pragma once
#include "database.hpp"
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace le
{
    /// @brief The Library named `name`, created if none exists yet - the
    /// read_lef/read_def/read_verilog `-library` target
    /// (NEW_FEATURES_SEPT_2026.md item 4).
    inline LibraryId get_or_create_library(Root &root, const std::string &name)
    {
        const LibraryId existing = root.get_library_by_name(name);
        if (existing.valid())
            return existing;
        return root.create_library(LibraryData{.name = name});
    }

    /// @brief The Design named `name`, created in `library_id` if none
    /// exists yet. Design names are global (Design.name is one flat
    /// index, and DEF/Verilog resolve references by name alone), so an
    /// existing Design keeps its own library even when `library_id`
    /// differs - a read then adds its view to that Design. Logs a warning
    /// naming both libraries in that case when `caller` is non-empty
    /// (read_verilog passes empty: its netlist stubs deliberately attach
    /// to LEF cells living in another library).
    inline DesignId get_or_create_design(Root &root, LibraryId library_id, const std::string &name, std::string_view caller)
    {
        const DesignId existing = root.get_design_by_name(name);
        if (!existing.valid())
            return root.create_design(DesignData{.library = library_id, .name = name});

        if (!caller.empty())
        {
            const DesignData *design = root.get_design(existing);
            if (design && design->library != library_id)
            {
                const LibraryData *existing_library = root.get_library(design->library);
                const LibraryData *requested_library = root.get_library(library_id);
                spdlog::warn("{}: design {} already exists in library {} - adding this view to it there, not to library {}",
                             caller, name, existing_library ? existing_library->name : "?",
                             requested_library ? requested_library->name : "?");
            }
        }
        return existing;
    }
}
