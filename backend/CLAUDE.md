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
  concept distinct from the LEF/DEF-mirroring `database`. `ViewLayerPurpose`
  (a closed, application-owned enum, not a LEF/DEF vocabulary term) has 8
  members: `TERMINAL`/`OBSTRUCTION`/`TRACK`/`ROUTING_BLOCKAGE` (one
  `ViewLayer` of each per physical `Layer`) and `BOUNDARY`/`ROW`/
  `GCELLGRID`/`PLACEMENT_BLOCKAGE` (each its own single `ViewLayer`/row not
  tied to any physical `Layer` — `Row`/`GCellGrid` have no `Layer`
  association at all, and a PLACEMENT blockage's own `Shape` uses
  `Shape.purpose = ShapePurpose::PLACEMENT_BLOCKAGE` instead of a real
  `Shape.layer` for the same reason `Abstract.boundary`/`Layout.diearea`
  do — see `Shape.layer`/`.purpose`'s own `schema.py` comments).
  `ROUTING_BLOCKAGE` and `PLACEMENT_BLOCKAGE` are deliberately two separate
  purposes, not one shared `BLOCKAGE` purpose, so a user can toggle
  visibility/selectability of each independently (they serve very
  different purposes for a user - routing keep-out vs. placement
  keep-out) — the same "never merge across LEF/DEF-distinct kinds" choice
  `TERMINAL`/`OBSTRUCTION` of the same physical `Layer` already make.
  `ViewLayerSet::build_for_technology` builds the full set for a
  `Technology` once, shared/global. Each physical `Layer` gets one color
  from a default palette, shared by all 4 of that Layer's `ViewLayer`s;
  `FillPattern` (a separate, per-purpose visual treatment — `BRICK` for
  `OBSTRUCTION`, `DOTS` for `ROUTING_BLOCKAGE`/`PLACEMENT_BLOCKAGE`, plain
  outline for `TRACK`/`ROW`/`GCELLGRID`) is what actually distinguishes
  same-color `ViewLayer`s from each other; see the class's own doc
  comments for the palette/wraparound details. `TRACK`/`ROUTING_BLOCKAGE`/
  `ROW`/`GCELLGRID`/`PLACEMENT_BLOCKAGE` (Migration Step 2) plus `ROUTE`/
  `REGION` (Step 3 Phase A) are the purposes `LayoutGeometryStage`
  (`src/pipelines/stages/`) walks a `Layout`'s own direct content onto —
  see that module's own bullet below. Fully covered by `view_style_test.cpp`.
- `src/scene/` — `Scene`, per-handle mutable view state: currently
  displayed `AbstractId` *and*, independently, `LayoutId` (Migration Step
  3 Phase C — `current_abstract()`/`current_layout()` are mutually
  exclusive by convention, enforced by every `api.cpp` caller that
  changes the view, not by `Scene` itself), a `hierarchy_depth()` (how
  many further `Placement → Design` levels a Layout view recurses into
  before falling back to a placed instance's own Abstract — see
  `src/pipelines/`'s own `HierarchyResolver` bullet), pan/scale/viewport-size transform,
  per-`ViewLayer` visibility, selection, and current interaction mode.
  Distinct from the persistent `Root` database.
  Layer visibility is keyed by `ViewLayerId`, not `LayerId` — a physical
  layer has independently toggleable `TERMINAL`/`OBSTRUCTION` visibility.
  Selection is `std::variant<TerminalId, ObstructionId>` — extend the
  variant as more selectable kinds need it rather than generalizing early.
  `Scene::Mode` (`SELECT`/`EDIT`, UPDATES.md item 11) is Select by
  default — Select is the only mode where `le_mouse_up` changes the
  current selection; Edit mode restricts mouse interaction to editing
  whatever is already selected (behavior TBD, a later item).
- `src/core/` — header-only generic building blocks (UPDATES.md item 16):
  `RenderedShape`/`TinyShapeDot` (`pipelines`' own shape-generation output/
  render-input type) and `VersionedStage<Key, Value>` — a single-slot
  memoization primitive (`get(key, compute_fn)`) with its own monotonic
  `version()`, bumped on every real recompute; `pipelines`' own
  `MemoizingStage` (`src/pipelines/tbb_core.hpp`) is the oneTBB-flow-graph
  equivalent for most stages, including `HierarchyResolver`'s own
  per-`NodeKey` nodes since 2026-08-30 (see `src/pipelines/`'s own
  bullet) — `top_layout_picture_stage_`, `HierarchyResolver`'s one
  remaining `VersionedStage` use, is a thin cache in front of that
  `MemoizingStage`-based graph, not a stand-in for it. A
  downstream stage composes its own cache key from an upstream stage's
  `version()` instead of manually re-deriving everything the upstream
  depends on — the fix for a caching-bug class where a new upstream
  trigger (e.g. `Root::mutation_version()`) had to be hand-copied into
  every downstream key or a change silently went unseen. `CachedStage<Key,
  Value>` is a backward-compatible alias for `VersionedStage`.
- `src/pipelines/` — the render pipeline, built on oneTBB's `flow::graph`
  (`backend/ONETBB_INTEGRATION.md`'s migration; replaced the earlier
  hand-rolled `src/pipeline`/`src/render`/`src/instancing` split, whose
  own module docs are preserved in git history, not duplicated here).
  `tbb_core.hpp` defines the two reusable primitives every stage builds
  on: `MemoizingStage<InputData, OutputData, PipelineOptions>` (a
  `tbb::flow::function_node` wrapper, Template Method pattern — a
  subclass implements `compute()`, `MemoizingStage::execute()` calls it
  only when `data_version` or, per an optional `options_did_change()`
  override, `PipelineOptions` actually changed since the last
  invocation, else returns the cached result; also emits a Tracy
  `ZoneScoped`/`ZoneName` per real recompute) and `FanInCollectStage`
  (a versioned parallel fan-in accumulator — since 2026-08-30, used by
  `HierarchyResolver`'s own `HierarchyLayoutNodeStage::wire_fan_in` to
  gather a Layout node's variable-arity (0 to 1,000,000), runtime-
  determined placement count, which `join_node`/`indexer_node`'s
  compile-time-fixed arity can't express; still otherwise unused
  elsewhere, kept generic for a future per-`ViewLayerId` parallelism
  pass too).
  `PipelineOptions` (`pipeline_options.hpp`) is the one options type every
  stage in the module shares — `PipelineContext` (raw, non-owning
  `Root`/`ViewLayerSet`/`Scene` pointers), `FrameEpoch`
  (`root_mutation_version`/`view_layers_generation`), `ViewportOptions`
  (`viewport_version`/`visibility_version`/`scale`), `InteractionOptions`
  (`mouse_version`/`selection_version`/`ruler_version`) — a stage's
  `options_did_change` compares only the sub-fields it actually depends
  on. `stages/*.hpp` are the per-file stage classes (`AbstractGeometryStage`/
  `LayoutGeometryStage`, `ViewportFilterStage`/`LayerVisibilityFilterStage`
  and their `Tiny*` siblings, `PixelTransformStage`/`TinyPixelTransformStage`,
  `BuildDesignPictureStage`/`BuildTinyDotsPictureStage`/
  `BuildLayoutPictureStage`, `RasterizePictureStage`, `MouseOverlayStage`/
  `SelectionOverlayStage`/`RulerOverlayStage`, `ComposeStage`) — each a
  near-verbatim port of the equivalent pre-migration stage's `compute()`
  body, only the caching mechanism changed; each stage class's own doc
  comment names which pre-migration stage it ports and its exact
  recompute triggers. `via_shapes.hpp` (via-geometry expansion, shared by
  `AbstractGeometryStage`/`LayoutGeometryStage`), `draw_helpers.hpp`
  (style constants and free Skia drawing helpers — `draw_grid`,
  `draw_group`, `pattern_shader`, `default_typeface()`'s own declaration,
  etc.) and `pixel_types.hpp` (`PixelShape`/`PixelBuffer`/`RasterizedFrame`/
  etc., pixel-space mirrors of `core`'s dbu-space types) are shared
  support headers, not stages themselves.
  Four `tbb::flow::graph`-owning pipeline classes wire stages together via
  `make_edge`, each exposing a synchronous `run()`-style surface
  (`try_put` + `wait_for_all` + read the sink) rather than requiring a
  caller to drive the graph directly: `AbstractShapePipeline`/
  `LayoutShapePipeline` (shape generation + viewport/size/layer
  filtering — `run()`/`run_tiny_shapes()`; `AbstractShapePipeline` also
  exposes `run_generate_shapes()`, the unfiltered `AbstractGeometryStage`
  output alone, for fit-to-content/select-all callers that would
  otherwise wrongly lose off-screen/tiny content to viewport/sub-pixel
  culling), `DesignRenderPipeline` (pixel-transform → build-picture →
  rasterize, design and tiny-shapes chains), `MouseTargetLayerPipeline`/
  `SelectionGhostLayerPipeline` (the overlay/ghost layers), and
  `FrameRenderPipeline` (top-level orchestrator combining the previous
  three plus its own `ComposeStage` — mirrors the old `Renderer::render()`'s
  own call order). `SynchronousStageRunner<Stage, InputData, OutputData>`
  (`synchronous_stage_runner.hpp`) is the same synchronous-wrapper pattern
  factored out standalone, for a caller that needs one stage's own cached
  result without a full pipeline graph around it (`HierarchyResolver`'s
  own persistent shape-generation/overlay stages; `api.cpp`'s few
  standalone-filter call sites). `SynchronousStageChain<Stage1, InputData1,
  Mid, Stage2, OutputData>` (same file) is the two-stage sibling — wires
  `stage1 → stage2` via a real `make_edge` so the second stage's own
  `data_version` is always exactly the first stage's own bumped
  `version()`, never a value a caller threads through by hand (an
  internal adapter node still lets the two stages take genuinely
  different `PipelineOptions`, e.g. `record_local_picture`'s own
  `ViewportThenLayerVisibilityChain` — `hierarchy_stage_support.hpp` —
  needs a throwaway enclosing-viewport `Scene` for culling but the real
  `Scene` for layer visibility). Added after a real bug
  (BUGS_AND_ENHANCEMENTS.md B3's own postmortem): two independent
  `SynchronousStageRunner`s called back to back, fed the same upstream
  `data_version` by hand instead of chaining through `.last_version()`,
  let a sub-pixel cull decision go stale across a live scale change and
  never recover. `hit_test.hpp`'s `hit_test_point`/
  `hit_test_rect` are plain free functions (not stages — the query point/
  rect changes every call, nothing to memoize), ported verbatim from the
  original `Pipeline` class's own static methods.
  `HierarchyResolver` (`hierarchy_resolver.hpp`) is the
  `Placement → Design` hierarchical-instance resolver — same design as
  the original `InstanceRenderer` it replaced (same "local pixel space"
  cached-`SkPicture` convention; same `ViewportFilterStage`/
  `LayerVisibilityFilterStage`-per-node load-bearing correctness fix,
  now *permanent, per-node-instance* members rather than fresh-per-call
  locals — see the class's own doc comment for why that's *strictly
  safer* than the original fresh-per-call rule, not merely still-safe:
  each node is permanently bound to one id for its whole lifetime, so
  the original rule's own aliasing hazard — a shared runner reused
  across many different ids in one frame — can't occur by construction).
  As of 2026-08-30 it *is* built on `MemoizingStage`/`flow::graph`,
  reversing the original design's own decision to avoid that — see git
  history for the pre-2026-08-30 version and its own "data-dependent
  topology, 1,000,000-placement/frame frequency, reentrancy hazard"
  rationale, which this design resolves differently rather than
  ignoring: a single-threaded discovery pass
  (`ensure_node_built`/`discover_layout_children`) walks the database
  first, deciding the graph's own shape — one node per distinct
  `NodeKey` (`Kind::Abstract` keyed on `AbstractId` alone, since an
  Abstract's content never depends on `remaining_depth`; `Kind::Layout`
  keyed `{LayoutId, remaining_depth}`, mirroring the old
  `design_pictures_`/`layout_pictures_` map keys exactly) — *before* any
  node ever executes, incrementally extending one persistent,
  epoch-scoped `flow_graph` as new keys are discovered rather than
  rebuilding a fixed topology per call. No node's own `compute()` ever
  calls `try_put`/`wait_for_all` back into its own enclosing graph — the
  original reentrancy hazard — since topology is decided by the
  discovery pass, not by nodes recursing into each other. A Layout
  node's own placement count (0 to 1,000,000 in the stress fixture) is
  handled by `FanInCollectStage` (`join_node`/`indexer_node` need
  compile-time-fixed arity), one fan-in edge per *placement*, so a
  design placed N times is still resolved once and its single picture
  broadcast to all N. Node lifetime within one epoch is bounded by
  generation-stamped, reachability-based pruning
  (`HierarchyNodeBase::last_touched_generation`/`touch_children`/
  `sweep_stale_nodes` — needed since a `scene.hierarchy_depth()` change
  alone doesn't bump `Epoch`, and each node now carries three permanent
  private nested-graph runners, not just an `sk_sp<SkPicture>`); pruning
  is two-pass (unwire every stale node's own incoming edges before
  destroying any of them) since TBB `flow::graph` nodes don't
  self-deregister from a predecessor's/successor's edge list on
  destruction. `run_pending()`'s own `wait_for_all()` runs
  *unconditionally* on every top-level call, even with nothing in its
  explicit trigger lists — `HierarchyLayoutNodeStage::wire_fan_in`'s
  "already computed" shortcut (an already-settled child shared by a
  second top-level request within the same epoch, fed into the new
  parent's fan-in via a direct `try_put`) seeds real async TBB work
  untracked by those lists; skipping `wait_for_all()` in that case raced
  `node->last_picture()` and `sweep_stale_nodes()`'s own destruction
  against a still-in-flight `compute()` task — a real bug found while
  building this (flaky recompute counts, a null picture, a "Pure
  virtual function called" abort at teardown), not a theoretical one —
  and costs nothing extra once fixed, since `wait_for_all()` on an
  already-quiescent graph returns immediately. `top_layout_picture_stage_`
  (the top-level `render_layout_frame` cache) stays a plain
  `core::VersionedStage`, now a thin cache *in front of* the epoch's
  graph rather than a node inside it (a graph node would force a rebuild
  on every pan tick, defeating "rebuild only on epoch change"). See
  `BENCHMARKS.md`'s 2026-08-30 entry for a same-fixture before/after
  against the numbers below — a genuinely mixed, not-yet-fully-explained
  result (`BuildLayoutPicture`'s own cold-resolve got faster;
  `RenderLayoutFrame`'s own cold-full-depth case got slower) on a
  fixture with only 3 distinct nodes despite 4,000,000 placements, so it
  says little yet about this design's actual concurrent-node payoff case
  — a wider fixture is still needed (that entry's own "Deferred" note).
  `render_layout_frame` (what `api.cpp`'s
  `le_render_pixel_buffer` calls when `Scene::current_layout()` is
  active) shares `FrameRenderPipeline`'s own `RasterizePictureStage`/
  `ComposeStage` instances with the Abstract-view path via
  `run_design_rasterize`/`run_selection_rasterize`/etc., keeping the two
  domains' version numbers disjoint with a `kLayoutVersionDomainTag`
  high bit — the same trick `InstanceRenderer` used to fix a real bug
  (two unrelated pictures landing on the same small version number).
  `pipelines.cpp` is the module's one compiled TU (`add_library(pipelines
  STATIC ...)`, not header-only) — isolates `SkFontMgr_mac_ct.h`/
  `ApplicationServices.h` (legacy Carbon `Rect`/`Point`/`Polygon`
  typedefs collide with `le::` types under `using namespace le`) to this
  one file, which defines a free `default_typeface()` (declared in
  `draw_helpers.hpp`); don't change this back to `INTERFACE`. Every
  individual stage class is fully covered by `tests/pipelines_test.cpp`/
  `render_pipelines_test.cpp`/`hierarchy_resolver_test.cpp` (real Skia
  rasterize-and-sample assertions, cache hit/miss/invalidation behavior —
  not just "didn't crash"); `benchmarks/` (`pipeline_benchmark.cpp`,
  `render_preview.cpp`, `stress_data.hpp`/`layout_stress_data.hpp` — the
  1,000,000-shape and 1,000,000-instance stress fixtures, `layout_stress_data.hpp`'s
  own comment has the exact fixture shape) mirrors the old modules'
  benchmark coverage; see `BENCHMARKS.md` for numbers and history.
  Single-threaded internally — see README's Threading open design
  question. Depends on a machine-specific Skia checkout, not committed
  to this repo — see Open gaps below.
- `src/io/` — format readers/writers. `lef_reader.{hpp,cpp}`/
  `lef_writer.{hpp,cpp}` drive the vendored `lefr*`/`lefw*` LEF-parser C
  API and populate/walk `Root` via the generated create/get API. Tested
  against `src/lefdef/lef/TEST/complete.5.8.lef` (the vendored parser's
  own regression fixture) plus small hand-written `.lef` files under
  `src/io/tests/fixtures/` for cases that fixture doesn't hit. `LEFReader`
  only supports a subset of LEF; extend the tests as more constructs get
  support. `orientation_from_parser`/`routing_direction_from_parser`/
  `signal_direction_from_parser` are `public` (unlike the rest of
  `LEFReader`) so they can be unit-tested directly — pure, no
  parser/instance state.
  `def_reader.{hpp,cpp}` mirrors `LEFReader`'s own shape for the vendored
  `defr*` DEF-parser C API, populating `Layout`/`Row`/`Track`/`GCellGrid`/
  `Placement`/`PhysicalPort`/`PhysicalPortSegment`/`Blockage`/`LayoutVia`/
  `Region`/`Route`/`NonDefaultRule` (`schema.py`) — the full Step 1 reader
  scope (DESIGN/VERSION/UNITS/DIEAREA/ROW/TRACKS/GCELLGRID/COMPONENTS/
  PINS/BLOCKAGES/VIAS/REGIONS/NETS/SPECIALNETS/NONDEFAULTRULES). NETS/
  SPECIALNETS cover routing *geometry* only, not connectivity — see
  `Route`'s own `schema.py` comment.
  `def_writer.{hpp,cpp}` mirrors `LEFWriter`'s own shape for the direct
  (non-callback) `defwWriter.hpp` API, covering the mirror image of
  `DEFReader`'s scope. Unlike LEF, DEF coordinates need no
  `microns_to_dbu()`-style conversion at write time either (see the DBU
  paragraph below) — the one exception, NONDEFAULTRULES LAYER WIDTH/etc,
  is real vendored-writer gap (see `LEFDEF_BUGS.md`'s "DEF writer" section:
  `defwNonDefaultRuleLayer` only accepts a plain `int`, so this loses
  sub-micron precision on write, same asymmetric-precision shape as the
  reader-side finding). `write_def` uses `defwInitCbk()`, not `defwInit()`
  — also documented in `LEFDEF_BUGS.md` (a state-machine gap: `defwInit()`
  writes `VERSION` as text but never updates the internal version-gate
  variable later calls like `defwTracks`' `MASK` check against, and the
  "proper" fix, calling `defwVersion()` after, requires a `defwState` only
  `defwInitCbk()` leaves behind). Tested the same way as `DEFReader`
  (`def_writer_test.cpp`, reusing `complete.5.8.def` for a
  read→write→re-read round trip per construct, plus a `defdiff`-based
  `DEFWriterLefdiffFidelity` test — deliberately narrower than LEF's own
  analogous `LEFWriterLefdiffFidelity`, since `DEFWriter`'s own scope
  (mirroring `DEFReader`'s) excludes far more of `complete.5.8.def`'s
  content than `LEFWriter` excludes of `complete.5.8.lef`'s — see that
  test's own comment for the full excluded-construct list).
  Most DEF coordinate/dimension values (ROW/TRACKS/GCELLGRID/DIEAREA/
  COMPONENTS placement, routed-path points, etc.) are already expressed
  directly in database units in the file itself, unlike LEF (fully
  micron-based, every value needing `microns_to_dbu()`) — confirmed
  against `complete.5.8.def`'s own fixture data, `DEFReader` casts these
  directly to `int64_t`/`int`, scaled through `scale_dbu()`/
  `DEFReader::unit_scale_` (see below) rather than passed through
  unconverted. The one confirmed exception is NONDEFAULTRULES LAYER
  WIDTH/SPACING/WIREEXT/DIAGWIDTH, written in real microns —
  `defiNonDefault`'s own `layerWidthVal()`-style accessors looked like
  the DBU-converted form but actually just truncate the raw micron double
  to an int (found by testing against real fixture values: `10.1` came
  back as `10`, not `10100`) — real conversion needs the plain micron
  accessor times the shared `Technology`'s own `database_units_microns`,
  same as every LEF conversion (this one is unaffected by `unit_scale_` -
  it's derived from real microns directly, not from this DEF's own raw
  dbu integers, so there's nothing to rescale). `DEFReader` only
  resolves/creates that shared `Technology` (mirroring `LEFReader`'s own
  reuse-or-create), and `defrUnitsCbkFn` writes `database_units_microns`
  from DEF's own `UNITS DISTANCE MICRONS` the same way LEF's own units
  callback does - **only** when the Technology didn't already have one
  (e.g. from an earlier LEF read). When this DEF's own UNITS instead
  *disagrees* with an already-established Technology scale,
  `defrUnitsCbkFn` leaves `database_units_microns` alone (every
  already-created Shape assumed that original scale) and instead sets
  `unit_scale_ = technology->database_units_microns / units`, which every
  later raw-value conversion site (`scale_dbu()`, threaded through
  `polygon_from_die_area`/`shapes_from_pin_like`/
  `append_shapes_from_path`/every other geometry callback) multiplies
  through - a real, previously-missing conversion (the old code logged an
  error and left the DEF's raw values unconverted, silently misreading
  them at the wrong grid resolution). Logs a `WARNING` (not an `ERROR` -
  matches `LEFReader::lefrUnitsCbkFn`'s own tone for the same situation,
  which doesn't need this scaling at all since LEF geometry is always
  real microns re-derived through the Technology's scale regardless of
  what the file's own UNITS said) either way; when this DEF's own units
  are coarser than the technology's (`units < database_units_microns`),
  the message calls out that the scaled-up geometry still can't carry
  more precision than its own coarser original grid had.
  `Shape.layer` (both readers) and `Placement.reference_design` (`DEFReader`
  only) are resolved references, not stored names/strings — every reader
  callsite resolves the LEF/DEF-declared name against the shared
  `Technology`/`Library` (`get_layer_by_name`/`get_design_by_name`) at
  creation time and logs an error + skips creating that Shape/Placement
  if it doesn't resolve, rather than storing an unresolved name that could
  later go stale (e.g. if the referenced Layer/Design is renamed). This
  requires the real Technology (tech LEF) to always be read before a DEF
  that references its layers, and the referenced macro/cell Designs to
  already exist before the DEF that instantiates them — the normal real-
  world read order anyway. `Shape.layer` is `Optional[Layer]`, not
  required, though: a Shape that isn't real LEF/DEF routing/terminal/
  obstruction/routing-blockage geometry (`Abstract.boundary`/
  `Layout.diearea`, a DEF PLACEMENT blockage's own region - unlike a
  ROUTING blockage, which sits on a real routing Layer like any other
  real geometry) has no physical layer at all, so it sets `Shape.purpose`
  (`ShapePurpose::BOUNDARY`/`PLACEMENT_BLOCKAGE`) instead - see `Shape.
  layer`/`.purpose`'s own schema.py comments for why exactly one of the
  two is ever set (documented convention, not database-enforced, same as
  e.g. `Blockage.spacing`/`design_rule_width`'s own precedent) rather
  than a fake Technology `Layer` standing in for a non-physical-layer
  concept, which would pollute `Technology.layers` (a real LEF/DEF
  stack) for every consumer that walks it. `ViewLayerSet` (`src/
  view_style/`) builds the `BOUNDARY` `ViewLayer`/row directly off this
  closed enum, independent of `Technology.layers`, the same way
  `ViewLayerPurpose` (a parallel, rendering-only enum) already keeps
  the "what kind of object drew this" concept separate from LEF
  vocabulary; `ShapePurpose::PLACEMENT_BLOCKAGE` has no `ViewLayer`
  resolution yet - Layout/DEF content isn't rendered at all yet (Step
  2/3 of the migration plan), so there's no consumer for one until that
  lands. See `codegen/codegen/schema.py`'s `Field.
  is_plain_reference_field()`/`Klass.get_reference_create_fields()` for
  the small, separate codegen mechanism giving a plain (non-parent)
  reference field like `Shape.layer` its own resolved-by-token
  `create_<type>`/`update_<type>` flag (required or optional - an
  omitted optional one follows the same "invalid/default id means
  unset" convention a parent field's own token already does, no
  has-flag needed at create time, though `update_<type>` still gets one
  there since nothing is ever required to update), deliberately kept
  apart from `get_parent_fields()` (which several structural concerns -
  `is_child` enumeration, delete cascade, `tcl_scope`'s current-instance-
  anchor algorithm - depend on and must not see a field like this as a
  parent/ownership relationship).
  Tested against `src/lefdef/def/TEST/complete.5.8.def` (`def_reader_test.cpp`)
  — that fixture has no companion LEF of its own (a grammar-coverage
  fixture, not a real design), so `DEFReaderCompleteFixture`'s own
  `SetUp()` pre-populates the Technology/Library with exactly the layer
  names/macro names it references, playing the role a real LEF read
  would otherwise.
- `src/api/` — `api.hpp`/`api.cpp`, the C API surface a Flutter plugin's
  Dart FFI binds to: an opaque `LeHandle` (`le_create`/`le_destroy`)
  wrapping one `Root`/`ViewLayerSet`/`Scene` plus the `pipelines`-module
  objects (`AbstractShapePipeline`/`LayoutShapePipeline`/
  `FrameRenderPipeline`/`HierarchyResolver`) per handle (reused across
  calls, not reconstructed per call); `le_read_lef`
  (callable multiple times on one handle — e.g. tech file then macro
  file(s)); `le_design_count`/`le_design_name`/`le_set_current_design`;
  `le_set_pan`/`le_set_scale`/`le_set_viewport_size`; and
  `le_render_pixel_buffer`. `api.hpp` must stay plain C — no `std::` types,
  default arguments, or overloads in any public declaration — so it parses
  cleanly for `ffigen`/Dart FFI; `LeHandle`'s real definition lives only in
  `api.cpp`. Every function null-checks its handle and degrades gracefully
  rather than crashing. Fully covered by `api_test.cpp`, using a small
  hand-written `.lef` fixture. Depends on `database`, `geometry`, `scene`,
  `view_style`, `pipelines`, `io`.
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
  `Shape.rects`/`.polygons`/`.paths` — a `-rects {{{ll_x ll_y} {ur_x ur_y}}
  ...}`-shaped flag per field, brace-nested per point since
  BUGS_AND_ENHANCEMENTS.md E21, matching the same convention
  `get_properties`'s own display already used), not just a single one — the former
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
- `src/gui/` — Dear ImGui prototype: `le_gui.hpp`'s one public function,
  `run_main_thread_loop(LeHandle*)`, opens a GLFW + Dear ImGui window on
  `show_gui` (a Tcl command, `le_tcl_procs.tcl`), rendering the handle's
  own `le_render_pixel_buffer` output into a GL texture each frame and
  translating GLFW/ImGui mouse/keyboard input into the same `le_*` calls
  a script's own zoom/pan/select/mode commands would use — replacing
  Flutter for a CPU-only-Linux-VM deploy target Flutter's own GPU-
  oriented rendering performs poorly on. Depends only on `api` plus GLFW/
  Dear ImGui (unconditional, always fetched/built — not an optional
  build feature; same for readline, below) — no Tcl/SWIG dependency, and
  no knowledge that `le_shell` (its only caller) exists.
  `le_shell.cpp` is the only place Tcl and `gui` meet: its own `main()`
  creates one `LeHandle`, spawns the interactive Tcl console on a
  background thread (injecting that same handle via
  `set_session_handle`, the same mechanism the Flutter plugin's own
  `LeTclBridge` uses), and calls `le::gui::run_main_thread_loop` on the
  process's own true main thread — required there since GLFW only
  allows window/context creation on the main thread on macOS (harmless
  on Linux, which has no such restriction); this is also why the console
  can no longer run directly on the process's own main thread the way it
  did before this existed. `show_gui`'s own signal
  (`LeHandle::gui_show_requested_`/`le_request_show_gui`/
  `le_take_show_gui_request`, `api.hpp`) is a one-shot atomic flag,
  mirroring `is_rendering_`/`le_is_rendering`'s own established shape —
  the Tcl console thread sets it and returns immediately, the GUI
  thread's own idle loop polls and consumes it. No GL loader dependency
  (glad/gl3w/GLEW): every GL call this module makes directly (texture
  upload) is OpenGL 1.1 core, declared by the system GL headers on both
  target platforms without one, and Dear ImGui's own opengl3 backend
  bundles its own minimal loader for its internal GL 3.2 core-profile
  calls. `le_shell`'s own interactive prompt also links GNU readline
  unconditionally (real line editing, recall history, Tab completion via
  `complete_command` — see this file's `src/tcl/` bullet) — both this
  and the GUI window are mandatory dependencies now, since `le_shell` is
  the only user-facing way to run Tcl commands or open a design window;
  a machine missing either fails configure with a clear CMake error
  rather than silently degrading (e.g. the Rocky Linux 8 bootstrap
  effort, see Open gaps below, needs to provision both, not route around
  them). No automated test coverage of the render/input loop itself
  (inherently interactive/visual) — verified manually only, on macOS, as
  of this writing; Linux packaging (`Dockerfile.linux-ci` system
  packages, confirming the configure+build itself succeeds there) is a
  known, tracked next step, not yet done.
- `src/lefdef/` — vendored LEF/DEF 6.0.62-p004 C parser source (Si2 distribution).
  Both `lef/` and `def/` are built by their own `Makefile`s via separate
  `ExternalProject_Add` steps (`lef_lib`/`def_lib`) in the top-level
  `CMakeLists.txt`, each producing a static archive (`liblef.a`/`libdef.a`)
  linked into `io`. Never hand-edit — it's third-party source, license in
  `src/lefdef/{lef,def}/LICENSE.TXT`.
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

A *plain* (non-parent, non-child) reference-to-pooled-klass field —
`Shape.layer`, `Placement.reference_design` — gets the same token-
resolved treatment as a parent field (one `Le<Type>Id`/token flag,
`resolve_<type>_id()` in the shim), but via a small, deliberately separate
mechanism (`Field.is_plain_reference_field()`/`Klass.
get_reference_create_fields()`), not a broadening of `get_parent_fields()`
itself — several other structural concerns (`is_child` enumeration, the
delete cascade, `tcl_scope`'s current-instance-anchor algorithm) depend on
`get_parent_fields()`/`has_parent()` meaning a real ownership relationship,
which a field like this isn't. Both required and `is_optional=True` are
supported (e.g. `Shape.layer`, unset when `Shape.purpose` is set instead)
- an omitted optional one follows the same "invalid/default id means
unset" convention a parent field's own token already does at the API
layer, and the same "empty token means unset" convention a str/enum
field already does at the shim layer - no `has_<field>` companion needed
at create time (unlike a compound/numeric optional field), though
`update_<type>` still gets one there, same as every other field, since
nothing is ever required to update. Unlike a parent field, it needs no
`Root`-level index bookkeeping at all — `Root::create_<klass>`/
`update_<klass>` just carry it as a bare `<Type>Id` (never
`std::optional<...>`-wrapped, whether or not `is_optional` - "unset" is
just the id's own default-invalid value, matching a parent field's own
convention), alongside every other create field. A generated property
table still shows it via the *reference* branch of `wrap_with_to_property()`/
`wrap_with_to_display_property()` (`to_string(id)` — a bare
`Id{index=.., generation=..}` debug string, not a friendly name, since
`to_properties()` has no `Root` to resolve one from) — a real, known
display gap shared with `Instance.reference_design` (same shape, never
surfaced since nothing populates `Instance` yet); a caller wanting the
resolved name uses a hand-written accessor (`le_shape_layer_name`) or a
filter/`get_properties` *hop* through the field instead (`.layer.name`,
walking to the referenced object's own plain `name` property, unaffected
by this gap since hops resolve through the target's own fields). Note
`Field._optional_value_needs_unwrap()` guards every `is_optional`-driven
`.value_or(...)` in these four `wrap_with_*` methods specifically to
exclude this field kind - get_cpp_type()'s own bare-id storage (never
`std::optional`-wrapped) means calling `.value_or(...)` on one would be a
compile error, not just wrong output; a plain enum field (also
technically `is_reference()`, just enum rather than pooled) still needs
the unwrap, so this checks the exact `has_pool`-and-not-`is_enum`
condition, not merely "is a reference".

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

- Migration Step 1 (DEF reader/writer + `Layout` klass) is done —
  `DEFReader`/`DEFWriter` both cover the full scope described in their own
  `src/io/` bullet above. NETS/SPECIALNETS connectivity (as opposed to
  routing geometry), per-layer WIDTH overrides, direct net-level RECT/
  POLYGON/VIA forms, SHIELD nets, and STYLE/SHAPE/TAPERRULE metadata
  remain deferred within that scope. Migration Step 2 (layer/purpose
  generation for DEF's Row/Track/GCellGrid/Blockage constructs) is also
  done — see `src/view_style/`'s own bullet above for the resulting
  `ViewLayerPurpose` members. Migration Step 3 (render pipeline updates -
  walking a `Layout`'s own content into drawable shapes, hierarchical
  `Placement`-instance rendering with per-instance picture caching, the
  1M-instance stress test) is also done — see `src/pipelines/`'s own
  bullet (`LayoutGeometryStage` for Phase A; `HierarchyResolver` for
  Phases B/C), and `BENCHMARKS.md`'s 2026-08-23 entry (Phase D, the real
  performance numbers, measured pre-migration against `InstanceRenderer`
  but still representative of `HierarchyResolver`'s own equivalent
  design). Deep per-shape selection into instanced content (as opposed
  to whole-placement selection) remains deliberately out of scope - a
  real, documented deferral, not a gap found later; per-instance culling
  (skipping an off-screen instance's own `concat`+`drawPicture` call
  before it reaches Skia's own quickReject) was also deliberately not
  added - the Phase D benchmark numbers are the ones to revisit before
  deciding whether it's actually needed.
- Skia isn't vendored/built by this project — the top-level
  `CMakeLists.txt`'s own `skia` target points `SKIA_DIR` at a pre-built checkout
  (default `/Volumes/Docking/Projects/synthosilicon/skia/skia`, override with
  `-DSKIA_DIR=...`). That checkout must have `out/MacStatic/libskia.a`
  built with `is_component_build=false` (static). Links `libskia.a` +
  Homebrew `harfbuzz`/`icu4c`/`jpeg`/`png`/`z`/`webp`/`webpdemux` + macOS
  `CoreText`/`CoreFoundation`/`CoreGraphics`/`CoreServices` frameworks — no
  GPU (Ganesh/Metal) frameworks needed, only raster (CPU) surface APIs are
  used.
- ~~Linux build needs a fontconfig/FreeType-backed `SkFontMgr`~~ — done
  (Docker/Ubuntu Linux CI session), then revised again: `pipelines.cpp`'s
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
aren't meaningful. `src/pipelines/benchmarks/stress_data.hpp` generates a
deliberately unrealistic 1M-shape single-macro LEF file and builds the
`Scene` used to view it; `pipeline_benchmark.cpp` times each pipelines-module
stage in isolation (via a fresh `SynchronousStageRunner` per iteration,
forcing a real cache miss) plus the full `AbstractShapePipeline`/
`FrameRenderPipeline`/`HierarchyResolver` chains under several call
patterns. See `BENCHMARKS.md` for current numbers and full history. Add
`--benchmark_repetitions=5 --benchmark_report_aggregates_only=true` for
stable numbers when comparing two approaches, and
`--benchmark_filter=<regex>` to run a subset.

`src/pipelines/benchmarks/render_preview.cpp` (target `render_preview`) is a
dev-only tool, not a benchmark: `./build/render_preview a.lef [b.lef ...]`
reads every given LEF file into one shared `Root` and writes one PNG per
Design (`preview/<library-name>__<design-name>.png`) via the real
`FrameRenderPipeline` path, so real LEF renders can be visually
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
