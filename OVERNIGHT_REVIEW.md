# Overnight review — 2026-09-27/28

Scope, as requested: **continue with the native file format update**
(NATIVE_FILE_FORMAT_RESEARCH.md). Phase 1 (schema descriptor, fingerprint,
schema history) was done earlier in the day. Tonight picks up Phase 2
onwards. The usual pattern applies: research → implement → test, per
piece. Judgment calls are marked **Judgment call:** so they're easy to find
and override.

The previous night's review (2026-09-26/27) was replaced without explicit
permission to delete it, following the same precedent that night set. It
is committed (`7ba0cf1`), so `git show 7ba0cf1:OVERNIGHT_REVIEW.md`
recovers it.

**Commit policy for tonight: nothing is committed.** The skill's
"exception to the exception" names this case: *"a data-correctness change
to a file writer"*. A brand-new on-disk format that users will rely on
being readable forever is the highest-blast-radius kind of change there
is. Phase 1 (from the daytime session) was never committed either, and
tonight's work builds directly on it. So everything is left in the
working tree for sign-off, and the sections below are written to make
that review quick.

## Summary

| Piece | State |
|---|---|
| 1. Codegen support (pool bulk load, index rebuild, generated tables) | done, tested |
| 2. `persistence` library: the `.led` container, save and load | done, tested |
| 3. `le_write_db` / `le_read_db` / `le_db_info`, TCL `write_db` / `read_db` / `db_info` | done, tested |
| 4. Golden-file corpus (§4.6 of the research doc) | done, first version (0.49.0) |
| 5. Benchmark against `read_def` | done: ~10× faster cold load, ~7× smaller file |
| 6. Phase 4, part one: migration files, symbolic replay, `makemigration`, renames applied on load | done, tested |
| Phase 4, part two: `DynamicDb` data runtime (`ConvertField`, `ExtractToChild`, ...) | **not started**: see the closing summary |

Test state at the end: codegen unit tests are 30 run, and only the same 3
failures as before tonight (`test_cli`, `test_solar_system` and
`test_validation`, stale upstream expectations untouched since the fork).
Backend ctest is all green; see the closing summary for the final count.

---

## 1. Codegen support

**What was needed.** Loading a file must fill each `Pool` directly and then
rebuild the indexes. Replaying `create_x()` per object would need an id
remap, a topological create order, and one change-log entry per object,
and it would be far slower (research doc §5). Nothing in the generated
`Root` allowed that, and the persistence code needed a per-class
description of every stored field.

**Changes** (all in codegen templates; the generated code stays uncommitted
as before):
- `pool_hpp_j2.py`:
  - `load_dense(values)` and `load_dense(n)` (rows become `Id{i, 0}`)
  - `alive_count()`
  - **Bug fix:** `Pool::clear()` cleared `slots_` but not `free_`, so the
    next `create()` could pop a slot index past the end of the empty
    vector and write out of bounds. It was latent: only the generated,
    never-called `Root::clear_<x>()` used it.
- `root_hpp_j2.py`:
  - `create_<x>()`'s index maintenance is factored into a private
    `index_insert_<x>_()`. `create_<x>` is unchanged in behaviour and the
    full suite still passes.
  - New `pool_<x>()` accessors, documented as "native file format only".
  - `clear_all()`.
  - `rebuild_indexes()` and a per-class `rebuild_<x>_index()`. These report
    `unique_per_parent` violations instead of silently overwriting.
  - `replace_contents_from(Root&&)`. It keeps `mutation_version()`, so a
    version-keyed cache can't mistake the loaded contents for an old
    version.
- New `native_tables_hpp_j2.py` → `generated/native_tables.hpp`:
  - `Fields<T>::members`, a tuple of `{name, &T::member}` for every pooled
    class and embedded struct
  - `Pooled<Tag>`: name, dense index, pool accessor, `list_order()`,
    `rebuild_index()`
  - `EnumInfo<E>`: stored by *name*
  - `for_each_pooled()`

  The persistence library is generic over these tables, so there is no
  per-class hand-written code.
- `descriptor.py` gains an explicit `"presence": true` on fields stored as
  `std::optional`. Without it, a reader has to re-derive the rule "list
  wins over optional; references are a bare `Id`" (`Field.get_cpp_type()`).
  With it, files stay decodable even if codegen's C++ type rules change.

  **Judgment call:** this changed the fingerprint, so the uncommitted
  baseline snapshot `schema_history/0.49.0.json` was rewritten with
  `--update-snapshot`. The fingerprint is now `db27ba6b68a31200`.

## 2. The `persistence` library (`backend/src/persistence/`)

The files:
- `byte_io.hpp`: varint and zig-zag encoding, CRC-32, and a bounds-checked
  reader.
