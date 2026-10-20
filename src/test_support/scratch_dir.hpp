#pragma once

// Scratch files for tests. ctest runs every discovered GTest as its own
// process, in parallel, and several build trees may run their tests at
// once, so a scratch file under a fixed name in the temp directory is
// shared between tests that overwrite and read it concurrently. Each
// process gets its own directory instead: unique while the process lives,
// emptied on first use (a pid can be reused) and removed at exit.
// scripts/check_test_scratch_paths.py keeps tests from naming files in the
// temp directory any other way.

#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>

namespace le::test
{
    /// @brief This process's scratch directory, created on first use.
    inline const std::filesystem::path &scratch_dir()
    {
        struct Dir
        {
            std::filesystem::path path = std::filesystem::temp_directory_path() / ("le_test_" + std::to_string(::getpid()));
            Dir()
            {
                std::filesystem::remove_all(path);
                std::filesystem::create_directories(path);
            }
            ~Dir()
            {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        };
        static const Dir dir;
        return dir.path;
    }

    /// @brief `name` (which may have subdirectories, created here) in this process's scratch directory.
    inline std::string scratch_path(std::string_view name)
    {
        const std::filesystem::path path = scratch_dir() / name;
        std::filesystem::create_directories(path.parent_path());
        return path.string();
    }
}
