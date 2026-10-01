#include "byte_io.hpp"
#include "codec.hpp"
#include "native_format.hpp"
#include "schema_version.hpp"

#include "def_reader.hpp"
#include "lef_reader.hpp"
#include "sv_reader.hpp"

#include <gtest/gtest.h>
#include <json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

// Tests for the native .led database file.

namespace le::persistence
{
    namespace
    {
        namespace fs = std::filesystem;

        std::vector<uint8_t> read_bytes(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }

        void write_bytes(const fs::path &path, const std::vector<uint8_t> &bytes)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        class TempDir
        {
        public:
            TempDir()
            {
                path_ = fs::temp_directory_path() / ("le_native_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++));
                fs::create_directories(path_);
            }
            ~TempDir() { fs::remove_all(path_); }
            fs::path file(const std::string &name) const { return path_ / name; }

        private:
            fs::path path_;
            static inline int counter_ = 0;
        };

        // An independent reader/writer for uncompressed files, written from
        // the layout comment in native_format.hpp - so tests can edit a
        // file's embedded schema, columns and strings the way an older
        // build would have written them.
        struct Block
        {
            std::vector<uint8_t> bytes;
        };

        Block read_raw_block(ByteReader &r)
        {
            EXPECT_EQ(r.u8(), 0) << "tests edit uncompressed files only";
            const auto raw_size = r.fixed<uint64_t>();
            const auto stored_size = r.fixed<uint64_t>();
            r.fixed<uint32_t>();
            EXPECT_EQ(raw_size, stored_size);
            const uint8_t *p = r.raw(stored_size);
            return {{p, p + stored_size}};
        }

        void write_raw_block(ByteWriter &w, const Block &block)
        {
            w.u8(0);
            w.fixed<uint64_t>(block.bytes.size());
            w.fixed<uint64_t>(block.bytes.size());
            w.fixed<uint32_t>(crc32(block.bytes.data(), block.bytes.size()));
            w.raw(block.bytes.data(), block.bytes.size());
        }

        struct EditableClass
        {
            std::string name;
            uint64_t rows = 0;
            std::vector<std::pair<std::string, Block>> columns;
        };

        struct EditableFile
        {
            uint32_t container_version = 0;
            nlohmann::json schema;
            std::vector<EditableClass> classes;
            std::vector<std::string> strings;

            static EditableFile read(const fs::path &path)
            {
                const auto bytes = read_bytes(path);
                ByteReader r(bytes.data(), bytes.size(), "test");
                r.raw(8);
                EditableFile file;
                file.container_version = r.fixed<uint32_t>();
                r.fixed<uint32_t>();
                while (!r.at_end())
                {
                    std::string tag(reinterpret_cast<const char *>(r.raw(4)), 4);
                    const auto size = r.fixed<uint64_t>();
                    ByteReader payload(r.raw(size), size, tag);
                    if (tag == "SCHM")
                    {
                        const Block block = read_raw_block(payload);
                        file.schema = nlohmann::json::parse(block.bytes.begin(), block.bytes.end());
                    }
                    else if (tag == "CLAS")
                    {
                        EditableClass klass;
                        klass.name = std::string(payload.string());
                        klass.rows = payload.fixed<uint64_t>();
                        const auto columns = payload.fixed<uint32_t>();
                        for (uint32_t c = 0; c < columns; ++c)
                        {
                            // Segments hold consecutive rows, so their raw
                            // bytes concatenate into the whole column.
                            std::string name(payload.string());
                            Block column;
                            const auto segments = payload.fixed<uint32_t>();
                            for (uint32_t i = 0; i < segments; ++i)
                            {
                                payload.fixed<uint64_t>();
                                const Block segment = read_raw_block(payload);
                                column.bytes.insert(column.bytes.end(), segment.bytes.begin(), segment.bytes.end());
                            }
                            klass.columns.emplace_back(std::move(name), std::move(column));
                        }
                        file.classes.push_back(std::move(klass));
                    }
                    else if (tag == "STRS")
                    {
                        const Block block = read_raw_block(payload);
                        ByteReader s(block.bytes.data(), block.bytes.size(), "strings");
                        const uint64_t n = s.varint();
                        for (uint64_t i = 0; i < n; ++i)
                            file.strings.emplace_back(s.string());
                    }
                }
                return file;
            }

            void write(const fs::path &path) const
            {
                ByteWriter w;
                w.raw("LEDB\r\n\x1a\n", 8);
                w.fixed<uint32_t>(container_version);
                w.fixed<uint32_t>(0);
                auto chunk = [&](const char *tag, const ByteWriter &payload) {
                    w.raw(tag, 4);
                    w.fixed<uint64_t>(payload.size());
                    w.raw(payload.bytes().data(), payload.size());
                };
                {
                    const std::string json = schema.dump();
                    ByteWriter payload;
                    write_raw_block(payload, Block{{json.begin(), json.end()}});
                    chunk("SCHM", payload);
                }
                for (const EditableClass &klass : classes)
                {
                    ByteWriter payload;
                    payload.string(klass.name);
                    payload.fixed<uint64_t>(klass.rows);
                    payload.fixed<uint32_t>(static_cast<uint32_t>(klass.columns.size()));
                    for (const auto &[name, block] : klass.columns)
                    {
                        payload.string(name);
                        payload.fixed<uint32_t>(1); // one segment holding every row
                        payload.fixed<uint64_t>(klass.rows);
                        write_raw_block(payload, block);
                    }
                    chunk("CLAS", payload);
                }
                {
                    ByteWriter table;
                    table.varint(strings.size());
                    for (const std::string &s : strings)
                        table.string(s);
                    ByteWriter payload;
                    write_raw_block(payload, Block{table.bytes()});
                    chunk("STRS", payload);
                }
                chunk("END ", ByteWriter{});
                write_bytes(path, w.bytes());
            }

            nlohmann::json &descriptor_class(const std::string &name)
            {
                for (auto &klass : schema["core"]["descriptor"]["classes"])
                    if (klass["name"] == name)
                        return klass;
                throw std::runtime_error("no class " + name);
            }

            EditableClass &stored_class(const std::string &name)
            {
                for (auto &klass : classes)
                    if (klass.name == name)
                        return klass;
                throw std::runtime_error("no stored class " + name);
            }
        };

        /// A small hand-built database: one technology with three layers,
        /// a library with a design, and the design's schematic with two nets.
        struct SmallDesign
        {
            Root root;
            TechnologyId technology;
            LayerId m1, m2, m3;
            LibraryId library;
            DesignId design;
            SchematicId schematic;

            SmallDesign()
            {
                technology = root.create_technology(TechnologyData{.database_units_microns = 2000.0, .capacitance_units_pf = 1.5});
                m1 = root.create_layer(LayerData{.technology = technology, .name = "M1", .type = "ROUTING", .direction = RoutingDirection::H, .width = 140});
                m2 = root.create_layer(LayerData{.technology = technology, .name = "M2", .type = "ROUTING", .direction = RoutingDirection::V, .pitch = 380});
                m3 = root.create_layer(LayerData{.technology = technology, .name = "V1", .type = "CUT"});
                library = root.create_library(LibraryData{.name = "lib"});
                design = root.create_design(DesignData{.library = library, .name = "top"});
                schematic = root.create_schematic(SchematicData{.design = design});
                root.create_net(NetData{.schematic = schematic, .name = "clk"});
                root.create_net(NetData{.schematic = schematic, .name = "rst", .bit_index = 3});
            }
        };

        bool contains(const std::vector<std::string> &messages, const std::string &needle)
        {
            return std::any_of(messages.begin(), messages.end(), [&](const std::string &m) { return m.find(needle) != std::string::npos; });
        }
    }

