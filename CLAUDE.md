# Layout Engine

C++23 EDA layout tool: reads LEF/DEF and SystemVerilog into an in-memory
database, renders it through a layer-based, Blend2D-backed pipeline, and
edits it from a Tcl shell (`le_shell`) with a Dear ImGui window. The goal is
an architecture for editing hierarchical designs with millions of objects.
Design docs, research and benchmark history live in `docs/`
(`docs/LEFDEF_BUGS.md` lists the vendored LEF/DEF parser/writer bugs and how
`src/io/` works around each).

## Requirements (non-negotiable)

- Target: Linux servers, little/no GPU. Optimize for memory and CPU, not GPU.
- Tests are written alongside the code they cover, not after.
- Performance decisions must be backed by a benchmark, not intuition.
- C++23. Keep abstractions minimal and justified by present, not hypothetical, needs.
- Keep responses and docs concise.

## Layout

Each module's tests live beside it in `tests/` (hand-written GTest).

- `src/database/` — the object-pool database. `schema.py` is the source of
  truth; `generated/` is produced from it and never hand-edited (see
  Database codegen). `database.hpp` is the single public include. Also
  hand-written helpers: `filter.hpp` (the `-filter` expression parser/
  evaluator and property-path resolver), `library_helpers.hpp`
  (get-or-create Library/Design by name for every reader),
  `hierarchical_resolver.hpp`/`schematic_layout_linker.hpp`/
  `rename_propagation.hpp` (Schematic<->Layout linking, design in
  `docs/LINKING_STRATEGY_RESEARCH.md`).
- `src/geometry/` — `Geometry`, a Boost.Geometry wrapper (bbox, overlap,
  transforms, union/buffer, label placement, piece hit-tests,
  ITERATE expansion) over the database's `Point`/`Rect`/`Polygon`/`Path`/
  `Shape`. `shape_ops.hpp` backs the `shape_*` Tcl commands (boolean ops,
  conversions, sizing) on a Shape's merged area; a holed result is emitted
  as exact rects since `Polygon` can't hold a hole. Results go to the
  current Abstract/Layout's `free_shapes` unless `-parent` says otherwise -
  never written by `write_lef`/`write_def`.
- `src/view_style/` — `ViewLayerSet`/`ViewLayer`: the rendering-layer
  concept, distinct from LEF/DEF layers. `ViewLayerPurpose` is a closed,
  application-owned enum: per physical Layer `TERMINAL`/`OBSTRUCTION`/
  `TRACK_PREFERRED`/`TRACK_NON_PREFERRED`/`ROUTING_BLOCKAGE`/`ROUTE`/
  `CUSTOM_SHAPE`, plus pseudo-rows with no Layer (`ROW`, `BOUNDARY`,
  `PLACEMENT`, `GCELLGRID`, `PLACEMENT_BLOCKAGE`, `REGION`, `DEBUG`,
  `FLIGHTLINE`, `PORT_MARKER`). Its raw ordinals cross the C API and are
  mirrored by hand in `layer_manager.cpp` and `le_tcl_procs.tcl` - append,
  don't reorder. Each Layer gets one palette color shared by its columns;
  `FillPattern` distinguishes them. User-picked colors
  (`LeHandle::layer_color_overrides`) are applied on top.
- `src/core/` — header-only editing/hit-test geometry shared by `api` and
  `pipelines`: `placement_geometry.hpp` (placement world bboxes; Placement,
  Abstract-view and Layout-view hit-tests), `row_geometry.hpp` (a Row's
  synthesized footprint), `placement_move.hpp`/`fin_grid.hpp` (Placement
  Move planning and snapping: SITE rows for CORE cells, fin grid,
  manufacturing grid), `shape_resize.hpp` (Resize handles and snapping;
  also how Move snaps paths/vias), `flightlines.hpp` (net connections of the
  selected placements), `object_filters.hpp` (the Layers panel's
  Placement.type/Route.use value filters).
