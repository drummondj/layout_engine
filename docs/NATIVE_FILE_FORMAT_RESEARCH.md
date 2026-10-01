# Native File Format — Architecture Research

Follow-up to [EXTENSION_MECHANISM_RESEARCH.md](EXTENSION_MECHANISM_RESEARCH.md) §4 ("Open issue: persistence").

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
| Schema changes | An ordered chain of **declarative migration files** (`migrations/NNNN_*.py`), one per schema version. Each is a list of typed ops (`AddField`, `RenameField`, `ConvertField`, `ExtractToChild`, ...). `codegen makemigration` drafts each one by diffing the newest schema snapshot against `schema.py`. codegen compiles the chain into C++ that runs on a generic, name-based in-memory form of the file. |
| Build-time proof | codegen replays the whole chain symbolically from the first snapshot. The result must equal the current `schema.py`, or the build fails. So every schema change must come with a migration, and every op must be valid against the schema it applies to. |
| The "always" guarantee | The chain proves the *shape* is right. A **golden-file corpus** proves the *data* is right: every schema version ever released has sample files checked in, and CI loads them all on every build. |
| Loading | Pools are filled directly with dense ids, then indexes are rebuilt from generated code. `create_x()` is not called per object. |

**Rejected:**
- Protobuf and FlatBuffers. Both need hand-maintained stable field numbers, both have size-limit and speed problems at this scale, and neither handles structural migrations.
- SQLite. Poor fit for nested geometry lists, and slow bulk loading.
- Parquet/Arrow. A heavy dependency, and still no answer for structural migrations.
- A TCL-script dump. Slow, and it can't be migrated.
- Version-chained typed readers with no embedded schema. Every old reader has to be kept compiling forever.

---

## 1. What exists today

Paths below are relative to `src/`.

**Database shape**
- Every pooled class is a plain struct, `XxxData` (for example `database/generated/net.hpp`, `NetData`), stored in `Pool<XxxData, XxxId>` (`database/generated/pool.hpp`).
- A pool is a slot vector with `generation` and `alive` flags plus a free list. An `Id` is `{uint32 index, uint32 generation}` (`database/generated/ids.hpp`).

**Field types**, all from codegen's `TYPEMAP` (`codegen/codegen/schema.py:11`):
- scalars (`int`, `double`, `bool`, `str`, `dbu`/`dbu2` = `int64_t`)
- enums (`enum class ShapePurpose : uint8_t` with explicit values)
- references (`NetId`)
- embedded non-pooled structs (`Rect`, `Path`, `Polygon`, `Point`)
- `std::optional<T>` and `std::vector<T>` of any of the above

`ShapeData` (`database/generated/shape.hpp:52`) is the most demanding case: nine reference fields and nine lists of embedded structs.

**Relationships**
- Stored **only as the child's reference to its parent**, e.g. `NetData::schematic`.
- Parent → children lists and name lookups are **derived**, in `Root::index_` (`database/generated/index.hpp`). `create_x()` maintains them incrementally (`create_net`, `database/generated/root.hpp:7516`).
- As a result, only the pools need saving. Indexes can always be rebuilt.

**Versioning**
- `Schema.version = "0.49.0"`, and until Phase 1 **nothing read it**. It has not changed since the first schema commit (`29d2a95`), even though the schema has changed many times since. *(Phase 1 is now implemented: see §10.)*
- The schema *does* change structurally, not just additively. Example: commit `d5d16a2` turned `Abstract.boundary` from a `List[Polygon]` field into a child `Shape` object, and `Layout.diearea` likewise. An older file that holds polygon lists must become `Shape` objects on load. That is exactly the kind of change a naive format can't survive.

**Existing support in codegen**
- `Field.default` already exists and is validated (`codegen/codegen/validation.py:150`).
- `EnumValue` carries a name and a value (`codegen/codegen/schema.py:3886`).
- `Pool::clear()` exists, and `Root::clear_<x>()` exists per class.

**Not database state, so not saved** (members of `LeHandle`, `api/le_handle.hpp`):
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