    // --- Round trips -------------------------------------------------------------

    TEST(NativeFormat, SmallDesignRoundTripsValuesIndexesAndOptionals)
    {
        TempDir dir;
        SmallDesign original;
        const auto path = dir.file("small.led");
        const SaveReport saved = save_native(original.root, path.string());
        ASSERT_TRUE(saved.ok()) << saved.error;
        EXPECT_EQ(saved.objects, 9u);
        EXPECT_EQ(saved.dangling_references, 0u);

        Root loaded;
        const LoadReport report = load_native(loaded, path.string());
        ASSERT_TRUE(report.ok()) << report.error;
        EXPECT_TRUE(report.schema_matches);
        EXPECT_TRUE(report.warnings.empty());
        EXPECT_EQ(report.file_schema_version, schema_info::kVersion);

        const auto technologies = loaded.get_technology_ids();
        ASSERT_EQ(technologies.size(), 1u);
        const TechnologyData *technology = loaded.get_technology(technologies.front());
        EXPECT_DOUBLE_EQ(technology->database_units_microns, 2000.0);
        EXPECT_EQ(technology->capacitance_units_pf, 1.5);
        EXPECT_FALSE(technology->resistance_units_ohms.has_value());

        const auto &layers = loaded.get_technology_layers(technologies.front());
        ASSERT_EQ(layers.size(), 3u);
        const LayerData *m1 = loaded.get_layer(loaded.get_layer_by_name("M1"));
        ASSERT_NE(m1, nullptr);
        EXPECT_EQ(m1->direction, RoutingDirection::H);
        EXPECT_EQ(m1->width, 140);
        EXPECT_FALSE(m1->pitch.has_value());
        EXPECT_EQ(loaded.get_layer(layers[1])->name, "M2");
        EXPECT_EQ(loaded.get_layer(layers[1])->pitch, 380);

        const DesignId design = loaded.get_design_ids().front();
        EXPECT_EQ(loaded.get_design(design)->name, "top");
        EXPECT_EQ(loaded.get_library(loaded.get_design(design)->library)->name, "lib");
        EXPECT_EQ(loaded.get_library_designs(loaded.get_design(design)->library), std::vector<DesignId>{design});

        const SchematicId schematic = loaded.get_schematic_ids().front();
        const NetId rst = loaded.get_net_by_name(schematic, "rst");
        ASSERT_TRUE(rst.valid());
        EXPECT_EQ(loaded.get_net(rst)->bit_index, 3);
        EXPECT_FALSE(loaded.get_net(loaded.get_net_by_name(schematic, "clk"))->bit_index.has_value());
        EXPECT_EQ(loaded.get_schematic_nets(schematic).size(), 2u);
    }

