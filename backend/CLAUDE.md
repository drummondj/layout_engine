# Layout Engine MVP — Backend

C++23 backend that reads LEF/DEF and SystemVerilog EDA data into an in-memory
database, then renders it through a layer-based pipeline into Skia commands
consumed by a Flutter plugin. This is an MVP/proof-of-concept: the goal right
now is finding the right architecture for editing hierarchical designs with
millions of objects, not shipping features. See `README.md` for the full
brief and the live plan checklist; see `BENCHMARKS.md` for benchmark history
and design-decision writeups; see `LEFDEF_BUGS.md` for confirmed bugs in
the vendored LEF/DEF parser/writer and how `src/io/` works around each —
none of these are duplicated here.

## Requirements (non-negotiable)

- Target: Linux servers, little/no GPU. Optimize for memory and CPU, not GPU.
- Tests are written alongside the code they cover, not after.
- Performance decisions must be backed by a benchmark, not intuition.
- C++23. Keep abstractions minimal and justified by present, not hypothetical, needs.
- Keep responses and docs concise — this repo's own README asks for that explicitly.

## Layout

- `src/database/` — the object-pool database. `schema.py` is the source of
  truth (a `codegen.Schema` of `Klass`/`Field` definitions); `generated/` is
  produced from it and must never be hand-edited (see Database codegen below).
  `database.hpp` is the single public include (`#include "generated/root.hpp"`).
- `src/geometry/` — `Geometry`, a Boost.Geometry-backed wrapper (bbox, overlap,
  transform, polygon union/buffer, label placement, overlap-merging) over the
  database's `Point`/`Rect`/`Polygon`/`Path`/`Shape` types. Fully covered by
  `geometry_test.cpp`.
- `src/view_style/` — `ViewLayerSet`/`ViewLayer`: the rendering-purpose layer
  concept distinct from the LEF-mirroring `database` — a `TERMINAL` and
  `OBSTRUCTION` `ViewLayer` per physical `Layer`, plus one `BOUNDARY`
  `ViewLayer` not tied to any physical `Layer`. `ViewLayerSet::build_for_technology`
  builds the full set for a `Technology` once, shared/global. Each physical
  `Layer` gets one color from a default palette (shared by its `TERMINAL`/
  `OBSTRUCTION` `ViewLayer`s — no purpose-based fill pattern yet); see the
  class's own doc comments for the palette/wraparound details. Fully covered
  by `view_style_test.cpp`.
- `src/scene/` — `Scene`, per-handle mutable view state (currently displayed
  `AbstractId`, pan/scale/viewport-size transform, per-`ViewLayer`
  visibility, selection, and current interaction mode). Distinct from the
  persistent `Root` database.
  Layer visibility is keyed by `ViewLayerId`, not `LayerId` — a physical
  layer has independently toggleable `TERMINAL`/`OBSTRUCTION` visibility.
  Selection is `std::variant<TerminalId, ObstructionId>` — extend the
  variant as more selectable kinds need it rather than generalizing early.
  `Scene::Mode` (`SELECT`/`EDIT`, UPDATES.md item 11) is Select by
  default — Select is the only mode where `le_mouse_up` changes the
  current selection; Edit mode restricts mouse interaction to editing
  whatever is already selected (behavior TBD, a later item).
