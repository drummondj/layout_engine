#include "native_format.hpp"

#include "byte_io.hpp"
#include "codec.hpp"
#include "generated/database/schema_version.hpp"

#include <json.hpp>
#include <oneapi/tbb/parallel_for.h>
#include <zstd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <span>
#include <unistd.h>

namespace le::persistence
{
    namespace
    {
        constexpr std::array<uint8_t, 8> kMagic = {'L', 'E', 'D', 'B', '\r', '\n', 0x1a, '\n'};
        constexpr uint8_t kCodecRaw = 0;
        constexpr uint8_t kCodecZstd = 1;
        constexpr size_t kHeaderSize = kMagic.size() + 4 + 4;
        /// Rows per column segment: each segment is its own block, so one
        /// big column still decompresses and decodes on many threads.
        constexpr uint64_t kSegmentRows = 1u << 16;

        using Tag = std::array<char, 4>;
        constexpr Tag kSchemaTag = {'S', 'C', 'H', 'M'};
        constexpr Tag kClassTag = {'C', 'L', 'A', 'S'};
        constexpr Tag kStringsTag = {'S', 'T', 'R', 'S'};
        constexpr Tag kEndTag = {'E', 'N', 'D', ' '};

        std::string tag_name(const Tag &tag) { return std::string(tag.data(), tag.size()); }

        class PhaseTimer
        {
        public:
            explicit PhaseTimer(std::vector<std::pair<std::string, double>> &phases) : phases_(phases) {}
            void lap(std::string name)
            {
                const auto now = std::chrono::steady_clock::now();
                phases_.emplace_back(std::move(name), std::chrono::duration<double, std::milli>(now - start_).count());
                start_ = now;
            }

        private:
            std::vector<std::pair<std::string, double>> &phases_;
            std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
        };

        // --- Writing ---------------------------------------------------------

        void append_block(ByteWriter &out, const uint8_t *raw, size_t raw_size, int level)
        {
            std::vector<uint8_t> compressed;
            size_t n = 0;
            if (level > 0 && raw_size)
            {
                compressed.resize(ZSTD_compressBound(raw_size));
                n = ZSTD_compress(compressed.data(), compressed.size(), raw, raw_size, level);
            }
            const bool use_zstd = level > 0 && raw_size && !ZSTD_isError(n) && n < raw_size;
            const uint8_t *stored = use_zstd ? compressed.data() : raw;
            const size_t stored_size = use_zstd ? n : raw_size;
            out.u8(use_zstd ? kCodecZstd : kCodecRaw);
            out.fixed<uint64_t>(raw_size);
            out.fixed<uint64_t>(stored_size);
            out.fixed<uint32_t>(crc32(stored, stored_size));
            out.raw(stored, stored_size);
        }

        /// @brief A file written through a C FILE, so it can be fsync'd.
        class OutputFile
        {
        public:
            explicit OutputFile(const std::string &path) : file_(std::fopen(path.c_str(), "wb"))
            {
                if (!file_)
                    throw FormatError("cannot open " + path + " for writing");
            }
            ~OutputFile()
            {
                if (file_)
                    std::fclose(file_);
            }

            void write(const void *data, size_t size)
            {
                if (size && std::fwrite(data, 1, size, file_) != size)
                    throw FormatError("write failed (disk full?)");
                bytes_ += size;
            }

            void chunk(const Tag &tag, const std::vector<uint8_t> &payload)
            {
                write(tag.data(), tag.size());
                const uint64_t size = payload.size();
                write(&size, sizeof size);
                write(payload.data(), payload.size());
            }

            void close_synced()
            {
                if (std::fflush(file_) != 0 || ::fsync(fileno(file_)) != 0)
                    throw FormatError("flush failed");
                const int result = std::fclose(file_);
                file_ = nullptr;
                if (result != 0)
                    throw FormatError("close failed");
            }

            uint64_t bytes() const { return bytes_; }

        private:
            std::FILE *file_;
            uint64_t bytes_ = 0;
        };

        template <class P>
        std::vector<uint32_t> row_order(const Root &root)
        {
            const auto &slots = P::pool(root).slots();
            std::vector<uint8_t> seen(slots.size());
            std::vector<uint32_t> order;
            order.reserve(P::pool(root).alive_count());
            auto add = [&](uint32_t slot) {
                if (slot < slots.size() && slots[slot].alive && !seen[slot])
                {
                    seen[slot] = 1;
                    order.push_back(slot);
                }
            };
            P::list_order(root, add);
            for (uint32_t slot = 0; slot < slots.size(); ++slot)
                add(slot);
            return order;
        }

