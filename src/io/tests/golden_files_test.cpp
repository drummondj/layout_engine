#include "codec.hpp"
#include "native_format.hpp"
#include "generated/database/schema_version.hpp"

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

        // complete.5.8.lef: RX's PITCH 1.8, at 20000 database units per micron.
        constexpr int64_t kRxPitch = 36000;

        std::vector<uint8_t> read_bytes(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }

        struct Sample
        {
            std::string name;
            std::function<void(Root &)> build;
            /// Spot values the file must still hold after loading with this
            /// build, whichever version wrote it.
            std::function<void(const Root &)> check;
        };

        /// The Schematic of Design `name`, or an invalid id.
        SchematicId schematic_of(const Root &root, const std::string &name)
        {
            const DesignId design = root.get_design_by_name(name);
            return design.valid() ? root.get_design_schematic(design) : SchematicId{};
        }

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
                 },
                 [](const Root &root) {
                     const LayerData *rx = root.get_layer(root.get_layer_by_name("RX"));
                     ASSERT_NE(rx, nullptr);
                     EXPECT_EQ(rx->type, "ROUTING");
                     EXPECT_EQ(rx->pitch, kRxPitch);
                     const DesignId design = root.get_design_by_name("design"); // the DEF's DESIGN
                     ASSERT_TRUE(design.valid());
                     const PlacementData *i1 = root.get_placement(root.get_placement_by_name(root.get_design_layout(design), "I1"));
                     ASSERT_NE(i1, nullptr);
                     EXPECT_EQ(root.get_design(i1->reference_design)->name, "B");
                     // PLACED ( 100 100 ) at the DEF's 1000 units, rescaled to the LEF's 20000.
                     ASSERT_TRUE(i1->location.has_value());
                     EXPECT_EQ(i1->location->x, 2000);
                     EXPECT_EQ(i1->location->y, 2000);
                     EXPECT_EQ(i1->weight, 100.0);
                 }},
                {"testcell",
                 [](Root &root) {
                     ASSERT_EQ(LEFReader().read_lef(std::string(API_TEST_FIXTURES_DIR) + "/testcell.lef", root, "testcell"), 0);
                     ASSERT_EQ(DEFReader().read_def(std::string(API_TEST_FIXTURES_DIR) + "/testcell.def", root, "testcell"), 0);
                 },
                 [](const Root &root) {
                     const LayerData *m1 = root.get_layer(root.get_layer_by_name("M1"));
                     ASSERT_NE(m1, nullptr);
                     EXPECT_EQ(m1->width, 1000);
                     EXPECT_EQ(m1->pitch, 2000);
                     EXPECT_EQ(m1->direction, RoutingDirection::H);
                     const AbstractId cell = root.get_design_abstract(root.get_design_by_name("TESTCELL"));
                     ASSERT_TRUE(cell.valid());
                     const std::optional<Point> size = root.get_abstract(cell)->size;
                     ASSERT_TRUE(size.has_value());
                     EXPECT_EQ(size->x, 10000);
                     EXPECT_EQ(size->y, 10000);
                     const TerminalData *a = root.get_terminal(root.get_terminal_by_name(cell, "A"));
                     ASSERT_NE(a, nullptr);
                     EXPECT_EQ(a->direction, SignalDirection::INPUT);
                 }},
                {"netlist",
                 [](Root &root) { ASSERT_EQ(SVReader().read_netlist({std::string(IO_TEST_FIXTURES_DIR) + "/gate_netlist_clean.v"}, root, "sv_lib"), 0); },
                 [](const Root &root) {
                     const SchematicId top = schematic_of(root, "top");
                     ASSERT_TRUE(top.valid());
                     EXPECT_TRUE(root.get_net_by_name(top, "clk").valid());
                     const InstanceData *and0 = root.get_instance(root.get_instance_by_name(top, "u_and0"));
                     ASSERT_NE(and0, nullptr);
                     EXPECT_EQ(root.get_design(and0->reference_design)->name, "AND2");
                 }},
                // read_rtl keeps what it can't elaborate as source text.
                {"rtl",
                 [](Root &root) {
                     ASSERT_EQ(SVReader().read_rtl({std::string(IO_TEST_FIXTURES_DIR) + "/rtl_invalid_body.sv"}, root, "rtl_lib"), 0);
                     // No machine's paths in a committed file.
                     for (const InstanceId id : root.get_instance_ids())
                         for (auto *text : {&root.get_instance(id)->source_file, &root.get_instance(id)->diagnostic_summary})
                             if (text->has_value())
                                 for (size_t at; (at = (*text)->find(IO_TEST_FIXTURES_DIR "/")) != std::string::npos;)
                                     (*text)->erase(at, std::string_view(IO_TEST_FIXTURES_DIR "/").size());
                 },
                 [](const Root &root) {
                     EXPECT_TRUE(root.get_port_by_name(schematic_of(root, "BUFX1"), "A").valid());
                     const SchematicId spike = schematic_of(root, "spike_mixed");
                     ASSERT_TRUE(spike.valid());
                     bool kept_always_ff = false;
                     for (const InstanceId id : root.get_schematic_instances(spike))
                     {
                         const InstanceData *instance = root.get_instance(id);
                         if (instance->rtl_text && instance->rtl_text->find("always_ff") != std::string::npos)
                             kept_always_ff = instance->diagnostic_summary.has_value();
                     }
                     EXPECT_TRUE(kept_always_ff);
                 }},
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

        /// The sample a golden file was written from (its name before ".c<container>.led").
        const Sample *sample_named(const std::string &file)
        {
            static const std::vector<Sample> all = samples();
            for (const Sample &sample : all)
                if (file.starts_with(sample.name + ".c"))
                    return &sample;
            return nullptr;
        }
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

                if (const Sample *sample = sample_named(name))
                    sample->check(root);

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
        EXPECT_GE(files, 4);
    }
}
