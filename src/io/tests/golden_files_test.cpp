#include "codec.hpp"
#include "native_format.hpp"
#include "schema_version.hpp"

#include "def_reader.hpp"
#include "lef_reader.hpp"
#include "sv_reader.hpp"

#include <gtest/gtest.h>
#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>

// The golden-file corpus (docs/NATIVE_FILE_FORMAT_RESEARCH.md §4.6): every
// schema version's sample .led files, checked in under
// golden/<schema version>/, must load with every later build. This is
// what backs "a build can always read files written by any older
// version" with evidence rather than a promise.
//
// When the schema version is bumped, add that version's files:
//
//   ./build/backend_tests --gtest_also_run_disabled_tests \
//       --gtest_filter='GoldenFiles.DISABLED_WriteForCurrentSchemaVersion'
//
// and commit golden/<new version>/ alongside the schema change. Files are
// written uncompressed (so they don't depend on zstd's exact output) and
// named <sample>.c<container version>.led.

namespace le::persistence
{
    namespace
    {
        namespace fs = std::filesystem;

        const fs::path kGoldenDir = fs::path(NATIVE_FORMAT_GOLDEN_DIR);

        std::vector<uint8_t> read_bytes(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }

        struct Sample
        {
            std::string name;
            std::function<void(Root &)> build;
        };

        std::vector<Sample> samples()
        {
            return {
                {"lef_def",
                 [](Root &root) {
                     ASSERT_EQ(LEFReader().read_lef(std::string(LEFDEF_TEST_DIR) + "/complete.5.8.lef", root, "lef_lib"), 0);
                     const TechnologyId technology = root.get_technology_ids().front();
                     for (const char *name : {"M1", "M2", "M3", "METAL1", "V1"})
                         if (!root.get_layer_by_name(name).valid())
                             root.create_layer(LayerData{.technology = technology, .name = name, .type = "ROUTING"});
                     const LibraryId library = root.create_library(LibraryData{.name = "def_lib"});
                     for (const char *name : {"A", "B", "CHK3A"})
                         root.create_design(DesignData{.library = library, .name = name});
                     ASSERT_EQ(DEFReader().read_def(std::string(DEF_TEST_DIR) + "/complete.5.8.def", root, "def_lib"), 0);
                 }},
                {"testcell",
                 [](Root &root) {
                     ASSERT_EQ(LEFReader().read_lef(std::string(API_TEST_FIXTURES_DIR) + "/testcell.lef", root, "testcell"), 0);
                     ASSERT_EQ(DEFReader().read_def(std::string(API_TEST_FIXTURES_DIR) + "/testcell.def", root, "testcell"), 0);
                 }},
                {"netlist",
                 [](Root &root) { ASSERT_EQ(SVReader().read_netlist({std::string(IO_TEST_FIXTURES_DIR) + "/gate_netlist_clean.v"}, root, "sv_lib"), 0); }},
            };
        }

        std::map<std::string, uint64_t> class_counts(const Root &root)
        {
            std::map<std::string, uint64_t> counts;
            nt::for_each_pooled([&]<class P>(P) {
                if (const size_t n = P::pool(root).alive_count())
                    counts[std::string(P::name)] = n;
            });
            return counts;
        }

        std::string file_name(const std::string &sample) { return sample + ".c" + std::to_string(kContainerVersion) + ".led"; }
    }

    TEST(GoldenFiles, DISABLED_WriteForCurrentSchemaVersion)
    {
        const fs::path dir = kGoldenDir / std::string(schema_info::kVersion);
        fs::create_directories(dir);
        nlohmann::json manifest = nlohmann::json::object();
        const fs::path manifest_path = dir / "manifest.json";
        if (fs::exists(manifest_path))
            manifest = nlohmann::json::parse(std::ifstream(manifest_path));
        for (const Sample &sample : samples())
        {
            Root root;
            sample.build(root);
            const std::string name = file_name(sample.name);
            const SaveReport report = save_native(root, (dir / name).string(), SaveOptions{.compression_level = 0});
            ASSERT_TRUE(report.ok()) << report.error;
            manifest[name] = class_counts(root);
        }
        std::ofstream(manifest_path) << manifest.dump(2) << "\n";
    }

    TEST(GoldenFiles, EveryVersionsFilesStillLoad)
    {
        ASSERT_TRUE(fs::is_directory(kGoldenDir)) << kGoldenDir;
        ASSERT_TRUE(fs::exists(kGoldenDir / std::string(schema_info::kVersion) / "manifest.json"))
            << "no golden files for the current schema version " << schema_info::kVersion
            << " - run the DISABLED_WriteForCurrentSchemaVersion test (see this file's header) and commit them";

        int files = 0;
        for (const auto &version_dir : fs::directory_iterator(kGoldenDir))
        {
            if (!version_dir.is_directory())
                continue;
            const std::string version = version_dir.path().filename().string();
            const auto manifest = nlohmann::json::parse(std::ifstream(version_dir.path() / "manifest.json"));
            for (const auto &[name, expected_counts] : manifest.items())
            {
                SCOPED_TRACE(version + "/" + name);
                const fs::path path = version_dir.path() / name;
                ASSERT_TRUE(fs::exists(path));
                ++files;

                Root root;
                const LoadReport report = load_native(root, path.string());
                ASSERT_TRUE(report.ok()) << report.error;
                EXPECT_EQ(report.file_schema_version, version);

                // Every class this build still has keeps every object.
                const auto counts = class_counts(root);
                for (const auto &[klass, rows] : expected_counts.items())
                {
                    bool exists = false;
                    nt::for_each_pooled([&]<class P>(P) { exists = exists || P::name == klass; });
                    if (exists)
                        EXPECT_EQ(counts.contains(klass) ? counts.at(klass) : 0u, rows.get<uint64_t>()) << klass;
                }

                // A file of the current version and container re-saves
                // byte-identically: the encoding hasn't drifted.
                if (version == schema_info::kVersion && name.ends_with(".c" + std::to_string(kContainerVersion) + ".led"))
                {
                    const fs::path resaved = fs::temp_directory_path() / ("le_golden_resave_" + name);
                    ASSERT_TRUE(save_native(root, resaved.string(), SaveOptions{.compression_level = 0}).ok());
                    EXPECT_EQ(read_bytes(resaved), read_bytes(path)) << "the writer's output changed for an unchanged schema - a format change needs a "
                                                                        "container or schema version bump, not a silent re-encoding";
                    fs::remove(resaved);
                }
            }
        }
        EXPECT_GE(files, 3);
    }
}