```
+--------------------------------------------------------------+
| Header   magic "LEDB\r\n\x1a\n", container_version (u32),    |
|          flags, writer app version string                     |
+--------------------------------------------------------------+
| SCHEMA chunk   writer's schema descriptor (see below)        |
|                + schema version + schema fingerprint (hash)   |
|                + one descriptor per extension (name, version) |
+--------------------------------------------------------------+
| STRINGS chunk  deduplicated string table (names are heavily   |
|                repeated: layer names, cell names, net names)  |
+--------------------------------------------------------------+
| CLASS chunk × N   one per pooled class that has objects:      |
|    class name, row count,                                     |
|    COLUMN × M   field name, encoding, compressed bytes, CRC   |
+--------------------------------------------------------------+
| SESSION chunk (optional)  JSON: view layers, current ids,     |
|                           viewport                            |
+--------------------------------------------------------------+
| Footer   chunk directory (offsets) + whole-file CRC           |
+--------------------------------------------------------------+
```

- Every chunk has a type tag and a length, so a reader can **skip chunk types it doesn't recognise**. That is how future chunk kinds stay compatible.
- The footer directory allows `mmap` and parallel decoding. Chunks are independent, so TBB (already a dependency) can decompress and decode them concurrently.
- Lengths are 64-bit, so there is no 2 GB limit.

### Schema descriptor

A compact dump of the schema as it was when the file was written. codegen emits it as a constant, and the writer copies it in verbatim.

```
class Net      fields: schematic:ref(Schematic)  name:str  bus:ref(NetBus)?  bit_index:int?
class Shape    fields: ... rects:list(struct Rect) purpose:enum(ShapePurpose)? ...
struct Rect    fields: ll:struct(Point) ur:struct(Point)
enum ShapePurpose  BOUNDARY=0 PLACEMENT_BLOCKAGE=1 DEBUG=2
```

Because the file carries its own schema, the reader never needs the old `schema.py` or the old generated code to decode an old file. It decodes the file generically, then brings it up to date with the migration chain (§4).

### Value encoding (columnar)

Each field of each class becomes one column:

| Field kind | Encoding |
|---|---|
| Reference (`NetId`) | Dense row index of the target within its class chunk, `u32` with `0xFFFFFFFF` meaning null. The writer remaps live slots to 0..n-1, so the file never contains dead slots or generations. |
| `int` / `dbu` / `dbu2` | zig-zag varint, delta-encoded when that is smaller (coordinates, sorted parent ids) |
| `double` | raw 8 bytes |
| `bool` | bit-packed |
| `str` | index into the STRINGS table |
| enum | **name** index into the string table, never the ordinal, so reordering or renumbering enum values is harmless |
| `optional<T>` | presence bitmap plus the values that are present |
| `vector<T>` | offsets column (prefix sums) plus a flattened child column (Arrow-style) |
| embedded struct | recursively flattened into sub-columns (`rects.ll.x`, `rects.ll.y`, ...) |

Columnar layout plus zstd compresses coordinate data very well, since neighbouring values are similar. It also makes schema evolution cheap: a removed field is a column the reader skips, and an added field is a column that isn't there.

---

## 4. Schema migrations: reading old files

Schema evolution is handled by a single mechanism: an **ordered chain of declarative migration files**. The design follows Django migrations and Alembic. Each file describes exactly how data shaped like schema version *N* becomes data shaped like version *N+1*. `schema.py` only ever describes the **current** schema; it carries no rename history or conversion rules.

### 4.1 Three artifacts per schema version

| Artifact | Location | Written by | Purpose |
|---|---|---|---|
| Schema snapshot | `src/database/schema_history/<version>.json` | codegen, on every version bump | The exact shape of version *N*: classes, fields, types, enums, parent relations. The "before" side of the next migration, and the reference the chain is checked against. |
| Migration file | `src/database/migrations/NNNN_<slug>.py` | drafted by `codegen makemigration`, finished and reviewed by the author | Ordered ops transforming *N-1* → *N*. |
| Golden files | `test_data/native_format/<version>/*.led` | a script that runs `write_db` on the standard inputs | Real data at version *N*, loaded by CI forever after. |

A schema change is one commit containing:
- the `schema.py` edit
- the new migration file
- the new snapshot
- the new golden files

The migration file is the reviewable statement of "here is how every existing user file will be converted".

### 4.2 Drafting a migration: `codegen makemigration`

