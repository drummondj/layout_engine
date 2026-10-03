#pragma once

#include <expected>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>

#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

namespace le
{
    /// @brief The running executable's directory, or "" if it can't be
    /// determined (always "" off Linux).
    inline std::string executable_dir()
    {
#if defined(__linux__)
        char buf[PATH_MAX];
        const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len <= 0)
            return {};
        buf[len] = '\0';
        const std::string exe_path(buf);
        const size_t slash = exe_path.find_last_of('/');
        if (slash == std::string::npos)
            return ".";
        return slash == 0 ? "/" : exe_path.substr(0, slash);
#else
        return {};
#endif
    }

    /// @brief Finds a file shipped with the application: `build_path` (a
    /// compile-time path into the build tree, valid for a local run) if it
    /// exists, else `exe_relative` beside the running executable, where a
    /// release bundle puts it. Returns the path found, or every path tried.
    inline std::expected<std::string, std::vector<std::string>> find_resource(const std::string &build_path, const std::string &exe_relative)
    {
        struct stat st{};
        if (stat(build_path.c_str(), &st) == 0)
            return build_path;
        std::vector<std::string> tried{build_path};
        if (const std::string dir = executable_dir(); !dir.empty())
        {
            std::string candidate = dir + "/" + exe_relative;
            if (stat(candidate.c_str(), &st) == 0)
                return candidate;
            tried.push_back(std::move(candidate));
        }
        return std::unexpected(std::move(tried));
    }

    /// @brief `find_resource`'s tried paths as "'a', 'b'", for an error.
    inline std::string quoted_paths(const std::vector<std::string> &paths)
    {
        std::string text;
        for (const std::string &path : paths)
        {
            if (!text.empty())
                text += ", ";
            text += "'" + path + "'";
        }
        return text;
    }
}