        // The SCHM JSON. Only the extensions with objects in `root` are
        // listed, so a file holding none of an extension's objects still
        // opens in a build without that extension.
        std::string schema_chunk_json(const Root &root)
        {
            std::set<std::string_view> present;
            nt::for_each_pooled([&]<class P>(P) {
                if (!P::extension.empty() && P::pool(root).alive_count() > 0)
                    present.insert(P::extension);
            });
            std::string json = R"({"writer":"layout_engine","core":{"version":")";
            json += schema_info::kVersion;
            json += R"(","fingerprint":")";
            json += schema_info::kFingerprint;
            json += R"(","descriptor":)";
            json += schema_info::kDescriptorJson;
            json += "}";
            if (!present.empty())
            {
                json += R"(,"extensions":[)";
                bool first = true;
                for (std::size_t i = 0; i < schema_info::kExtensionCount; ++i)
                {
                    const schema_info::ExtensionSchema &ext = schema_info::kExtensions[i];
                    if (!present.contains(ext.name))
                        continue;
                    json += first ? "" : ",";
                    first = false;
                    json += R"({"name":")" + std::string(ext.name) + R"(","package_version":")" + std::string(ext.package_version) +
                            R"(","version":")" + std::string(ext.version) + R"(","fingerprint":")" + std::string(ext.fingerprint) +
                            R"(","descriptor":)" + std::string(ext.descriptor_json) + "}";
                }
                json += "]";
            }
            json += "}";
            return json;
        }

        // --- Reading ---------------------------------------------------------

        std::vector<uint8_t> read_file(const std::string &path)
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in)
                throw FormatError("cannot open " + path);
            const auto size = static_cast<size_t>(in.tellg());
            std::vector<uint8_t> bytes(size);
            in.seekg(0);
            if (size && !in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
                throw FormatError("cannot read " + path);
            return bytes;
        }

        struct Span
        {
            const uint8_t *data = nullptr;
            size_t size = 0;
        };

        struct Chunk
        {
            Tag tag;
            Span payload;
        };

        struct Container
        {
            uint32_t version = 0;
            std::vector<Chunk> chunks;
        };

        Container parse_container(const std::vector<uint8_t> &bytes)
        {
            ByteReader r(bytes.data(), bytes.size(), "file");
            if (bytes.size() < kHeaderSize || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
                throw FormatError("not a Layout Engine database file (bad magic)");
            r.raw(kMagic.size());
            Container container;
            container.version = r.fixed<uint32_t>();
            if (container.version > kContainerVersion)
                throw FormatError("written by a newer Layout Engine (container version " + std::to_string(container.version) + ", this build reads up to " +
                                  std::to_string(kContainerVersion) + ")");
            r.fixed<uint32_t>(); // flags
            bool ended = false;
            while (!r.at_end())
            {
                if (r.remaining() < 12)
                    throw FormatError("incomplete chunk header - the file is truncated");
                Chunk chunk;
                std::memcpy(chunk.tag.data(), r.raw(4), 4);
                const auto size = r.fixed<uint64_t>();
                if (size > r.remaining())
                    throw FormatError("chunk " + tag_name(chunk.tag) + " runs past the end of the file (truncated?)");
                chunk.payload = {r.raw(size), static_cast<size_t>(size)};
                if (chunk.tag == kEndTag)
                {
                    ended = true;
                    if (!r.at_end())
                        throw FormatError("data after the END chunk");
                    break;
                }
                container.chunks.push_back(chunk);
            }
            if (!ended)
                throw FormatError("no END chunk - the file is truncated");
            return container;
        }

        /// @brief Decode a block at `r`, verifying its CRC and size.
        std::vector<uint8_t> read_block(ByteReader &r)
        {
            const uint8_t codec = r.u8();
            const auto raw_size = r.fixed<uint64_t>();
            const auto stored_size = r.fixed<uint64_t>();
            const auto crc = r.fixed<uint32_t>();
            if (stored_size > r.remaining())
                r.fail("block runs past its chunk");
            const uint8_t *stored = r.raw(stored_size);
            if (crc32(stored, stored_size) != crc)
                r.fail("checksum mismatch - the file is corrupt");
            if (codec == kCodecRaw)
            {
                if (raw_size != stored_size)
                    r.fail("raw block size mismatch");
                return {stored, stored + stored_size};
            }
            if (codec != kCodecZstd)
                r.fail("unknown block codec " + std::to_string(codec));
            if (ZSTD_getFrameContentSize(stored, stored_size) != raw_size)
                r.fail("compressed block size mismatch");
            std::vector<uint8_t> raw(raw_size);
            const size_t n = ZSTD_decompress(raw.data(), raw.size(), stored, stored_size);
            if (ZSTD_isError(n) || n != raw_size)
                r.fail("decompression failed");
            return raw;
        }

        struct FileExtension
        {
            std::string name;
            std::string package_version;
            std::string version; // its schema version
            std::string fingerprint;
        };

        struct FileSchema
        {
            std::string version;
            std::string fingerprint;
            std::string writer;
            std::vector<FileExtension> extensions;
            std::map<std::string, std::unique_ptr<FileStruct>> structs; // pooled classes and embedded structs, core and extensions'
        };

        FileType::Kind scalar_kind(const std::string &type)
        {
            if (type == "str" || type == "char*" || type == "signed char*" || type == "unsigned char*")
                return FileType::Kind::Str;
            if (type == "bool")
                return FileType::Kind::Bool;
            if (type == "float" || type == "double" || type == "long double")
                return FileType::Kind::Float;
            return FileType::Kind::Int; // every other TYPEMAP type is an integer (int, dbu, dbu2, ...)
        }

        FileSchema parse_schema(const Span &payload)
        {
            ByteReader r(payload.data, payload.size, "SCHM chunk");
            const std::vector<uint8_t> raw = read_block(r);
            nlohmann::json json;
            try
            {
                json = nlohmann::json::parse(raw.begin(), raw.end());
            }
            catch (const nlohmann::json::exception &e)
            {
                throw FormatError(std::string("schema descriptor is not valid JSON: ") + e.what());
            }
            try
            {
                FileSchema schema;
                schema.writer = json.value("writer", "");
                const auto &core = json.at("core");
                schema.version = core.at("version").get<std::string>();
                schema.fingerprint = core.at("fingerprint").get<std::string>();
                std::vector<const nlohmann::json *> descriptors{&core.at("descriptor")};
                if (json.contains("extensions"))
                    for (const auto &ext : json.at("extensions"))
                    {
                        schema.extensions.push_back({ext.at("name").get<std::string>(), ext.at("package_version").get<std::string>(),
                                                     ext.at("version").get<std::string>(), ext.at("fingerprint").get<std::string>()});
                        descriptors.push_back(&ext.at("descriptor"));
                    }

                for (const nlohmann::json *descriptor_ptr : descriptors)
                {
                    const auto &descriptor = *descriptor_ptr;
                    if (descriptor.at("format").get<int>() != 1)
                        throw FormatError("unsupported schema descriptor format " + descriptor.at("format").dump());
                    for (const auto &klass : descriptor.at("classes"))
                    {
                        const std::string kind = klass.at("kind").get<std::string>();
                        if (kind == "enum")
                            continue;
                        auto structure = std::make_unique<FileStruct>();
                        structure->name = klass.at("name").get<std::string>();
                        structure->pooled = kind == "pooled";
                        for (const auto &field : klass.at("fields"))
                        {
                            const std::string field_kind = field.at("kind").get<std::string>();
                            if (field_kind == "child")
                                continue; // derived from the index, never stored
                            FileField f;
                            f.name = field.at("name").get<std::string>();
                            if (field_kind == "owner")
                            {
                                f.type.kind = FileType::Kind::Owner;
                                for (const auto &option : field.at("options"))
                                    f.type.owner_options.emplace_back(option.at("name").get<std::string>(), option.at("type").get<std::string>());
                                structure->fields.push_back(std::move(f));
                                continue;
                            }
                            f.type.name = field.at("type").get<std::string>();
                            f.type.presence = field.value("presence", false);
                            f.type.list = field.value("list", false);
                            if (field_kind == "parent" || field_kind == "ref")
                                f.type.kind = FileType::Kind::Ref;
                            else if (field_kind == "enum")
                                f.type.kind = FileType::Kind::Enum;
                            else if (field_kind == "struct")
                                f.type.kind = FileType::Kind::Struct;
                            else if (field_kind == "scalar")
                                f.type.kind = scalar_kind(f.type.name);
                            else
                                throw FormatError("unknown field kind " + field_kind);
                            structure->fields.push_back(std::move(f));
                        }
                        const std::string name = structure->name;
                        if (!schema.structs.emplace(name, std::move(structure)).second)
                            throw FormatError("class " + name + " is described twice");
                    }
                }
                for (auto &[name, structure] : schema.structs)
                    for (FileField &field : structure->fields)
                        if (field.type.kind == FileType::Kind::Struct)
                        {
                            auto it = schema.structs.find(field.type.name);
                            if (it == schema.structs.end() || it->second->pooled)
                                throw FormatError("field " + name + "." + field.name + " has unknown struct type " + field.type.name);
                            field.type.structure = it->second.get();
                        }
                return schema;
            }
            catch (const nlohmann::json::exception &e)
            {
                throw FormatError(std::string("malformed schema descriptor: ") + e.what());
            }
        }

        std::vector<std::string_view> parse_strings(const std::vector<uint8_t> &raw)
        {
            ByteReader r(raw.data(), raw.size(), "string table");
            const uint64_t n = r.count();
            std::vector<std::string_view> strings;
            strings.reserve(n);
            for (uint64_t i = 0; i < n; ++i)
                strings.push_back(r.string());
            if (!r.at_end())
                r.fail("trailing bytes");
            return strings;
        }

        struct Segment
        {
            uint64_t first_row = 0;
            uint64_t rows = 0;
            Span block; // the segment's whole block, header included
        };

        struct FileColumn
        {
            std::string name;
            std::vector<Segment> segments;
            size_t stored_bytes = 0;
        };

        struct FileClass
        {
            std::string name;
            uint64_t rows = 0;
            std::vector<FileColumn> columns;
        };

        FileClass parse_class_header(const Span &payload)
        {
            ByteReader r(payload.data, payload.size, "CLAS chunk");
            FileClass klass;
            klass.name = std::string(r.string());
            klass.rows = r.fixed<uint64_t>();
            // Ids are 32-bit (and the header isn't checksummed): refuse a
            // row count no pool could hold before allocating for it.
            if (klass.rows >= std::numeric_limits<uint32_t>::max())
                r.fail("row count " + std::to_string(klass.rows) + " is too large");
            const auto columns = r.fixed<uint32_t>();
            for (uint32_t c = 0; c < columns; ++c)
            {
                FileColumn column;
                column.name = std::string(r.string());
                const auto segments = r.fixed<uint32_t>();
                uint64_t first_row = 0;
                for (uint32_t i = 0; i < segments; ++i)
                {
                    Segment segment;
                    segment.first_row = first_row;
                    segment.rows = r.fixed<uint64_t>();
                    // Remember where the block starts, then skip over it.
                    const size_t start = r.position();
                    r.u8();
                    r.fixed<uint64_t>();
                    const auto stored = r.fixed<uint64_t>();
                    r.fixed<uint32_t>();
                    if (stored > r.remaining())
                        r.fail("column " + column.name + " runs past its chunk");
                    r.raw(stored);
                    segment.block = {payload.data + start, r.position() - start};
                    column.stored_bytes += segment.block.size;
                    first_row += segment.rows;
                    column.segments.push_back(segment);
                }
                if (first_row != klass.rows)
                    r.fail("column " + column.name + "'s segments hold " + std::to_string(first_row) + " rows, not " + std::to_string(klass.rows));
                klass.columns.push_back(std::move(column));
            }
            if (!r.at_end())
                r.fail("trailing bytes");
            return klass;
        }

        bool version_newer(const std::string &a, const std::string &b)
        {
            auto parse = [](const std::string &v) {
                std::array<long, 3> parts{};
                if (std::sscanf(v.c_str(), "%ld.%ld.%ld", &parts[0], &parts[1], &parts[2]) != 3)
                    return std::array<long, 3>{};
                return parts;
            };
            return parse(a) > parse(b);
        }

        const schema_info::ExtensionSchema *built_extension(const std::string &name)
        {
            for (std::size_t i = 0; i < schema_info::kExtensionCount; ++i)
                if (schema_info::kExtensions[i].name == name)
                    return &schema_info::kExtensions[i];
            return nullptr;
        }

        /// @brief Whether the file's schema is exactly this build's: core and
        /// every extension it holds.
        bool schema_matches_build(const FileSchema &schema)
        {
            if (schema.fingerprint != schema_info::kFingerprint)
                return false;
            for (const FileExtension &ext : schema.extensions)
            {
                const schema_info::ExtensionSchema *built = built_extension(ext.name);
                if (!built || built->fingerprint != ext.fingerprint)
                    return false;
            }
            return true;
        }

        /// @brief Refuses a file holding objects of an extension this build
        /// doesn't have, or written with a newer schema of one it has: it
        /// can't be loaded, or re-saved, without losing that data.
        void check_extensions(const FileSchema &schema)
        {
            std::string problems;
            for (const FileExtension &ext : schema.extensions)
            {
                const schema_info::ExtensionSchema *built = built_extension(ext.name);
                std::string problem;
                if (!built)
                    problem = "extension " + ext.name + " " + ext.package_version +
                              ", which this build doesn't have - add it to the project (le add) or build with it (LE_EXTENSION_DIRS)";
                else if (built->fingerprint != ext.fingerprint && version_newer(ext.version, std::string(built->version)))
                    problem = "extension " + ext.name + " " + ext.package_version + " (schema " + ext.version + "), newer than this build's " +
                              std::string(built->package_version) + " (schema " + std::string(built->version) + ") - update the extension";
                if (!problem.empty())
                    problems += (problems.empty() ? "" : "; ") + problem;
            }
            if (!problems.empty())
                throw FormatError("the file needs " + problems);
        }

        struct Opened
        {
            std::vector<uint8_t> bytes;
            Container container;
            FileSchema schema;
            std::vector<FileClass> classes;
            Span strings_payload;
        };

        Opened open_file(const std::string &path)
        {
            Opened opened;
            opened.bytes = read_file(path);
            opened.container = parse_container(opened.bytes);
            bool have_schema = false, have_strings = false;
            for (const Chunk &chunk : opened.container.chunks)
            {
                if (chunk.tag == kSchemaTag)
                {
                    if (have_schema)
                        throw FormatError("more than one SCHM chunk");
                    opened.schema = parse_schema(chunk.payload);
                    have_schema = true;
                }
                else if (chunk.tag == kClassTag)
                    opened.classes.push_back(parse_class_header(chunk.payload));
                else if (chunk.tag == kStringsTag)
                {
                    opened.strings_payload = chunk.payload;
                    have_strings = true;
                }
                // Unknown chunk kinds are skipped: that is how later
                // container additions stay readable by this build.
            }
            if (!have_schema)
                throw FormatError("no SCHM chunk");
            if (!have_strings)
                throw FormatError("no STRS chunk");
            return opened;
        }
    }

    namespace
    {
        using EnumRenames = std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>>;

        /// @brief Bring an older file's schema names up to date: apply every
        /// op of every migration past `file_version`, in chain order.
        void apply_migrations(Opened &file, EnumRenames &enum_renames, std::span<const migrations::Op> chain, std::vector<std::string> &applied)
        {
            const std::string &file_version = file.schema.version;
            std::string last_migration;
            for (const migrations::Op &op : chain)
            {
                const std::string to_version(op.to_version);
                if (!version_newer(to_version, file_version))
                    continue;
                if (to_version != last_migration)
                {
                    applied.push_back("applied migration to " + to_version + ": " + std::string(op.description));
                    last_migration = to_version;
                }
                const std::string klass(op.klass), old_name(op.old_name), new_name(op.new_name);
                switch (op.kind)
                {
                case migrations::OpKind::RenameClass:
                {
                    if (auto node = file.schema.structs.extract(old_name))
                    {
                        node.key() = new_name;
                        node.mapped()->name = new_name;
                        file.schema.structs.insert(std::move(node));
                    }
                    for (auto &[name, structure] : file.schema.structs)
                        for (FileField &field : structure->fields)
                            if (field.type.name == old_name && field.type.kind != FileType::Kind::Int && field.type.kind != FileType::Kind::Float &&
                                field.type.kind != FileType::Kind::Bool && field.type.kind != FileType::Kind::Str)
                                field.type.name = new_name;
                    for (FileClass &stored : file.classes)
                        if (stored.name == old_name)
                            stored.name = new_name;
                    if (auto node = enum_renames.extract(old_name))
                    {
                        node.key() = new_name;
                        enum_renames.insert(std::move(node));
                    }
                    break;
                }
                case migrations::OpKind::RenameField:
                {
                    if (auto it = file.schema.structs.find(klass); it != file.schema.structs.end())
                        for (FileField &field : it->second->fields)
                            if (field.name == old_name)
                                field.name = new_name;
                    for (FileClass &stored : file.classes)
                        if (stored.name == klass)
                            for (FileColumn &column : stored.columns)
                                if (column.name == old_name)
                                    column.name = new_name;
                    break;
                }
                case migrations::OpKind::RenameEnumValue:
                {
                    // Keyed by the stored name: compose with earlier renames
                    // (A -> B, then B -> C makes A -> C).
                    auto &renames = enum_renames[klass];
                    for (auto &[stored, current] : renames)
                        if (current == old_name)
                            current = new_name;
                    renames.try_emplace(old_name, new_name);
                    break;
                }
                case migrations::OpKind::Unsupported:
                    throw FormatError("written with schema version " + file_version + "; reading it needs the migration to " + to_version + " (" +
                                      std::string(op.description) + "), which this build can't apply to stored data yet");
                }
            }
        }
    }

    // --- Public API ----------------------------------------------------------

    SaveReport save_native(const Root &root, const std::string &path, const SaveOptions &options)
    {
        SaveReport report;
        const std::string tmp_path = path + ".tmp";
        try
        {
            PhaseTimer timer(report.phase_ms);
            EncodeContext ctx;
            ctx.root = &root;
            std::array<std::vector<uint32_t>, nt::kPooledClassCount> orders;
            nt::for_each_pooled([&]<class P>(P) {
                orders[P::index] = row_order<P>(root);
                auto &dense = ctx.dense[P::index];
                dense.assign(P::pool(root).slots().size(), kNoRow);
                for (uint32_t row = 0; row < orders[P::index].size(); ++row)
                    dense[orders[P::index][row]] = row;
            });
            timer.lap("order");

            OutputFile out(tmp_path);
            ByteWriter header;
            header.raw(kMagic.data(), kMagic.size());
            header.fixed<uint32_t>(kContainerVersion);
            header.fixed<uint32_t>(0);
            out.write(header.bytes().data(), header.size());

            {
                const std::string json = schema_chunk_json(root);
                ByteWriter payload;
                append_block(payload, reinterpret_cast<const uint8_t *>(json.data()), json.size(), options.compression_level);
                out.chunk(kSchemaTag, payload.bytes());
            }

            nt::for_each_pooled([&]<class P>(P) {
                const auto &order = orders[P::index];
                if (order.empty())
                    return;
                const auto &slots = P::pool(root).slots();
                ByteWriter payload;
                payload.string(P::name);
                payload.fixed<uint64_t>(order.size());
                payload.fixed<uint32_t>(static_cast<uint32_t>(std::tuple_size_v<std::remove_cvref_t<decltype(nt::Fields<typename P::Data>::members)>>));
                std::apply(
                    [&](const auto &...member) {
                        (
                            [&] {
                                // Encoded in one pass (the string table's
                                // ids must come out in a fixed order), cut
                                // into segments at row boundaries, and the
                                // segments compressed in parallel.
                                ByteWriter column;
                                std::vector<size_t> cuts{0};
                                for (size_t row = 0; row < order.size(); ++row)
                                {
                                    encode_value(column, slots[order[row]].value.*(member.ptr), ctx);
                                    if ((row + 1) % kSegmentRows == 0 || row + 1 == order.size())
                                        cuts.push_back(column.size());
                                }
                                const size_t segments = cuts.size() - 1;
                                std::vector<ByteWriter> blocks(segments);
                                tbb::parallel_for(size_t{0}, segments, [&](size_t i) {
                                    append_block(blocks[i], column.bytes().data() + cuts[i], cuts[i + 1] - cuts[i], options.compression_level);
                                });
                                payload.string(member.name);
                                payload.fixed<uint32_t>(static_cast<uint32_t>(segments));
                                for (size_t i = 0; i < segments; ++i)
                                {
                                    payload.fixed<uint64_t>(std::min<uint64_t>(kSegmentRows, order.size() - i * kSegmentRows));
                                    payload.raw(blocks[i].bytes().data(), blocks[i].size());
                                }
                            }(),
                            ...);
                    },
                    nt::Fields<typename P::Data>::members);
                out.chunk(kClassTag, payload.bytes());
                report.objects += order.size();
                timer.lap("encode " + std::string(P::name));
            });

            {
                ByteWriter table;
                table.varint(ctx.strings().size());
                for (const auto &s : ctx.strings())
                    table.string(*s);
                ByteWriter payload;
                append_block(payload, table.bytes().data(), table.size(), options.compression_level);
                out.chunk(kStringsTag, payload.bytes());
            }
            out.chunk(kEndTag, {});
            out.close_synced();
            timer.lap("strings+write");
            report.file_bytes = out.bytes();
            report.dangling_references = ctx.dangling_references;

            std::error_code ec;
            std::filesystem::rename(tmp_path, path, ec);
            if (ec)
                throw FormatError("cannot replace " + path + ": " + ec.message());
        }
        catch (const std::exception &e)
        {
            std::error_code ignored;
            std::filesystem::remove(tmp_path, ignored);
            report.error = e.what();
        }
        return report;
    }

    LoadReport load_native(Root &root, const std::string &path) { return load_native(root, path, migrations::kOps); }

    LoadReport load_native(Root &root, const std::string &path, std::span<const migrations::Op> chain)
    {
        LoadReport report;
        try
        {
            PhaseTimer timer(report.phase_ms);
            Opened file = open_file(path);
            timer.lap("read");
            report.file_schema_version = file.schema.version;
            report.file_fingerprint = file.schema.fingerprint;
            report.schema_matches = schema_matches_build(file.schema);
            if (file.schema.fingerprint != schema_info::kFingerprint && version_newer(file.schema.version, std::string(schema_info::kVersion)))
                throw FormatError("written with schema version " + file.schema.version + ", newer than this build's " + std::string(schema_info::kVersion) +
                                  " - open it with a newer Layout Engine");
            check_extensions(file.schema);

            EnumRenames enum_renames;
            if (!report.schema_matches)
                apply_migrations(file, enum_renames, chain, report.warnings);

            ByteReader strings_reader(file.strings_payload.data, file.strings_payload.size, "STRS chunk");
            const std::vector<uint8_t> strings_raw = read_block(strings_reader);
            const std::vector<std::string_view> strings = parse_strings(strings_raw);
            DecodeContext ctx;
            ctx.strings = &strings;
            ctx.enum_renames = enum_renames.empty() ? nullptr : &enum_renames;

            // Row counts first: every reference is range-checked against them.
            std::array<const FileClass *, nt::kPooledClassCount> by_index{};
            std::map<std::string, const FileClass *> file_classes;
            for (const FileClass &klass : file.classes)
                if (!file_classes.emplace(klass.name, &klass).second)
                    throw FormatError("class " + klass.name + " stored twice");
            nt::for_each_pooled([&]<class P>(P) {
                auto it = file_classes.find(std::string(P::name));
                if (it == file_classes.end())
                    return;
                by_index[P::index] = it->second;
                ctx.row_counts[P::index] = it->second->rows;
                file_classes.erase(it);
            });
            for (const auto &[name, klass] : file_classes)
                report.warnings.push_back("class " + name + " (" + std::to_string(klass->rows) + " objects) no longer exists; dropped");

            // Match every stored column to a current member and check they
            // are compatible - sequentially, so warnings come out in a
            // stable order - collecting one decode job per column. The jobs
            // then run in parallel: each writes one member of every row, so
            // no two touch the same memory. One job per column, not per
            // segment: decoding is dominated by allocating each row's
            // lists, and measured on aes_scaling_8x8 (WSL2), a cold load
            // decodes fastest with a few columns allocating at once (3.0s)
            // - slower single-threaded (4.4s) and slower still with every
            // segment in parallel (6.4s, page-fault/allocator contention).
            Root loaded;
            std::vector<std::pair<size_t, std::function<void()>>> jobs; // (stored bytes, job)
            nt::for_each_pooled([&]<class P>(P) {
                const FileClass *klass = by_index[P::index];
                if (!klass)
                    return;
                auto file_struct = file.schema.structs.find(klass->name);
                if (file_struct == file.schema.structs.end() || !file_struct->second->pooled)
                    throw FormatError("class " + klass->name + " is stored but not described as a pooled class");
                const FileStruct &described = *file_struct->second;

                using Data = typename P::Data;
                P::pool(loaded).load_dense(static_cast<size_t>(klass->rows));
                auto &slots = P::pool(loaded).slots();
                report.objects += klass->rows;

                std::vector<bool> filled(std::tuple_size_v<std::remove_cvref_t<decltype(nt::Fields<Data>::members)>>);
                for (const FileColumn &column : klass->columns)
                {
                    const std::string &column_name = column.name;
                    const FileField *field = nullptr;
                    for (const FileField &f : described.fields)
                        if (f.name == column_name)
                            field = &f;
                    if (!field)
                        throw FormatError(klass->name + " has a column " + column_name + " its schema doesn't describe");

                    bool matched = false;
                    int member_index = 0;
                    std::apply(
                        [&](const auto &...member) {
                            (
                                [&] {
                                    const int this_index = member_index++;
                                    if (matched || member.name != column_name)
                                        return;
                                    matched = true;
                                    // Two stored columns feeding one member would be two decode
                                    // jobs writing the same memory at once - refuse (a corrupt file,
                                    // or a rename onto a name the file also stores).
                                    if (filled[this_index])
                                        throw FormatError(klass->name + "." + column_name + " is stored twice");
                                    filled[this_index] = true;
                                    using M = typename std::remove_cvref_t<decltype(member)>::type;
                                    const std::string problem = incompatibility<M>(field->type, field->type.presence, field->type.list);
                                    if (!problem.empty())
                                        throw FormatError(klass->name + "." + column_name + " " + problem + " - a migration is needed to read this file");
                                    jobs.emplace_back(column.stored_bytes, [&slots, &ctx, field, &column, what = klass->name + "." + column_name, ptr = member.ptr] {
                                        DecodeContext local;
                                        local.strings = ctx.strings;
                                        local.row_counts = ctx.row_counts;
                                        local.enum_renames = ctx.enum_renames;
                                        for (const Segment &segment : column.segments)
                                        {
                                            ByteReader block_reader(segment.block.data, segment.block.size, what);
                                            const std::vector<uint8_t> raw = read_block(block_reader);
                                            ByteReader r(raw.data(), raw.size(), what);
                                            for (uint64_t row = segment.first_row; row < segment.first_row + segment.rows; ++row)
                                                decode_value(r, field->type, field->type.presence, field->type.list, slots[row].value.*ptr, local);
                                            if (!r.at_end())
                                                r.fail("trailing bytes in column segment");
                                        }
                                    });
                                }(),
                                ...);
                        },
                        nt::Fields<Data>::members);
                    if (!matched)
                        report.warnings.push_back("field " + klass->name + "." + column_name + " no longer exists; dropped");
                }
                if (!report.schema_matches)
                {
                    int member_index = 0;
                    std::apply(
                        [&](const auto &...member) {
                            ((filled[member_index++] ? void() : report.warnings.push_back("field " + klass->name + "." + std::string(member.name) + " is not in the file; left at its default")), ...);
                        },
                        nt::Fields<Data>::members);
                }
            });
            timer.lap("match");

            // Biggest columns first, so the longest jobs don't start last.
            std::stable_sort(jobs.begin(), jobs.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            tbb::parallel_for(size_t{0}, jobs.size(), [&](size_t j) { jobs[j].second(); }, tbb::simple_partitioner{});
            timer.lap("decode");

            // Every class fills only its own index maps, so they rebuild in
            // parallel (loaded's index is still empty - nothing was created
            // through it).
            std::array<std::vector<std::string>, nt::kPooledClassCount> class_problems;
            std::array<void (*)(Root &, std::vector<std::string> &), nt::kPooledClassCount> rebuilders{};
            nt::for_each_pooled([&]<class P>(P) { rebuilders[P::index] = &P::rebuild_index; });
            tbb::parallel_for(size_t{0}, nt::kPooledClassCount, [&](size_t i) { rebuilders[i](loaded, class_problems[i]); }, tbb::simple_partitioner{});
            std::vector<std::string> problems;
            for (auto &list : class_problems)
                problems.insert(problems.end(), list.begin(), list.end());
            timer.lap("index");
            if (!problems.empty())
            {
                std::string message = "inconsistent data: " + problems.front();
                if (problems.size() > 1)
                    message += " (and " + std::to_string(problems.size() - 1) + " more)";
                throw FormatError(message);
            }
            root.replace_contents_from(std::move(loaded));
            timer.lap("replace");
        }
        catch (const std::exception &e)
        {
            report.error = e.what();
        }
        return report;
    }

    FileInfo inspect_native(const std::string &path)
    {
        FileInfo info;
        try
        {
            Opened file = open_file(path);
            info.container_version = file.container.version;
            info.schema_version = file.schema.version;
            info.fingerprint = file.schema.fingerprint;
            info.writer = file.schema.writer;
            info.schema_matches = schema_matches_build(file.schema);
            info.file_bytes = file.bytes.size();
            for (const FileExtension &ext : file.schema.extensions)
            {
                const schema_info::ExtensionSchema *built = built_extension(ext.name);
                info.extensions.push_back({ext.name, ext.package_version, ext.version, built ? std::string(built->version) : std::string()});
            }
            for (const FileClass &klass : file.classes)
                info.classes.emplace_back(klass.name, klass.rows);
        }
        catch (const std::exception &e)
        {
            info.error = e.what();
        }
        return info;
    }

    bool database_is_empty(const Root &root)
    {
        bool empty = true;
        nt::for_each_pooled([&]<class P>(P) { empty = empty && P::pool(root).alive_count() == 0; });
        return empty;
    }

        // --- byte_io.hpp / codec.hpp out-of-line pieces ----------------------------

    uint32_t crc32(const uint8_t *data, size_t size)
    {
        static const auto table = [] {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    void skip_value(ByteReader &r, const FileType &ft, bool presence, bool list, DecodeContext &ctx)
    {
        if (list)
        {
            const uint64_t n = r.count();
            for (uint64_t i = 0; i < n; ++i)
                skip_value(r, ft, false, false, ctx);
            return;
        }
        if (presence && r.u8() == 0)
            return;
        switch (ft.kind)
        {
        case FileType::Kind::Int:
            r.svarint();
            break;
        case FileType::Kind::Float:
            r.fixed<double>();
            break;
        case FileType::Kind::Bool:
            r.u8();
            break;
        case FileType::Kind::Str:
        case FileType::Kind::Enum:
        case FileType::Kind::Ref:
            r.varint();
            break;
        case FileType::Kind::Struct:
            for (const FileField &field : ft.structure->fields)
                skip_value(r, field.type, field.type.presence, field.type.list, ctx);
            break;
        case FileType::Kind::Owner:
            if (r.u8() != 0)
            {
                r.varint(); // option name
                r.varint(); // parent row
            }
            break;
        }
    }
}