- `src/pipelines/` — the render pipeline, one oneTBB `flow::graph`
  (`ViewRenderPipeline`, design in `docs/PIPELINE_REFACTOR.md`):
  `LayerGenerationStage` -> `HierarchyResolverStage` -> `ViewportCullStage`
  -> `RasterizeBlend2DStage` -> `ComposeStage`. Every stage is a
  `MemoizingStage` (`tbb_core.hpp`) that recomputes only when its input
  version or the options it reads (`options_did_change`) change;
  `ViewRenderOptions` (`pipeline_options.hpp`) is the one shared options
  type. A stage's dependencies travel in `data`, not `options`.
  - `HierarchyResolverStage` walks `Placement -> Design` from the top level,
    one hop per unit of `hierarchy_depth`, resolving each Abstract/Layout
    once however often it's placed. After an edit it updates incrementally
    from the Root change log: a node's shapes are immutable shared
    `ViewShapeChunk`s (a Layout's fixed chunks plus spatial tiles of ~2000
    routes/placements), and only touched chunks rebuild; anything it can't
    place falls back to a full resolve (`last_compute_was_incremental()`).
    Hidden Placement.type/Route.use values (`hidden_objects`) are left out
    here; changing them re-resolves everything.
    Chunks carry `ChunkSources` so Layout-view selection queries the last
    resolved render tree (`ViewRenderPipeline::resolved_output()`, api.cpp's
    `layout_candidates`) instead of scanning the Layout.
  - `ViewportCullStage` prunes to the viewport with per-node spatial
    indexes; sub-pixel placements are culled whole.
  - `RasterizeBlend2DStage` rasterizes each node's own shapes to a
    `BLImage` (fill patterns, labels via a cached monospace glyph atlas,
    port markers, the background grid for the top level); `ComposeStage`
    composites children and draws the overlays (selection, flightlines,
    Move ghost, Resize hover, cursor box, rulers, drag rectangle).
  - `via_shapes.hpp` expands vias/via arrays at render time;
    `draw_helpers.hpp` holds style constants and shared drawing helpers.
  - `pipelines.cpp` is the module's one compiled TU
    (`default_blend2d_font_face()`, loading the bundled font from
    `LE_FONT_DIR` = `assets/fonts/`).
- `src/io/` — `LEFReader`/`LEFWriter`/`DEFReader`/`DEFWriter` over the
  vendored `lefr*`/`lefw*`/`defr*`/`defw*` APIs. See "LEF/DEF notes" below.
- `src/sv/` — `SVReader`: SystemVerilog/Verilog into the logical model
  (`Schematic`/`Port`/`Net`/`Instance`/`Pin`) via the slang frontend.
  `read_netlist` elaborates (accurate, intolerant of errors); `read_rtl` is
  syntax-only and stores unsupported content on `Instance.rtl_text`. Both
  end with `link_unresolved_instances`, which is re-runnable so a later
  LEF read can resolve standard cells.
- `src/persistence/` — the native `.led` database file (`write_db`/
  `read_db`), columnar and zstd-compressed, driven by codegen's
  `native_tables.hpp`; loads older schema versions by name plus the
  migration chain. Design in `docs/NATIVE_FILE_FORMAT_RESEARCH.md`.
- `src/editing/` — undo/redo: `CommandHistory` (one per handle),
  `Transaction`, `ICommand`. Every generated create/update/delete records
  itself into the recording transaction; `le_repl_eval` and GUI edits
  (Move, Resize, Delete) bracket one.