- `codec.hpp`: value encode, name-matching decode, and compatibility
  checks.
- `native_format.{hpp,cpp}`: the container, `save_native`, `load_native`,
  `inspect_native` and `database_is_empty`.

The format is documented at the top of `native_format.hpp`.

**Save:**
- Rows are written in each parent's own **child-list order**
  (`Pooled::list_order`), not slot order. As a result the rebuilt indexes
  reproduce every child list exactly, even after deletes and slot reuse.
  `ChildListOrderSurvivesDeletedAndReusedSlots` fails with plain slot order
  (verified: `M1, M4, V1` instead of `M1, V1, M4`).
- The file is written to `path.tmp`, then `fsync`, then `rename`.
- A reference to a dead object is written as unset and counted
  (`dangling_references`, logged by `le_write_db`).

**Load:**
- The file's embedded schema drives decoding. Classes and fields are
  matched **by name**.
- **Handled with a warning:** added, removed or reordered classes and
  fields. A value can widen `T` → `optional<T>` → `list<T>`, or int →
  double.
- **Refused with "a migration is needed":** a retyped field, a removed enum
  value, or optional → required.
- **Refused as newer:** a newer schema version or container version.
- **Checked:** every reference is range-checked. Every block has a CRC. A
  truncated file is detected by the missing `END` chunk.
- Loading goes into a temporary `Root` and is swapped in only when nothing
  can fail any more. On any error the target is untouched; every corruption
  test asserts this.

**Judgment calls:**
- **No chunk directory or footer.** The research doc sketched one; a
  sequential chunk scan is just as cheap. Truncation is caught by the
  `END` chunk instead.
- **Row-major values within a column,** rather than the doc's
  Arrow-style sub-columns for nested structs. It is far simpler, and
  field-level evolution is unaffected.
- **Columns are split into 65,536-row segments,** each its own
  compressed block. Save compresses segments in parallel. Load
  deliberately decodes **one task per column**, not per segment:
  measurements are in §5.
- **Exceptions stay inside the library.** `FormatError` is caught at the
  `save_native` / `load_native` boundary and returned as a message, which
  keeps the project's no-exceptions convention at the API.
- **zstd** is new: v1.5.6 via `FetchContent`, pinned by SHA-256 (the
  release tarball hash, verified locally), static and single-threaded.
  Level 3 is the default. `compression_level <= 0` stores raw blocks; the
  tests use this to edit files.

**Tests** (`src/persistence/tests/native_format_test.cpp`, 26):
- A hand-built design round-trips: values, optionals, enums, indexes,
  per-parent unique lookups.
- The LEF + DEF + Verilog fixtures (`complete.5.8.lef`/`.def` and
  `gate_netlist_clean.v`, 40+ classes) round-trip **byte-identically**
  when re-saved.
- A multi-segment class (150k nets) round-trips.
- Child-list order, dangling references, and replace-not-damage when
  saving.
- Corruption: truncation, a flipped byte, bad magic, a newer container,
  and a missing file.
- **Older schemas.** The test has its own independent reader and writer
  for uncompressed files, written from the format comment rather than the
  implementation. It edits a file into what an older build would have
  written:
  - a renamed field (drop plus default)
  - reordered fields
  - a removed class
  - a retyped field (refused)
  - an unknown enum value (refused)
  - a newer version (refused)
  - an out-of-range reference (refused)
  - a duplicate unique name (refused)
- Codec-level cases: optional → list, int → double, optional → required,
  narrowing overflow, varints, and the CRC-32 check value.

## 3. API and TCL commands

**API** (`api.hpp`):
- `le_write_db`:
  - Uses a write lock, the same as `le_write_def`.
  - Marks the database saved, for item 18's exit dialog.
  - Logs dangling references.
- `le_read_db`:
  - Clears undo/redo; this needed `CommandHistory::clear_undo_redo()`.
  - Clears the selection and the current-* ids.
  - Then does what `le_read_lef` does once a Technology exists: hides
    layers that aren't ROUTING or CUT, rebuilds the view layers, applies
    any pending grid, and sets the current technology.
  - Leaves the database saved.
  - Logs each schema-difference warning.
- `le_db_info`: a text summary.

**TCL:** `write_db <file>` errors on failure, like `write_def`. `read_db
<file>` returns a status, like `read_def`. `db_info <file>`. All three
register help, and `TCL_COMMANDS.md` was regenerated; its diff is exactly
the three new entries.

**Judgment calls:**
- **`read_db` only loads into an empty session** (no objects at all).
  Replacing a populated session needs a proper "close design" path that
  resets every `LeHandle` cache, and that's a bigger, riskier change than
  tonight warranted. The error message says why. Worth a follow-up.
