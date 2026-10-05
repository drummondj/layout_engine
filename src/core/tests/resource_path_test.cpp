#include "core/resource_path.hpp"

#include <gtest/gtest.h>

#include <filesystem>

using namespace le;

namespace
{
    // This test binary's own file name - a file known to exist beside the
    // running executable.
    std::string own_exe_name()
    {
        return std::filesystem::read_symlink("/proc/self/exe").filename().string();
    }
}

TEST(ResourcePath, ExecutableDirIsTheRunningBinarysDirectory)
{
    EXPECT_EQ(executable_dir(), std::filesystem::read_symlink("/proc/self/exe").parent_path().string());
}

TEST(ResourcePath, PrefersBesideTheExecutable)
{
    // Both exist: the copy beside the executable wins, so an installed
    // bundle never picks up a build tree's files.
    const std::string build_path = std::filesystem::read_symlink("/proc/self/exe").string();
    const auto found = find_resource(build_path, own_exe_name());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, executable_dir() + "/" + own_exe_name());
}

TEST(ResourcePath, FallsBackToTheBuildPath)
{
    const std::string build_path = std::filesystem::read_symlink("/proc/self/exe").string();
    EXPECT_EQ(find_resource(build_path, "no_such_file"), build_path);
}

TEST(ResourcePath, ReportsEveryPathTriedWhenNothingExists)
{
    const auto found = find_resource("/no/such/build/path", "fonts/no_such_font.ttf");
    ASSERT_FALSE(found.has_value());
    EXPECT_EQ(found.error(), (std::vector<std::string>{executable_dir() + "/fonts/no_such_font.ttf", "/no/such/build/path"}));
    EXPECT_EQ(quoted_paths(found.error()), "'" + executable_dir() + "/fonts/no_such_font.ttf', '/no/such/build/path'");
}
