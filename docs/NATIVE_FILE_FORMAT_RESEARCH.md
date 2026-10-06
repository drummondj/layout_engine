# Native File Format — Architecture Research

Follow-up to [EXTENSION_MECHANISM_RESEARCH.md](EXTENSION_MECHANISM_RESEARCH.md) §4 ("Open issue: persistence"). How a project's set of extensions is chosen and pinned is in [PACKAGE_MANAGER_RESEARCH.md](PACKAGE_MANAGER_RESEARCH.md).

**Status:** phases 1–3 are built (`src/io/native_format.*`, `write_db`/`read_db`/`db_info`). Phase 4 is partly built: renames are applied, but structural ops are not yet. See §10. Where the as-built format differs from the original design, the sections below describe what was built and mark the rest as planned.

## Goal

Layout Engine needs its own native file format, so a session's database can be saved and loaded directly instead of being rebuilt from LEF, DEF and Verilog. The hard requirement:

> **A build of Layout Engine must always be able to read a file written by any older version, whatever the schema looked like when that file was written.**

Secondary goals:
- **Speed.** Loading should be much faster than reparsing DEF. Test designs reach 2.4 GB of DEF (`test_data/aes_4M.def`).
- **Size.** Files should be compact.
- **Extension objects** (see the extension research) must round-trip.
- Corruption should be detected rather than silently loaded.

## Summary of the recommendation

| Concern | Recommendation |
|---|---|
| Encoding | A custom **self-describing, columnar binary** container, `.led`. **codegen** generates both the reader and the writer from `schema.py`. Chunks are zstd-compressed and each carries a CRC. |
| Old-schema reading | The file **embeds the schema it was written with**: class names, field names and types. It records the schema version too. |
| Schema changes | An ordered chain of **declarative migration files** (`migrations/NNNN_*.py`), one per schema version. Each is a list of typed ops (`AddField`, `RenameField`, `ConvertField`, `ExtractToChild`, ...). `codegen --target makemigration` drafts each one by diffing the newest schema snapshot against `schema.py`. codegen compiles the chain into C++ that runs on a generic, name-based in-memory form of the file. |
| Build-time proof | codegen replays the whole chain symbolically from the first snapshot. The result must equal the current `schema.py`, or the build fails. So every schema change must come with a migration, and every op must be valid against the schema it applies to. |
| The "always" guarantee | The chain proves the *shape* is right. A **golden-file corpus** proves the *data* is right: every schema version ever released has sample files checked in, and CI loads them all on every build. |
| Loading | Pools are filled directly with dense ids (`Pool::load_dense`), then indexes are rebuilt from generated code (`Root::rebuild_indexes()`). `create_x()` is not called per object. |

**Rejected:**
- Protobuf and FlatBuffers. Both need hand-maintained stable field numbers, both have size-limit and speed problems at this scale, and neither handles structural migrations.
- SQLite. Poor fit for nested geometry lists, and slow bulk loading.
- Parquet/Arrow. A heavy dependency, and still no answer for structural migrations.
- A TCL-script dump. Slow, and it can't be migrated.
- Version-chained typed readers with no embedded schema. Every old reader has to be kept compiling forever.

---

## 1. The database being saved

Paths below are relative to the repo root.

**Database shape**
- Every pooled class is a plain struct, `XxxData` (for example the generated `net.hpp`, `NetData`), stored in `Pool<XxxData, XxxId>` (generated `pool.hpp`). Generated code lives in `<build>/generated/database/`.
- A pool is a slot vector with `generation` and `alive` flags plus a free list. An `Id` is `{uint32 index, uint32 generation}` (generated `ids.hpp`).

**Field types**, all from codegen's `TYPEMAP` (`codegen/codegen/schema.py:11`):
- scalars (`int`, `double`, `bool`, `str`, `dbu`/`dbu2` = `int64_t`)
- enums (`enum class ShapePurpose : uint8_t` with explicit values)
- references (`NetId`)
- embedded non-pooled structs (`Rect`, `Path`, `Polygon`, `Point`)
- `std::optional<T>` and `std::vector<T>` of any of the above

`ShapeData` (generated `shape.hpp`) is the most demanding case: one parent reference per possible owner, plus a list of embedded structs per geometry kind.

**Relationships**
- Stored **only as the child's reference to its parent**, e.g. `NetData::schematic`.
- Parent → children lists and name lookups are **derived**, in `Root::index_` (generated `index.hpp`). `create_x()` maintains them incrementally (`create_net`, generated `root.hpp`).
- As a result, only the pools need saving. Indexes can always be rebuilt.