- `src/api/` — `api.hpp`/`api.cpp`, the plain-C API every front end calls
  (no `std::` types, default arguments or overloads in public
  declarations). `LeHandle` (`le_handle.hpp`, never included by `api.hpp`)
  owns one `Root`, `ViewLayerSet`, `ViewRenderPipeline`, `CommandHistory`,
  and all view/interaction state: current Abstract *or* Layout (mutually
  exclusive by convention), pan/scale, visibility/selectability, selection
  (`SelectedObject` = `std::variant<ShapePiece, RowId, PlacementId,
  RegionId>`), rulers, Move/Resize state, mode (`SELECT`/`EDIT`/`RULER`;
  only Select changes the selection). Every function null-checks its
  handle. Read-only calls take the handle's `shared_mutex` shared so the
  GUI stays live during a render; mutations take it exclusive. Vias and
  via arrays are selectable pieces hit-tested in api.cpp (their geometry
  comes from `pipelines/via_shapes.hpp`, which `core` can't depend on).
- `src/tcl/` — the Tcl surface: `le_api.i` (SWIG) wraps `le_tcl_shim` into
  `le_tcl.so`; `le_tcl_procs.tcl` parses `-flag value` syntax and builds
  dicts/lists. Domain-verb command names, no visible handle, friendly ids
  (`terminal:IN0`, `shape:12`). Property reading, `get_<type>`,
  `create_<type>`, `update_<type>` and `delete_<type>` are generated for
  every readable class (see TCL codegen). `le_repl_eval` is the bracket
  point that makes a typed command undoable and recallable. `le_shell.cpp`
  is the shell: readline console on a spawned thread, the GUI loop on the
  main thread (GLFW requires it on macOS), one shared `LeHandle` injected
  via `set_session_handle`.
- `src/gui/` — the Dear ImGui GUI (`le_gui.cpp`, `gui_provider.*`,
  `components/`). `GuiProvider` is its only contact with the C API.
  Rendering runs on a background thread woken by
  `le_wait_for_render_needed`; panels read under the shared lock so they
  never block on a render. Actions that should appear in command history
  (layer visibility, hierarchy depth, ...) are queued as Tcl commands
  (`le_enqueue_tcl_command`) for le_shell's console thread to run. Settings
  persist to `~/.layout_engine/settings.json` (loaded in interactive mode
  only), the dock layout to `~/.layout_engine/window_layout.ini`. No
  automated coverage of the render/input loop itself.
- `src/lefdef/` — vendored Si2 LEF/DEF 6.0.62-p004 parser source, built by
  its own Makefiles via `ExternalProject_Add` (`lef_lib`/`def_lib`). Never
  hand-edit.

## LEF/DEF notes

- `LEFReader` supports a subset of LEF >= 5.4, tested against
  `src/lefdef/lef/TEST/complete.5.8.lef` plus small fixtures in
  `src/io/tests/fixtures/`. The vendored parser reuses one scratch struct
  per callback and never resets fields - always check the matching
  `has*()` guard before trusting a getter.
- `DEFReader`/`DEFWriter` cover DESIGN/VERSION/UNITS/DIEAREA/ROW/TRACKS/
  GCELLGRID/COMPONENTS/PINS/BLOCKAGES/VIAS/REGIONS/NETS/SPECIALNETS/
  NONDEFAULTRULES. NETS/SPECIALNETS carry routing geometry only;
  connectivity comes from `link` against a netlist.
- DEF values are already in database units; `DEFReader` rescales them
  (`unit_scale_`) when the file's UNITS disagree with the Technology's.
  NONDEFAULTRULES LAYER WIDTH/SPACING/... are real microns (the vendored
  int accessors truncate them).
- PINS geometry is stored in design coordinates: `DEFReader` applies
  `Geometry::pin_transform`, `DEFWriter` inverts it.
- `Shape.layer` and `Placement.reference_design` are resolved references:
  the tech LEF must be read before a DEF using its layers, and cells
  before the DEF placing them. A Shape with no physical layer
  (boundary, diearea, placement blockage) sets `Shape.purpose` instead.

## Database codegen

`codegen/` is this project's fork of
[cmg](https://github.com/johndru-astrophysics/cmg) (INDEXED_POOLS export
style). Every `Klass` in `schema.py` becomes an `XxxData` struct, an
`XxxId` `{index, generation}` handle, and a generational
`Pool<XxxData, XxxId>`. `Root` (`generated/root.hpp`) owns every pool and
index and exposes `create_x`/`get_x`/`update_x`/`delete_x` per class.
`update_x` is the only way a pooled field changes after creation (each
parameter is `std::optional`: present means apply). A `Klass` is pooled
unless `has_pool=False` (an embedded value type like `Point`/`Rect`).

- `Field.unique_per_parent` (with `index=True`) makes `create_x` fail on a
  sibling name clash (e.g. `Terminal.name` within its Abstract).
- A class pair may have several parent/`is_child` relationships;
  `Klass.link()` pairs each `is_child` field with the parent field whose
  `parent=` names it.
- `Root` keeps a change log (`change_log()`, a fixed ring): every
  generated create/update/delete records the object and its owner(s), so
  consumers update incrementally. An edit through a mutable `get_x()`
  pointer is invisible to it - call `note_<klass>_changed(id)`. Bulk
  operations (every read and `link`) saturate it, meaning "everything
  changed".

To change the schema: edit `src/database/schema.py`, bump `Schema.version`
for a real shape change, and regenerate with the `regen-database` skill.
codegen fingerprints the schema and compares it with
`src/database/schema_history/`; a new version needs its snapshot, a
migration (`src/database/migrations/`, drafted by
`codegen --target makemigration`) and golden files
(`src/persistence/tests/golden/<version>/`), all committed together.

## TCL codegen

A separate target (`codegen --target tcl`, the `regen-tcl` skill)
generating `src/api/generated_tcl/` and `src/tcl/generated/`. Every
readable `Klass` gets a property table, friendly-id resolution, `is_child`
enumeration, `get_<type>`, and `create_<type>`/`update_<type>`/
`delete_<type>`.

- `Klass.has_current_access` (`Technology`/`Abstract`/`Schematic`) adds a
  `current_X ?id?` command; every other `get_<type>`'s default scope (no
  `-of`) derives from these (`codegen/codegen/tcl_scope.py`). Opening a
  Design moves both this and the GUI's `LeHandle::current_abstract()`.
- `create_<type>` has one flag per scalar, per flattenable embedded
  struct, and per list of one (`Field.list_compound_kind()`, e.g.
  `-rects {{{llx lly} {urx ury}} ...}`); `Field.create_excluded` opts a
  field out. An omitted optional flag means unset. Lengths cross in
  microns and areas in square microns; enums by name.
- `update_<type>` mirrors those flags but omitted means unchanged; a list
  flag replaces the whole list. A single-parent class can be reparented.
- `delete_<type>` cascades through `Klass.tcl_child_list_fields()`,
  unrolled at codegen time, recording deepest-first for undo.