- **Saving holds the handle's write lock for its whole duration,** so the
  GUI waits during a save: about 6 s on the largest test design. This is
  the research doc's open question 6. Fine for now.

**Tests:**
- `src/api/tests/native_db_api_test.cpp` (5): round-trip into a fresh
  handle (design count, layer count, abstract and layout views, not
  unsaved, nothing to undo), the non-empty-session refusal, bad files,
  write failures, and `db_info`.
- `src/tcl/tests/native_db_test.tcl` (ctest `le_tcl_native_db`):
  `read_verilog`, then `write_db` and `db_info`. It then re-runs itself in
  a second `tclsh`, because `read_db` needs an empty session, and checks
  that all 7 nets come back.

## 4. Golden-file corpus

- `src/persistence/tests/golden/0.49.0/` holds three uncompressed sample
  files (`lef_def`, `testcell`, `netlist`; 184 KB in total) and a
  `manifest.json` of per-class object counts.
- `GoldenFiles.EveryVersionsFilesStillLoad` loads every version's files and
  checks their counts.
- For the current version, it also checks that re-saving is
  byte-identical. That catches a silent encoding change that isn't
  accompanied by a version bump.
- It fails if the current schema version has no golden files. The
  `regen-database` skill now says to generate them with the disabled
  `GoldenFiles.DISABLED_WriteForCurrentSchemaVersion` test and commit them
  with each schema bump.
- File names carry the container version (`*.c1.led`), so a future
  container change adds files instead of replacing the v1 evidence.

**Judgment call:** the files live under `backend/src/persistence/tests/`,
not `test_data/` as the research doc suggested, because `test_data/` isn't
tracked by git.

## 5. Performance

New dev tool: `native_format_profile <label> [levels] [threads]`, the same
style as `resolver_profile`. Release build, cold process:

| | aes_scaling_1x1 | aes_scaling_8x8 |
|---|---|---|
| LEF + DEF read | 862 ms (24 MB DEF) | 52.9 s (1476 MB DEF) |
| `.led` save, zstd level 3 | 91 ms | 6.4 s |
| `.led` file, zstd level 3 | 3.0 MB | 203 MB |
| `.led` load (cold) | 56 ms | **5.3 s** |
| re-save byte-identical | yes | yes |

The 8x8 load went from 8.2 s to 5.3 s through three changes, each measured
before it was made:

| Change | Result |
|---|---|
| Decode columns in parallel | decode 5.5 → 3.0 s |
| Parallel per-class index rebuild | 2.0 → 1.45 s |
| Build pool rows in place | 1.5 → 0.84 s |

Wider parallelism made cold loads **slower**. Decoding every segment in
parallel took 6.4 s, versus 4.4 s single-threaded and 3.0 s one task per
column. Decoding is dominated by allocating millions of small per-row
vectors in fresh memory, which contends under WSL2. This is recorded in a
comment at the parallel decode site.

## 6. Phase 4, part one: the migration chain

**What was needed.** Research doc §4: every schema change carries a
declarative migration, replayed symbolically so the chain is proven to
reach the current schema, plus a runtime that uses it to read older files.

**Done:**
- **`codegen/codegen/migration.py`** has the ops:
  - classes: `AddClass`, `RemoveClass`, `RenameClass`
  - fields: `AddField`, `RemoveField`, `RenameField`, `AlterField`
  - enum values: `Add`/`Remove(map_to=)`/`Rename`/`AlterEnumValue`
  - `RunCode` and `Todo`

  Each op has a symbolic effect on a descriptor and legality checks. The
  module also has the `Migration` file format, `load_migrations`,
  `replay`, `check_migrations`, `draft_ops` (with rename detection),
  rendering, and `runtime_table`.