**Versioning**
- `Schema.version` is `"0.50.0"`. It is the baseline: `src/database/schema_history/0.50.0.json` is the only snapshot, and no migration exists yet (0.49.0 was re-baselined away by the `Shape.owner` change, §4.1). Every `codegen --target database` run checks the schema against the history (§4.5), so the version can't drift from the schema unnoticed.
- The schema *does* change structurally, not just additively. Example: commit `d5d16a2` (before the baseline) turned `Abstract.boundary` from a `List[Polygon]` field into a child `Shape` object, and `Layout.diearea` likewise. An older file that holds polygon lists must become `Shape` objects on load. That is exactly the kind of change a naive format can't survive.

**Support in codegen**
- `Field.default` exists and is validated (`codegen/codegen/validation.py:150`).
- `EnumValue` carries a name and a value (`codegen/codegen/schema.py:3868`).
- `Pool::clear()`/`Pool::load_dense()` exist, and so do `Root::clear_<x>()` per class, `Root::rebuild_indexes()` and `Root::replace_contents_from()`.

**Not database state, so not saved** (members of `LeHandle`, `src/api/le_handle.hpp`):
- `command_history` (undo)
- the render pipeline's caches
- the flightline `NetEndpointIndex`
- the change log

`view_layers` (colours and visibility) and the per-class "current" ids are session state. They are worth saving optionally (§8).

---

## 2. Options considered

| Option | Old-schema reads | Structural migrations | Speed / size at 1–2 GB | Dependencies | Verdict |
|---|---|---|---|---|---|
| **A. Custom self-describing columnar (codegen)** | Automatic for add/remove/reorder; declared renames | Yes, on the generic form | Excellent: columnar + zstd, parallel chunks | zstd only | **Recommended** |
| B. Protobuf / FlatBuffers / Cap'n Proto | Via hand-assigned field numbers | No (only "unknown field" skipping) | Protobuf: 2 GB message limit, slow repeated nested messages. FlatBuffers: fast but tedious to evolve | Large codegen toolchain | Reject |
| C. SQLite (table per class) | `ALTER TABLE` plus version table | SQL migrations; nested lists become blobs | Slow bulk insert and read-back; large files | sqlite3 | Reject |
| D. Parquet / Arrow IPC | Name-based, self-describing | No | Very good | Arrow C++ is huge | Reject; borrow its ideas |
| E. TCL script (`create_x ...` lines) | Only if every old command stays valid | Hand-written | Very slow at millions of objects | none | Keep as a debugging export only |
| F. Version number + one typed reader per old version | Yes, by keeping every old reader | Yes | Good | none | Reject: old readers depend on old generated structs, which is unmaintainable |

The deciding factor is migrations. Every off-the-shelf format handles *additive* evolution. None of them helps with "this list of polygons is now a child Shape object". Option A pairs a self-describing file with a structured, declarative migration chain (§4), and codegen already knows everything needed to generate both.

---

## 3. The recommended format: `.led`

### Container layout

As built (`src/io/native_format.hpp`, container version 1):

```
header   "LEDB\r\n\x1a\n", u32 container version, u32 flags
chunks   tag[4], u64 payload size, payload - in this order:
  SCHM   block: JSON {"core": {"version", "fingerprint", "descriptor"}, "writer",
         "extensions": [{"name", "package_version", "version", "fingerprint",
         "descriptor"}] - only extensions with objects in the file, omitted if none}
  CLAS   one per non-empty pooled class: name, u64 rows, u32 columns,
         then per column: field name, u32 segments, and per segment
         (65,536 rows each, the last one fewer): u64 rows, block
  STRS   block: deduplicated string table (layer, cell and net names repeat heavily)
  END    empty - its absence means a truncated file
block    u8 codec (0 raw, 1 zstd), u64 raw size, u64 stored size,
         u32 CRC-32 of the stored bytes, stored bytes
```

- Every chunk has a type tag and a length, so a reader can **skip chunk types it doesn't recognise**. That is how future chunk kinds stay compatible.
- Segments are independent blocks, so one large column decodes on many threads (TBB).
- Lengths are 64-bit, so there is no 2 GB limit.
- Rows are written in each parent's child-list order, so rebuilt indexes reproduce every child list's order, not just its membership.

**Planned additions**, which old readers skip or ignore:
- an optional `SESS` chunk (§8)

The original design's footer chunk directory and whole-file CRC were dropped. Per-block CRCs plus the END chunk detect corruption and truncation.

### Schema descriptor

A dump of the schema as it was when the file was written: the same JSON as the `schema_history/` snapshots, produced by `codegen/codegen/descriptor.py`. codegen emits it as a constant (generated `schema_version.hpp`), and the writer copies it into SCHM verbatim. In outline:

```
class Net      fields: schematic:ref(Schematic)  name:str  bus:ref(NetBus)?  bit_index:int?
class Shape    fields: ... rects:list(struct Rect) purpose:enum(ShapePurpose)? ...
struct Rect    fields: ll:struct(Point) ur:struct(Point)
enum ShapePurpose  BOUNDARY=0 PLACEMENT_BLOCKAGE=1 DEBUG=2
```