```
codegen makemigration --schema src/database/schema.py --name boundary_to_shape
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
4. **Bump the version and write the snapshot** for the new version.

For the real `d5d16a2` change, the author would finish the draft into this:

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
| `SplitClass(cls, by=field, into={value: new_cls})` / `MergeClasses([a, b], into, tag_field=…)` | Split one class by a discriminator, or merge several into one with a discriminator field (the pattern behind item 11's purpose merge). | yes |
| `RunCode("m0012_fixup")` | Escape hatch: a named, hand-written C++ function over `DynamicDb` (§4.4). Must declare the classes and fields it reads and writes, so the symbolic replay (§4.5) can still track the schema. | no |

The vocabulary is expected to grow. A new op is added when a second `RunCode` needs the same pattern.

### 4.4 How migrations run

1. **Generic decode.** When the file's schema fingerprint differs from the running build's, the reader decodes the file into a **`DynamicDb`**: for each class name, a table of rows, each row mapping field names to `Value` variants (int, double, string, enum-name, ref, list, struct). The decoder is driven entirely by the file's embedded schema descriptor, so any file ever written can be decoded, with no old generated code needed.
2. **Pick the chain.** The chain runs from the file's `schema_version` to the build's. Versions are totally ordered, and each migration has exactly one `from` and one `to`, so the chain is a straight line. Branching is prevented by a validation rule: two migrations may not share a `from_version`.
3. **Run the ops.** codegen compiles every migration file into C++, as `migrations_generated.cpp`. Each op becomes a call into a small runtime library (`src/persistence/migrate/`) that implements the op vocabulary on `DynamicDb`. `RunCode` ops call the named hand-written functions. Ops run in order, and each migration runs as one step.
4. **Materialize.** The migrated `DynamicDb` now matches the current schema exactly, which §4.5 guarantees. The generic materializer writes it into the pools (§5).

**Fast path:** if the file's fingerprint equals the build's, steps 1–3 are skipped. Generated typed decoders write columns straight into the pools. Old files take the slower generic path, and the app can offer to re-save them in the current format.

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

Symbolic replay proves the chain produces the right *shape*. It can't prove the data survives, for example that a `LookupBy` actually finds the layers. So `test_data/native_format/<version>/` holds small but representative `.led` files written by each version:
- a LEF-only tech/library
- a DEF with placements, routes and vias
- a Verilog schematic
- one extension object

A test loads every file with the current build, which runs the full chain from that version. It then checks invariants: object counts, spot values, reference validity, and `Root::validate()`. Finally it re-saves and reloads for idempotence. Adding a version's golden files is part of the schema-change commit (§4.1). As long as this test passes, "reads every older version" is being checked on every build, not just claimed.

### 4.7 Other uses of the same machinery

- **`migrate_db old.led new.led`** (TCL command and command-line tool): an offline upgrade, useful for batch-converting archives.
- **`db_info <file>`**: lists the migrations that would run on a file, with their descriptions.
- **Extensions:** each extension keeps its own `migrations/` directory and snapshot history, keyed to *its own* version. Its migrations are interleaved with the core chain (§4.8).
- **Live migration after an extension upgrade:** dump the in-memory `Root` to a `DynamicDb`, run the extension's pending migrations, then re-materialize. This is the same code path, with no file involved.

### 4.8 Core and extension migrations together

An extension's schema depends on the core schema:
- its references point at core classes (`AcmeRouteGuide.net → Net`)
- its classes may have core parents (`parent="acme_route_guides"` on `Net`)
- it may add fields to core classes

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

Every `.led` file records the core schema version and, separately, each extension's version, in the SCHEMA chunk.

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
| `RenameClass` | Reference columns of that type are retagged everywhere. |
| `RemoveClass` | Extension objects whose *parent* was removed are cascaded away (the normal delete rule). Plain references are nulled, with a warning and a count. |
| `MergeClasses` / `SplitClass` | Reference values are remapped through the op's row mapping. `SplitClass` must say which new class a reference to the old class follows. |
| Any op on a core class | Fields an extension added to that class travel with their rows, like any other column. |

So most core changes need **no action from the extension author**: the core migration already carries their data along.

**4. Validation replays the merged plan against all the schemas.**
- `checkmigrations` replays the merged plan symbolically. The result must equal core `schema.py` plus every `schema_ext.py`.
- During replay, core ops also rewrite the extension schema. For example, `RenameClass(Net → SignalNet)` changes `AcmeRouteGuide.net`'s type in the replayed extension schema.
- An extension whose `schema_ext.py` still says `type="Net"` then fails with a precise message: *"core migration 0013 renamed Net → SignalNet; update schema_ext.py: AcmeRouteGuide.net"*. The author just edits `schema_ext.py`; no extension migration is needed because the data was already handled.
- Extension ops may only touch the extension's own classes, plus fields the extension added to core classes.
- An extension's `depends_on_core` values must never decrease along its chain.

**What a customer sees when upgrading layout_engine:**
1. Bump the submodule and build. `checkmigrations` runs the merged replay.
2. If the core change only moved things around (renames, merges), the build at most names the `schema_ext.py` lines to update. Existing files keep loading.
3. If the core change removed or reshaped something the extension relied on, the old data needs real transformation. The customer runs `makemigration` for their extension; the new migration records the new core version, so it is placed after the core change in the plan.
4. The extension's own golden files, each containing core and extension data at a known pair of versions, go through the full merged plan in the customer's CI. Anything the symbolic checks can't catch shows up there, before users hit it.

**Edge case: a lagging branch.** With linear history the "never decrease" rule holds automatically, since `makemigration` always records the current core version. It can fail on a branch that lags behind core. For example, a file written by core 0.53 + acme 1.3 can't then take an acme 1.4 migration written against core 0.52. The loader refuses that case with a clear message ("re-create acme 1.4's migration against core ≥ 0.53") rather than guessing.

**Upstream coverage:** layout_engine's own `hello_ext` example (EXTENSION_MECHANISM_RESEARCH.md §9) should carry golden files and at least one extension migration. Then a core migration that mishandles extension references (rule 3) fails layout_engine's CI, not a customer's.

### 4.9 Newer files in older builds (forward compatibility)

Migrations run **forward only**. Many ops are reversible (see §4.3), but `RemoveField`, `RemoveClass`, lossy converters and `RunCode` aren't, and forward compatibility isn't a requirement. So:
- Each file records its `schema_version`.
- A build refuses a file whose version is newer than its own, with a message naming both versions.
- Optionally, a migration can be marked `additive_only` (only `AddClass`/`AddField`). An older build may then read such a newer file by skipping the unknown columns, since nothing it knows about changed meaning.

---

## 5. Loading into `Root`

For each class:
- `Pool::bulk_load(n)` produces `n` alive slots at indices `0..n-1`, all with generation 0.
- Row `i` in the file therefore becomes `XxxId{i, 0}`, so references decode directly into ids with **no remap table**.
- The generated decoder writes each field straight into `XxxData`.

After all pools are loaded:
- A generated `Root::rebuild_indexes()` rebuilds `index_` from the parent and index metadata codegen already has. It is the same logic as the incremental code in `create_x`, just run once.
- A generated `Root::validate()` checks:
  - every reference is in range and points at a live slot
  - `unique_per_parent` constraints hold
  - the mutually exclusive parent fields hold (e.g. at most one owner on `Shape`)

  Any failure makes the load fail with a precise error, rather than leaving a half-built database.

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

Loading is not undoable, just as `read_def` into a fresh session isn't.

**Saving:**
- Take a read lock and snapshot the pools. Since compression dominates the cost, a later refinement could copy the pools and encode off-lock.
- Write to `file.led.tmp`, `fsync`, then `rename()`. A crash mid-save never destroys the previous file.

---

## 6. Extension objects

This connects to the extension mechanism research:
- Extension classes are ordinary `Root` classes after the build-time merge, so they are saved like any other.
- The SCHEMA chunk lists each extension's name and version separately. Each extension has its own `migrations/` chain and snapshot history in its own tree, keyed to *its* version and interleaved with the core chain by the core version each migration was written against (§4.8).
- Opening a file that contains an extension the running build doesn't have:
  - by default, load everything else and **warn**, naming the extension and the object counts dropped
  - optionally, `-keep_unknown` holds the raw chunks and writes them back on save, but only if no core object they reference was deleted; otherwise they are dropped with a warning
- References *from* core classes *to* extension classes can't exist, because core never knows about extensions. That keeps the "missing extension" case clean.

---

## 7. Performance expectations

> **Measured (Phase 2):** on `aes_scaling_8x8` (a 1476 MB DEF, Release, WSL2, cold process), `read_def` takes 52.9 s. The `.led` file loads in 5.3 s and saves in 6.4 s, at 203 MB with zstd level 3. On `aes_scaling_1x1`: 862 ms against 56 ms, and 24 MB against 3.0 MB. The dev tool is `native_format_profile`.

- **DEF cost.** Parsing, name lookup and hierarchical linking dominate. The native format stores already-linked, already-resolved references as integers, so loading is essentially decompression plus array fills plus an index rebuild.
- **Parallelism.** Columns and classes decode in parallel. The index rebuild can be parallel per index, since the indexes are independent maps.
- **Target:** load `aes_scaling_8x8` (1.4 GB DEF) at least an order of magnitude faster than `read_def`, with a file several times smaller than the DEF. This should be confirmed with a prototype that serialises only `Shape`, `Placement` and `Route`. The existing benchmark executables are a natural place for it.

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

- **C API:** `le_save_db(handle, path, flags)` and `le_load_db(handle, path, flags)`, following `api.hpp` conventions (int status, errors logged through spdlog).
- **TCL:** `write_db <file> [-no_session]` and `read_db <file> [-keep_unknown]`, registered with `register_command_help` in the usual way.
- **Inspection:** `db_info <file>` prints the version, extensions, per-class counts, and the migrations that would run on the file. Useful for support.
- **Offline upgrade:** `migrate_db <in.led> <out.led>`.
- **Developer:** `codegen makemigration --name <slug>` drafts the next migration file; `codegen checkmigrations` runs the symbolic replay (§4.5).
- **GUI:** File → Open / Save / Save As. This fits the menu bar proposed in the extension research. The existing exit dialog (item 18) can offer "save changes" once there is a native save.

---

## 10. Implementation phases

| Phase | Work |
|---|---|
| 1 ✅ | codegen: schema descriptor, fingerprint, `schema_version.hpp`, the first `schema_history/` snapshot (the baseline, with no migration before it), and a "schema changed without a snapshot" check. **Done:** `codegen/codegen/descriptor.py`; the check runs inside every `codegen --target database` run, and the baseline is `src/database/schema_history/0.49.0.json`. |
| 2 ✅ | Container writer/reader (chunks, strings, CRC, zstd), generated typed tables, `Pool::load_dense`, `Root::rebuild_indexes()`. **Done** (`src/persistence/`, see OVERNIGHT_REVIEW.md 2026-09-27/28). Differences from §3: no chunk directory (an END chunk detects truncation instead); nested structs are row-major inside their column; columns are split into 65,536-row segments; the name-matching decode (§4, additive changes) is built in, while `DynamicDb` waits for Phase 4. |
| 3 ✅ | C API, TCL commands, golden corpus (first version) plus the corpus test. **Done:** `le_write_db`/`le_read_db`/`le_db_info`, TCL `write_db`/`read_db`/`db_info`. `read_db` loads into an empty session only. Golden files are in `src/persistence/tests/golden/<version>/`. |
| 4 🟡 | Migration framework. **Done so far** (`codegen/codegen/migration.py`): the op classes, symbolic replay and per-op validation, which run on every `codegen --target database` and replace the phase-1 check; `makemigration` with diffing and rename prompts (`Todo` when non-interactive); `checkmigrations`. The generated `migrations.hpp` table lets the loader apply **renames** (class, field, enum value, and `RemoveEnumValue(map_to=)`) to an older file's schema before name matching. **Not yet:** `DynamicDb` and the data runtime for `ConvertField`, `ExtractToChild`/`InlineChild`, split/merge and `RunCode`. Those ops are declared unsupported at runtime, so a file needing one is refused with the migration's description. Op names differ slightly from §4.3: `AlterField` stands in for `ConvertField` with storage-compatible conversions only. |
| 5 | SESSION chunk, GUI File menu, extension migration chains, `-keep_unknown`. |
| 6 | Performance: parallel encode and decode, benchmark against `read_def` on `aes_scaling_*`. |

---

## 11. Open questions

1. **Version granularity.** One migration per schema-changing commit (like Django), or one squashed migration per release? Per-commit is simpler to author and review, and keeps golden files small. The chain may get long, but migrations are cheap to run.
   - The recommendation is per-commit, with `makemigration` bumping the patch number.
   - Development-only migrations may be squashed before a release, but only if no golden files or user files exist for the intermediate versions.
2. **Migrations written in Python, compiled to C++.** Should the Python op files be the source of truth, as proposed? The alternative is writing ops directly as a C++ DSL. Python keeps them next to `schema.py`, and lets codegen do the symbolic replay without parsing C++.
3. **Where the golden corpus lives.** Files should be small (tens of KB to a few MB) so they can be committed. Large-design round-trip tests stay on local `test_data/`.
4. **Forward compatibility.** Is "older builds refuse newer files, except after `additive_only` migrations" (§4.9) acceptable?
5. **Unknown extension data.** Default to dropping it with a warning, or to preserving it?
6. **Save during the GUI session.** Is a read lock held for the snapshot duration acceptable, or does save need copy-on-write snapshots so the GUI stays interactive during multi-second saves of very large designs?