- Plain (non-parent) references like `Shape.layer` take a token flag via
  `Field.is_plain_reference_field()`, deliberately separate from
  `get_parent_fields()` (ownership).
- The only hand-written CRUD left is `remove_shape_rect/_polygon/_path`.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Keep a second tree, `build_release` (Release), up to date too - it's what
`le_shell` users and benchmarks run. See the `build-test` skill and
`BUILD.md` (rootless Rocky Linux 8 build). Docker: `docker compose run --rm
ci` (`Dockerfile.linux-ci`); releases come from `Dockerfile.linux-release`.

Dependencies: spdlog/fmt/Boost (headers) via `find_package` with
`FetchContent` fallbacks; oneTBB required; Blend2D, slang, Dear ImGui,
GLFW (fallback), GoogleTest/Benchmark and others via `FetchContent`.

Gotchas:
- The vendored LEF/DEF Makefiles race under a parallel jobserver; the
  `ExternalProject_Add` steps force `make -j1` with `MAKEFLAGS` unset.
- They build in-source (`src/lefdef/{lef,def}/`) and trust timestamps, so
  stale objects from another toolchain (or a missing `include/` beside old
  `.o` files) break the build - delete the lefdef build output the way
  `Dockerfile.linux-ci`'s CMD does.
- `ENABLE_COVERAGE` is a cached option and forces `-O0`; pass
  `-DENABLE_COVERAGE=OFF` explicitly (or use a fresh tree) for real
  numbers.

Coverage: `-DENABLE_COVERAGE=ON`, then `cmake --build build --target
coverage` (Clang source-based; report in `build/coverage/`).

Benchmarks (Release builds only): `pipeline_benchmarks` (per-stage, over
the `aes_scaling_*` tiling fixture in `test_data/`), `pipeline_stage_benchmark`
(+ `scripts/pipeline_stage_benchmark.py`), `resolver_profile`
(+ `scripts/resolver_profile.py`), `selection_profile`,
`native_format_profile`, `spatial_index_benchmark`; tools
`generate_tiled_design` and `lef_roundtrip_diff`. Use
`--benchmark_repetitions=5 --benchmark_report_aggregates_only=true` when
comparing. Results history: `docs/BENCHMARKS.md`,
`docs/PIPELINE_REFACTOR_BENCHMARK_RESULTS.md`.

## Known gaps

- Layout view: Blockage/Row/Region own shapes aren't hit-tested yet;
  selection into instanced content is whole-placement only.
- Free-standing shapes (`free_shapes`) aren't selectable.
- `scripts/rocky8-bootstrap.sh` is unverified on a real Rocky 8 machine,
  still stages GTK3 (unused), and doesn't yet provision X11 dev packages,
  readline or oneTBB.
- Some tests are skipped with `GTEST_SKIP` pending root cause (see each
  skip's comment).

## Conventions

- Everything lives in `namespace le`.
- Doxygen-style `/// @brief` one-liners on public methods.
- No exceptions for expected-missing-data paths - pool lookups return
  nullable pointers (`get(id)` -> `T*`) or `std::optional`/`std::expected`.

## Comments

Comments describe how the code works now and why it is this way. They are
not a changelog.

- Write in the present tense: invariants, non-obvious constraints, units,
  ownership, gotchas. If the code already says it, don't comment.
- No tracker references — no item numbers, `E1`-style IDs, "Phase N", or
  `.md` files cited as justification. A reader shouldn't need another
  document to understand a comment.
- No discovery stories ("confirmed by hitting…", "originally…", "used to…",
  "now-deleted…"). Keep the lesson as a one-line reason; drop the story.
- When behaviour changes, update or delete the comments that describe it.
- Why something changed belongs in the commit message and its GitHub issue
  (`Fixes #N`), not in the code.

## Work tracking

Bugs and features are GitHub issues (`gh issue list`, `gh issue view N`).
Long-lived designs and research live in `docs/`.

Code changes for an issue go on their own branch, never straight onto
`main`:

- Branch from an up-to-date `main`, named `<issue>-<short-slug>` (e.g.
  `2-port-marker-one-piece`).
- Commit messages explain the *why*; the commit that completes the issue
  ends with `Fixes #N`.
- When the work is done and tests pass, push the branch and open a PR
  with `gh pr create`, referencing the issue. Merging is the user's call.

## Skills

- `build-test` — configure, build and test (`build` and `build_release`).
- `regen-database` — regenerate `src/database/generated/` from `schema.py`.
- `regen-tcl` — regenerate the generated Tcl/API surface.
- `generate-tcl-docs` — regenerate `TCL_COMMANDS.md`.
- `cpp-review` — local review of pending changes (tests, allocations,
  memory safety); reports via `ReportFindings`, doesn't fix.
- `overnight-review` — unattended pass over GitHub issues labelled
  `overnight`.