Because the file carries its own schema, the reader never needs the old `schema.py` or the old generated code to decode an old file. It decodes the file generically, then brings it up to date with the migration chain (§4).

### Value encoding (columnar)

Each field of each class becomes one column. As built (`src/io/codec.hpp`), each column holds one value per row:

| Field kind | Encoding |
|---|---|
| Reference (`NetId`) | varint: dense row index of the target within its class + 1, with 0 meaning unset. The writer remaps live slots to 0..n-1, so the file never contains dead slots or generations. |
| owner (`<Klass>Owner`, e.g. `Shape.owner`) | 1 presence byte; if set, varint string-table index of the owner field's **name** (e.g. `route`, `in_layout`), then varint dense row of the parent in that field's class. The descriptor lists it as one field of kind `owner` with its options (name, parent class), so it doesn't depend on which extensions are built in. |
| `int` / `dbu` / `dbu2` | zig-zag varint |
| `double` | raw 8 bytes |
| `bool` | 1 byte |
| `str` | varint index into the STRS table |
| enum | varint string-table index of the value's **name**, never the ordinal, so reordering or renumbering enum values is harmless |
| `optional<T>` | 1 presence byte, then the value if present |
| `vector<T>` | varint count, then the elements |
| embedded struct | its stored fields in the file's declared order (row-major inside the column) |

Columnar layout plus zstd compresses coordinate data very well, since neighbouring values are similar. It also makes schema evolution cheap: a removed field is a column the reader skips, and an added field is a column that isn't there. The original design's Arrow-style refinements (bit-packed bools and presence bitmaps, delta encoding, offsets columns, struct members flattened into sub-columns) were left out; they are candidates for phase 6 if a benchmark shows they pay off.

---

## 4. Schema migrations: reading old files

Schema evolution is handled by a single mechanism: an **ordered chain of declarative migration files**. The design follows Django migrations and Alembic. Each file describes exactly how data shaped like schema version *N* becomes data shaped like version *N+1*. `schema.py` only ever describes the **current** schema; it carries no rename history or conversion rules.

### 4.1 Three artifacts per schema version

