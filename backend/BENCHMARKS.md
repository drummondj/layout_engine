# Pipeline Benchmarks

Results from `src/pipeline/benchmarks/pipeline_benchmark.cpp` — a synthetic
1M-shape single-macro LEF (10% PINs / 90% OBS, alternating M1/M2 layers and
RECT/POLYGON/PATH geometry). Run with:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_COVERAGE=OFF
cmake --build build --target pipeline_benchmarks
./build/pipeline_benchmarks --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

`ENABLE_COVERAGE` is a cached CMake option — always pass it explicitly as
`OFF` when reconfiguring for a benchmark (see CLAUDE.md's Coverage gotcha).
Machine: 10-core Apple Silicon Mac (macOS), Clang.

## 2026-08-03 — 4-stage pipeline (generate → viewport/size-filter → resolve view layers → layer-visibility-filter)

Commit: `71f9acd`

| Stage                         | Mean     | cv    |
| ----------------------------- | -------- | ----- |
| `generate_shapes`             | 46.9 ms  | 0.58% |
| `filter_by_viewport_and_size` | 4.50 ms  | 1.31% |
| `resolve_view_layers`         | 1.89 ms  | 0.56% |
| `filter_by_layer_visibility`  | 0.825 ms | 1.01% |
| `run()` (all 4 stages)        | 55.7 ms  | 0.39% |

`resolve_view_layers` as its own stage _after_ the viewport/size filter
pays the layer-lookup cost on only the ~25% of shapes that survive
culling — an earlier design resolving during `generate_shapes` measured
~46% slower. `generate_shapes` is ~84% of `run()` and the natural
optimization target if pipeline throughput ever needs to improve.

## 2026-08-03 — PipelineCache: cascading per-stage caching (later merged into Pipeline, see below)

Added a `PipelineCache` wrapping the four stages with cascading
last-seen-input comparison. v1 chained `resolve_view_layers`'s validity to
the viewport filter's, so it re-ran on every pan/zoom despite depending
only on `AbstractId`; v2 decoupled it (key: `AbstractId` alone), trading
+42ms cold-start (`resolve_view_layers` now runs on the full 1M shapes
instead of the ~250K viewport-filtered survivors) for -1.7ms on every
subsequent pan/zoom frame:

| Scenario                        | v1                          | v2       |
| ------------------------------- | --------------------------- | -------- |
| No change (steady state)        | ~0 ms                       | ~0 ms    |
| Pan-only                        | 7.83 ms                     | 6.11 ms  |
| Visibility-only                 | 0.579 ms                    | 0.614 ms |
| Cold start (fresh, one `run()`) | 55.7 ms (uncached baseline) | 97.7 ms  |

Investigated why `resolve_view_layers` on the full 1M set (40.3ms) is far
more than 4x the 250K-subset cost (1.88ms) despite both lookups being
O(1) — not algorithmic; best explanation is that materializing ~1M `Shape`
copies (one heap allocation each) hits cache/TLB/allocator effects
disproportionately, the same reason `generate_shapes` itself costs
~47-50ms. Kept the v2 design: pan/zoom/select dominates real usage, so the
one-time cold-start cost is worth the per-frame win. This copy-volume
finding directly motivated merging the two stages, below.

## 2026-08-03 — PipelineCache merged into Pipeline; resolve_view_layers merged into generate_shapes

Two structural changes landed close together:

- `PipelineCache` (a separate class, cascading invalidation via
  manually-set boolean flags) was hard to read. Replaced: `Pipeline`'s
  stage methods became non-static instance methods, each owning a small
  `CachedStage<Key, Value>` member (remembers the last key/result,
  recomputes only when the key changes) and chaining to the previous
  stage internally. Pure structural change — confirmed via the full
  benchmark suite, all deltas within normal noise.
- Once merged, `generate_shapes` and `resolve_view_layers` turned out to
  always share the same cache key (`AbstractId`) and always recompute
  together — the reason they were originally split (resolving during
  generation measured ~46% slower) no longer applied, since that finding
  was for a design where viewport-filtering ran _between_ them; the
  current design always resolves on the full set regardless. Merged:
  `generate_shapes` now resolves each shape's `ViewLayerId` inline;
  `TaggedShape` is gone; pipeline is down to 3 stages.

| Benchmark               | Before (PipelineCache v2)                | After both merges |
| ----------------------- | ---------------------------------------- | ----------------- |
| `BM_Run` (cold start)   | 97.7 ms                                  | **58.9 ms**       |
| `generate_shapes` alone | ~52.6ms + ~40.3ms (two stages) ≈ 92.9 ms | **52.6 ms**       |
| Reused, pan-only        | 6.11 ms                                  | 5.91 ms           |
| Reused, visibility-only | 0.614 ms                                 | 0.587 ms          |

Cold-start dropped ~39%, and the merged `generate_shapes` costs far less
than the sum of the two stages it replaced — consistent with eliminating
one full 1M-`Shape` copy pass. Warm path unchanged, as expected.

## 2026-08-03 — render module: dbu→pixel transform + SkPicture generation

New `src/render/render.hpp`: `Renderer` adds two more `CachedStage`-backed
stages on top of `Pipeline`'s output — `transform_to_pixels` and
`build_picture` (Skia `SkPictureRecorder` draw calls).

| Benchmark                                       | Mean                                 |
| ----------------------------------------------- | ------------------------------------ |
| `BM_TransformToPixels` (isolated, ~250K shapes) | 0.715 ms                             |
| `BM_BuildPicture` (isolated, ~250K shapes)      | 1.43 ms                              |
| `BM_Render` (pipeline + render, cold)           | 62.0 ms (+2.5ms over pipeline-only)  |
| `BM_RenderReused_PanOnly` (warm)                | 8.56 ms (+2.75ms over pipeline-only) |
| `BM_RenderReused_NoChange`                      | ~0 ms                                |

**Threading decision, backed by these numbers**: warm/interactive path is
8.56ms at 1M shapes, comfortably under a 60fps/16.6ms budget — no case for
a background thread yet. Single-threaded stays the right choice until a
future benchmark (larger data, real text rendering, actual rasterization)
shows otherwise.

## 2026-08-03 — Pipeline groups by ViewLayerId (bottom-up draw order) + Terminal text labels

- `filter_by_layer_visibility`/`run()` now return `std::map<ViewLayerId,
vector<RenderedShape>>` instead of a flat vector — checks visibility
  once per distinct ViewLayerId, and map iteration order gives correct
  bottom-up draw order for free (`ViewLayerId`'s ordering, via `Id<Tag>`'s
  new `operator<=>` added to `cmg`'s template, matches LEF-declared layer
  stacking order). `build_picture` follows suit, building each ViewLayer's
  `SkPaint` once per group instead of once per shape.
- `generate_shapes` attaches one text label per Terminal (via
  `Geometry::get_label_location`), riding on a real geometric Shape so it
  survives the viewport/size filter's bbox check.

| Benchmark                              | Before  | After   |
| -------------------------------------- | ------- | ------- |
| `BM_GenerateShapes` (1M shapes)        | 52.6 ms | 101 ms  |
| `BM_FilterByLayerVisibility`           | 0.82 ms | 2.28 ms |
| `BM_Run` (cold)                        | 58.9 ms | 108 ms  |
| `BM_RunReused_PanOnly`                 | 5.91 ms | 8.47 ms |
| `BM_RenderReused_PanOnly` (full chain) | 8.56 ms | 12.3 ms |

The `generate_shapes` regression is ~36ms of `Geometry::get_label_location`
cost (confirmed via an isolated 100K-call micro-benchmark, 356ns/call) and
the `filter_by_layer_visibility` regression is `std::map`'s per-shape tree
insert vs. a flat vector scan — both understood, not mysteries. One real
inefficiency was caught and fixed along the way (a needless per-Terminal
`std::vector<Shape>` copy), recovering ~5ms. Still comfortably under a
60fps budget (12.3ms/frame warm), so the ~2x cold-start regression wasn't
chased further.

## 2026-08-03 — One label per distinct layer, not one per Terminal

A Terminal's Shapes can span multiple physical layers (e.g. M1 and M2),
each needing its own label tied to its own `ViewLayerId`. `generate_shapes`
now buckets a Terminal's Shapes by `layer_name` instead of one combined
label per Terminal; single-layer Terminals (the common case) still get
exactly one label.

| Benchmark                       | Before | After      |
| ------------------------------- | ------ | ---------- |
| `BM_GenerateShapes` (1M shapes) | 102 ms | 106 ms     |
| `BM_Run` (cold)                 | 110 ms | 112-113 ms |

~4ms from one `unordered_map` construction per Terminal, even single-layer
ones. Not pursued further: small relative to the already-accepted
label-placement cost, and the warm/interactive path is unaffected
(`generate_shapes` is cached per-`AbstractId`, doesn't rerun on pan/zoom).

## 2026-08-04 — Merge overlapping rects/polygons within a Shape

Found visually (`render_preview` against `Nangate45_stdcell.lef`): a
Terminal Port's or Obstruction's Shape can contain several rects that
overlap _each other_ (e.g. an L/T-shaped pin), and drawing each with a
translucent fill double-painted the overlap into a visibly darker patch.
`Geometry::merge_overlapping_fills(Shape&)` unions a Shape's own
rects+polygons into a minimal non-overlapping set; paths are left
untouched. Scoped to one Shape at a time.

No measurable regression on the 1M-shape stress benchmark (every shape
there has exactly one geometry item, so the merge's fast path is a no-op).
Isolated micro-benchmark (N half-overlapping rects) to measure the actual
union cost, since the stress data can't:

| N rects | Copy alone | Copy + merge |
| ------- | ---------- | ------------ |
| 2       | 0.016 us   | 3.48 us      |
| 5       | 0.017 us   | 13.6 us      |
| 10      | 0.024 us   | 30.1 us      |
| 50      | 0.033 us   | 176 us       |

Grows slightly worse than linearly (each iterative union operates on a
result that's grown from previous ones). Real-world check: the full
`Nangate45_stdcell.lef` library (135 designs) went from 11.1ms to 30.5ms
total `generate_shapes` time combined (worst single design under 1ms) —
real cells do have some overlapping pin geometry, but not much per shape.
The dense `fakeram45_1024x32` SRAM macro was unaffected (~0.6ms either
way) — its density comes from many separate adjacent shapes, not
overlapping rects within one shape. `generate_shapes` is cached
per-`AbstractId`, so this cost is paid once per Abstract switch, not per
frame.

## 2026-08-04 — Renderer::rasterize(): SkPicture -> raw RGBA8888 PixelBuffer

Added the pixel-buffer rasterization step: `Renderer::rasterize()` turns
`build_picture`'s `SkPicture` into a `PixelBuffer` (pointer + width +
height + row_bytes), a third `CachedStage`-backed stage. Two decisions:

- **Explicit `kRGBA_8888_SkColorType`**, not `SkImageInfo::MakeN32Premul`'s
  platform-native `kN32_SkColorType` (BGRA on some platforms, RGBA on
  others) — guarantees identical bytes between the macOS dev machine and
  the Linux target.
- **Y-axis flip applied once per frame as a canvas transform** in
  `rasterize()` (not in `transform_to_pixels`, which stays unflipped) —
  dbu-space y increases upward, Skia's canvas is y-down.

**Real bug found via visual testing, not the unit tests**: a whole-canvas
flip also mirrors glyph rendering — Terminal labels came out
upside-down/mirrored ("VSS" as "SSV") in `render_preview` output. None of
the existing text tests caught this since they only scan for _any_ opaque
pixel, not a legible glyph. Fixed with a local counter-flip
(`save`/`translate`/`scale(1,-1)`/`drawString`/`restore`) around each
label's own anchor in `build_picture`, which couples that `SkPicture` to
being drawn through `rasterize()`'s specific flip for text to render
upright (documented in both methods). A reminder that pixel-level
assertions don't catch every rendering bug — occasionally looking at the
actual image still matters.

| Benchmark                                    | Result      |
| -------------------------------------------- | ----------- |
| `BM_Rasterize` (isolated)                    | 6.21 ms     |
| `BM_Render` (cold, full chain)               | 117 ms      |
| `BM_RenderReused_NoChange`                   | ~0 ms       |
| `BM_RenderReused_PanOnly` (warm, full chain) | **21.5 ms** |

**Headline finding**: the warm/interactive pan-only path went from 12.5ms
to 21.5ms (confirmed via in-session A/B, not just a cross-session
comparison) — the first time this project has _exceeded_ a 60fps/16.6ms
budget rather than stayed under it. Investigated one hypothesis (per-frame
raster-surface allocation) and ruled it out (isolated measurement: 0.25ms,
too small to explain the gap); more likely explanation is that the
isolated benchmark and the reused one aren't quite apples-to-apples (fixed
vs. varying pan value → different surviving shape counts each iteration).
Not chased further — flagged prominently here and in README's reopened
Threading question instead.

## 2026-08-07 — Pipeline::hit_test_point (UPDATES.md 7.1 mouse hover)

Unlike every stage above, `hit_test_point` isn't `CachedStage`-backed —
it's called fresh on _every_ pointer-move event (`le_set_mouse_position`
in api.cpp), against the already viewport-culled/visibility-filtered
shape set `Pipeline::run` produces, not the full 1M-shape design. The
concern this benchmark was written to settle: is bounding the candidate
set to on-screen shapes alone enough, or does per-candidate geometry cost
also need optimizing?

`BM_HitTestPoint` measures a worst-case miss (scans every visible
candidate without ever finding a hit) at the center of the stress scene's
visible viewport (`make_scene`'s own `[0, 100,000,000)` dbu range on each
axis, roughly a quarter of the design's ~250K on-screen candidates after
viewport culling and the M2-layer-hidden visibility filter).

| Version                                                            | `BM_HitTestPoint` |
| ------------------------------------------------------------------ | ----------------- |
| Naive `Geometry::contains` (no bbox pre-check)                     | 16.9 ms           |
| With a cheap bbox pre-check before `bg::within`/`path_to_polygons` | **60.0 µs**       |

Bounding the candidate set to on-screen shapes alone was **not** enough -
16.9ms/call would cap interactive hover responsiveness well below 60fps,
confirmed by direct comparison against `BM_RunReused_PanOnly`'s 9.76ms
(the cost of re-filtering the _entire_ shape set on every viewport
change) elsewhere in this file: hit-testing on every mouse pixel move was
costing _more_ than a full pipeline re-filter that only happens on
pan/zoom. Root cause: `Geometry::contains` called `path_to_polygons`
(a real Boost.Geometry buffer operation) on every visible path-shaped
candidate regardless of whether the query point was anywhere near it -
about a third of the stress design's shapes are paths.

Fix: `Geometry::contains` now checks each polygon/path's own bbox first
(a handful of integer comparisons, reusing the existing private
`bbox_of` helper) and only falls through to `bg::within`/
`path_to_polygons` if that cheap check passes - rects were already this
cheap. 280x faster, comfortably under the frame budget, so the naive
linear-scan design (bounded to on-screen shapes) stands without needing
a spatial index - confirming the plan's "benchmark before optimizing
further" approach rather than building an R-tree speculatively.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_COVERAGE=OFF
cmake --build build --target pipeline_benchmarks
./build/pipeline_benchmarks --benchmark_filter=BM_HitTestPoint --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

## 2026-08-07 — compose_with_overlays: selection-overlay picture replay scaled with selection size

Reported symptom: after a piece-level selection fix (`Pipeline::hit_test_rect`/
`draw_selected_piece_outline` now trace a selected PATH piece's _buffered_
outline via `Geometry::path_to_polygons`, not a cheap centerline halo),
interactive mouse-move/zoom/multi-select got progressively slower the more
objects were selected. First fix attempt split `build_overlay_picture`
(mouse-driven chrome: drag rect/cursor/hover) from a new
`build_selection_overlay_picture` (selected-piece outlines), so a pure
mouse move no longer _re-records_ the selection outline SkPicture. That
fix alone didn't resolve the report - added `BM_ComposeWithOverlays_
ManySelectedPieces_MouseMoveOnly` (selects N real pieces from the stress
design's own filtered output, mixed RECT/POLYGON/PATH, then measures
`compose_with_overlays` cost with only mouse position changing) to find
out why, instead of continuing to reason about it from code alone:

| Selected pieces | Before raster fix | After raster fix |
| --------------- | ----------------- | ---------------- |
| 0               | 1.37 ms           | 2.29 ms          |
| 100             | 1.59 ms           | 2.28 ms          |
| 1,000           | 1.66 ms           | 2.32 ms          |
| 5,000           | 2.56 ms           | 2.30 ms          |
| 20,000          | 6.28 ms           | 2.33 ms          |

Root cause: `compose_with_overlays` _replayed_ `selection_overlay_picture`
(`canvas->drawPicture`) on every call, regardless of whether the picture
had actually been re-recorded. SkPicture replay cost, like recording
cost, scales with the number of recorded draw ops - so even with
recording fixed, every mouse move still cost proportionally to selection
size just from re-executing thousands of already-recorded stroke/path
draw calls. Fix: added `rasterize_selection_overlay_frame`, mirroring the
existing `rasterize_frame` (design picture) pattern - the selection
overlay is now rasterized into its own cached raster image (keyed on
`{viewport_version, visibility_version, selection_version}`, no
`mouse_version`) and `compose_with_overlays` blits that image
(`SkCanvas::drawImage`, cost independent of content complexity) instead
of replaying the picture. After the fix, cost is flat regardless of
selection size (~2.3ms, matching the always-present two-image-blit
baseline) - the ~0.9ms baseline increase from 1.37ms to 2.29ms at zero
selected pieces is the fixed cost of the second full-viewport blit,
always paid now instead of a near-free empty `drawPicture` call.

This is the second time this session a "the picture-cache call count
looks right" test passed while the actual regression (replay cost, not
recompute cost) remained - a reminder that `CachedStage` call-count
assertions only prove _recomputation_ was avoided, not that _replay_ of
whatever's cached is cheap; when what's cached is an `SkPicture` rather
than a flat buffer, replay cost still needs its own reasoning (or
benchmark) before assuming a cache hit means "cheap."

## 2026-08-07 — Scene::select(): O(N²) dedup scan, not the overlay/composite cost above

The `compose_with_overlays` fix directly above did not resolve the user's
reported symptom ("many seconds of delay" moving the mouse after
selecting many shapes) - a clear sign the real cost was somewhere neither
of the two overlay fixes had touched, since both were millisecond-scale
and the report was multi-second. Asked to actually profile rather than
keep guessing: a benchmark exercising the real repro end-to-end
(`le_read_lef` the stress LEF, `le_fit_scene`, then a full-viewport
`le_mouse_down`/`le_mouse_up` drag-select, added as
`BM_RefreshSelectedObjects_ManySelectedPieces`) ran for **7+ minutes**
without completing what should be a sub-second setup step - had to be
killed. Root cause: `Scene::select()`'s dedup (`src/scene/scene.hpp`)
scanned the _entire current selection_, doing a full structural
rect/polygon/path comparison (`same_piece`) against every existing entry
sharing the candidate's origin, on every single insert. The stress-test
LEF's ~900,000 obstruction shapes all share **one** `ObstructionId`
origin (one `OBS` block), which is exactly the worst case for this: every
`select()` call during a big drag-select scans against _every previously
selected piece_, since they all share that one origin and the existing
origin-mismatch short-circuit never fires. `le_mouse_up`'s drag branch
(`src/api/api.cpp`) calls `scene.select()` once per enclosed piece, so a
drag enclosing M pieces cost O(M²) `same_piece` calls - and ran inside the
handle-wide mutex, blocking everything else for however long it took.

Fix: replaced the linear scan with a signature-bucketed index
(`piece_signature()` - a hash consistent with `same_piece`'s existing
equality, `selection_index_` - `std::unordered_multimap<size_t /*sig*/,
size_t /*index*/>`), so dedup only compares against the small bucket of
pieces that could plausibly match, not the whole selection. Isolated
benchmark (`BM_SceneSelect_ManyPiecesSameOrigin` - N distinct pieces
sharing one synthetic origin, timing only the `select()` loop, not LEF
parsing or `hit_test_rect`):

| N pieces | Before (O(N²)) | After (O(N) average) |
| -------- | -------------- | -------------------- |
| 1,000    | 2.38 ms        | 0.073 ms             |
| 5,000    | 58.5 ms        | 0.413 ms             |
| 20,000   | 930 ms         | 1.71 ms              |

544x faster at 20,000 pieces, and the "before" column's growth
(~24.6x time for 5x items, ~15.9x time for 4x items) confirms the
quadratic behavior directly, not just asserted from reading the code.
`items_per_second` after the fix stays flat (~12-13M/s) across all three
sizes, confirming O(1) average per call, not just "faster."

A second, independent, additive cause was found the same investigation:
the Flutter frontend (`frontend/lib/providers/le_provider.dart`)
unconditionally rebuilds its entire selected-object list - several FFI
calls per selected object - on _every_ mouse-move event, regardless of
whether the selection changed. Fixed by exposing `Scene::selection_version()`
through the C API (`le_selection_version`) so the frontend can skip that
rebuild when nothing selection-related has actually changed since the
last check - not benchmarked in isolation the same way (Dart/FFI-side,
not something GoogleBenchmark measures), verified instead by rebuilding
the app and reproducing the original repro.

(A separate incremental-refresh design was attempted on top of this - a
`le_selected_object_signature` per-entry comparison token letting the
frontend diff old vs. new selection instead of always rebuilding fully -
but was reverted: it added real complexity across three layers (C API,
FFI bindings, Dart) without addressing the actual remaining bottleneck,
which turned out to be entirely different - see below.)

## 2026-08-07 — build_picture's dead whole-object selection-outline pass

Even after both fixes above, the reported symptom persisted: selecting
more than a handful of objects was slow, and stayed slow adding just one
more object to an already-large selection - not explained by either
prior fix (`Scene::select()` was already confirmed O(1) average; the
Dart-side refresh gate only helps _unchanged_ frames, not actual
selection changes). Asked directly to profile rather than keep
hypothesizing, and to justify prior claims about where time was going
(none of which had been profiler-verified) - used three independent
methods, escalating in rigor:

1. **Isolated GoogleBenchmark**, `BM_BuildPicture_WithLargeSelection` (a
   selection of N pieces populated once, then `build_picture` called
   repeatedly): 3.95 ms (N=0) -> 12.9 ms (N=1,000) -> 64.5 ms (N=5,000) ->
   64.3 ms (N=20,000, plateaus - ran out of distinct candidates to
   select). ~16x slower at 5,000 selected than 0.
2. **macOS `sample`** (statistical call-stack sampler, no source changes)
   attached to a Release build running that same benchmark: of 6,245
   samples inside `build_picture`'s recompute call, 5,630 (90%) landed
   directly in the lambda's own _inlined_ code, vs. only ~321 total for
   the separately-visible `draw_group` (the actual per-shape drawing
   work) - and 272 samples specifically in `std::variant`'s equality-
   dispatch machinery, the one piece of evidence that survived inlining
   and pointed concretely at `SelectionRef == SelectionRef` comparisons.
3. **Same benchmark, Debug build** (no inlining, so every function keeps
   its own call-tree frame) - removed all ambiguity: `Scene::
is_selected_as_whole_object` (`scene.hpp:552`) accounted for 1,056
   samples in one call-tree branch alone, ~30x more than `draw_group`
   (~35) and ~100x more than `Scene::select` (~9) in the same branch -
   with `find_if.h:24-25` (the linear scan inside it) and
   `scene.hpp:554` (the comparison lambda) as its own direct children.

Root cause: `build_picture`'s whole-object selection-outline pass (drawn
once per _visible_ `PixelShape`, gated on `Scene::is_selected_as_whole_object`

- itself a linear scan of the _entire current selection_) was O(visible
  shapes \* selection size), and reran on every selection change since
  `selection_version()` was part of `build_picture`'s cache key. It was
  also provably dead: `api.cpp`'s `le_mouse_up` only ever calls
  `Scene::select()` with a piece (confirmed by grep, both call sites), so
  `is_selected_as_whole_object` could never return true through the public
  API - this was pure wasted cost with no possible visual effect.

Fix: removed the whole-object pass, `Scene::is_selected_as_whole_object`,
and `draw_selection_outline` entirely (all otherwise unused). Since
`build_picture` no longer has any selection-dependent content at all,
also dropped `selection_version()` from its cache key and
`rasterize_frame`'s (kept in sync by hand, per their documented
invariant) - a selection change no longer invalidates either.

| N pieces selected | Before  | After   |
| ----------------- | ------- | ------- |
| 0                 | 3.95 ms | 4.02 ms |
| 1,000             | 12.9 ms | 4.02 ms |
| 5,000             | 64.5 ms | 3.99 ms |
| 20,000            | 64.3 ms | 4.00 ms |

Flat regardless of selection size. Re-profiled with `sample` after the
fix as a final check, not just re-benchmarked: neither `build_picture`'s
lambda nor any selection/variant-comparison symbol appears anywhere in
the post-fix profile at all - the hot functions are now legitimate
per-shape Skia drawing work (`SkPaint`, `SkPathData`, `SkMatrix`) and
one-time LEF parsing setup, exactly what should be there.

## 2026-08-07 — cmg-generated to_string/to_properties/operator<< took their struct by value

Reported: the slowdown was still reproducible selecting Obstruction
pieces specifically, never Terminal pieces - a real clue, not a red
herring. The user's own hypothesis (too many shapes embedded in _one_
Obstruction, not too many selected pieces) was correct. Root cause:
`cmg` (the schema code generator, `/Volumes/Docking/Projects/synthosilicon/cmg`)
generates `to_string`/`to_properties`/`operator<<` for every schema
class taking the struct _by value_
(`cmg/templates/indexed_pools/struct_hpp_j2.py`). Invisible for small
classes, but `ObstructionData::shapes` (`std::vector<Shape>`) is an
_embedded_ struct field, not pool-referenced like `TerminalData`'s ports
(fetched separately via `Root::get_terminal_ports`) - so every call to
`le::to_properties(*obstruction)` (`api.cpp`'s
`build_selected_object_properties`, called on every property fetch for
any selected Obstruction piece) deep-copied the whole `shapes` vector
just to read a couple of fields.

Isolated benchmark (`BM_ToPropertiesObstructionCopyCost`) against the
real stress-design Obstruction (900K shapes, all under one
`ObstructionId` - the same one this whole investigation has used):

|                                           | Before  | After                                                                    |
| ----------------------------------------- | ------- | ------------------------------------------------------------------------ |
| `to_properties(ObstructionData)` per call | 29.1 ms | sub-microsecond (50M+ iterations in the benchmark's own min-time window) |

Fixed at the generator (not by hand-patching generated/, which must
never be hand-edited): changed the three signatures in `cmg`'s template
to `const T&`, none of the three bodies mutate their own copy. Fixes
_every_ schema class generically, not just Obstruction - any future
class with a large embedded field gets this for free. Regenerated via
the local `cmg` checkout (`poetry run cmg --schema ... --output
src/database/generated --export-style INDEXED_POOLS`, per the
`regen-database` skill) - `src/database/generated/` is gitignored build
output, not committed, so there's no diff to review here beyond
rebuilding/retesting.

`BM_RefreshSelectedObjects_ManySelectedPieces` - added earlier this
session to measure the full `le_selected_object_*` FFI call sequence,
but unable to complete even once until now (killed once at O(N²)
`Scene::select`, silently paying this same 29ms-per-call cost on every
run after that was fixed) - finally completed cleanly: 36.4 ms at
50,996 selected pieces, 88.5 ms at 122,333 - real remaining cost now
proportional to _actual FFI call count_ (kind + properties per selected
object), not a hidden per-call landmine.

## 2026-08-08 — PATH rendering: buffered outline + pattern fill + centerline

Reported: unselected PATH shapes on both Terminals and Obstructions
rendered as solid-colored blocks with no visible fill pattern, unlike
RECT/POLYGON on the same layer. Confirmed visually via `render_preview`
against a small hand-written LEF (PIN/OBS PATH shapes on M1) before
touching any code. Root cause: `draw_group`'s PATH branch drew an
outline-colored "border" stroke at `path.width + 2*kPathOutlineMarginPx`
directly _underneath_ a narrower pattern-shaded "wire" stroke - since a
layer's `outline_color` is also `pattern_shader`'s own tile color, that
border acted as an opaque same-color backing plate showing straight
through every transparent gap in the pattern.

Fix: `RenderedShape` (`pipeline.hpp`) gains `path_outlines` - each
path's buffered outline (`Geometry::path_to_polygons`, flat ends, miter
joins - the same buffering already used for hover/selected-piece
outlines), computed once at `generate_shapes` time. `PixelPath`
(`render.hpp`) carries the transformed result as `buffered_outline`.
`draw_group` now fills/outlines a PATH's `buffered_outline` exactly like
a real POLYGON (pattern fill, thin boundary), plus a new centerline
stroke along the path's own original polygon so it still reads as a
wire. Verified visually (border + BRICK pattern + a clearly visible
centerline, confirmed by rendering a large single path in isolation) and
via new pixel-level regression tests.

**Performance, measured, not assumed** - `Geometry::path_to_polygons`
costs ~768ns/call in isolation (`BM_PathToPolygonsSingleCall`), so
computing it once per path at `generate_shapes` time (cached per-
AbstractId, not per-frame) rather than in `transform_to_pixels` (reruns
every pan/zoom) was the deliberate design choice - confirmed by checking
each stage's own cache key before choosing where this goes, not assumed.

Two real regressions were found via benchmarking that the design above
didn't anticipate, both against a clean "before this session's PATH
work" baseline (`git stash` on `pipeline.hpp`/`render.hpp` only, same
machine/session state, `--benchmark_min_time=8-10s` for a stable
reading - the first attempt at this comparison used the default ~1s
min-time and only 3-8 iterations, which gave wildly unstable numbers
ranging 43-173ms for the same benchmark on the same code; only the
long-running, ~1700+-iteration numbers below should be trusted):

| Benchmark                                    | Before  | After   |
| -------------------------------------------- | ------- | ------- |
| `BM_GenerateShapes` (1M shapes, cold)        | 137 ms  | 434 ms  |
| `BM_RunReused_PanOnly` (pipeline only, warm) | 10.3 ms | 11.7 ms |
| `BM_RenderReused_PanOnly` (full chain, warm) | 39.0 ms | 52.1 ms |

`BM_GenerateShapes`'s +297ms is the expected, accepted cost from point
above (~330K paths × ~768ns + overhead) - paid once per Abstract-load,
within this project's documented cold-start budget (see
`pipeline_latency_budget` memory: 1-2s is fine, spinner shown).
`BM_RunReused_PanOnly`'s +1.4ms is small and was _not_ the concerning
number it first appeared to be: `RenderedShape::path_outlines` was
originally a plain `std::vector<std::vector<Polygon>>`, and
`filter_by_viewport_and_size`/`filter_by_layer_visibility` both already
copy every surviving `RenderedShape` wholesale on every pan/zoom (an
existing pattern, not new) - that plain vector made each such copy
meaningfully heavier, wrapped in `std::shared_ptr<const ...>` instead so
copying a `RenderedShape` costs an O(1) refcount bump regardless of how
much outline geometry a path-heavy `Shape` holds.

`BM_RenderReused_PanOnly`'s +13ms is real and not fully explained by the
pipeline-only number - the difference is in `Renderer`'s own stages
(`transform_to_pixels` now also transforms `buffered_outline` polygons;
`draw_group` now issues a shader-filled polygon draw plus a boundary
stroke plus a centerline stroke per path, instead of two plain strokes).
Not investigated further: this project's render pipeline was _already_
well over its 60fps/16.6ms interactive-frame budget before this change
(39.0ms warm-pan, flagged as an open question in README's Threading
section since 2026-08-04 - see the `Renderer::rasterize()` entry above),
so this doesn't cross a new qualitative threshold, and the underlying
open question (single-threaded rendering) is the same one that would
need solving regardless of this specific fix.

## 2026-08-08 — UPDATES.md item 6: single-pixel dot fallback for sub-pixel shapes

Requested explicitly with a performance number, not just an
implementation: `Pipeline::filter_by_viewport_and_size` already dropped
any shape under 1px in both dimensions at the current scale; zoomed out
far enough on a large design this could silently drop a large fraction
of shapes, making the design look emptier than it is. Added a
completely separate, parallel chain - `TinyShapeDot` /
`tiny_shapes_by_viewport` / `tiny_shapes_by_layer_visibility`
(`pipeline.hpp`), `transform_tiny_shapes_to_pixels` /
`build_tiny_shapes_picture` (`render.hpp`, one batched
`SkCanvas::drawPoints` call per `ViewLayer` group, hairline stroke width
so each point rasterizes as exactly one device pixel) - rather than
touching `filter_by_viewport_and_size`/`build_picture`/`hit_test_point`/
`hit_test_rect` at all, so "not selectable" holds by construction (tiny
dots are a different type hit-testing never sees), not by an added
exclusion check, and every existing selection-critical test stays a
valid regression guard untouched.

All numbers below use `make_zoomed_out_scene` (a new scene builder in
`pipeline_benchmark.cpp`, scale `1e-7` - the whole 1M-shape stress
design's ~200,000um extent fits inside a 2000x2000px viewport, `min_visible_dbu`
10,000um vs. the design's own ~100um max shape size), the deliberate
worst case: virtually all ~1M shapes (900K+) become tiny dots at once.
Per [[stress_lef_not_representative]], real designs don't have anywhere
near this shape density, so treat these as upper bounds, not typical
frame costs. `--benchmark_min_time=8s --benchmark_repetitions=3
--benchmark_report_aggregates_only=true`, medians reported.

Isolated per-stage cost (fresh `Renderer`/one-shot `Pipeline` call per
iteration, matching this file's existing isolated-stage convention):

| Benchmark                                                               | Time    |
| ----------------------------------------------------------------------- | ------- |
| `BM_TinyShapesByViewport` (second pass over `generate_shapes`'s output) | 4.09 ms |
| `BM_TransformTinyShapesToPixels`                                        | 1.44 ms |
| `BM_BuildTinyShapesPicture` (record only)                               | 0.43 ms |

`BM_TinyShapesByViewport`'s 4.09ms is small relative to
`BM_GenerateShapes`'s own 449ms cold cost (same stress design, same
session) - confirms the doc comment's claim that this second pass is
"cheap relative to generate_shapes itself," not just an assumption.

Full-chain warm/pan-only (one `Pipeline`+`Renderer` reused across
iterations, only `pan` changing each call - the interactive-panning
case `BM_RenderReused_PanOnly` already benchmarks at a normal scale):

| Benchmark                                                                                                                     | Time    |
| ----------------------------------------------------------------------------------------------------------------------------- | ------- |
| `BM_RenderReused_PanOnly_ZoomedOut` (design content only, `rasterize()` direct)                                               | 4.40 ms |
| `BM_ComposeWithOverlays_PanOnly_ZoomedOut_NoTinyShapes` (same content, via `compose_with_overlays`, null tiny-shapes picture) | 9.10 ms |
| `BM_RenderReusedWithTinyShapes_PanOnly_ZoomedOut` (real tiny-shapes chain)                                                    | 22.3 ms |

Read as two deltas, not one: `compose_with_overlays`'s own pre-existing
fixed overhead (rasterizing+blitting a second full-viewport surface for
the selection overlay, even when empty) already costs ~4.7ms at this
2000x2000 viewport size, regardless of tiny shapes - `9.10 - 4.40`. The
tiny-shapes feature's own marginal cost on top of that is `22.3 - 9.10 ≈
13.2ms`. The three isolated stages above only account for `4.09 + 1.44 +
0.43 ≈ 6.0ms` of that 13.2ms; the remaining ~7ms is `rasterize_tiny_
shapes_frame` (not independently benchmarkable - it's a private method,
only reachable through the full chain) actually playing back ~900K
points onto a 2000x2000 raster surface plus its own extra `drawImage`
blit in `compose_with_overlays`.

**Verdict**: real, non-trivial cost at this deliberately worst-case
density (900K simultaneous dots), consistent with this project's render
pipeline already being well over its 60fps/16.6ms interactive-frame
budget before this change (see the PATH-rendering entry above - the
same open single-threaded-rendering question applies here, not a new
one this feature introduces). Not optimized further without a
real-design report of this actually mattering - per
[[stress_lef_not_representative]], a real design's largest single-shape
population is closer to hundreds or low thousands, not 900K, so the
realistic per-pan-event cost of this feature is closer to the isolated
per-stage numbers above (a few ms) than the worst-case 13ms figure.
Revisit `rasterize_tiny_shapes_frame` (e.g. drawing dots directly into
`compose_with_overlays`'s own canvas instead of a separate cached raster
surface) if a real design's dot count and pan-frequency ever make this
show up as a reported interactive-lag complaint.

## 2026-08-08 — UPDATES.md item 8: label placement (fracture-into-rects) algorithm

Replaced `Geometry::get_label_location`'s old union+11×11-grid-search
algorithm with a fracture-into-rects one (see `fracture_into_rects`,
`geometry.hpp`): explicit `Shape::rects` are used directly as
candidates; each polygon/buffered-path is sliced into slabs along its
dominant axis (vertical cuts if wider than tall, horizontal otherwise),
each slab intersected against the polygon and approximated by its own
bbox; the largest candidate by area wins, label placed at its center.
Also fixes a latent flaw in the old algorithm for free: its last-resort
fallback could return a point outside the shape entirely (disjoint
geometry with no interior grid sample) - the new algorithm can only
return an off-shape point when the shape has literally no geometry,
since every candidate rect's center is trivially inside that rect.

**First benchmark pass showed a real regression, not assumed fine per
CLAUDE.md's rule**: `BM_GenerateShapes` (1M-shape stress design,
`--benchmark_min_time=8s --benchmark_repetitions=5`, clean `git stash`
of `geometry.hpp` only for the "before" number) went from 430ms to
743ms (+313ms, +73%) - `BM_GetLabelLocationSingleRect` (trivial, no
Boost calls) stayed at ~2ns as expected, but `BM_GetLabelLocationLShapedPolygon`
(a real 3-cut fracture case) cost ~8.2μs/call, and the stress design's
10% Terminal-PIN population includes plenty of POLYGON/PATH-based
labels, not just rects.

**Root-caused with `sample`, not guessed** (per explicit request):
built a Debug `pipeline_benchmarks`, ran `BM_GenerateShapes` in the
background with `--benchmark_min_time=30s`, sampled the running process
for 15s at a 10ms interval (`sample <pid> 15 10 -file ...`). Of 1344
total samples, 523 (39%) were inside `get_label_location`, and 457 of
those (34% of the _entire_ `generate_shapes` cost) were inside one line

- the `bg::intersection` call in `fracture_into_rects`.

Reading that call site with the profile in hand found the fix wasn't a
trade-off: with exactly 2 distinct cuts (one slab), that slab's own
strip is - by construction - identical to the polygon's whole bbox in
both dimensions, so intersecting the polygon against it is _always_ a
no-op (`bg::intersection(polygon, its own bbox) == polygon`, whatever
the polygon's actual shape) and enveloping that gives back the same
bbox already computed at the top. So `fracture_into_rects`'s early-out
was widened from `cuts.size() < 2` to `cuts.size() < 3` - an exact
simplification, not an approximation, and confirmed as such: all 367
tests (including the new fracture/L-shape tests, which specifically
exercise the real 3-cut path) pass byte-identical before and after.

Most real LEF `POLYGON` statements and any straight buffered `Path`
are already exactly their own bbox (2 cuts), so this fast path is the
common case, not an edge case - confirmed by the fix essentially
eliminating the regression entirely:

| Benchmark                                                                          | Before (old algorithm) | After (fracture, unoptimized) | After (fracture, 2-cut fast path) |
| ---------------------------------------------------------------------------------- | ---------------------- | ----------------------------- | --------------------------------- |
| `BM_GenerateShapes`                                                                | 430 ms                 | 743 ms                        | 432 ms                            |
| `BM_GetLabelLocationSingleRect`                                                    | -                      | 2.00 ns                       | 2.00 ns                           |
| `BM_GetLabelLocationLShapedPolygon` (real 3-cut case, unaffected by the fast path) | -                      | 8160 ns                       | 8293 ns                           |

Net: a behaviorally different (and, per the old algorithm's own
disjoint-geometry flaw, strictly more correct) label-placement algorithm
at effectively the same cold-start cost as before.

## 2026-08-08 — Release build: LTO tried, measured no benefit, reverted

`flutter_plugin` was switched to link `backend/build_release` (Release)
instead of `backend/build` (Debug) for the actual runtime library it
embeds (see `macos/layout_engine_plugin.podspec`) - prompted the question of
whether Release was using every reasonable optimization option, not just
CMake's own `-O3 -DNDEBUG` default (confirmed via `CMakeCache.txt` - no
LTO, no CPU-specific tuning, nothing custom beyond that default).

Tried `CMAKE_INTERPROCEDURAL_OPTIMIZATION` (LTO, `check_ipo_supported`-
gated, Release-only) and benchmarked before/after on a clean rebuild
(`--benchmark_min_time=8s --benchmark_repetitions=5`, confirmed `-flto=thin`
actually present in `compile_commands.json`/`link.txt` before trusting
the numbers):

| Benchmark                                     | Before (no LTO) | After (LTO) |
| --------------------------------------------- | --------------- | ----------- |
| `BM_GenerateShapes`                           | 423 ms          | 421 ms      |
| `BM_Render` (full cold chain)                 | 467 ms          | 470 ms      |
| `BM_RenderReused_PanOnly` (warm, interactive) | 53.9 ms         | 54.1 ms     |

All three within noise - no measurable benefit. In hindsight this tracks:
most of this project's hot path (`geometry`/`pipeline`/`scene`/
`view_style`) is already header-only, so a single translation unit
including those headers directly (as every benchmark/caller here does)
already gives `-O3` full visibility into the same code LTO would
otherwise expose across a real library boundary - there's little
cross-TU boundary left in the actual hot paths for LTO to optimize
across. The one real compiled-library boundary on the render path
(`Renderer`'s own methods, `render.cpp`) mostly calls into prebuilt
Skia, which wasn't itself built with LTO, so it's unreachable by this
project's LTO flag either way.

**Reverted** - per this project's own rule, no measured benefit doesn't
justify the added build time. Release stays plain `-O3 -DNDEBUG`. Revisit
if a future profile shows real time inside a genuine cross-library-
boundary call this project's own code owns (not inside Skia), or
consider `-march=native` instead (untried - more likely to show a real
difference for tight numeric loops, at a real portability cost: ties the
binary to the building machine's specific CPU).

## 2026-08-10 — `Shape` pooled (TCL_EXPLORATION.md Phase 3): +~40% on `BM_GenerateShapes`, accepted

`Shape` moved from an embedded-by-value field
(`TerminalPortData::shapes`/`ObstructionData::shapes`, plain
`std::vector<Shape>`) to a pooled, `Root`-addressed class with its own
`ShapeId` and two parent-link fields (`terminal_port`, `obstruction`,
mutually exclusive), needed so a Tcl command can update one existing
shape (including its layer) by a stable id independent of its parent -
impossible to address that way while shapes were anonymous vector
elements. Flagged as a hot-path risk before starting (this project's own
rule: benchmark a change like this, don't assume it's free) -
`Pipeline::generate_shapes`'s two shape-collecting loops went from
iterating an embedded `std::vector<Shape>` directly (contiguous, no
indirection) to `Root::get_terminal_port_shapes(id)`/
`get_obstruction_shapes(id)` (an index lookup returning `vector<ShapeId>`)
followed by one `Root::get_shape(id)` pool lookup per shape.

| Benchmark                       | Before (embedded `Shape`) | After (pooled `Shape`)       |
| ------------------------------- | ------------------------- | ---------------------------- |
| `BM_GenerateShapes` (1M shapes) | 423 ms                    | 590 ms (mean of 5, cv 1.52%) |

A real, reproducible ~40% regression (+167 ms) on the full 1M-shape
stress design, not noise - confirms the concern was justified, not
hypothetical. **Accepted for now**: this is the cost of a capability that
was explicitly requested (stable-id shape addressing, not previously
possible at all) and `generate_shapes`'s own result is already cached
per-`AbstractId` (see `pipeline.hpp`'s own `CachedStage` comments) - this
cost is paid once per structural change to a Design, not per frame/pan/
zoom, which is where interactive responsiveness actually lives. One
partly offsetting effect measured in the same session:
`BM_ToPropertiesObstructionCopyCost` (a _different_ hot path, `api.cpp`'s
`build_selected_object_properties`) went from a fixed cost to reading
`ObstructionData` (now just one `AbstractId`, no embedded shapes vector
at all) - previously already fixed to be cheap by passing structs by
reference (see the 2026-08-07 entry above), now structurally cannot
regress back to that bug for this field, since there's no `shapes` field
left to accidentally copy.

Not optimized further in this pass - no profiling done yet to find
exactly where the extra time goes (index lookup itself vs. pointer-chasing
vs. lost cache locality from shapes no longer being contiguous in their
parent's memory). Revisit with a profiler if `generate_shapes`'s cold cost
becomes a real interactive-latency complaint, per this project's own
"benchmark before optimizing" rule - not preemptively.

## 2026-08-12 — Pipeline stage classes + VersionedStage (UPDATES.md item 16)

Confirming no regression from splitting `Pipeline`'s 5 stages into their
own classes (`GenerateShapesStage`, `FilterByViewportAndSizeStage`, etc.,
each wrapping a `VersionedStage` - the renamed, version-tracking
`CachedStage`) and switching downstream cache keys to compose via an
upstream stage's own `version()` instead of manually re-deriving its
triggers - the fix for the caching-bug _class_ the 2026-08-10 `mutation_version()`
fix (see TCL_EXPLORATION.md) needed 9 hand-touched cache keys to patch.

True A/B via `git stash` (pipeline.hpp/pipeline_test.cpp only, same
machine/session, same 1M-shape stress design):

| Benchmark                       | Before (one class, hand-written keys) | After (per-stage classes, version() composition) |
| ------------------------------- | ------------------------------------- | ------------------------------------------------ |
| `BM_GenerateShapes` (1M shapes) | 652 ms (mean of 3, cv 5.61%)          | 644 ms (mean of 3, cv 7.47%)                     |
| `BM_FilterByViewportAndSize`    | 18.1 ms (cv 0.68%)                    | 18.1 ms (cv 1.08%)                               |
| `BM_FilterByLayerVisibility`    | 4.58 ms (cv 1.26%)                    | 4.63 ms (cv 2.60%)                               |
| `BM_Run` (cold)                 | 595 ms (cv 0.29%)                     | 590 ms (cv 0.81%)                                |

Statistically indistinguishable - every "after" number falls inside the
"before" number's own run-to-run noise band, in both directions. Both
sets are elevated versus this file's own historical baselines (e.g.
~430-449 ms `BM_GenerateShapes` from earlier 2026-08 sessions) - a
session-wide effect (background system load, not this change: the _before_
number is equally elevated) rather than a regression, confirmed by
comparing before/after on the same loaded machine rather than against an
older session's numbers.

`BM_RunReused_NoChange` initially looked alarming (~600 ms/call, i.e. as
expensive as a cold `BM_GenerateShapes`) until checking the raw
(non-aggregated) output: `Iterations = 1` - Google Benchmark's
auto-tuner stopped after one call because it already exceeded the default
minimum measurement time, so the reported number was purely the cold
first call, never reaching a genuine cached-steady-state measurement at
all (true for _any_ implementation under this benchmark's existing
design, not something this refactor introduced). Forcing more iterations
(`--benchmark_min_time=6s`) confirms the cache genuinely works:
2,185,791,604 iterations at 0.000 ms/call.

## 2026-08-12 — Renderer stage classes + version() composition, "compose everywhere" (UPDATES.md item 16)

Same refactor as the Pipeline entry above, applied to `Renderer`'s 10
`CachedStage`-backed methods. Two differences from Pipeline's own version:

1. **8 stage classes, not 10**: `rasterize_frame`/`rasterize_tiny_shapes_frame`/
   `rasterize_selection_overlay_frame` had identical bodies (build an
   RGBA8888 surface, Y-flip, `drawPicture`, `peekPixels`) - collapsed into
   one generic `RasterizeStage`, instantiated three times, rather than
   three copy-pasted classes.
2. **"Compose everywhere" required rewriting 2 tests**, unlike Pipeline
   (where composing turned out to need zero test changes once checked).
   Several Renderer methods take an already-built value (`pixel_shapes`,
   an `SkPicture`) as an explicit parameter, and two tests
   (`BuildPictureReusesCacheUntilVisibilityVersionChanges`,
   `RasterizeReusesCacheUntilViewportVersionChanges`) deliberately reused a
   stale artifact without re-running the upstream stage, to prove
   self-sufficient staleness detection - a guarantee added specifically
   because trusting upstream freshness had already caused two real bugs
   (see `rasterize_frame`'s pre-refactor doc comment, `git log`). Chose to
   compose anyway (same call as Pipeline's own "update the tests too");
   both tests were rewritten to re-run the real chain instead of reusing a
   stale value, keeping their original assertions/semantics. Traced every
   real call site (`api.cpp`, `render_preview.cpp`, every relevant
   benchmark) first to confirm none of them actually rely on the dropped
   guarantee - see the plan file / commit message for the full writeup.

True A/B via `git stash` (render.cpp/render.hpp/render_test.cpp + the new
draw_helpers.hpp/pixel_types.hpp/stages/ files, same machine/session, same
1M-shape stress design), Release build, `--benchmark_repetitions=5
--benchmark_report_aggregates_only=true`:

| Benchmark                                                                            | Before (one class, hand-written keys) | After (8 stage classes, version() composition) |
| ------------------------------------------------------------------------------------ | ------------------------------------- | ---------------------------------------------- |
| `BM_Rasterize`                                                                       | 29.9 ms (cv 2.07%)                    | 29.2 ms (cv 0.40%)                             |
| `BM_Render` (cold, full chain)                                                       | 623 ms (cv 0.84%)                     | 622 ms (cv 0.35%)                              |
| `BM_RenderReused_NoChange`                                                           | 575 ms (cv 2.37%)                     | 570 ms (cv 0.32%)                              |
| `BM_RenderReused_PanOnly`                                                            | 569 ms (cv 0.62%)                     | 578 ms (cv 0.82%)                              |
| `BM_RenderReused_PanOnly_ZoomedOut`                                                  | 534 ms (cv 0.75%)                     | 537 ms (cv 0.87%)                              |
| `BM_RenderReusedWithTinyShapes_PanOnly_ZoomedOut`                                    | 565 ms (cv 0.18%)                     | 570 ms (cv 0.78%)                              |
| `BM_ComposeWithOverlays_PanOnly_ZoomedOut_NoTinyShapes`                              | 550 ms (cv 1.79%)                     | 544 ms (cv 0.99%)                              |
| `BM_ComposeWithOverlays_ManySelectedPieces_MouseMoveOnly` (0/100/1k/5k/20k selected) | 3.35/3.29/3.36/3.34/3.47 ms           | 3.27/3.26/3.29/3.33/3.42 ms                    |

Statistically indistinguishable across the board - every "after" number
falls inside the "before" number's own run-to-run noise band. Specifically
confirms the one real behavior change from this refactor -
`ComposeWithOverlaysStage` now calls its three `RasterizeStage` upstreams
unconditionally (to read a guaranteed-current `.version()`) instead of only
inside a cache-miss lambda, so a truly-nothing-changed repeat call now
does 3 extra cheap (already-cached, O(1) tuple-compare) calls it didn't do
before - `BM_RenderReused_NoChange` and
`BM_ComposeWithOverlays_ManySelectedPieces_MouseMoveOnly` are exactly the
benchmarks that would show this, and both stayed flat.

## 2026-08-14 — Shape-level selection (Property Viewer database-hierarchy redesign)

`Scene::SelectedObject` simplified from `{SelectionRef origin;
std::optional<Shape> piece;}` (dedup via `piece_signature`/`same_piece`, a
geometry-hash-bucketed comparison - see the 2026-08-07 entry above) to a
bare `{ShapeId shape_id;}`, dedup via a plain `std::unordered_set<ShapeId>`
- selection identity is now just "which ShapeId", no geometry comparison
at all. Release build, `--benchmark_repetitions=3
--benchmark_report_aggregates_only=true`, same 1M-shape stress design:

| N pieces | Before (geometry-hash bucket, 2026-08-07) | After (plain ShapeId set) |
| -------- | ------------------------------------------ | -------------------------- |
| 1,000    | 0.073 ms                                    | 0.027 ms                   |
| 5,000    | 0.413 ms                                    | 0.147 ms                   |
| 20,000   | 1.71 ms                                     | 0.703 ms                   |

~2.4-2.8x faster, as expected - a plain hash-set insert has no geometry to
hash or compare against a bucket. `BM_ComposeWithOverlays_ManySelectedPieces_MouseMoveOnly`
(0/100/1k/5k/20k selected) measured flat at 4.21/4.22/4.23/4.27/4.39 ms
this run (absolute level shifted vs. the 3.2-3.5 ms in the 2026-08 table
above, attributed to machine load at measurement time, not a regression -
same machine/design either way; what matters, and held, is that it stays
flat across selection size, unchanged from before). `BM_BuildSelectionOverlayPicture_ManySelectedPieces`
(100/1k/5k/20k selected, no prior baseline recorded) measured 0.84/1.75/5.34/20.1 ms -
this stage now looks up each selected `ShapeId`'s geometry from the
Pipeline's own generated `shapes` map (a `ShapeId -> RenderedShape*` hash
map built once per recompute) instead of reading a `Shape` copy stored
directly on `Scene`; not compared against a "before" number since the
lookup structure itself is new, but scales linearly with selection size
as expected, no quadratic behavior reintroduced.

## 2026-08-19 — Removed shape-merging (`Geometry::merge_overlapping_fills`)

Reverted the 2026-08-04 entry above: `merge_overlapping_fills` unioned a
Shape's own overlapping rects/polygons into a minimal non-overlapping
polygon set purely to avoid double-blended translucent fill on overlap.
Removed at the user's request - it made the rendered picture diverge from
the database's actual stored structure (rect count, kind, and index),
which became actively confusing once selection/Move became piece-granular
(UPDATES.md item 21's follow-up): a click on one raw rect could report a
"merged" polygon with no addressable index back into `Root`'s own
`rects`/`polygons`, and the render no longer matched what editing a piece
actually changed. Selection/Move already re-derive pieces against `Root`'s
raw geometry rather than trusting Pipeline's rendered output (see
`HoverTarget`'s own comment in scene.hpp) specifically because that
divergence is real for other reasons too (RECT/POLYGON/PATH ITERATE
expansion still adds render-only pieces past the raw indices) - so this
removal is a correctness/clarity win with no follow-up code change needed
there. No fill-overlap regression measured on the 1M-shape stress design
(same reasoning as the original entry: real designs rarely have deeply
overlapping same-Shape rects) or on `Nangate45_stdcell.lef`; a real
overlapping-pin macro will now show a visibly darker double-blended patch
again, an accepted, deliberate tradeoff.

## 2026-08-23 — Migration Step 3 Phase D: 1,000,000-instance hierarchical rendering

A different fixture from the 1M-*shape* one above: `src/pipeline/benchmarks/layout_stress_data.hpp`,
built directly via `Root::create_*` calls (not a DEF-text round trip - far
cheaper to construct at this scale). Matches the user's own worked
example exactly - a "sub-block" `Design` with a `Layout` containing
1,000,000 `Placement`s of a small fixed "leaf" cell (10x10um, one M1
obstruction), then a "top-level" `Design` with a `Layout` containing 4
`Placement`s of that sub-block, one per `Orientation`. Fixture setup
(building all 1,000,000 `Placement` rows) took 0.23-0.24s, not itself
part of any measured benchmark. Release build,
`--benchmark_repetitions=5 --benchmark_report_aggregates_only=true`:

| Benchmark | Mean | cv |
| --- | --- | --- |
| `BM_InstanceRenderer_BuildLayoutPicture_ColdCache_FullDepth` (picture-resolution only, no compose/rasterize, 4,000,000 leaf instances) | 87.9 ms | 2.31% |
| `BM_InstanceRenderer_RenderLayoutFrame_ColdCache_FullDepth` (full frame, first-ever render, 4,000,000 leaf instances) | 262 ms | 1.11% |
| `BM_InstanceRenderer_RenderLayoutFrame_WarmCache_FullDepth` (full frame, unchanged Scene, every cache a hit) | ~0.000 ms | 1.29% |
| `BM_InstanceRenderer_RenderLayoutFrame_ColdCache_ShallowDepth` (`hierarchy_depth=0` - every top-level placement falls back to the sub-block's own empty Abstract, no leaf recursion at all) | 6.00 ms | 3.00% |

This is the real verification `PROJECT_MIGRATION.md`'s Step 3 asked
for: picture-level caching (`InstanceRenderer`, not per-instance shape
materialization into the `RenderedShape` map) delivers roughly
constant-per-instance cost rather than scaling with the sub-block's own
internal shape count. Concretely: cold-resolving all 4,000,000 leaf
instances into cached `SkPicture`s costs ~88ms (~22ns/instance) - the
leaf's own M1 geometry is walked once (per distinct resolved picture, not
per placement) and replayed via cheap `canvas->concat()`+`drawPicture()`
dispatch thereafter; the further ~174ms in the full cold-frame number is
Skia's own per-frame rasterization walk of those 4,000,000 nested
`drawPicture` calls (expected, unavoidable per-frame cost, not a
caching gap - `RasterizeStage`'s own instance is a genuine cache hit
after the first call, hence the warm-cache number reading as ~0). The
~6ms shallow-depth floor (only 4 placements resolved, no leaf recursion
at all) is the fixed per-call cost of viewport-sized surface allocation
+ grid drawing + compose, present regardless of depth - the real
comparison point the full-depth numbers above are measured against,
confirming the ~262ms cold / ~0ms warm split is genuinely about
instance-count-driven caching, not merely "the shallow case does
nothing." Per-instance-culling (skipping an off-screen instance's own
`concat`+`drawPicture` call before it reaches Skia's own quickReject) was
deliberately not added in this phase - these numbers are the ones to
revisit before deciding whether it's actually needed.

## 2026-08-30 — HierarchyResolver rewrite onto MemoizingStage + a dynamic tbb::flow::graph

Same fixture as Phase D above (`layout_stress_data.hpp`, unmodified),
same `BM_HierarchyResolver_*` benchmark names (renamed from
`BM_InstanceRenderer_*` when `HierarchyResolver` replaced
`InstanceRenderer` - the numbers above remain the correct pre-migration
baseline for this comparison). Release build, `--benchmark_repetitions=5
--benchmark_report_aggregates_only=true`:

| Benchmark | Mean | cv | vs. Phase D baseline |
| --- | --- | --- | --- |
| `BM_HierarchyResolver_BuildLayoutPicture_ColdCache_FullDepth` | 41.0 ms | 1.64% | **~2.1x faster** (was 87.9 ms) |
| `BM_HierarchyResolver_RenderLayoutFrame_ColdCache_FullDepth` | 483-486 ms | 1.11% (time), 21-30% (CPU/items - noisy, see below) | **~1.8-1.9x slower** (was 262 ms) |
| `BM_HierarchyResolver_RenderLayoutFrame_WarmCache_FullDepth` | 0.003 ms | 1.44% | flat (was ~0.000 ms) |
| `BM_HierarchyResolver_RenderLayoutFrame_ColdCache_ShallowDepth` | 6.12-6.18 ms | 3.74% | flat (was 6.00 ms) |

This is a genuinely mixed result, reported as measured rather than
smoothed into a single "faster"/"slower" story:

- **`BuildLayoutPicture` cold-resolve got faster**, not flat-or-worse as
  anticipated going in (see the design writeup this entry accompanies).
  This fixture has only 3 distinct `NodeKey`s (the sub-block's `Layout`,
  its leaf `Abstract`, degenerate top-level dispatch) despite 4,000,000
  leaf placements, so there's no real opportunity for the new design's
  concurrent-node execution to help here - the win looks like it comes
  from the discovery pass itself: a plain `graph_->nodes.find(key)` map
  lookup per already-seen placement is cheaper than the old design's
  per-call `SynchronousStageRunner` construction inside the recursive
  walk. Not confirmed by profiling - a plausible read of the numbers,
  not a verified root cause.
- **`RenderLayoutFrame`'s cold-full-depth case got measurably slower.**
  The per-call bookkeeping this rewrite adds on top of `BuildLayoutPicture`'s
  own path - `graph_->current_generation` bumped on every top-level
  call, `discover_layout_children` walking the top layout's own
  placements a second time (once for `BuildLayoutPicture`'s own nested
  resolution during a prior warm-up-adjacent call in some benchmark
  variants, again for `render_layout_frame`'s own top-level picture),
  and `sweep_stale_nodes()`'s O(node count) scan after every call - is
  the leading suspect, but this has **not been root-caused by profiling
  in this pass**; flagged as follow-up work rather than papered over.
  The CPU/items-per-second figures for this one case carry high variance
  (21-30% cv) that the wall-clock time figures (1.11% cv) don't share -
  consistent with background system load during this run (a `Load
  Average` of 4+ was observed), not instability in the measurement
  itself; re-run in a quieter environment before treating the exact
  1.8-1.9x figure as precise.
- Warm-cache and shallow-depth numbers are unchanged within noise, as
  expected - neither path exercises the new discovery/graph machinery
  differently from the old recursive one.

**Deferred**: the design writeup for this rewrite calls for a second,
"wide" stress fixture (many distinct designs, e.g. 10,000 distinct leaf
types x 100 placements each) specifically to exercise the new design's
per-distinct-node concurrency, since this existing fixture structurally
can't (only 3 live nodes, ever). That fixture, plus a benchmark
repeatedly toggling `hierarchy_depth` to measure `sweep_stale_nodes`'s
own scan cost, were not built in this pass - the numbers above are
useful as a same-fixture before/after comparison, but say nothing yet
about the new design's actual payoff case or pruning overhead at scale.

## 2026-08-30 — TiledRasterizePictureStage: row-band-parallel rasterize for RasterizeComposePipeline's design slot

Tracy identified `RasterizeComposePipeline`'s `design_rasterize_` step as
the actual bottleneck. A per-`ViewLayerId` content split
(`BuildParallelDesignPictureStage`/`MultipleRasterizePictureStage`/
`ComposePictures`, from this same session's earlier `DesignRenderPipeline`
work) was tried and removed - it only ever reached the Abstract-view path,
since a Layout view's `design_picture` is one already-composited
`SkPicture` with no per-layer structure left to split by the time it
reaches a rasterize stage. `TiledRasterizePictureStage` splits the
*output* space instead (row-band tiles, each a separate `SkSurface` via
`SkSurfaces::WrapPixels` aliasing a disjoint row-range of the same backing
buffer - no merge/blit step needed afterward), which works on any single
`sk_sp<SkPicture>` regardless of how its content is organized, so both
view paths benefit. Same `sk_sp<SkPicture> -> RasterizedFrame` contract as
`RasterizePictureStage`, used only for `design_rasterize_` (the
content-heavy slot); `tiny_rasterize_`/`selection_rasterize_`/
`ruler_rasterize_` stay on plain `RasterizePictureStage`, where per-tile
overhead would outweigh the benefit on much lighter content.

Release build, A/B via a temporary revert of `design_rasterize_`'s own
type (`TiledRasterizePictureStage` vs. plain `RasterizePictureStage`,
everything else unchanged), `--benchmark_repetitions=5` (8 for the
noisiest case). System was under heavy, fluctuating background load
during this run (`Load Average` observed between ~7 and ~33 across
different invocations) - flagged per-row below rather than smoothed over,
since it visibly affects which numbers are trustworthy.

| Benchmark | Untiled | Tiled | Δ | Load during run |
| --- | --- | --- | --- | --- |
| `BM_RenderReused_PanOnly` (full 1M-shape content, real per-tick recompute) | 5509 ms (cv 11.76%) | 2460 ms (cv 7.56%) | **~55% faster** | ~8 → ~22 |
| `BM_HierarchyResolver_RenderLayoutFrame_ColdCache_FullDepth` (4M-placement Layout fixture) | 491 ms (cv 1.48%) | 418 ms (cv 2.91%) | **~15% faster** | ~7-9 (quiet) |
| `BM_HierarchyResolver_RenderLayoutFrame_ColdCache_ShallowDepth` | 7.17 ms (cv 18.09%) | 6.17 ms (cv 0.90%) | ~14% faster, low confidence | ~7-9 (quiet) |
| `BM_ComposeWithOverlays_PanOnly_ZoomedOut_NoTinyShapes` | 2104 ms (cv 7.58%) | 2080 ms (cv 4.89%) | flat, within noise | ~7-9 (quiet) |
| `BM_RenderReused_NoChange` (full content, no pan - mostly cache hits) | 7045 ms (cv 22.08%) | 5712 ms (cv 22.19%) | inconclusive - both too noisy | ~22 → ~33 |

- **The two reliable, load-quiet results (`PanOnly` and Layout `ColdCache_FullDepth`) both show a real win**, and both go through
  `design_rasterize_` with genuinely dense, mostly-uncullled content -
  the case this change targets. `PanOnly`'s ~55% dwarfs Layout's ~15%
  because `PanOnly` isolates the design-picture-build-and-rasterize path
  almost entirely (`FrameRenderPipeline::run()`'s own overhead is small
  next to a 1M-shape rasterize), while `RenderLayoutFrame` pays a lot of
  other cost per call (hierarchy discovery, node touch-propagation,
  `sweep_stale_nodes`) that tiling doesn't touch - see the entry above
  this one for that path's own overhead breakdown.
- **`BM_RenderReused_PanOnly_ZoomedOut` was deliberately excluded from
  this table** - after this session's earlier `DesignRenderPipeline`
  refactor, that benchmark calls `DesignRenderPipeline::run()` directly,
  which no longer rasterizes at all (`RasterizeComposePipeline` owns that
  now) - it doesn't exercise this change in either direction, and an
  earlier draft of this comparison mistakenly read its ~12% run-to-run
  delta as a regression before catching that it isn't wired to
  `design_rasterize_` any more.
- **The `_ZoomedOut` fixture that *does* reach `design_rasterize_`
  (`ComposeWithOverlays_..._NoTinyShapes`) shows no meaningful change.**
  That fixture is deliberately scaled so nearly every shape is tiny and
  dropped from `BuildDesignPictureStage`'s own output (see that
  benchmark's own comment) - there's very little picture content left to
  rasterize by the time it reaches this stage, so tiling has little room
  to help and its own small fixed overhead (N `SkSurface`/`SkCanvas`
  objects, N `parallel_for` task dispatches) roughly cancels out. Tiling
  is a real win specifically on dense content, not a universal one.
- `NoChange`/`ShallowDepth` are flagged low-confidence/inconclusive
  rather than reported as clean deltas - both ran under visibly higher
  background load (`NoChange`) or already carried high baseline variance
  before this change (`ShallowDepth`, 18% cv on its own untiled number).
  Worth re-running in a quieter environment before trusting the exact
  percentages, though the direction (faster) is consistent with every
  other result here.

Kept the change: the two load-quiet, architecturally-relevant benchmarks
both show a clear win with no correctness regressions (full 633-test
suite passes unchanged). No `SkRTreeFactory`/BBH was added to the picture
recorders in this pass (each tile's `drawPicture` still walks every
recorded op, not just the ones intersecting its own row range) - a
plausible next increment, deferred so this change stays isolated to one
variable per this project's own benchmarking convention.

## 2026-08-30 — draw_group: hairline simplification for shapes sub-pixel in exactly one dimension

`ViewportFilterStage` already drops shapes sub-pixel in *both* dimensions
(in favor of the tiny-shape-dot mechanism), but deliberately keeps a shape
sub-pixel in only *one* dimension - "a long thin wire survives even if its
width alone is sub-pixel" (that stage's own doc comment). Real routing
wires are exactly this population, and `draw_group` used to rasterize
them at full fidelity regardless: a rect got a full AA fill + outline
pass, and a path got its buffered_outline (a real `Geometry::buffer()`
polygon) filled, outlined, *and* a redundant centerline stroke on top -
three draws that all collapse to "a barely-there thin line" once the
declared width is sub-pixel. Changed `draw_group` to draw a single
hairline (`SkPaint`'s 0-width stroke - exactly 1 device pixel regardless
of the current transform) along the long axis/centerline instead, for
both the rect and path cases; `transform_shapes_to_pixel_space` also now
skips transforming a sub-pixel-width path's `buffered_outline` points to
pixel space at all, since `draw_group` will never read them for that path.

**Not exercisable by the existing 1M-shape stress fixture** -
`stress_data.hpp`'s `item_geometry` deliberately uses the same `size_um`
for both a shape's length and width, so a shape there is only ever "both
dimensions sub-pixel" (culled upstream) or "both dimensions visible" -
never the single-dimension case this change targets. Added a small,
purpose-built fixture instead (`thin_wire_group` in
`pipeline_benchmark.cpp`) - 200,000 shapes with a fixed sub-pixel width
(0.3px) and length spanning many pixels (20-40px), calling `draw_group`
directly (the one function this change touches) rather than through a
whole pipeline. A/B via a temporary `git stash` of `draw_helpers.hpp`
alone (benchmark harness unchanged in both runs). Release build,
`--benchmark_repetitions=8 --benchmark_report_aggregates_only=true`,
quiet system (`Load Average` ~3.3-3.5 throughout, both runs):

| Benchmark | Before | After | Δ |
| --- | --- | --- | --- |
| `BM_DrawGroup_ThinRects` (200K sub-pixel-width rects) | 70.6 ms (cv 1.37%) | 31.0 ms (cv 0.81%) | **~56% faster** (2.28x) |
| `BM_DrawGroup_ThinPaths` (200K sub-pixel-width paths) | 207 ms (cv 1.38%) | 62.9 ms (cv 1.93%) | **~70% faster** (3.29x) |

The path case wins more than the rect case, as expected - a rect
collapses 2 draws (fill + outline) into 1 (a line), while a path
collapses 3 (buffered-outline fill, buffered-outline stroke, centerline
stroke - each over real polygon geometry, not just a rect) into the same
1. Both numbers are clean (cv well under 2%, consistent with the quiet
system this ran on, unlike some of this session's earlier noisy runs).

**Real-world impact depends on how much of this population an actual
design has** - this synthetic fixture is deliberately 100% long-thin-wire
content to isolate the mechanism, not representative of a real design's
own mix. The user has a real Tracy-profiled fixture for this
(`frontend/tcl/aes_5x5.tcl`, run via
`frontend/scripts/profile_tcl_script.sh` against the live app) that would
give the actual real-design number; not run in this pass (would need a
full Flutter app build + GUI session unavailable in this environment) -
deferred to the user to run themselves, or a follow-up session with GUI
access. Correctness confirmed by 6 new direct `draw_group` unit tests
(`draw_helpers_test.cpp`) plus the full 639-test suite passing unchanged.
A real bug was caught while writing those tests: the path hairline
branch's first draft passed `fill` (a `kFill_Style` paint) to
`SkCanvas::drawPath`, which - unlike `drawLine`, whose own doc says
"Style is ignored, as if kStroke_Style" - honors Style, so it silently
filled a degenerate 2-point open path (drawing nothing) instead of
stroking it whenever a layer had a fill color. Fixed by copying `stroke`
(already the right style/width/dash-effect) and only swapping its color.

**Real-design confirmation** (self-reported by the user, not independently
captured/CSV-verified in this pass - see the caveat above about GUI/Tracy
access): `aes_5x5.tcl` (`hierarchy_depth=2`, `zoom -factor 0.3`) against
the live app, cold-path single frame render, **both this session's
changes together** (`TiledRasterizePictureStage` above + this hairline
simplification, not isolated per-change) - **5 s -> 1.47 s (~3.4x
faster)**. Directionally consistent with the synthetic numbers on both
entries; the exact split between the two changes' own contributions on
this specific design isn't known, since they weren't measured separately
against this fixture.