- **Generation enforces the chain.** Every `codegen --target database` now
  runs `check_migrations`. It fails when:
  - a snapshotted version has no migration
  - a migration doesn't land exactly on its version's snapshot (the error
    lists what's left, e.g. `+ Net.w`)
  - a `Todo` remains
  - the chain doesn't reach the current schema
- **New CLI targets:** `--target makemigration --name X [--non-interactive]`
  and `--target checkmigrations`. `--output` is now needed only for
  `database` and `tcl`.
- **The loader applies renames.** The generated `migrations.hpp` lists the
  ops with a runtime effect. The loader applies every op past the file's
  version to the file's embedded schema before name matching. Class, field
  and enum-value renames therefore keep their data (enum renames compose
  across migrations), and ops the data runtime would be needed for refuse
  the file with the migration's description.
  `load_native(root, path, chain)` takes an explicit chain for tests.
- **End-to-end check** on a scratch copy of the real schema (`Net.bit_index`
  → `bit`, 0.49.0 → 0.50.0):
  1. Generation fails with "no migration".
  2. `makemigration --non-interactive` drafts a `Todo` rename question.
  3. Replacing it with `RenameField` makes generation pass.
  4. `migrations.hpp` carries the rename.

**Not done (next session):** the research doc's `DynamicDb` data runtime,
and with it `ConvertField` (string → reference lookups, scaling),
`ExtractToChild`/`InlineChild`, `SplitClass`/`MergeClasses`, and real
`RunCode` bodies. Until then, a file that needs one of these is refused,
never misread. The core/extension interleaving in research doc §4.8 also
waits for the extension mechanism itself.

**Judgment calls:**
- **A migration is required for every version bump,** as the research doc
  intends. This replaces the phase-1 "was the version bumped?" rule. The
  generation that writes a new snapshot is the one that then fails for the
  missing migration. That keeps the order simple: bump, regenerate, draft
  the migration, regenerate.
- **`AlterField` covers only storage-compatible type changes** (int →
  float, T → optional → list); the loader already accepts those. A real
  conversion will be a `ConvertField` op once the data runtime exists.
- **"applied migration to X: …" lines go into `LoadReport::warnings`,** so
  `read_db` logs them. They're informational; a separate `notes` list
  would also be fine.

**Tests:**
- `codegen/tests/test_migration.py` (13): op effects and illegal ops,
  runtime entries, drafting with and without rename answers, the rendered
  migration loading and replaying, chain checks (missing, incomplete,
  `Todo`, wrong start, schema beyond the chain), and the generator
  integration including `migrations.hpp` content.
- 6 loader tests in `native_format_test.cpp`: a renamed field keeps its
  data, migrations at or before the file's version are skipped, a renamed
  class keeps data and references, enum renames compose, an unsupported op
  refuses, and a current-schema file ignores the chain.

## Late fixes from a self-review of `native_format.cpp`

- **Duplicate columns.** A column stored twice, or two stored columns that
  map to one member after a rename, would have been two parallel decode
  jobs writing the same memory. Such files are now refused with
  "X.y is stored twice". Test: `ColumnStoredTwiceIsRejected`.
- **Row-count bound.** A class's row count (the class header isn't
  checksummed) must fit the 32-bit id range, so a corrupt count can't
  trigger a huge allocation.

---

## Closing summary

**Final state:**
- Backend ctest: **961/961 passing** (Debug), and `build_release` is
  rebuilt (`api pipelines io le_shell le_tcl native_format_profile`).
- codegen: 43 tests, with only the same 3 failures as before tonight.
- **Nothing is committed or pushed.** See the note at the top.

**Needs your sign-off (everything, as one change):**
1. **The container format itself** (`native_format.hpp` header comment).
   Once a user has a `.led` file this layout is permanent, so it's worth
   one careful read before the first commit: chunks, 65,536-row column
   segments, row-major structs, enums stored by name, and the explicit
   `presence` flag.
2. **Committed artifacts that must go in together:**
   - `backend/src/database/schema_history/0.49.0.json`: the baseline,
     fingerprint `db27ba6b68a31200`
   - `backend/src/persistence/tests/golden/0.49.0/`
3. **zstd** is a new third-party dependency.
4. **`read_db` loads into an empty session only** (§3 judgment call).

**Follow-ups for `BUGS_AND_ENHANCEMENTS.md`:**
- **Phase 4, part two:** the `DynamicDb` data runtime and the ops that
  need it (`ConvertField` with `LookupBy`/`ScaleBy`, `ExtractToChild`/
  `InlineChild`, `SplitClass`/`MergeClasses`, `RunCode`).
- **A "close design" / "new session" command** that resets every
  `LeHandle` cache. It would let `read_db` replace a populated session,
  and back a GUI File → Open.
- **Phase 5:** the SESSION chunk (view-layer colours, current ids,
  viewport), and the GUI File → Open / Save / Save As items. The latter
  need the menu bar proposed in EXTENSION_MECHANISM_RESEARCH.md.
- **Phase 6 performance:**
  - Saving holds the write lock for its whole duration (about 6 s on
    8x8); consider a copy-on-write snapshot.
  - Save's encode is single-threaded per column. Deterministic
    string-table ids are what stops it being parallel; a per-segment
    local table could fix that.
  - A cold load is allocation-bound: millions of small `std::vector`s per
    `Shape`. A different allocator (tbbmalloc) or a flatter `Shape`
    layout would help both loading and `read_def`.
- **Pre-existing, noticed on the way:** three codegen tests have been
  failing since the fork (`test_cli` and `test_solar_system` expect
  upstream cmg's `.cpp`/CMake output; `test_validation` expects 11 errors
  and gets 6).