    TEST(NativeFormat, ChildListOrderSurvivesDeletedAndReusedSlots)
    {
        TempDir dir;
        SmallDesign original;
        // Delete M2 and create M4, which reuses M2's slot: the list is
        // M1, V1, M4, but slot order would be M1, M4, V1.
        ASSERT_TRUE(original.root.delete_layer(original.m2));
        const LayerId m4 = original.root.create_layer(LayerData{.technology = original.technology, .name = "M4", .type = "ROUTING"});
        ASSERT_EQ(m4.index, original.m2.index);

        const auto path = dir.file("order.led");
        ASSERT_TRUE(save_native(original.root, path.string()).ok());
        Root loaded;
        ASSERT_TRUE(load_native(loaded, path.string()).ok());

        std::vector<std::string> names;
        for (LayerId id : loaded.get_technology_layers(loaded.get_technology_ids().front()))
            names.push_back(loaded.get_layer(id)->name);
        EXPECT_EQ(names, (std::vector<std::string>{"M1", "V1", "M4"}));
    }

    TEST(NativeFormat, CompleteLefDefAndVerilogRoundTripByteIdentically)
    {
        TempDir dir;
        Root original;
        ASSERT_EQ(LEFReader().read_lef(std::string(LEFDEF_TEST_DIR) + "/complete.5.8.lef", original, "lef_lib"), 0);
        // complete.5.8.def names layers/macros the LEF above doesn't have
        // (see def_reader_test.cpp's fixture) - add them as that test does.
        const TechnologyId technology = original.get_technology_ids().front();
        for (const char *name : {"M1", "M2", "M3", "METAL1", "V1"})
            if (!original.get_layer_by_name(name).valid())
                original.create_layer(LayerData{.technology = technology, .name = name, .type = "ROUTING"});
        const LibraryId def_library = original.create_library(LibraryData{.name = "def_lib"});
        for (const char *name : {"A", "B", "CHK3A"})
            original.create_design(DesignData{.library = def_library, .name = name});
        ASSERT_EQ(DEFReader().read_def(std::string(DEF_TEST_DIR) + "/complete.5.8.def", original, "def_lib"), 0);
        ASSERT_EQ(SVReader().read_netlist({std::string(SV_TEST_FIXTURES_DIR) + "/gate_netlist_clean.v"}, original, "sv_lib"), 0);

        const auto first = dir.file("first.led");
        const SaveReport saved = save_native(original, first.string());
        ASSERT_TRUE(saved.ok()) << saved.error;

        const FileInfo info = inspect_native(first.string());
        ASSERT_TRUE(info.ok()) << info.error;
        EXPECT_GE(info.classes.size(), 40u) << "the fixtures should exercise most of the schema";

        Root loaded;
        const LoadReport report = load_native(loaded, first.string());
        ASSERT_TRUE(report.ok()) << report.error;
        EXPECT_EQ(report.objects, saved.objects);

        // Saving what was loaded must reproduce the file exactly: every
        // value, reference and child-list order survived.
        const auto second = dir.file("second.led");
        ASSERT_TRUE(save_native(loaded, second.string()).ok());
        EXPECT_EQ(read_bytes(first), read_bytes(second));
    }