- `src/core/` — header-only generic building blocks shared between
  `pipeline` and `render` (UPDATES.md item 16), so neither module depends
  on the other for them: `RenderedShape`/`TinyShapeDot` (`pipeline`'s
  output type, `render`'s input type) and `VersionedStage<Key, Value>` —
  a single-slot memoization primitive (`get(key, compute_fn)`) with its
  own monotonic `version()`, bumped on every real recompute. A downstream
  stage composes its own cache key from an upstream stage's `version()`
  instead of manually re-deriving everything the upstream depends on —
  the fix for a caching-bug class where a new upstream trigger (e.g.
  `Root::mutation_version()`) had to be hand-copied into every downstream
  key or a change silently went unseen. `CachedStage<Key, Value>` is a
  backward-compatible alias for `VersionedStage`, still used by `render`'s
  own stages.
- `src/pipeline/` — `Pipeline` chains five stage classes, one per file
  under `src/pipeline/stages/`, each built on `core`'s `VersionedStage`:
  `GenerateShapesStage` → `FilterByViewportAndSizeStage` →
  `FilterByLayerVisibilityStage` (the `run()` chain) and
  `GenerateShapesStage` → `TinyShapesByViewportStage` →
  `TinyShapesByLayerVisibilityStage` (the `run_tiny_shapes()` chain,
  sharing `GenerateShapesStage` with the first). `Pipeline` itself is a
  thin owner of one instance of each stage plus orchestration
  (`run`/`run_tiny_shapes`/`hit_test_point`/`hit_test_rect`) — every
  public method keeps its original signature, delegating to its stage in
  one line; reuse one `Pipeline` instance per `Scene`-equivalent
  lifetime. `GenerateShapesStage::run` resolves each `Shape` straight to
  its `ViewLayerId` in the same pass (no separate resolve stage) and
  attaches one text label per distinct layer a Terminal has geometry on.
  Deliberately does *not* merge/union a Shape's own overlapping rects/
  polygons for rendering (a `Geometry::merge_overlapping_fills` step
  existed for this, removed - see BENCHMARKS.md's 2026-08-19 entry) -
  the rendered picture always matches the database's actual stored
  rect/polygon/path count, kind, and index exactly, which selection/Move
  (UPDATES.md item 21) depend on.
  `FilterByLayerVisibilityStage`/`TinyShapesByLayerVisibilityStage` group
  into `std::map<ViewLayerId, ...>` (not `unordered_map`) — deliberate,
  since `ViewLayerId`'s ordering matches LEF-declared layer stacking
  order, giving correct bottom-up draw order for free; don't change this
  to `unordered_map`. See each stage class's own doc comment for its
  exact cache key and why it's shaped that way, and `BENCHMARKS.md` for
  current numbers and history. Fully covered by `pipeline_test.cpp`.
- `src/render/` — `Renderer` chains eight stage classes, one per file
  under `src/render/stages/` (each built on `core`'s `VersionedStage`;
  `render` links `core` directly, not `pipeline` — it never names the
  `Pipeline` class itself, only takes its output by reference), plus two
  shared support headers: `pixel_types.hpp` (`PixelShape`/`PixelBuffer`/
  `RasterizedFrame`/etc., pixel-space mirrors of the dbu-space types in
  `core`) and `draw_helpers.hpp` (style constants and free Skia drawing
  functions - `draw_grid`, `draw_group`, `pattern_shader`, etc. - shared
  across several stage classes). `render.hpp` itself is a thin aggregator
  - the `Renderer` class, one member per stage, every public method a
    one-line delegate — same shape as `Pipeline`'s own refactor.
    `RasterizeStage` (SkPicture → Y-flipped RGBA8888 `RasterizedFrame`) is
    instantiated three times (design/tiny-shapes/selection-overlay frames)
    rather than three separate classes — those three methods had identical
    bodies, differing only in which picture and which upstream `.version()`
    fed them (UPDATES.md item 16 point 4's "generic class for future
    expansion"). Unlike `Pipeline`, cache keys compose via upstream
    `.version()` **uniformly**, including two stages
    (`BuildPictureStage`/`RasterizeStage`) whose pre-refactor keys
    deliberately did _not_ trust upstream freshness (a fix for two earlier
    real bugs — see git history / BENCHMARKS.md 2026-08-12's Renderer
    entry for the full trade-off writeup) — composing means a caller that
    reuses a stale artifact without re-running its upstream now silently
    gets a stale-but-cached frame; verified every real call site
    (`api.cpp`, `render_preview.cpp`, every benchmark) always re-runs the
    full chain in order, so this isn't a live risk today.
    `Renderer::render(root, shapes, tiny_shapes, scene, view_layers)` wires
    the whole nine-call chain (mirrors `Pipeline::run()`'s own role) - takes
    `Pipeline`'s output (`shapes`/`tiny_shapes`, `core`'s own boundary
    types), not a `Pipeline&`, since `render` doesn't link `pipeline`; the
    caller runs `Pipeline::run()`/`run_tiny_shapes()` first. `api.cpp`'s
    `le_render_pixel_buffer` and `render_preview.cpp` both just call this
    now instead of wiring all nine calls by hand. Every individual stage
    method still exists alongside it for partial-chain callers (isolation
    benchmarks in `pipeline_benchmark.cpp`, `render_test.cpp`) - `render()`
    is a convenience wrapper, not a replacement.
    `TransformToPixelsStage` (dbu→pixel, no Y-flip) → `BuildPictureStage`
    (Skia `SkPictureRecorder` draw calls) → `RasterizeStage` (SkPicture →
    raw `PixelBuffer`) is the main chain; `RasterizeStage`/`ComposeWithOverlaysStage`
    use explicit `kRGBA_8888_SkColorType` (not Skia's platform-native
    `kN32_SkColorType`) so byte layout matches between the macOS dev
    machine and the Linux target, and apply the Y-axis flip
    (`TransformToPixelsStage` deliberately doesn't) as one whole-canvas
    transform — `draw_group` (in `draw_helpers.hpp`) counter-flips each
    text label locally to keep glyphs upright under that flip, so the two
    are coupled; check both if touching either. `render` is a compiled
    library (`add_library(render STATIC src/render/render.cpp)`), not
    header-only like its siblings — isolates `SkFontMgr_mac_ct.h`/
    `ApplicationServices.h` (legacy Carbon `Rect`/`Point`/`Polygon`
    typedefs collide with `le::` types under `using namespace le`) to
    `render.cpp`, which now defines a free `default_typeface()` function
    (declared in `draw_helpers.hpp`) rather than a `Renderer::` static
    method; don't change this back to `INTERFACE`. Single-threaded — see
    README's Threading open design question and `BENCHMARKS.md` for
    current warm-path numbers. Fully covered by `render_test.cpp`,
    including real pixel-byte assertions, not just "didn't crash". Depends
    on a machine-specific Skia checkout, not committed to this repo — see
    Open gaps below.
- `src/io/` — format readers. Currently `lef_reader.{hpp,cpp}`, which drives
  the vendored `lefr*` LEF-parser C callbacks and populates `Root` via the
  generated create/get API. Tested against `src/lefdef/lef/TEST/complete.5.8.lef`
  (the vendored parser's own regression fixture) plus small hand-written
  `.lef` files under `src/io/tests/fixtures/` for cases that fixture
  doesn't hit. `LEFReader` only supports a subset of LEF; extend the tests
  as more constructs get support. `orientation_from_parser`/
  `routing_direction_from_parser`/`signal_direction_from_parser` are
  `public` (unlike the rest of `LEFReader`) so they can be unit-tested
  directly — pure, no parser/instance state.
- `src/api/` — `api.hpp`/`api.cpp`, the C API surface a Flutter plugin's
  Dart FFI binds to: an opaque `LeHandle` (`le_create`/`le_destroy`)
  wrapping one `Root`/`ViewLayerSet`/`Scene`/`Pipeline`/`Renderer` per
  handle (reused across calls, not reconstructed per call); `le_read_lef`
  (callable multiple times on one handle — e.g. tech file then macro
  file(s)); `le_design_count`/`le_design_name`/`le_set_current_design`;
  `le_set_pan`/`le_set_scale`/`le_set_viewport_size`; and
  `le_render_pixel_buffer`. `api.hpp` must stay plain C — no `std::` types,
  default arguments, or overloads in any public declaration — so it parses
  cleanly for `ffigen`/Dart FFI; `LeHandle`'s real definition lives only in
  `api.cpp`. Every function null-checks its handle and degrades gracefully
  rather than crashing. Fully covered by `api_test.cpp`, using a small
  hand-written `.lef` fixture. Depends on `database`, `geometry`, `scene`,
  `view_style`, `pipeline`, `render`, `io`.
- `src/tcl/` — `le_api.i` (SWIG), `le_tcl_shim.hpp`/`.cpp`, `le_tcl_procs.tcl`:
  a Tcl-facing scripting surface wrapping `api.hpp` (see TCL_EXPLORATION.md),
  distinct from `src/api/`'s Dart-FFI-facing one — domain verb command
  names, no visible handle, friendly string ids (`"terminal:NAME"`/
  `"layer:M1"`/`"shape:3"`, name-based or numeric depending on the class -
  see `le_tcl_shim.hpp`'s own "IDs" comment) instead of raw `Le*Id` structs.
  `le_shell` (Tcl_Main-based) and any `tclsh` can both load `le_tcl.so` and
  source `le_tcl_procs.tcl`. Property *reading* (property tables,
  friendly-id resolution, `is_child` enumeration), `get_<type>` search,
  `create_<type>`, `update_<type>`, and `delete_<type>` are all generated
  uniformly for every TCL-readable class — see "TCL codegen" below.
  `update_<type>` is the *only* way any field is ever mutated after
  creation — there is no generated or hand-written per-field setter
  reachable from TCL (a narrower, pre-existing generated
  `Root::set_<klass>_<field>()` still exists at the C++ `Root` layer for
  fields with `.parent`/`.index` set, but nothing calls it — see "Database
  codegen" below). `delete_<type>` cascades to every owned pool-backed
  child reachable through `Klass.tcl_child_list_fields()`, however many
  schema-graph levels deep that goes for a given class (recursively
  expanded at Python codegen time, not a runtime-recursive C++ helper —
  see `Klass.delete_api_body()`'s own docstring, `codegen/codegen/schema.py`);
  a class with no such fields (most of the ~35) gets a trivial,
  non-cascading delete instead. `delete_<type>` used to be the last
  hand-written CRUD surface, for `Terminal`/`TerminalPort`/`Obstruction`/
  `Shape` — the classes this MVP actually edits at all beyond creation —
  but is generated uniformly now too, including for read-only LEF
  technology reference data (`Technology`/`Layer`/`Via`/...), which
  nonetheless still gets a generated `create_<type>`/`update_<type>`/
  `delete_<type>` triple like every other class (nothing calls any of the
  three for those classes today, but it costs nothing extra to generate
  uniformly). `create_<type>`/`update_<type>` also cover a *list* of
  flattenable embedded structs (`Field.list_compound_kind()`, e.g.
  `Shape.rects`/`.polygons`/`.paths` — a `-rects {{ll_x ll_y ur_x ur_y}
  ...}`-shaped flag per field), not just a single one — the former
  hand-written `add_shape_rect`/`_polygon`/`_path` are gone, superseded by
  `create_shape`/`update_shape`'s own generated flags (`update_shape`'s
  own flag replaces the *whole* list, it doesn't append — a script
  updating one entry among several reads the current list via
  `get_properties`/`shape_rects` etc. and passes the full replacement).
  `remove_shape_rect`/`_polygon`/`_path` (remove one entry by index) are
  the sole remaining hand-written CRUD-ish leftover, since removing one
  geometry entry by index isn't per-class flag-driven CRUD in the same
  sense `delete_<type>` is. `Abstract.boundary`/`Layout.diearea` are each
  a real child `Shape` (not a bare polygon list) — reusing `Shape`'s own
  proven create/update machinery instead of adding bespoke single/list-
  Polygon-field support, since no other field in the schema has that
  shape — see `Field.create_excluded` in `codegen/codegen/schema.py`
  for fields still deliberately deferred. Fully covered by
  `src/tcl/tests/smoke_test.tcl`/`crud_test.tcl`/`shell_test.tcl` (run via
  `tclsh8.6`, not the generic `tclsh` — see the `build-test` skill).
- `src/lefdef/` — vendored LEF/DEF 6.0.62-p004 C parser source (Si2 distribution).
  Built by its own `Makefile` via `ExternalProject_Add` in the top-level
  `CMakeLists.txt`; only `lef/` is wired into the build so far (`def/` is
  vendored but unbuilt until a DEF reader exists). Never hand-edit — it's
  third-party source, license in `src/lefdef/{lef,def}/LICENSE.TXT`.
- Each module's tests live alongside it in a `tests/` subdirectory (e.g.
  `src/database/tests/database_test.cpp`), hand-written GTest.

## Database codegen (codegen)

Generated code follows the **INDEXED_POOLS** export style, produced by this
project's own `codegen` fork (repo root: `codegen/` — a project-specific
fork of [cmg](https://github.com/johndru-astrophysics/cmg), which stays
generic/reusable; `codegen` owns this project's own display/formatting
conventions instead, e.g. the `dbu` field type and its LEF/DEF unit-conversion
formatting — see `codegen/codegen/schema.py`'s `TYPEMAP` and
`Field.wrap_with_to_property*`). Every `Klass` in `schema.py` becomes:

- `XxxData` — a plain data struct.
- `XxxId` — a `{index, generation}` handle (see `generated/ids.hpp`), not a
  pointer, fully ordered (usable as a `std::map` key with no custom comparator).
- Storage in a `Pool<XxxData, XxxId>` (`generated/pool.hpp`) — a generational
  slot array, so erased objects can't alias a reused slot.
- `Root` (`generated/root.hpp`) owns every pool plus an `index_` for
  parent→children and lookup-by-field indices, and exposes
  `create_x`/`get_x`/`get_x_ids`/`for_each_x_id`/`clear_x`/`get_x_size` per
  class, plus `update_x` (see `Klass.update_root_body()`,
  `codegen/codegen/schema.py`) — the *only* place a pool-backed class's
  fields are ever mutated after creation. Every parameter beyond the id
  (and, for a single-parent class, the parent) is `std::optional<T>`;
  `has_value()` means "apply this field", omitted means "leave unchanged" —
  the opposite of `create_x`'s own "omitted means unset" `XxxData`
  convention. A single-parent class's `update_x` can also reassign the
  parent (with correct index maintenance, including moving a
  `unique_per_parent` field's own sibling bucket to the new parent — a gap
  the older, narrower `set_x_<field>` below has always had); a
  multi-parent class (`Shape`, `ViaLayer`, `Foreign`,
  `LayerDensityEntry`) gets no parent parameter at all, since reassigning
  one parent field alone would violate its "exactly one parent set"
  invariant.

A field's `has_pool` defaults `True` — a `Klass` is embedded (a plain value
type inline in its owner's `XxxData`, e.g. `Point`/`Rect`/`Symmetry`) only by
explicitly setting `has_pool=False`. Converting an embedded struct to pooled
(add a back-reference `parent=` `Field` on it per owner relationship, mark
the owner's own field `is_child=True`) needs no template changes — every
pool-backed `Klass` gets the same `create_x`/`get_x`/`get_<owner>_<field>()`
surface uniformly, whether it's one of the ~15 originally-pooled top-level
classes (`Layer`/`Via`/`Terminal`/...) or one of the ~20 former embedded
structs pooled in a later round specifically so they'd also get their own
generated property table and (see below) `create_<type>` command.

`Field.unique_per_parent` (paired with `index=True`) makes `create_x`
fallible for that `Klass`: it builds a per-parent-scoped index (nested by the
owning `Klass`'s own parent field) instead of the default flat/global one a
plain `index=True` field gets, and returns an invalid id — without
inserting — if a sibling under the same parent already has that value,
instead of always succeeding. `Terminal.name` is the only field using this
today (a Terminal's name only needs to be unique within its own Abstract,
not globally — real LEF libraries reuse pin names like VDD/IN0 across
different Abstracts) — see `Field.unique_per_parent`'s own docstring in
`codegen/codegen/schema.py` for the full mechanism (nested index shape,
`create_x`/`set_x_<field>`/`delete_x` bookkeeping, the `get_x_by_<field>`
accessor's parent-scoped signature). `set_x_<field>` here is the older,
narrower per-field setter still generated for any field with `.parent`/
`.index` set (`root_hpp_j2.py`'s own `{%- if field.parent or field.index
%}` gate) — nothing calls it anymore (superseded by `update_x` above,
which alone handles reparenting *and* a `unique_per_parent` rename
correctly together in one call); it stays generated, untouched, purely as
a documented characteristic of this codegen fork, not a mutation path
this project's own code still uses.

To change the schema: edit `src/database/schema.py`, bump `Schema.version`
(only needed for a real field/class shape change, not a pure codegen-side
formatting change), then regenerate with the `regen-database` skill rather
than editing `generated/` by hand. Real test coverage lives in each module's
own `tests/` directory, not `generated/` — codegen doesn't emit test files.

## TCL codegen (codegen, `--target tcl`)

A separate generation target from the database one above (`regen-tcl`
skill, not `regen-database`) — covers `src/tcl/`'s property-*reading*,
`get_<type>` *search*, and `create_<type>` surface: `src/api/generated_tcl/`
(`ids.inc`/`declarations.inc`/`handle_fields.inc`/
`property_accessors_internal.inc`/`property_accessors_public.inc`/
`filter_tables.inc`/`search.inc`, `#include`d from `api.hpp`/`api.cpp`) and
`src/tcl/generated/` (`le_tcl_shim_generated.hpp`/`.inc`,
`le_api_generated.i`, `le_tcl_procs_generated.tcl`,
`#include`d/`%include`d/`source`d from
`le_tcl_shim.hpp`/`.cpp`/`le_api.i`/`le_tcl_procs.tcl`). Every pool-backed
`Klass` gets a generated property table, friendly-id resolution,
`is_child`-field enumeration, a `get_<type>` search command, and a
`create_<type>`/`update_<type>`/`delete_<type>` triple by default
(`Klass.tcl_readable`/`Klass.tcl_id_field` in `codegen/codegen/schema.py`
— see the `regen-tcl` skill for the opt-out/override mechanics and the
full list of injection points) — uniformly across all ~35 classes today,
including `Terminal`/`TerminalPort`/`Obstruction`/`Shape`, whose
`create_X`/per-field setters/`delete_X` (and `Shape`'s own former
`add_shape_rect`/`_polygon`/`_path`) all used to be hand-written and are
now fully generated too, superseded by `create_<type>`/`update_<type>`/
`delete_<type>`, see below. `Shape`'s own `remove_shape_rect`/`_polygon`/
`_path` (removing one geometry entry by index) is the sole remaining
hand-written CRUD-ish surface — not per-class flag-driven CRUD in the
same sense.
`Klass.has_current_access = True` (`Technology`/`Abstract`/`Schematic`) marks
a class with a generated "current instance" concept — one command,
`current_X ?id?` (with no argument, reads it back; given a friendly-id
token, selects it first, then returns it) — that every *other* readable
class's `get_<type>` default scope (`-of` omitted) derives from
automatically, purely from schema graph structure — see
`codegen/codegen/tcl_scope.py`'s own module docstring for the algorithm, and
the `regen-tcl` skill for the full injection-point list. `le_set_current_design`/
`le_set_current_design_by_id` (`api.cpp`) also move this alongside
`Scene::current_abstract()` (the separate GUI-rendering "current view"),
so selecting a Design means the same thing whether it came from a
Dart-driven GUI or a TCL script's `open_design`; a script that builds an
`Abstract` from scratch and calls `current_abstract <id>` directly (no
`Design` to `open_design` into at all) still only touches this generated
state, never `Scene`.

`create_<type>` covers one flag per scalar field (`str`/`int`/`double`/
`dbu`/`bool`/enum), one flag per *flattenable* embedded-struct field
(`Point`/`Rect`/`Symmetry`/`DensityCheckWindow`/... — see
`Klass.embedded_scalar_leaves()`; the one embedded struct that isn't
flattenable, `ParallelRunLengthSpacingTable`, a genuine variable-size
table, stays out of scope), and one flag per *list* of a flattenable
embedded struct (`Field.list_compound_kind()` — e.g. `Shape.rects`:
`List[Rect]`, `.polygons`: `List[Polygon]`, `.paths`: `List[Path]`; see
its own docstring for the three recognized element shapes — "flat" (a
fixed-arity record like `Rect`), "points" (a variable-length list of
points, like `Polygon`), "points_plus_scalars" (one point-list field
plus sibling scalars, like `Path`'s `polygon`/`width`) — and
`Field.create_excluded` for fields that structurally qualify but are
deliberately deferred, e.g. `Layer.min_sizes`).
`is_child` fields stay out of scope entirely (an `add_X`/`set_X`
relationship concern, not a value one). A flag is required iff
`Field.create_required()` (mirrors
`is_optional`, except `bool` and compound fields are always optional —
`false` is already a zero-cost "not specified" default for `bool`, and a
compound field's own `is_optional` is frequently just a scoping accident
from an earlier round, not a deliberate LEF-syntax judgment; requiring
either would be pure noise); an *omitted* optional flag ends up genuinely
unset (`std::nullopt`), not a zero-value default — a `str`/enum field passes
`nullptr` through the C layer (Tcl can't produce a null `const char*`
directly, so an empty string is treated as "omitted", the same convention
this codebase's hand-written `-flag` parsing already used before this
generator existed), a numeric or compound field gets a companion
`has_<field>` int32. `dbu` fields (plain or nested inside a compound one)
cross the C boundary in microns (`<field>_um`, converted via
`database_units_microns()`/`to_dbu()`), and an enum field crosses as its
`to_string()`/`from_string()` spelling (e.g. `"INPUT"`, parsed via the
matching generated `<enum>_from_string()` — see `enum_hpp_j2.py` — not a raw
numeric code). A single-struct compound field explodes into one C slot
per scalar leaf (`Point` → 2 doubles, `Rect` → 4, `Symmetry` → 3
`int32_t` flags), each individually arity-checked in Tcl before the
`_cmd` call — a wrong-arity flag (`-size {1 2 3}`) then fails with a
real, flag-naming Tcl error instead of deep inside C++ with no context;
a `Symmetry`-shaped field instead takes a case-insensitive keyword set
(`-symmetry {X Y R90}`, mirroring LEF's own `SYMMETRY X Y R90 ;`
grammar). A *list*-of-struct compound field (`Field.list_compound_kind()`)
instead flattens its whole nested Tcl list into a single `(const
double*, int32_t count)` pair, reusing the existing `POINTS_ARRAY_UM`
typemap (`le_api.i`) under its own `<field>_flat_um`/`<field>_flat_count`
parameter names (`Klass.list_compound_swig_applies()` emits one `%apply`
line per such field) — a "flat" element (`Rect`) needs no length prefix
(fixed arity, arity-checked the same way a single-struct field is), a
"points"/"points_plus_scalars" element (`Polygon`/`Path`) prefixes the
whole flag with a record count and each record with its own point count,
since each record's own length varies (`Field.list_compound_parse_lines()`
parses this back apart api.cpp-side, with `count == 0` handled as a
genuinely valid "no records" input, not a malformed one). `Klass.
cmd_tcl_preamble(mode)` generates all of this Tcl-side flattening/arity
checking (shared between `create_<type>` and `update_<type>`); `Field.
cmd_param_slots(mode)` is the single place "one field → one or more C
slots" is defined, so a signature can't drift from a call site. A
multi-parent class (`Shape.terminal_port`/`.obstruction`, `ViaLayer`,
`Foreign`, `LayerDensityEntry`'s `ac_layer`/`dc_layer`) takes one
`Le<Parent>Id`/token flag per parent field, generically validated to require
*exactly one* resolving (not zero, not both) — this is what unified the
formerly hand-written `create_terminal_port_shape`/`create_obstruction_shape`
split into one generated `create_shape -terminal_port|-obstruction`. All of
this construction logic (per-field validation, the exactly-one-parent check,
the `<Klass>Data{...}` initializer) is built as one Python string in
`Klass.create_api_body()` (`codegen/codegen/schema.py`), not deeply nested
Jinja — the per-field-type/optionality branching reads far more clearly as
real Python control flow.

`update_<type>` mirrors `create_<type>`'s own flag set field-for-field
(`Klass.update_api_body()`/`update_root_body()`), but every flag's own
meaning flips: omitted means *leave unchanged*, not *unset* — so every
field gets a `has_<field>`-shaped "was this provided" signal here (even
`bool`, which doesn't need one in `create_<type>`), and nothing is ever
required. A single-parent class also accepts an optional parent flag to
reassign it (a multi-parent class gets none at all — see "Database
codegen" above for why); a `unique_per_parent` field can be renamed and
reparented in the same call, with the reparent applied first so the
rename's own sibling-collision check already reflects the new parent.
`update_<type>` is the *only* way any field is ever mutated after
creation — see the `src/tcl/` bullet above for the "no per-field setters
anywhere" constraint this enforces. A `list_compound_kind()` field's own
flag *replaces* the whole list when provided (matching every other
field's "provide it, apply it" semantics), not appends — a caller adding
one entry among several already-present ones reads the current list
first (`get_properties`/`shape_rects` etc.) and passes the full
replacement.

`delete_<type>` (`Klass.delete_api_body()`) needs no flags at all — just
the object's own friendly id — but does the most work of the three:
every owned pool-backed child reachable through
`Klass.tcl_child_list_fields()` is deleted along with it, cascading
however many schema-graph levels deep that goes for a given class (e.g.
`Technology`'s own `non_default_rules` → `vias` → `layers` chain is 3
levels deep). The cascade is planned recursively at *Python codegen
time* (one flat, unrolled loop per schema-graph depth level in the
emitted C++, not a generic recursive C++ helper — this codebase's own
"flat generated code" aesthetic), and every cascaded object is recorded
into a currently-recording transaction (UPDATES.md item 21) deepest-first,
this object itself last — the reverse of `Transaction::undo_all`'s own
replay order, so undo recreates the top-level object before its
descendants, and each descendant's own undo-recreate lambda can then read
its immediate parent's already-recreated live id back out of a shared
`IdCellPtr` captured while it was still being collected as a "child" one
level up (see `Transaction::id_cell_for`, `src/editing/transaction.hpp`).
A class with no `tcl_child_list_fields()` at all (most of the ~35) gets a
trivial, non-cascading delete instead — snapshot, erase, record, nothing
else. `Field.create_excluded` fields (`Layer.min_sizes`, ...) stay a
separate, not-yet-enabled effort for `create_<type>`/`update_<type>` —
deferred by explicit opt-out, not
because the mechanism can't reach them; this has no bearing on
`delete_<type>`, which doesn't touch individual fields at all.

## Open gaps (tracked in README's Plan checklist)

- `src/lefdef/def` is vendored but not yet wired into `CMakeLists.txt` — add
  an `ExternalProject_Add(def_lib ...)` (mirroring `lef_lib`) when a DEF
  reader module is added.
- Skia isn't vendored/built by this project — `src/render/`'s
  `CMakeLists.txt` `skia` target points `SKIA_DIR` at a pre-built checkout
  (default `/Volumes/Docking/Projects/synthosilicon/skia/skia`, override with
  `-DSKIA_DIR=...`). That checkout must have `out/MacStatic/libskia.a`
  built with `is_component_build=false` (static). Links `libskia.a` +
  Homebrew `harfbuzz`/`icu4c`/`jpeg`/`png`/`z`/`webp`/`webpdemux` + macOS
  `CoreText`/`CoreFoundation`/`CoreGraphics`/`CoreServices` frameworks — no
  GPU (Ganesh/Metal) frameworks needed, only raster (CPU) surface APIs are
  used.
- ~~Linux build needs a fontconfig/FreeType-backed `SkFontMgr`~~ — done
  (Docker/Ubuntu Linux CI session), then revised again: `render.cpp`'s
  Linux `default_typeface()` now loads from a bundled font directory
  (`SkFontMgr_New_Custom_Directory`, `LE_FONT_DIR` — defaults to
  `assets/fonts/`, committed to the repo) rather than system fontconfig —
  a locked-down rootless-build target machine (see the Rocky 8 bullet
  below) can't be assumed to have any fonts installed or fontconfig
  configured at all, and a missing font under the fontconfig path failed
  silently (blank labels, no error).
- **Rootless Rocky Linux 8 build** (no root, no system package installs,
  no Docker) — `backend/scripts/rocky8-bootstrap.sh`/`rocky8-env.sh`
  assemble a toolchain (gcc-toolset-13, CMake/Ninja/Boost/SWIG, GTK3 +
  closure, Skia's own third-party-vendored build) entirely via rootless
  RPM extraction (`rpm2cpio`/`cpio`, no `dnf install`) and upstream
  release tarballs into `~/.local/layout_engine_toolchain`. Unverified
  against a real Rocky 8 machine as of this writing — expect real
  iteration, same as the Docker/Ubuntu path needed.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

A second tree, `build_release` (`-DCMAKE_BUILD_TYPE=Release`), is also
expected to exist and be kept up to date alongside `build` —
`flutter_plugin/macos/layout_engine_plugin.podspec` links `build_release`'s
`api`/`render`/`io` output directly (a real running Flutter app needs
actual optimized performance, not debug-build timings), so it's a
persistent tree, not a throwaway benchmarking artifact. See the
`build-test` skill and `flutter_plugin/CLAUDE.md`'s Native linking
section.

Dependencies: `spdlog`, `fmt`, `Boost` (headers only, for `geometry`) via
`find_package` — installed on this dev machine via Homebrew, falling back to
`FetchContent` for `spdlog`/`fmt` when no system install is found (needed for
the rootless Rocky 8 build below, where neither ships by default); GoogleTest
and GoogleBenchmark via `FetchContent` (no system install needed).
`src/lefdef/lef` is built as an `ExternalProject_Add` step that shells out to
its own vendored `Makefile`. For a rootless Linux build with no system
package installs available at all (e.g. a locked-down Rocky Linux 8
machine), see `scripts/rocky8-bootstrap.sh`/`rocky8-env.sh` and the Open
gaps entry above.

**Gotcha:** that vendored Makefile's `all: install release` target is not
safe under a parallel/inherited `make` jobserver — both traversals touch the
same bison-generated `lef.tab.c`/`liblef.a`, so running it under `-j` races
and fails. The `lef_lib` `ExternalProject_Add` step already forces
`--unset=MAKEFLAGS make -j1` — don't remove that when touching the build.

### Coverage (line + branch)

Off by default (instrumentation has a real perf cost, and this project's
own rule is benchmark first). Opt in at configure time:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
cmake --build build --target coverage
```

Rebuilds `io`/`backend_tests` with Clang source-based coverage, runs the
tests, and prints a `llvm-cov report --show-branch-summary` table (also
written to `build/coverage/report.txt` and `build/coverage/lcov.info`).
Requires Clang and `llvm-profdata`/`llvm-cov` — resolved via `xcrun`
automatically on macOS.

**Gotcha:** `ENABLE_COVERAGE` is a _cached_ option — reconfiguring with e.g.
`-DCMAKE_BUILD_TYPE=Release` alone does **not** reset a previously-set-ON
value back to OFF, and coverage instrumentation forces `-O0` regardless of
`CMAKE_BUILD_TYPE`. Always pass `-DENABLE_COVERAGE=OFF` explicitly (or use a
fresh `build/`) to get back to a normal, uninstrumented build — this
silently produced ~15-20x-inflated benchmark numbers once already.

### Benchmarks

```
cmake --build build --target pipeline_benchmarks
./build/pipeline_benchmarks
```

Build in `-DCMAKE_BUILD_TYPE=Release` for real numbers — Debug timings
aren't meaningful. `src/pipeline/benchmarks/stress_data.hpp` generates a
deliberately unrealistic 1M-shape single-macro LEF file and builds the
`Scene` used to view it; `pipeline_benchmark.cpp` times each `Pipeline`/
`Renderer` stage in isolation plus the full chain under several call
patterns. See `BENCHMARKS.md` for current numbers and full history. Add
`--benchmark_repetitions=5 --benchmark_report_aggregates_only=true` for
stable numbers when comparing two approaches, and
`--benchmark_filter=<regex>` to run a subset.

`src/pipeline/benchmarks/render_preview.cpp` (target `render_preview`) is a
dev-only tool, not a benchmark: `./build/render_preview a.lef [b.lef ...]`
reads every given LEF file into one shared `Root` and writes one PNG per
Design (`preview/<library-name>__<design-name>.png`) via the real
`Renderer::rasterize()` path, so real LEF renders can be visually
sanity-checked without waiting for Flutter texture wiring. Not run by
`ctest` or the `coverage` target.

## Conventions observed in existing code

- Everything lives in `namespace le`.
- Doxygen-style `/// @brief` one-liners on generated public methods — match
  this on hand-written public API.
- No exceptions for expected-missing-data paths — pool lookups return
  nullable pointers (`get(id)` → `T*`) or use `std::optional`/`std::expected`.
- The vendored LEF parser reuses one scratch struct per callback type across
  the whole file and does **not** reset fields to a neutral default between
  calls — always check the matching `has*()` guard (e.g.
  `lefiLayer::hasDirection()`) before trusting a getter, or a value can leak
  forward from a previous element that happened to set it.

## Related prior art

`../../layout_engine/backend` (sibling repo, same author) is an earlier,
more complete implementation of the same idea. This MVP deliberately
restarts the pipeline/rendering architecture decisions rather than
importing that one — treat it as reference/lessons-learned, not code to
copy wholesale.

## Skills

- `regen-database` — regenerate `src/database/generated/` from `schema.py` via the local `codegen` fork.
- `regen-tcl` — regenerate `src/tcl`'s generated property-reading surface from `schema.py` via the local `codegen` fork's `tcl` target.
- `build-test` — configure/build/test the CMake project once one exists.
- `cpp-review` — review pending changes for missing test coverage, unnecessary
  allocations/copies/moves, memory safety, and other issues; reports via
  `ReportFindings`, doesn't apply fixes. Named to avoid colliding with the
  built-in, billed `/code-review ultra`.
