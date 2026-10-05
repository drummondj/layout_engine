#include "../extension_index.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace le;

namespace
{
    class ExtensionIndexTest : public ::testing::Test
    {
    protected:
        std::filesystem::path dir = std::filesystem::temp_directory_path() / ("le_extension_index_" + std::to_string(::getpid()));

        void SetUp() override
        {
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir / "ext/hello_ext/tcl");
            std::ofstream(dir / "ext/hello_ext/tcl/hello.tcl") << "";
            std::ofstream(dir / "init.tcl") << "";
        }
        void TearDown() override { std::filesystem::remove_all(dir); }

        std::filesystem::path write(const std::string &json)
        {
            const auto path = dir / "extensions.json";
            std::ofstream(path) << json;
            return path;
        }

        static std::string index(const std::string &version = "0.2.0", int api = 1, const std::string &tier = "compiled",
                                 const std::string &extra = "")
        {
            return R"({"format": 1, "layout_engine": ")" + version + R"(", "extension_api": )" + std::to_string(api) +
                   R"(, "extensions": [{"name": "hello_ext", "version": "0.1.0", "tier": ")" + tier +
                   R"(", "dir": "ext/hello_ext", "procs": ["tcl/hello.tcl"]}])" + extra + "}";
        }
    };
}

TEST_F(ExtensionIndexTest, ResolvesProcsAndStartupRelativeToTheIndex)
{
    const auto read = read_extension_index(write(index("0.2.0", 1, "compiled", R"(, "startup": "init.tcl")")), "0.2.0", 1, {"hello_ext"});
    ASSERT_TRUE(read.has_value()) << read.error();
    ASSERT_EQ(read->extensions.size(), 1u);
    EXPECT_TRUE(read->extensions[0].compiled);
    EXPECT_EQ(read->extensions[0].procs, std::vector<std::filesystem::path>{dir / "ext/hello_ext/tcl/hello.tcl"});
    EXPECT_EQ(read->startup, dir / "init.tcl");
}

TEST_F(ExtensionIndexTest, RefusesAnotherBuildsIndex)
{
    EXPECT_NE(read_extension_index(write(index("0.3.0")), "0.2.0", 1, {"hello_ext"}).error().find("written for layout_engine 0.3.0"), std::string::npos);
    EXPECT_NE(read_extension_index(write(index("0.2.0", 2)), "0.2.0", 1, {"hello_ext"}).error().find("extension API 2"), std::string::npos);
    // A compiled extension the binary doesn't have, or one the index lacks.
    EXPECT_NE(read_extension_index(write(index()), "0.2.0", 1, {}).error().find("wasn't built with"), std::string::npos);
    EXPECT_NE(read_extension_index(write(index("0.2.0", 1, "script")), "0.2.0", 1, {"other_ext"}).error().find("doesn't list other_ext"),
              std::string::npos);
}

TEST_F(ExtensionIndexTest, AScriptExtensionNeedsNothingCompiledIn)
{
    const auto read = read_extension_index(write(index("0.2.0", 1, "script")), "0.2.0", 1, {});
    ASSERT_TRUE(read.has_value()) << read.error();
    EXPECT_FALSE(read->extensions[0].compiled);
}

TEST_F(ExtensionIndexTest, ReportsMissingFilesAndBadJson)
{
    std::filesystem::remove(dir / "ext/hello_ext/tcl/hello.tcl");
    EXPECT_NE(read_extension_index(write(index()), "0.2.0", 1, {"hello_ext"}).error().find("doesn't exist"), std::string::npos);
    EXPECT_FALSE(read_extension_index(write("{not json"), "0.2.0", 1, {}).has_value());
    EXPECT_FALSE(read_extension_index(dir / "missing.json", "0.2.0", 1, {}).has_value());
}