    TEST(NativeFormat, ClassesLargerThanOneSegmentRoundTrip)
    {
        // 65536 rows per column segment: 150000 nets span three.
        TempDir dir;
        SmallDesign original;
        for (int i = 0; i < 150000; ++i)
            original.root.create_net(NetData{.schematic = original.schematic, .name = "n" + std::to_string(i), .bit_index = i % 7 ? std::optional<int>(i) : std::nullopt});
        const auto path = dir.file("big.led");
        ASSERT_TRUE(save_native(original.root, path.string()).ok());
        Root loaded;
        const LoadReport report = load_native(loaded, path.string());
        ASSERT_TRUE(report.ok()) << report.error;
        const SchematicId schematic = loaded.get_schematic_ids().front();
        ASSERT_EQ(loaded.get_schematic_nets(schematic).size(), 150002u);
        EXPECT_EQ(loaded.get_net(loaded.get_net_by_name(schematic, "n70001"))->bit_index, 70001);
        EXPECT_FALSE(loaded.get_net(loaded.get_net_by_name(schematic, "n140000"))->bit_index.has_value());
        EXPECT_EQ(loaded.get_net(loaded.get_schematic_nets(schematic).back())->name, "n149999");

        const auto again = dir.file("again.led");
        ASSERT_TRUE(save_native(loaded, again.string()).ok());
        EXPECT_EQ(read_bytes(path), read_bytes(again));
    }

    TEST(NativeFormat, CompressedAndUncompressedFilesLoadTheSame)
    {
        TempDir dir;
        SmallDesign original;
        ASSERT_TRUE(save_native(original.root, dir.file("z.led").string()).ok());
        ASSERT_TRUE(save_native(original.root, dir.file("raw.led").string(), SaveOptions{.compression_level = 0}).ok());
        Root a, b;
        ASSERT_TRUE(load_native(a, dir.file("z.led").string()).ok());
        ASSERT_TRUE(load_native(b, dir.file("raw.led").string()).ok());
        ASSERT_TRUE(save_native(a, dir.file("a.led").string()).ok());
        ASSERT_TRUE(save_native(b, dir.file("b.led").string()).ok());
        EXPECT_EQ(read_bytes(dir.file("a.led")), read_bytes(dir.file("b.led")));
    }

    TEST(NativeFormat, DanglingReferencesAreCountedAndWrittenAsUnset)
    {
        TempDir dir;
        SmallDesign original;
        original.root.get_design(original.design)->library = LibraryId{42, 0};
        const auto path = dir.file("dangling.led");
        const SaveReport saved = save_native(original.root, path.string());
        ASSERT_TRUE(saved.ok()) << saved.error;
        EXPECT_EQ(saved.dangling_references, 1u);

        Root loaded;
        ASSERT_TRUE(load_native(loaded, path.string()).ok());
        EXPECT_FALSE(loaded.get_design(loaded.get_design_ids().front())->library.valid());
    }

    TEST(NativeFormat, SaveReplacesAnExistingFileAndLeavesNoTemporary)
    {
        TempDir dir;
        SmallDesign original;
        const auto path = dir.file("design.led");
        write_bytes(path, {1, 2, 3});
        ASSERT_TRUE(save_native(original.root, path.string()).ok());
        EXPECT_TRUE(inspect_native(path.string()).ok());
        EXPECT_FALSE(fs::exists(dir.file("design.led.tmp")));
    }

