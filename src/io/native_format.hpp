#pragma once
#include "generated/database/migrations.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace le
{
    class Root;
}

// The native Layout Engine database file (.led). Design: plans/NATIVE_FILE_FORMAT_RESEARCH.md.
//
// A self-describing, columnar, zstd-compressed container: the file embeds
// the schema it was written with, and loading matches classes and fields
// by name, so a file written by an older schema whose changes since were
// only additions, removals or reorderings loads with no migration code.
// Anything else (a renamed or retyped field, a removed enum value) is
// reported as needing a migration (see migrations.hpp).
//
// Layout:
//   header   "LEDB\r\n\x1a\n", u32 container version, u32 flags
//   chunks   tag[4], u64 payload size, payload - in this order:
//     SCHM   block: JSON {"core": {"version", "fingerprint", "descriptor"}, "writer"}
//     CLAS   one per non-empty pooled class: name, u64 rows, u32 columns,
//            then per column: field name, u32 segments, and per segment
//            (65536 rows each, the last one fewer): u64 rows, block -
//            so one large column decodes on many threads
//     STRS   block: the string table (varint count, then varint-length strings)
//     END    empty - its absence means a truncated file
//   block    u8 codec (0 raw, 1 zstd), u64 raw size, u64 stored size,
//            u32 CRC-32 of the stored bytes, stored bytes
//
// Rows are written in each parent's own child-list order, so the indexes
// rebuilt on load reproduce every child list's order exactly (not just
// the set of children).

namespace le::persistence
{
    inline constexpr uint32_t kContainerVersion = 1;

    struct SaveOptions
    {
        /// zstd level (1 fastest .. 19 smallest); 0 or less stores every
        /// block uncompressed (debugging, and tests that edit files).
        int compression_level = 3;
        /// The session to save beside the database (a JSON object; empty:
        /// none). An object of the form {"$ref": "<Class>", "index": i,
        /// "generation": g} anywhere in it is an object reference: it is
        /// stored as that object's row in the file, or null if the object
        /// no longer exists.
        std::string session_json;
    };

    struct SaveReport
    {
        std::string error; // empty on success
        uint64_t file_bytes = 0;
        uint64_t objects = 0;
        /// References to objects that no longer exist, written as unset.
        uint64_t dangling_references = 0;
        /// Where the time went (ms), for profiling: "order", "encode <Class>", "write".
        std::vector<std::pair<std::string, double>> phase_ms;
        bool ok() const { return error.empty(); }
    };

    /// @brief Write `root` to `path`, via `path`.tmp and a rename, so a
    /// failed or interrupted save never damages an existing file.
    SaveReport save_native(const Root &root, const std::string &path, const SaveOptions &options = {});

    struct LoadReport
    {
        std::string error; // empty on success
        /// Things dropped or defaulted because the file's schema differs.
        std::vector<std::string> warnings;
        std::string file_schema_version;
        std::string file_fingerprint;
        /// The file's schema, core and extensions, is exactly this build's
        /// (no name matching needed).
        bool schema_matches = false;
        uint64_t objects = 0;
        /// The file's session (empty if it has none), its object references
        /// as {"$ref": "<Class>", "index": row, "generation": 0} - the
        /// loaded object's id - or null if the class or row is gone.
        std::string session_json;
        /// Where the time went (ms), for profiling: "read", "decode <Class>", "index".
        std::vector<std::pair<std::string, double>> phase_ms;
        bool ok() const { return error.empty(); }
    };

    /// @brief Replace `root`'s contents with the file's. On any error,
    /// `root` is left exactly as it was.
    LoadReport load_native(Root &root, const std::string &path);

    /// @brief load_native() with an explicit migration chain instead of the
    /// schema's own (migrations::kOps) - for tests. Before matching an
    /// older file by name, every op of every migration past the file's
    /// schema version is applied to the file's embedded schema: renamed
    /// classes, fields and enum values take their current names, so their
    /// data is kept rather than dropped. An op that needs more than a
    /// rename (migrations::OpKind::Unsupported) refuses the file.
    LoadReport load_native(Root &root, const std::string &path, std::span<const migrations::Op> chain);

    struct FileInfo
    {
        std::string error; // empty on success
        uint32_t container_version = 0;
        std::string schema_version;
        std::string fingerprint;
        std::string writer;
        bool schema_matches = false;
        uint64_t file_bytes = 0;
        /// (class name, rows) for every class stored in the file.
        std::vector<std::pair<std::string, uint64_t>> classes;
        /// The extensions whose objects the file holds.
        struct Extension
        {
            std::string name;
            std::string package_version;
            std::string schema_version;
            std::string built_schema_version; // this build's, or empty if it doesn't have the extension
        };
        std::vector<Extension> extensions;
        /// The migrations loading the file runs, in order: core's past the
        /// file's version, and each extension's past the file's version of it.
        struct Migration
        {
            std::string extension; // "" for core
            std::string to_version;
            std::string description;
            bool unsupported = false; // can't be applied yet, so the file can't be opened
        };
        std::vector<Migration> migrations;
        bool has_session = false;
        bool ok() const { return error.empty(); }
    };

    /// @brief Describe a file without loading it.
    FileInfo inspect_native(const std::string &path);

    /// @brief inspect_native() against an explicit migration plan instead of
    /// the schema's own (migrations::kMigrations) - for tests.
    FileInfo inspect_native(const std::string &path, std::span<const migrations::Migration> plan);

    struct MigrateReport
    {
        std::string error; // empty on success
        std::string from_schema_version;
        /// The load's report: migrations applied, anything dropped or defaulted.
        std::vector<std::string> warnings;
        uint64_t objects = 0;
        uint64_t file_bytes = 0;
        bool ok() const { return error.empty(); }
    };

    /// @brief Load `in` and save it as `out` with this build's schema,
    /// keeping its session. `out` may be `in`: the save replaces it only
    /// once complete.
    MigrateReport migrate_native(const std::string &in, const std::string &out, const SaveOptions &options = {});

    /// @brief Whether `root` holds no objects at all.
    bool database_is_empty(const Root &root);
}
