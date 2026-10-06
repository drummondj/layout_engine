#pragma once
// extensions.json: the extensions a le_shell bundle or build tree provides,
// in load order - which procs files le_shell sources after le_tcl_procs.tcl,
// and an optional project startup script. Written by CMake
// (cmake/le_extensions.cmake) for the build tree and for the installed
// bundle. Format (docs/PACKAGE_MANAGER_RESEARCH.md §8):
//
//   {"format": 1, "layout_engine": "0.2.0", "extension_api": 1,
//    "extensions": [{"name": "hello_ext", "version": "0.1.0", "tier": "compiled",
//                    "dir": "ext/hello_ext", "procs": ["tcl/hello_ext.tcl"]}],
//    "startup": "init.tcl"}
//
// "dir" is relative to the index file's directory (or absolute); "procs"
// are relative to "dir"; "startup" is optional, relative to the index file.

#include <json.hpp>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace le
{
    struct ExtensionIndexEntry
    {
        std::string name;
        std::string version;
        bool compiled = false;
        std::filesystem::path directory;
        std::vector<std::filesystem::path> procs; // absolute, in source order
    };

    struct ExtensionIndex
    {
        std::vector<ExtensionIndexEntry> extensions;
        std::optional<std::filesystem::path> startup;
    };

    /// @brief Reads and checks `file` against the running binary: its
    /// layout_engine version and extension API, and `compiled` (the names of
    /// the compiled extensions the binary was built with, le_extension_name)
    /// - every compiled entry must be in the binary and every compiled
    /// extension in the index, so a stale index can't load another build's
    /// procs. Every procs file must exist. Returns why not on failure.
    inline std::expected<ExtensionIndex, std::string> read_extension_index(const std::filesystem::path &file, std::string_view layout_engine_version,
                                                                         int extension_api, const std::vector<std::string> &compiled)
    {
        const std::string where = file.string();
        std::ifstream in(file);
        if (!in)
            return std::unexpected(where + ": can't be read");
        nlohmann::json j;
        try
        {
            in >> j;
        }
        catch (const nlohmann::json::exception &e)
        {
            return std::unexpected(where + ": " + e.what());
        }

        try
        {
            if (j.value("format", 0) != 1)
                return std::unexpected(where + ": unsupported format " + j.value("format", nlohmann::json()).dump());
            const std::string version = j.at("layout_engine").get<std::string>();
            if (version != layout_engine_version)
                return std::unexpected(where + ": written for layout_engine " + version + ", this is " + std::string(layout_engine_version));
            const int api = j.at("extension_api").get<int>();
            if (api != extension_api)
                return std::unexpected(where + ": written for extension API " + std::to_string(api) + ", this build provides " +
                                       std::to_string(extension_api));

            const std::filesystem::path base = file.parent_path();
            ExtensionIndex index;
            for (const auto &e : j.at("extensions"))
            {
                ExtensionIndexEntry entry;
                entry.name = e.at("name").get<std::string>();
                entry.version = e.at("version").get<std::string>();
                const std::string tier = e.at("tier").get<std::string>();
                if (tier != "compiled" && tier != "script")
                    return std::unexpected(where + ": extension " + entry.name + " has unknown tier " + tier);
                entry.compiled = tier == "compiled";
                if (entry.compiled && std::ranges::find(compiled, entry.name) == compiled.end())
                    return std::unexpected(where + ": lists compiled extension " + entry.name + ", which this binary wasn't built with");
                const std::filesystem::path dir = base / e.at("dir").get<std::string>();
                entry.directory = dir;
                for (const auto &p : e.value("procs", nlohmann::json::array()))
                {
                    const std::filesystem::path procs = dir / p.get<std::string>();
                    if (!std::filesystem::exists(procs))
                        return std::unexpected(where + ": extension " + entry.name + "'s procs file " + procs.string() + " doesn't exist");
                    entry.procs.push_back(procs);
                }
                index.extensions.push_back(std::move(entry));
            }
            for (const std::string &name : compiled)
                if (std::ranges::none_of(index.extensions, [&](const ExtensionIndexEntry &e) { return e.name == name; }))
                    return std::unexpected(where + ": doesn't list " + name + ", which this binary was built with - "
                                                   "rebuild le_shell if its extensions changed, or pass the matching -extensions");
            if (j.contains("startup"))
            {
                index.startup = base / j.at("startup").get<std::string>();
                if (!std::filesystem::exists(*index.startup))
                    return std::unexpected(where + ": startup script " + index.startup->string() + " doesn't exist");
            }
            return index;
        }
        catch (const nlohmann::json::exception &e)
        {
            return std::unexpected(where + ": " + e.what());
        }
    }
}