    TEST(NativeFormat, SaveToAnUnwritableDirectoryFails)
    {
        SmallDesign original;
        const SaveReport report = save_native(original.root, "/nonexistent_dir/design.led");
        EXPECT_FALSE(report.ok());
        EXPECT_NE(report.error.find("cannot open"), std::string::npos);
    }

    // --- Corrupt and unreadable files --------------------------------------------

    class NativeFormatCorruption : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            SmallDesign original;
            path = dir.file("design.led");
            ASSERT_TRUE(save_native(original.root, path.string()).ok());
            bytes = read_bytes(path);
            target.create_library(LibraryData{.name = "untouched"});
        }

        LoadReport load_edited(const std::vector<uint8_t> &edited)
        {
            write_bytes(path, edited);
            return load_native(target, path.string());
        }

        void expect_target_untouched()
        {
            const auto libraries = target.get_library_ids();
            ASSERT_EQ(libraries.size(), 1u);
            EXPECT_EQ(target.get_library(libraries.front())->name, "untouched");
            EXPECT_TRUE(target.get_technology_ids().empty());
        }

        TempDir dir;
        fs::path path;
        std::vector<uint8_t> bytes;
        Root target;
    };

    TEST_F(NativeFormatCorruption, TruncatedFileIsRejected)
    {
        bytes.resize(bytes.size() - 5);
        const LoadReport report = load_edited(bytes);
        EXPECT_FALSE(report.ok());
        EXPECT_NE(report.error.find("truncated"), std::string::npos) << report.error;
        expect_target_untouched();
    }

    TEST_F(NativeFormatCorruption, FlippedByteFailsTheChecksum)
    {
        // Flip a byte in the middle of the file - inside some block's data.
        bytes[bytes.size() / 2] ^= 0x5a;
        const LoadReport report = load_edited(bytes);
        EXPECT_FALSE(report.ok());
        expect_target_untouched();
    }

    TEST_F(NativeFormatCorruption, NotALayoutEngineFile)
    {
        bytes[0] = 'X';
        const LoadReport report = load_edited(bytes);
        EXPECT_NE(report.error.find("bad magic"), std::string::npos) << report.error;
        expect_target_untouched();
    }

    TEST_F(NativeFormatCorruption, NewerContainerVersionIsRefused)
    {
        bytes[8] = static_cast<uint8_t>(kContainerVersion + 1);
        const LoadReport report = load_edited(bytes);
        EXPECT_NE(report.error.find("newer"), std::string::npos) << report.error;
        expect_target_untouched();
    }

    TEST_F(NativeFormatCorruption, MissingFile)
    {
        Root root;
        const LoadReport report = load_native(root, dir.file("absent.led").string());
        EXPECT_NE(report.error.find("cannot open"), std::string::npos) << report.error;
    }

    // --- Reading files written with a different schema ---------------------------
    //
    // Each test writes an uncompressed file with this build, then edits its
    // embedded schema/columns into what an older build would have written.

    class NativeFormatOlderSchema : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            SmallDesign original;
            path = dir.file("design.led");
            ASSERT_TRUE(save_native(original.root, path.string(), SaveOptions{.compression_level = 0}).ok());
            file = EditableFile::read(path);
            // An older file: its own version and fingerprint.
            file.schema["core"]["version"] = "0.0.1";
            file.schema["core"]["fingerprint"] = "0000000000000000";
        }

        LoadReport load()
        {
            file.write(path);
            return load_native(loaded, path.string());
        }

        TempDir dir;
        fs::path path;
        EditableFile file;
        Root loaded;
    };

    TEST_F(NativeFormatOlderSchema, UnchangedShapeLoadsWithoutWarnings)
    {
        const LoadReport report = load();
        ASSERT_TRUE(report.ok()) << report.error;
        EXPECT_FALSE(report.schema_matches);
        EXPECT_EQ(report.file_schema_version, "0.0.1");
        EXPECT_TRUE(report.warnings.empty());
        EXPECT_EQ(loaded.get_layer_ids().size(), 3u);
    }

    TEST_F(NativeFormatOlderSchema, FieldsAddedAndRemovedSinceAreMatchedByName)
    {
        // The older schema called Layer.type "legacy_type" - to this build
        // that's a removed field plus an added one.
        for (auto &field : file.descriptor_class("Layer")["fields"])
            if (field["name"] == "type")
                field["name"] = "legacy_type";
        for (auto &[name, block] : file.stored_class("Layer").columns)
            if (name == "type")
                name = "legacy_type";

        const LoadReport report = load();
        ASSERT_TRUE(report.ok()) << report.error;
        EXPECT_TRUE(contains(report.warnings, "Layer.legacy_type no longer exists")) << report.warnings.size();
        EXPECT_TRUE(contains(report.warnings, "Layer.type is not in the file"));
        const LayerData *m1 = loaded.get_layer(loaded.get_layer_by_name("M1"));
        ASSERT_NE(m1, nullptr);
        EXPECT_EQ(m1->type, "");
        EXPECT_EQ(m1->width, 140);
    }

    TEST_F(NativeFormatOlderSchema, ReorderedFieldsAndColumnsLoad)
    {
        auto &fields = file.descriptor_class("Layer")["fields"];
        std::reverse(fields.begin(), fields.end());
        auto &columns = file.stored_class("Layer").columns;
        std::reverse(columns.begin(), columns.end());

        const LoadReport report = load();
        ASSERT_TRUE(report.ok()) << report.error;
        EXPECT_TRUE(report.warnings.empty());
        EXPECT_EQ(loaded.get_layer(loaded.get_layer_by_name("M2"))->pitch, 380);
    }

    TEST_F(NativeFormatOlderSchema, ClassRemovedSinceIsDroppedWithAWarning)
    {
        file.descriptor_class("Library")["name"] = "Archive";
        file.stored_class("Library").name = "Archive";
        // Design (and so Schematic and Net) hang off it; drop them too, as
        // an older build without Library couldn't have had them either.
        file.classes.erase(std::remove_if(file.classes.begin(), file.classes.end(),
                                          [](const EditableClass &c) { return c.name == "Design" || c.name == "Schematic" || c.name == "Net"; }),
                           file.classes.end());

        const LoadReport report = load();
        ASSERT_TRUE(report.ok()) << report.error;
        EXPECT_TRUE(contains(report.warnings, "class Archive (1 objects) no longer exists"));
        EXPECT_TRUE(loaded.get_design_ids().empty());
        EXPECT_TRUE(loaded.get_library_ids().empty());
        EXPECT_EQ(loaded.get_layer_ids().size(), 3u);
    }

    TEST_F(NativeFormatOlderSchema, RetypedFieldNeedsAMigration)
    {
        for (auto &field : file.descriptor_class("Layer")["fields"])
            if (field["name"] == "name")
                field["type"] = "int";
        const LoadReport report = load();
        EXPECT_FALSE(report.ok());
        EXPECT_NE(report.error.find("Layer.name"), std::string::npos) << report.error;
        EXPECT_NE(report.error.find("migration"), std::string::npos) << report.error;
    }

    TEST_F(NativeFormatOlderSchema, RemovedEnumValueNeedsAMigration)
    {
        std::replace(file.strings.begin(), file.strings.end(), std::string("H"), std::string("HORIZONTAL"));
        const LoadReport report = load();
        EXPECT_FALSE(report.ok());
        EXPECT_NE(report.error.find("RoutingDirection has no value HORIZONTAL"), std::string::npos) << report.error;
    }

    TEST_F(NativeFormatOlderSchema, NewerSchemaVersionIsRefused)
    {
        file.schema["core"]["version"] = "999.0.0";
        const LoadReport report = load();
        EXPECT_FALSE(report.ok());
        EXPECT_NE(report.error.find("newer than this build"), std::string::npos) << report.error;
    }

    TEST_F(NativeFormatOlderSchema, ReferenceOutOfRangeIsRejected)
    {
        // Design.library -> row 7 of a one-row Library table.
        for (auto &[name, block] : file.stored_class("Design").columns)
            if (name == "library")
                block.bytes = {8};
        const LoadReport report = load();
        EXPECT_FALSE(report.ok());
        EXPECT_NE(report.error.find("past the last row"), std::string::npos) << report.error;
    }

    TEST_F(NativeFormatOlderSchema, ColumnStoredTwiceIsRejected)
    {
        auto &columns = file.stored_class("Layer").columns;
        for (auto &[name, block] : columns)
            if (name == "type")
            {
                columns.emplace_back(name, block);
                break;
            }
        const LoadReport report = load();
        EXPECT_FALSE(report.ok());
        EXPECT_NE(report.error.find("Layer.type is stored twice"), std::string::npos) << report.error;
    }

    TEST_F(NativeFormatOlderSchema, DuplicateUniqueNameIsRejected)
    {
        // Rename net rst to clk in the string table: two nets named clk in
        // one schematic violates Net.name's unique_per_parent.
        auto &strings = file.strings;
        std::replace(strings.begin(), strings.end(), std::string("rst"), std::string("clk"));
        const LoadReport report = load();
        EXPECT_FALSE(report.ok());
        EXPECT_NE(report.error.find("inconsistent data"), std::string::npos) << report.error;
    }

    // --- Migration chains (renames applied before name matching) ----------------

    using migrations::Op;
    using migrations::OpKind;

    TEST_F(NativeFormatOlderSchema, RenamedFieldKeepsItsData)
    {
        // The older schema called Layer.type "kind"; a migration renamed it.
        for (auto &field : file.descriptor_class("Layer")["fields"])
            if (field["name"] == "type")
                field["name"] = "kind";
        for (auto &[name, block] : file.stored_class("Layer").columns)
            if (name == "type")
                name = "kind";
        const std::array chain{Op{"0.10.0", OpKind::RenameField, "Layer", "kind", "type", "Layer.kind renamed to type"}};

        file.write(path);
        const LoadReport report = load_native(loaded, path.string(), chain);
        ASSERT_TRUE(report.ok()) << report.error;
        EXPECT_FALSE(contains(report.warnings, "no longer exists"));
        EXPECT_TRUE(contains(report.warnings, "applied migration to 0.10.0: Layer.kind renamed to type"));
        EXPECT_EQ(loaded.get_layer(loaded.get_layer_by_name("M1"))->type, "ROUTING");
    }

    TEST_F(NativeFormatOlderSchema, MigrationsAtOrBeforeTheFilesVersionAreSkipped)
    {
        for (auto &[name, block] : file.stored_class("Layer").columns)
            if (name == "type")
                name = "kind";
        for (auto &field : file.descriptor_class("Layer")["fields"])
            if (field["name"] == "type")
                field["name"] = "kind";
        // The file is 0.0.1 - a rename belonging to 0.0.1 itself is already in it.
        const std::array chain{Op{"0.0.1", OpKind::RenameField, "Layer", "kind", "type", "old"}};
        file.write(path);
        const LoadReport report = load_native(loaded, path.string(), chain);
        ASSERT_TRUE(report.ok()) << report.error;
        EXPECT_TRUE(contains(report.warnings, "Layer.kind no longer exists"));
    }

    TEST_F(NativeFormatOlderSchema, RenamedClassKeepsItsDataAndReferences)
    {
        file.descriptor_class("Library")["name"] = "Archive";
        file.stored_class("Library").name = "Archive";
        for (auto &field : file.descriptor_class("Design")["fields"])
            if (field["name"] == "library")
                field["type"] = "Archive";
        const std::array chain{Op{"0.20.0", OpKind::RenameClass, "", "Archive", "Library", "Archive renamed to Library"}};

        file.write(path);
        const LoadReport report = load_native(loaded, path.string(), chain);
        ASSERT_TRUE(report.ok()) << report.error;
        const DesignId design = loaded.get_design_ids().front();
        EXPECT_EQ(loaded.get_library(loaded.get_design(design)->library)->name, "lib");
    }

    TEST_F(NativeFormatOlderSchema, RenamedEnumValuesCompose)
    {
        // Stored as X1; renamed X1 -> X2 in 0.10.0, then X2 -> H in 0.20.0.
        std::replace(file.strings.begin(), file.strings.end(), std::string("H"), std::string("X1"));
        const std::array chain{
            Op{"0.10.0", OpKind::RenameEnumValue, "RoutingDirection", "X1", "X2", "first"},
            Op{"0.20.0", OpKind::RenameEnumValue, "RoutingDirection", "X2", "H", "second"},
        };
        file.write(path);
        const LoadReport report = load_native(loaded, path.string(), chain);
        ASSERT_TRUE(report.ok()) << report.error;
        EXPECT_EQ(loaded.get_layer(loaded.get_layer_by_name("M1"))->direction, RoutingDirection::H);
    }

    TEST_F(NativeFormatOlderSchema, MigrationNeedingTheDataRuntimeRefusesTheFile)
    {
        const std::array chain{Op{"0.30.0", OpKind::Unsupported, "", "split_layers", "Layer split in two", "Layer split in two"}};
        file.write(path);
        const LoadReport report = load_native(loaded, path.string(), chain);
        EXPECT_FALSE(report.ok());
        EXPECT_NE(report.error.find("needs the migration to 0.30.0 (Layer split in two)"), std::string::npos) << report.error;
        EXPECT_TRUE(loaded.get_layer_ids().empty());
    }

    TEST_F(NativeFormatOlderSchema, CurrentSchemaFilesIgnoreTheChain)
    {
        // A file already at this build's schema needs no migration, even if
        // the chain has entries.
        SmallDesign original;
        const auto current = dir.file("current.led");
        ASSERT_TRUE(save_native(original.root, current.string()).ok());
        const std::array chain{Op{"999.0.0", OpKind::Unsupported, "", "x", "y", "never applies"}};
        EXPECT_TRUE(load_native(loaded, current.string(), chain).ok());
    }

    // --- Value-level conversions (codec.hpp) -------------------------------------

    TEST(NativeCodec, OptionalValueReadsIntoAList)
    {
        EncodeContext encode;
        ByteWriter w;
        encode_value(w, std::optional<int>(7), encode);
        encode_value(w, std::optional<int>(), encode);

        const FileType ft{.kind = FileType::Kind::Int, .name = "int", .presence = true};
        EXPECT_EQ(incompatibility<std::vector<int>>(ft, true, false), "");
        DecodeContext decode;
        ByteReader r(w.bytes().data(), w.size(), "test");
        std::vector<int> present, absent{1, 2};
        decode_value(r, ft, true, false, present, decode);
        decode_value(r, ft, true, false, absent, decode);
        EXPECT_EQ(present, std::vector<int>{7});
        EXPECT_TRUE(absent.empty());
        EXPECT_TRUE(r.at_end());
    }

    TEST(NativeCodec, IntegerWidensToFloatingPointButNotBack)
    {
        EncodeContext encode;
        ByteWriter w;
        encode_value(w, int64_t{-12}, encode);
        const FileType int_type{.kind = FileType::Kind::Int, .name = "dbu"};
        EXPECT_EQ(incompatibility<double>(int_type, false, false), "");
        const FileType float_type{.kind = FileType::Kind::Float, .name = "double"};
        EXPECT_NE(incompatibility<int>(float_type, false, false), "");

        DecodeContext decode;
        ByteReader r(w.bytes().data(), w.size(), "test");
        double value = 0;
        decode_value(r, int_type, false, false, value, decode);
        EXPECT_DOUBLE_EQ(value, -12.0);
    }

    TEST(NativeCodec, OptionalToRequiredIsIncompatible)
    {
        const FileType ft{.kind = FileType::Kind::Int, .name = "int", .presence = true};
        EXPECT_EQ(incompatibility<int>(ft, true, false), "was optional, now required");
        EXPECT_EQ(incompatibility<std::optional<int>>(ft, true, false), "");
    }

    TEST(NativeCodec, IntegerOutOfRangeForANarrowerFieldIsAnError)
    {
        EncodeContext encode;
        ByteWriter w;
        encode_value(w, int64_t{1} << 40, encode);
        DecodeContext decode;
        ByteReader r(w.bytes().data(), w.size(), "test");
        int narrow = 0;
        EXPECT_THROW(decode_value(r, FileType{.kind = FileType::Kind::Int, .name = "dbu"}, false, false, narrow, decode), FormatError);
    }

    TEST(NativeCodec, VarintsAndZigzagRoundTrip)
    {
        for (int64_t v : {int64_t{0}, int64_t{1}, int64_t{-1}, int64_t{63}, int64_t{-64}, int64_t{1} << 40, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()})
        {
            ByteWriter w;
            w.svarint(v);
            ByteReader r(w.bytes().data(), w.size(), "test");
            EXPECT_EQ(r.svarint(), v);
            EXPECT_TRUE(r.at_end());
        }
    }

    TEST(NativeCodec, Crc32MatchesTheStandardCheckValue)
    {
        const std::string check = "123456789";
        EXPECT_EQ(crc32(reinterpret_cast<const uint8_t *>(check.data()), check.size()), 0xCBF43926u);
    }
}