| Artifact | Location | Written by | Purpose |
|---|---|---|---|
| Schema snapshot | `src/database/schema_history/<version>.json` | codegen, on every version bump | The exact shape of version *N*: classes, fields, types, enums, parent relations. The "before" side of the next migration, and the reference the chain is checked against. |
| Migration file | `src/database/migrations/NNNN_<slug>.py` (codegen's default `--migrations` directory; created by the first migration) | drafted by `codegen --target makemigration`, finished and reviewed by the author | Ordered ops transforming *N-1* → *N*. |
| Golden files | `src/io/tests/golden/<version>/<sample>.c<container>.led` plus a `manifest.json` of per-class object counts | the disabled test `GoldenFiles.DISABLED_WriteForCurrentSchemaVersion` (`src/io/tests/golden_files_test.cpp`), run once per new version | Real data at version *N*, loaded by CI forever after. |

**Before the first release** there are no user files to protect, so a schema change may **re-baseline** instead of migrating. The commit bumps the version, deletes the previous snapshot and its golden directory, and commits the new version's snapshot and golden files. The new version is then the baseline, with no migration before it. From the first release on, every schema change carries a migration, and no released version's snapshot or golden files are ever removed.

A schema change is one commit containing:
- the `schema.py` edit
- the new migration file
- the new snapshot
- the new golden files

The migration file is the reviewable statement of "here is how every existing user file will be converted".

### 4.2 Drafting a migration: `codegen --target makemigration`

```
codegen --target makemigration --schema src/database/schema.py --name boundary_to_shape
```

1. **Diff** the newest snapshot against the current `schema.py`, and emit ops for every change that can be detected unambiguously:
   - added and removed classes
   - added and removed fields
   - type changes
   - changes to optional and list flags
   - added, removed and renumbered enum values
   - changed `parent=` relations
2. **Ask about renames.** A removed field and an added field of compatible type in the same class are ambiguous: rename, or remove then add? The tool prompts, as Django does, because guessing wrong silently loses data. Classes and enum values get the same treatment. A `--non-interactive` mode writes a `TODO` op instead.
3. **Mark restructuring with `TODO` ops.** Some changes can't be inferred from a diff. For example, "list field removed from `Abstract`, parent field added on `Shape`" is really `ExtractToChild`. For these the tool writes a `TODO` op with a comment naming the suspected pattern. A `TODO` op fails validation (§4.5), so the build can't pass until the author replaces it.
The author bumps `Schema.version` in `schema.py` first; `makemigration` diffs the newest older snapshot against it and writes only the migration file. The next `codegen --target database` run writes the new version's snapshot (and fails if the schema changed without a version bump).

For a change like `d5d16a2`, the author would finish the draft into the following. (`ExtractToChild` and `ConvertField` are planned ops; see §4.3.)

```python
# src/database/migrations/0012_boundary_to_shape.py
from codegen.migration import *

migration = Migration(
    from_version="0.50.0",
    to_version="0.51.0",
    description="Abstract.boundary / Layout.diearea: Polygon list -> owned BOUNDARY Shape",
    ops=[
        AddField("Shape", "abstract", type="Abstract", parent="boundary"),
        AddField("Shape", "layout",   type="Layout",   parent="diearea"),
        ExtractToChild("Abstract", "boundary", into="Shape", parent_field="abstract",
                       as_field="polygons", set={"purpose": Enum("BOUNDARY")}),
        ExtractToChild("Layout", "diearea", into="Shape", parent_field="layout",
                       as_field="polygons", set={"purpose": Enum("BOUNDARY")}),
    ],
)
```

A string-to-reference change, such as `Shape.layer_name` becoming a `Layer` reference, looks like this:

```python
ops=[
    RenameField("Shape", "layer_name", "layer"),
    ConvertField("Shape", "layer", from_type="str", to_type="Layer",
                 via=LookupBy("Layer", "name", scope="technology"),
                 on_missing=Warn(set_null=True)),
]
```

### 4.3 The operation vocabulary

The ops map onto concepts the schema DSL already has: classes, fields, types, `parent=`, `index=True`, enums.

**Built** (`codegen/codegen/migration.py`): `AddClass`, `RemoveClass`, `RenameClass`, `AddField`, `RemoveField`, `RenameField`, `AlterField` (a storage-compatible retype, standing in for `ConvertField`), `AddEnumValue`, `RemoveEnumValue`, `RenameEnumValue`, `AlterEnumValue`, `RunCode` and `Todo`. All of them replay symbolically. At load time, renames are applied to the file's schema before name matching. Any op that needs to transform data is `Unsupported` in the generated `migrations.hpp`, and a file that needs one is refused, naming the migration. **Planned:** `ConvertField` with converters, `ChangeParent`, `ExtractToChild`/`InlineChild`, `SplitClass`/`MergeClasses`, plus the `DynamicDb` runtime that executes them and `RunCode`.

| Op | What it does | Reversible? |
|---|---|---|
| `AddClass(name)` | New empty pool. | yes |
| `RemoveClass(name, on_data=Drop \| Fail)` | Drops the pool, warning with an object count. `Fail` refuses old files that have data in it. | no |
| `RenameClass(old, new)` | Renames the pool; references to it follow automatically. | yes |
| `AddField(cls, name, type, default=…)` | New column filled with `default`. A required field with no default is a validation error. | yes |
| `RemoveField(cls, name)` | Drops the column. | no |
| `RenameField(cls, old, new)` | Renames the column. | yes |
| `ConvertField(cls, name, from_type, to_type, via=…)` | Retypes a column using a converter:<br>• `Widen`: int→int64/dbu, float→double, T→optional&lt;T&gt;, T→list&lt;T&gt;<br>• `ScaleBy(n)`<br>• `LookupBy(cls, indexed_field, scope=…)`: string → reference, using the indexes codegen already generates<br>• `MapEnum({old: new})`<br>• `FromEnumName`<br>• `Custom("fn")` | depends on converter |
| `RenameEnumValue(enum, old, new)` / `RemoveEnumValue(enum, name, map_to=…)` | Enums are stored by name in the file, so renumbering needs no op at all. | yes / no |
| `ChangeParent(cls, field, new_parent_cls, via=…)` | Re-homes objects under a different owner class; `via` says how to find each object's new owner. | depends |
| `ExtractToChild(cls, field, into, parent_field, as_field, set={…})` | A list or struct field becomes one owned child object (the boundary case). | yes (`InlineChild`) |
| `InlineChild(child_cls, parent_field, into_field)` | The inverse: the child's data folds back into a parent field. | yes |
| `SplitClass(cls, by=field, into={value: new_cls})` / `MergeClasses([a, b], into, tag_field=…)` | Split one class by a discriminator, or merge several into one with a discriminator field (e.g. folding several per-purpose classes into one class with a purpose enum). | yes |
| `RunCode("m0012_fixup")` | Escape hatch: a named, hand-written C++ function over `DynamicDb` (§4.4). Must declare the classes and fields it reads and writes, so the symbolic replay (§4.5) can still track the schema. | no |

The vocabulary is expected to grow. A new op is added when a second `RunCode` needs the same pattern.

### 4.4 How migrations run

1. **Generic decode.** When the file's schema fingerprint differs from the running build's, the reader decodes the file into a **`DynamicDb`**: for each class name, a table of rows, each row mapping field names to `Value` variants (int, double, string, enum-name, ref, list, struct). The decoder is driven entirely by the file's embedded schema descriptor, so any file ever written can be decoded, with no old generated code needed.
2. **Pick the chain.** The chain runs from the file's `schema_version` to the build's. Versions are totally ordered, and each migration has exactly one `from` and one `to`, so the chain is a straight line. Branching is prevented by a validation rule: two migrations may not share a `from_version`.
3. **Run the ops.** codegen compiles every migration file into C++. Today that is the generated `migrations.hpp` table of rename ops. Under this design each op becomes a call into a small runtime in `src/io/` that implements the op vocabulary on `DynamicDb`. `RunCode` ops call the named hand-written functions. Ops run in order, and each migration runs as one step.
4. **Materialize.** The migrated `DynamicDb` now matches the current schema exactly, which §4.5 guarantees. The generic materializer writes it into the pools (§5).

**Fast path:** if the file's fingerprint equals the build's, steps 1–3 are skipped. Generated typed decoders write columns straight into the pools. Old files take the slower generic path, and the app can offer to re-save them in the current format.

**As built,** there is a middle path. The decoder matches the file's classes, fields and struct members by name against the current types (`src/io/codec.hpp`), after applying any renames from `migrations.hpp`. So additions, removals, reorderings and renames already load with no `DynamicDb`; dropped data is reported in `LoadReport::warnings`.

**Why `DynamicDb` and not the generated structs?** A migration written against `0.51.0` must still compile and run when the current schema is `3.0.0`. Ops and `RunCode` functions only ever see names and `Value`s, never `ShapeData`. So nothing in the migration chain depends on types that later change, and **old migrations never need editing**. They are never deleted either.

### 4.5 Validation: the chain must equal the schema

On every build, codegen runs these checks:
1. **Symbolic replay.** Start from the first snapshot and apply every migration's ops to the *schema* rather than to data. Each op has a schema-level effect (`RenameField` renames the field in the descriptor, and so on). The result must **equal** the current `schema.py` descriptor, ignoring descriptions. A mismatch fails the build with a diff. This single check covers three failure modes:
   - someone edited `schema.py` without a migration
   - a migration doesn't match the change it claims to describe
   - the version wasn't bumped (the replayed version would disagree)
2. **Per-op legality**, checked against the schema at that point in the chain. For example:
   - the field being renamed exists
   - `ConvertField`'s converter accepts `from_type → to_type`
   - `LookupBy` names an `index=True` field
   - `AddField` on a required field has a default
   - `ExtractToChild` targets a class that has the named parent field
   - `RunCode` declares its effects
   - no `TODO` ops remain
3. **Snapshot consistency.** After each migration, the intermediate replay result must equal that version's `schema_history/<version>.json`. This catches hand-edited snapshots.

The check runs as part of `codegen --target database` (the `regen-database` flow), and also as a standalone CI step.

### 4.6 Data-level guarantee: the golden corpus

Symbolic replay proves the chain produces the right *shape*. It can't prove the data survives, for example that a `LookupBy` actually finds the layers. So `src/io/tests/golden/<version>/` holds small but representative `.led` files written by each version. Today, version 0.50.0 has `lef_def` (the vendored `complete.5.8` LEF and DEF), `testcell` (a small LEF+DEF) and `netlist` (a gate-level Verilog netlist). Still to add: an RTL (`read_rtl`) sample, and an extension object once `hello_ext` exists.

`GoldenFiles.EveryVersionsFilesStillLoad` (`src/io/tests/golden_files_test.cpp`) loads every file with the current build, which runs the full chain from that version. It checks that every class the build still has keeps its object count from `manifest.json`, and that a current-version file re-saves byte-identically, so the encoding can't drift silently. Spot-value checks are still to add. Adding a version's golden files is part of the schema-change commit (§4.1). As long as this test passes, "reads every older version" is being checked on every build, not just claimed.

### 4.7 Other uses of the same machinery

- **`migrate_db old.led new.led`** (planned TCL command and command-line tool): an offline upgrade, useful for batch-converting archives. Until it exists, `read_db` then `write_db` does the same.
- **`db_info <file>`**: lists the migrations that would run on a file, with their descriptions.
- **Extensions:** each extension keeps its own `migrations/` directory and snapshot history, keyed to *its own* version. Its migrations are interleaved with the core chain (§4.8).
- **Live migration after an extension upgrade:** dump the in-memory `Root` to a `DynamicDb`, run the extension's pending migrations, then re-materialize. This is the same code path, with no file involved.

### 4.8 Core and extension migrations together

An extension's schema depends on the core schema:
- its references point at core classes (`AcmeRouteGuide.net → Net`)
- its classes may have core parents (`parent="acme_route_guides"` on `Net`; the parent's child list is derived, never stored)
- it may **not** add fields to core classes or other extensions' classes (codegen enforces this), so every stored table has exactly one owner

Its migration ops are also written using core names **as they were at the time**. That causes two problems:
1. A later core migration can break extension *data*. If core renames `Net` to `SignalNet`, every extension reference column still says `Net`.
2. Running an old extension migration *after* newer core migrations would look up names that no longer exist.

The answer is **separate version numbers, one shared timeline**. This is the same model as Django's cross-app migration dependencies. Four rules make it work.

**1. Each extension migration records the core version it was written against.** `makemigration` fills it in automatically from the current build:

```python
migration = Migration(
    extension="acme_router", from_version="1.3.0", to_version="1.4.0",
    depends_on_core="0.52.0",          # auto-filled
    ops=[AddField("AcmeRouteGuide", "weight", type="double", default=1.0)],
)
```

Every `.led` file records the core schema version and, separately, each extension's schema version (plus its package version, for messages and for the package manager) in SCHM's `"extensions"` object.

Extension migration versions are the extension's **schema version**, not its package version from `le_extension.toml`. The two only move together when a release changes the schema.

**2. Loading builds one merged, ordered plan.**
- Core migrations form a straight line.
- Each extension migration is placed right after the core migration it depends on, and **before the next core migration**. That's the only point where the names it uses are exactly the names it was written against.
- Core migrations never depend on extensions.
- Ties between extensions are broken deterministically, by extension name.

```
file:  core 0.50, acme 1.2              build: core 0.53, acme 1.4
plan:  C0.51 → C0.52 → A1.3 (dep 0.52) → A1.4 (dep 0.52) → C0.53
```

**3. Core ops rewrite extension data too.** A core op runs over the *whole* `DynamicDb`, extension tables included. The file's embedded schema says which columns in any table reference which class, so each op knows exactly what to rewrite:

| Core op | Effect on extension data |
|---|---|
| `RenameClass` | Reference columns of that type are retagged everywhere, and so are owner options naming the class (built in the symbolic replay; owner values store the option *name*, not the class, so no data changes). |
| `RenameField` of an owner option | Owner values naming that option are rewritten (planned: a rename runtime for owner options, alongside the other rename ops). |
| `RemoveClass` | Extension objects whose *parent* was removed are cascaded away (the normal delete rule). Plain references are nulled, with a warning and a count. |
| `MergeClasses` / `SplitClass` | Reference values are remapped through the op's row mapping. `SplitClass` must say which new class a reference to the old class follows. |

So most core changes need **no action from the extension author**: the core migration already carries their data along.

**4. Validation replays the merged plan against all the schemas.**
- `checkmigrations` replays the merged plan symbolically. The result must equal core `schema.py` plus every `schema_ext.py`.
- During replay, core ops also rewrite the extension schema. For example, `RenameClass(Net → SignalNet)` changes `AcmeRouteGuide.net`'s type in the replayed extension schema.
- An extension whose `schema_ext.py` still says `type="Net"` then fails with a precise message: *"core migration 0013 renamed Net → SignalNet; update schema_ext.py: AcmeRouteGuide.net"*. The author just edits `schema_ext.py`; no extension migration is needed because the data was already handled.
- **Extension migrations are read-only outside their own extension.** An extension's ops may create, change or delete only its own classes. Core classes and other extensions' classes can be read (e.g. a `LookupBy` on `Layer.name`) but never written. codegen enforces this during the replay.
- Core migrations still carry extension data along (rule 3): they retarget references and cascade deletes, but never change an extension class's fields.
- An extension's `depends_on_core` values must never decrease along its chain.

**What a customer sees when upgrading layout_engine:**
1. `le update layout_engine` (or bump the submodule) and build. `checkmigrations` runs the merged replay.
2. If the core change only moved things around (renames, merges), the build at most names the `schema_ext.py` lines to update. Existing files keep loading.
3. If the core change removed or reshaped something the extension relied on, the old data needs real transformation. The customer runs `makemigration` for their extension; the new migration records the new core version, so it is placed after the core change in the plan.
4. The extension's own golden files, each containing core and extension data at a known pair of versions, go through the full merged plan in the customer's CI. Anything the symbolic checks can't catch shows up there, before users hit it.

**Edge case: a lagging branch.** With linear history the "never decrease" rule holds automatically, since `makemigration` always records the current core version. It can fail on a branch that lags behind core. For example, a file written by core 0.53 + acme 1.3 can't then take an acme 1.4 migration written against core 0.52. The loader refuses that case with a clear message ("re-create acme 1.4's migration against core ≥ 0.53") rather than guessing.

**Upstream coverage:** layout_engine's own `hello_ext` example (EXTENSION_MECHANISM_RESEARCH.md §9) should carry golden files and at least one extension migration. Then a core migration that mishandles extension references (rule 3) fails layout_engine's CI, not a customer's.

### 4.9 Newer files in older builds (forward compatibility)

Migrations run **forward only**. Many ops are reversible (see §4.3), but `RemoveField`, `RemoveClass`, lossy converters and `RunCode` aren't, and forward compatibility isn't a requirement. So:
- Each file records its `schema_version`.
- A build refuses a file whose version is newer than its own, with a message naming both versions. **Built:** both a newer schema version and a newer container version are refused (`src/io/native_format.cpp`).
- Optionally (not built), a migration can be marked `additive_only` (only `AddClass`/`AddField`). An older build may then read such a newer file by skipping the unknown columns, since nothing it knows about changed meaning.

---

## 5. Loading into `Root`

For each class:
- `Pool::load_dense(n)` produces `n` alive slots at indices `0..n-1`, all with generation 0.
- Row `i` in the file therefore becomes `XxxId{i, 0}`, so references decode directly into ids with **no remap table**.
- The generated decoder writes each field straight into `XxxData`, rejecting a reference past its target class's last row, an out-of-range integer, or an enum value name that no longer exists.

After all pools are loaded:
- Each class's generated `rebuild_index` rebuilds its part of `index_` from the parent and index metadata codegen already has, in parallel across classes (`Root::rebuild_indexes()` does all of them serially). It is the same logic as the incremental code in `create_x`, just run once, and it reports every `unique_per_parent` violation.
- Everything loads into a separate `Root`. Only when nothing can fail any more does `Root::replace_contents_from()` swap it in. Any failure makes the load fail with a precise error, rather than leaving a half-built database.
- **Not yet checked:** that at most one of `Shape`'s mutually exclusive parent fields is set. A generated validation pass for constraints like this is still to do.

**Why not replay `create_x()` per object?**
- It needs a topological ordering. Non-parent references such as `Net.bus` or `Shape.layer` can point "forward", so a second patch-up pass would be needed.
- It needs per-class id remap tables.
- It fills the change log with millions of entries.
- It is far slower.

Direct fill plus rebuild is simpler and much faster.

**Loading replaces the session wholesale, all under one `HandleWriteLock`:**
- clear `Root`
- load
- clear `command_history`
- reset the change log
- invalidate the `NetEndpointIndex` and the render pipeline
- `bump_mutation_version()`

Loading is not undoable, just as `read_def` into a fresh session isn't. As built, `read_db` only loads into an empty session (`database_is_empty`), so it never has to discard unsaved work.

**Saving:**
- As built, `le_write_db` holds the handle's exclusive `HandleWriteLock` for the whole save, so the GUI blocks until it finishes (6.4 s on `aes_scaling_8x8`, §7). That is intended: a save is rare and short next to a DEF read, and holding the lock guarantees a consistent snapshot without copying the pools.
- Write to `file.led.tmp`, `fsync`, then `rename()`. A crash mid-save never destroys the previous file.

---

## 6. Extension objects

This connects to the extension mechanism research:
- Extension classes are ordinary `Root` classes after the build-time merge, so they are saved like any other.
- SCHM lists each extension's name, schema version and package version separately. Each extension has its own `migrations/` chain and snapshot history in its own tree, keyed to *its* schema version and interleaved with the core chain by the core version each migration was written against (§4.8).
- **As built:** codegen keeps each extension's classes out of the core descriptor (and the child lists synthesized for them off core classes), so core's fingerprint is the same with or without extensions. SCHM's `"extensions"` lists only extensions with objects in the file, so a file holding none of an extension's objects opens in builds without it. The reader merges each listed extension's descriptor into the file's classes before matching by name.
- **Opening a file that contains an extension the running build doesn't have is an error.** `read_db` refuses the whole file, naming each missing extension and the package version that wrote it, before anything is loaded. Inside a project, the message gives the `le add` line that installs it ([PACKAGE_MANAGER_RESEARCH.md](PACKAGE_MANAGER_RESEARCH.md) §8). A file whose extension schema version is newer than the build's is refused the same way, as for core (§4.9). So no build ever drops, or re-saves without, data it doesn't understand.
- References *from* core classes *to* extension classes can't exist, because core never knows about extensions. That keeps the "missing extension" case clean.

---

## 7. Performance expectations

> **Measured (Phase 2):** on `aes_scaling_8x8` (a 1476 MB DEF, Release, WSL2, cold process), `read_def` takes 52.9 s. The `.led` file loads in 5.3 s and saves in 6.4 s, at 203 MB with zstd level 3. On `aes_scaling_1x1`: 862 ms against 56 ms, and 24 MB against 3.0 MB. The dev tool is `native_format_profile`.

- **DEF cost.** Parsing, name lookup and hierarchical linking dominate. The native format stores already-linked, already-resolved references as integers, so loading is essentially decompression plus array fills plus an index rebuild.
- **Parallelism.** Column segments decode in parallel (biggest first), and each class rebuilds its own indexes in parallel. Encoding is still serial.
- **Target:** load at least an order of magnitude faster than `read_def`, with a file several times smaller than the DEF. The measurement above meets both.

---

## 8. What is saved

| Saved | Not saved (derived or transient) |
|---|---|
| Every pool in `Root` (core + extensions) | `Root::index_` (rebuilt) |
| Optional SESSION chunk (JSON): `view_layers` colours and visibility, per-class "current" ids (`has_current_access`), last viewport | Undo history (`command_history`) |
| | Change log, `mutation_version` |
| | Render pipeline caches, `NetEndpointIndex`, flightlines |
| | Global app settings (already stored in `~/.layout_engine/settings.json`) |

The SESSION chunk is JSON so it can evolve loosely: unknown keys are ignored and missing keys get defaults, with no migration machinery needed.

---

## 9. API and user surface

Built:
- **C API:** `le_write_db(handle, path)`, `le_read_db(handle, path)` and `le_db_info(path)` (`src/api/api.hpp:349-365`), following `api.hpp` conventions (int status, errors logged through spdlog).
- **TCL:** `write_db <file>`, `read_db <file>` (empty session only) and `db_info <file>`, registered with `register_command_help` in the usual way. `db_info` prints the file's version, fingerprint and per-class counts without loading it.
- **Developer:** `codegen --target makemigration --name <slug>` drafts the next migration file; `codegen --target checkmigrations` runs the symbolic replay (§4.5).
- A successful `write_db` or `read_db` marks the database saved, so the GUI's exit dialog stops warning about unsaved changes.

Planned:
- `write_db -no_session`.
- `db_info` also lists the file's extensions and the migrations that would run on it. Useful for support, and the package manager reads the extensions list (PACKAGE_MANAGER_RESEARCH.md §8).
- **Offline upgrade:** `migrate_db <in.led> <out.led>`.
- **GUI:** File → Open / Save / Save As, in the menu bar proposed in the extension research. The exit dialog can then offer "Save" directly.

---

## 10. Implementation phases

| Phase | Work |
|---|---|
| 1 ✅ | codegen: schema descriptor, fingerprint, `schema_version.hpp`, the first `schema_history/` snapshot (the baseline, with no migration before it), and a "schema changed without a snapshot" check. **Done:** `codegen/codegen/descriptor.py`; the check runs inside every `codegen --target database` run, and the baseline is `src/database/schema_history/0.50.0.json` (0.49.0 was re-baselined away). |
| 2 ✅ | Container writer/reader (chunks, strings, CRC, zstd), generated typed tables, `Pool::load_dense`, `Root::rebuild_indexes()`. **Done** (`src/io/native_format.*`, `src/io/codec.hpp`). §3 describes the as-built container and value encoding; the name-matching decode (§4.4, additive changes) is built in, while `DynamicDb` waits for phase 4. |
| 3 ✅ | C API, TCL commands, golden corpus (first version) plus the corpus test. **Done:** `le_write_db`/`le_read_db`/`le_db_info`, TCL `write_db`/`read_db`/`db_info`. `read_db` loads into an empty session only. Golden files are in `src/io/tests/golden/<version>/`. |
| 4 🟡 | Migration framework. **Done so far** (`codegen/codegen/migration.py`): the op classes, symbolic replay and per-op validation, which run on every `codegen --target database` and replace the phase-1 check; `makemigration` with diffing and rename prompts (`Todo` when non-interactive); `checkmigrations`. The generated `migrations.hpp` table lets the loader apply **renames** (class, field, enum value, and `RemoveEnumValue(map_to=)`) to an older file's schema before name matching. **Not yet:** `DynamicDb` and the data runtime for `ConvertField`, `ExtractToChild`/`InlineChild`, split/merge and `RunCode`. Those ops are declared unsupported at runtime, so a file needing one is refused with the migration's description. §4.3 lists the built op set. No migration has been written yet: 0.50.0 is the only schema version. |
| 5 | SESSION chunk, GUI File menu, SCHM `"extensions"` object and extension migration chains (needs the extension mechanism's schema phase). |
| 6 | Performance: parallel encode (decode is already parallel), and the Arrow-style encodings §3 left out, each kept only if `native_format_profile` shows a gain. |

---

## 11. Open questions

Settled by the implementation:
- **Migrations in Python, compiled to C++.** Yes: `src/database/migrations/*.py` (`codegen/codegen/migration.py`) are the source of truth, replayed symbolically by codegen and compiled into `migrations.hpp`.
- **Where the golden corpus lives.** Committed, in `src/io/tests/golden/<version>/`; large-design round trips stay on local `test_data/` (`native_format_profile`).
- **Forward compatibility.** Older builds refuse newer files (§4.9). `additive_only` was not built; add it only if someone needs it.

- **Version granularity.** One schema version per schema-changing commit: the commit bumps `Schema.version` (patch number) and adds its migration, snapshot and golden files. Migrations are never squashed, so every released version stays loadable. Before the first release, a commit may re-baseline instead of migrating (§4.1).
- **Save during the GUI session.** Saving holds the exclusive lock, and the GUI freezing for the duration is acceptable (§5).

- **Unknown extension data.** A file containing an extension the build doesn't have is refused (§6), so there is no unknown data to drop or preserve.

No questions remain open.
