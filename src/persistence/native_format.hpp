#pragma once
#include "migrations.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace le
{
    class Root;
}

// The native Layout Engine database file (.led) - NATIVE_FILE_FORMAT_RESEARCH.md.
//
// A self-describing, columnar, zstd-compressed container: the file embeds
// the schema it was written with, and loading matches classes and fields
// by name, so a file written by an older schema whose changes since were
// only additions, removals or reorderings loads with no migration code.
// Anything else (a renamed or retyped field, a removed enum value) is
// reported as needing a migration (the migration chain is Phase 4).
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
        /// The file's schema is exactly this build's (no name matching needed).
        bool schema_matches = false;
        uint64_t objects = 0;
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
        bool ok() const { return error.empty(); }
    };

    /// @brief Describe a file without loading it.
    FileInfo inspect_native(const std::string &path);

    /// @brief Whether `root` holds no objects at all.
    bool database_is_empty(const Root &root);
}
