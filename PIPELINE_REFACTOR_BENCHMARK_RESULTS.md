
Commit: 12bd236a4e8399eede4f4722e59d9928fdecd639

| Pipeline | Stage           | 1x1     | 2x1     | 2x2     | 3x2     | 3x3     | Comments                          |
| -------- | --------------- | ------- | ------- | ------- | ------- | ------- | --------------------------------- |
| Cold     | LayerGeneration | 79.7 us | 79.2 us | 79.0 us | 78.6 us | 79.4 us | Flat/O(1) across all 5 tile sizes |

Commit: e61a8ab

| Pipeline | Stage             | 1x1    | 2x1    | 2x2    | 3x2    | 3x3     | Comments                                |
| -------- | ----------------- | ------ | ------ | ------ | ------ | ------- | --------------------------------------- |
| Cold     | HierarchyResolver | 104 ms | 216 ms | 554 ms | 829 ms | 1202 ms | Scales with design size, roughly linear |

Commit: 6e05291

| Pipeline | Stage             | 1x1    | 2x1    | 2x2    | 3x2     | 3x3     | Comments                                     |
| -------- | ----------------- | ------ | ------ | ------ | ------- | ------- | -------------------------------------------- |
| Cold     | HierarchyResolver | 156 ms | 289 ms | 762 ms | 1187 ms | 2017 ms | +PLACEMENT_BOUNDARY per-placement label cost |

Commit: bcf6293

| Pipeline | Stage             | 1x1    | 2x1    | 2x2    | 3x2    | 3x3     | Comments                                         |
| -------- | ----------------- | ------ | ------ | ------ | ------ | ------- | ------------------------------------------------ |
| Cold     | HierarchyResolver | 121 ms | 262 ms | 609 ms | 996 ms | 1571 ms | Batched PLACEMENT_BOUNDARY shapes, ~9-22% faster |

Commit: 997e943

| Pipeline | Stage             | 1x1    | 2x1    | 2x2    | 3x2    | 3x3     | Comments                                                             |
| -------- | ----------------- | ------ | ------ | ------ | ------ | ------- | -------------------------------------------------------------------- |
| Cold     | HierarchyResolver | 124 ms | 246 ms | 588 ms | 982 ms | 1463 ms | reserve()/batching everywhere else; 3x2/3x3 cv ~20-30%, within noise |

Commit: abc3013

1M-component target validation (test_data/aes_scaling_5x5.def, 1,033,600 components) - not part of the 5-point matrix above, run in isolation via `--benchmark_filter=5x5` so peak RSS reflects one design, not every cached fixture:

| Pipeline | Stage             | Time      | Peak RSS | Comments                                                                                                                   |
| -------- | ----------------- | --------- | -------- | -------------------------------------------------------------------------------------------------------------------------- |
| Cold     | LayerGeneration   | 79.7 us   | -        | Still flat/O(1) at ~1M components                                                                                          |
| Cold     | HierarchyResolver | 5.9-6.2 s | ~6.6 GB  | Exceeds the 5s/1M-component target (repeatable across 2 runs); peak RSS is whole-process (DEF parse into Root + compute()) |

Commit: f385beb

Same 5x5 point, now measured via the built-in PeakRSS_MB counter (`--benchmark_filter=5x5`) instead of manually wrapping with `/usr/bin/time -v` - confirms the earlier manual reading, no logic changes this commit (benchmark reporting infrastructure only):

| Pipeline | Stage             | Time    | Peak RSS | Comments                                                                                               |
| -------- | ----------------- | ------- | -------- | ------------------------------------------------------------------------------------------------------ |
| Cold     | LayerGeneration   | 80.8 us | 2.27 GB  | Peak RSS here is mostly just the DEF parse into Root - LayerGeneration itself never touches placements |
| Cold     | HierarchyResolver | 6.55 s  | 6.53 GB  | Consistent with the manual /usr/bin/time reading (~6.6-6.7GB)                                          |

Commit: f5abfd9

MemoizingStage now caches OutputData as a shared_ptr instead of a value (root cause of the above: execute() was deep-copying the full cached result on every call, cache hit or miss). Full matrix + 5x5, isolated per point via `--benchmark_filter`:

| Pipeline | Stage             | 1x1     | 2x1     | 2x2    | 3x2    | 3x3    | 5x5    | 5x5 Peak RSS | Comments |
| -------- | ----------------- | ------- | ------- | ------ | ------ | ------ | ------ | ------------ | -------- |
| Cold     | LayerGeneration   | 45.0 us | 44.5 us | 43.2 us | 44.3 us | 43.0 us | 45.8 us | 4.09 GB (cumulative, full suite) | ~45% faster than before too - was paying a smaller version of the same copy cost |
| Cold     | HierarchyResolver | 50.2 ms | 91.2 ms | 202 ms | 299 ms | 552 ms | 2.1 s  | ~3.6 GB (isolated) | 2-4x faster at every point; 5x5 now comfortably meets the 5s/1M-component target (was ~6.5-8.9s/~6.5GB) |

Commit: 6478286

Warm stage 1 (ViewportCullStage) - viewport is the central half of the design's own declared bbox by each axis; "zoom always re-computes" case (fresh runner per iteration, same convention as HierarchyResolver's own):

| Pipeline | Stage        | 1x1     | 2x1     | 2x2     | 3x2    | 3x3    | 5x5    | Comments                                                                            |
| -------- | ------------ | ------- | ------- | ------- | ------ | ------ | ------ | ------------------------------------------------------------------------------------ |
| Warm     | ViewportCull | 21.4 ms | 44.6 ms | 87.6 ms | 135 ms | 210 ms | 1.75 s | Linear in placement count (no spatial index yet); 500ms is the WHOLE Warm tier's budget (cull + rasterize + compose combined) - 1.75s in this one stage alone already blows it |

Commit: 22f7210

Warm stage 1, rebenchmarked: (1) per-node R-tree over local placement bboxes, built once per Cold input and reused across every later viewport-only call - benchmark now reuses one runner across a 16-position pan sequence instead of a fresh runner per iteration, matching real zoom/pan-tick usage; (2) ViewData::shapes changed from `std::vector<ViewShape>` to a shared_ptr handle (HierarchyResolverStage), so carrying a node's own shapes through unchanged is a refcount bump, not a copy - was the dominant remaining cost once placement culling was indexed (1.1M shapes on the 5x5 top-level node, copied by value on every call):

| Pipeline | Stage        | 1x1      | 2x1      | 2x2     | 3x2     | 3x3     | 5x5      | Comments                                                               |
| -------- | ------------ | -------- | -------- | ------- | ------- | ------- | -------- | ----------------------------------------------------------------------- |
| Warm     | ViewportCull | 0.043 ms | 0.096 ms | 0.138 ms | 0.179 ms | 0.239 ms | 0.675 ms | This stage now costs a negligible slice of the 500ms Warm-tier budget (was 1.75s pre-index, ~660ms with index alone before the shapes fix) - leaves essentially the whole 500ms for Rasterization + Compose, still to be benchmarked |

Commit: a0c20c7

Warm stages 2+3 added (RasterizeStage/ComposeStage - per-node raster bitmaps, composited via drawImage+transform per placement, not SkPicture recording - see PIPELINE_REFACTOR.md's own review notes for why). BM_WarmTier measures the WHOLE tier end to end (ViewportCull+Rasterize+Compose via ViewRenderPipeline::run_warm()) against its own single 500ms budget, one persistent pipeline reused across a 16-position pan sequence (same steady-state convention as BM_ViewportCull), rendering a fixed 1000x1000px output window at each point:

| Pipeline | Stage         | 1x1    | 2x1    | 2x2    | 3x2    | 3x3    | 5x5    | Comments                                                                 |
| -------- | ------------- | ------ | ------ | ------ | ------ | ------ | ------ | ------------------------------------------------------------------------- |
| Warm     | Full tier     | 200 ms | 210 ms | 407 ms | 473 ms | 750 ms | 1.53 s | Misses the 500ms tier budget at every point except 1x1/2x1 - ViewportCull alone is ~1ms, so this is essentially all Rasterize+Compose; not yet profiled which of the two dominates |

Commit: a441f68

BM_Rasterize/BM_Compose isolate each stage the same way BM_ViewportCull was isolated (precomputed inputs per pan position, timing only the stage under test):

| Pipeline | Stage      | 1x1    | 2x1    | 2x2    | 3x2    | 3x3    | 5x5     | Comments                                                    |
| -------- | ---------- | ------ | ------ | ------ | ------ | ------ | ------- | -------------------------------------------------------------- |
| Warm     | Rasterize  | 227 ms | 268 ms | 538 ms | 559 ms | 925 ms | 2.06 s  | Dominates the Warm tier entirely - accounts for essentially all of BM_WarmTier's own total |
| Warm     | Compose    | 3.83 ms | 2.54 ms | 5.18 ms | 4.77 ms | 7.40 ms | 13.0 ms | Cheap and roughly flat - 20-150x under Rasterize at every point, not the problem |

Commit: b329ba1

ViewRenderOptions::antialiasing_enabled added, default false (was unconditionally on) - RasterizeStage's own SkPaint fill/stroke/font antialiasing now opt-in. Every benchmark below leaves it at the new default (off), so this is a direct before/after of that one change alone:

| Pipeline | Stage      | 1x1    | 2x1    | 2x2    | 3x2    | 3x3    | 5x5    | Comments                                                                 |
| -------- | ---------- | ------ | ------ | ------ | ------ | ------ | ------ | ------------------------------------------------------------------------- |
| Warm     | Rasterize  | 93.2 ms | 121 ms | 247 ms | 314 ms | 449 ms | 1.24 s | ~1.7-2.4x faster than with AA (227ms-2.06s) - real, but not close to closing the gap to 500ms |
| Warm     | Full tier  | 82.6 ms | 120 ms | 232 ms | 313 ms | 470 ms | 1.19 s | 1x1/2x1/2x2 now under the 500ms tier budget; 3x2 upward still over it |

AA was a real, measurable contributor (roughly half of Rasterize's own cost) but not the dominant one - something else in RasterizeStage's own per-shape drawing accounts for the rest. Not yet investigated further.

Commit: 47b0ae3

HierarchyResolverStage now sorts each node's own `shapes` by ViewLayerId draw order (a correctness fix - shapes were previously drawn in whatever order this stage happened to collect them in, not bottom-to-top by ViewLayer, so e.g. a node's own BOUNDARY shape - created well before any physical layer's TERMINAL/OBSTRUCTION - could render on TOP of them instead of below). A real, one-time Cold-tier cost (std::stable_sort per node, dominated by the single large top-level node at scale), paid once per Cold recompute and amortized across every subsequent Warm-tier tick - not a per-frame cost:

| Pipeline | Stage             | 1x1    | 2x1    | 2x2    | 3x2    | 3x3     | 5x5    | Comments                                                        |
| -------- | ----------------- | ------ | ------ | ------ | ------ | ------- | ------ | ------------------------------------------------------------------ |
| Cold     | HierarchyResolver | 74.9 ms | 175 ms | 363 ms | 642 ms | 1.04 s | 3.37 s | ~1.5-2x slower than before the sort (was 50-91ms/2.1s) - still comfortably under the 5s/1M-component Cold target |

Warm-tier numbers (BM_Rasterize/BM_Compose/BM_WarmTier) are unaffected within normal run-to-run variance, as expected - they consume Cold's already-sorted output, so the sort's own cost never appears on a Warm-tier tick.

Commit: 85187c2

Superseded the sort above with a structural fix instead: HierarchyResolverStage now groups each node's own shapes into a `std::unordered_map<ViewLayerId, std::vector<Shape>>` (ViewLayerShapes) at collection time, rather than a flat vector sorted afterward - draw order comes from RasterizeStage walking `ViewLayerSet::all()` (that set's own bottom-to-top z-order) and looking up each layer's own group directly. This removes the sort's own O(n log n) cost entirely (grouping happens for free during insertion), and lets RasterizeStage construct one SkPaint per *layer* instead of per *shape* (hoisted out of the per-shape loop) - also the natural place a future per-layer visibility toggle would plug in, skipping a hidden layer's whole group with one map lookup:

| Pipeline | Stage             | 1x1    | 2x1    | 2x2    | 3x2    | 3x3    | 5x5    | Comments                                                                     |
| -------- | ----------------- | ------ | ------ | ------ | ------ | ------ | ------ | --------------------------------------------------------------------------- |
| Cold     | HierarchyResolver | 55.3 ms | 99.5 ms | 199 ms | 310 ms | 781 ms | 2.14 s | Sort cost fully gone - back to pre-sort numbers (~50-91ms/2.1s), correctness kept |
| Warm     | Rasterize         | 92.5 ms | 137 ms | 271 ms | 358 ms | 518 ms | 1.40 s | Essentially unchanged from per-shape paint construction - confirms paint construction was never the dominant cost here, something else in the per-shape draw calls is |

Full Cold->Warm chain (BM_WarmTier, LayerGeneration->HierarchyResolver->ViewportCull->Rasterize->Compose via ViewRenderPipeline::run_warm()), 5x5 only, run in isolation (`--benchmark_filter=BM_WarmTier/5x5`) so peak RSS reflects this one design, not every cached fixture:

| Pipeline  | Stage         | Time    | Peak RSS | Comments                                                    |
| --------- | ------------- | ------- | -------- | ------------------------------------------------------------ |
| Cold+Warm | Full pipeline | 1.25 s  | 3.69 GB  | **Wrong - see next entry.** BM_WarmTier's own timed loop read `pan_viewports[pan_index]` before incrementing it, so its first timed iteration replayed the untimed warm-up call's own exact (data_version, options) pair - a guaranteed, free MemoizingStage cache hit doing zero real work, inflating the reported mean by ~1/state.iterations() (~10% at this point's own 10 iterations) |

Commit: 92471a2

Fixed BM_WarmTier's own pan_index bug above (increment before use, matching BM_Rasterize/BM_Compose's own already-correct pattern) and reran. Mislabeled in the row above, too, not just the number: BM_WarmTier's own untimed warm-up call already runs Cold for real once, and root_mutation_version/top_level/hierarchy_depth never change for the rest of the benchmark - so every TIMED iteration is a guaranteed Cold cache hit. This number is Warm's own steady-state per-tick cost alone (the 2nd, 3rd, ... Nth pan/zoom tick against an already-resolved design), not "Cold+Warm" - Cold's own cost never appears in it at all:

| Pipeline | Stage           | Time    | Peak RSS | Comments                                                                 |
| -------- | --------------- | ------- | -------- | --------------------------------------------------------------------------- |
| Warm     | Full tier (warm) | 1.38 s  | 3.88 GB  | Now matches Rasterize (1.38s) + Compose (12.8ms) + ViewportCull (~1ms) almost exactly, as it should. This is the steady-state per-tick cost, NOT a Cold+Warm total. |

Commit: ff29bda

Added BM_WarmTierColdStart - the number the row above can't give: the TRUE first-render cost (Cold running for real, immediately followed by one Warm tick, no untimed warm-up priming any cache first - a fresh ViewRenderPipeline per timed iteration, `->Iterations(1)` forced since Google Benchmark's own calibration would otherwise invoke the whole thing, construction included, more than once):

| Pipeline  | Stage                    | Time                    | Peak RSS      | Comments                                                                 |
| --------- | ------------------------ | ------------------------ | ------------- | --------------------------------------------------------------------------- |
| Cold+Warm | First render (cold start) | 3.21 s mean (2.89-3.73 s, 3 runs) | 3.65-3.73 GB | Consistent with Cold (2.14s) + Warm (1.38s) summed independently (~3.5s) - confirms the two numbers above really do add up for a genuine first render |

Commit: 4c39ac7

Layer visibility (ROW/TRACK_PREFERRED/TRACK_NON_PREFERRED/GCELLGRID hidden by default, mirroring Scene's own long-standing pre-seeded defaults) wired into RasterizeStage. Re-ran the full matrix with everything still visible (BM_Rasterize/BM_Compose/BM_WarmTier/BM_HierarchyResolver) to confirm the new is_view_layer_visible check adds no real cost when nothing is actually hidden - within normal run-to-run variance of the numbers already on record (e.g. 5x5: HierarchyResolver 2.02s, Rasterize 1.47s, Compose 13.2ms, WarmTier 1.43s, WarmTierColdStart 3.01s - all consistent with prior entries).

Added BM_RasterizeDefaultVisibility - the number that actually matters for a real UI session, where rows/tracks/gcellgrid are hidden out of the box:

| Pipeline | Stage                          | 1x1    | 2x1    | 2x2    | 3x2    | 3x3    | 5x5    | Comments                                                              |
| -------- | ------------------------------ | ------ | ------ | ------ | ------ | ------ | ------ | -------------------------------------------------------------------------- |
| Warm     | Rasterize (everything visible) | 100 ms | 143 ms | 278 ms | 362 ms | 532 ms | 1.40 s | Baseline, same config every earlier Rasterize entry used                    |
| Warm     | Rasterize (default visibility) | 74.6 ms | 116 ms | 226 ms | 314 ms | 461 ms | 1.28 s | 25% faster at 1x1, shrinking to only ~8% faster at 5x5                      |

A real but modest win, and shrinking at scale - this actually rules out my own earlier hypothesis that ROW/TRACK/GCELLGRID's own individual draw calls were the dominant Rasterize cost (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md's antialiasing-fix entry above). Something else - likely ROUTE geometry or the sheer TERMINAL/OBSTRUCTION count across every distinct standard-cell Abstract - still dominates, and dominates more at larger scale, not less. Root cause still not found.

Commit: f835615

Ported four features from the pre-restart pipeline (git history, `src/pipelines.old/`) into the new module, at the user's explicit request to fill in known gaps before making further optimization decisions:

- **Fill patterns by object type** (`pipelines/draw_helpers.hpp`'s new `pattern_shader`/`draw_cross`, wired into `draw_view_shapes`) - BRICK for OBSTRUCTION, DOTS for ROUTING_BLOCKAGE, a diagonal-stripe/CROSS pattern for TERMINAL/ROUTE depending on Layer type/direction (`view_style.hpp`'s own long-standing `terminal_fill_pattern`, previously computed but never actually drawn). The tiled shader's own local matrix is scaled to counter the ambient dbu-to-pixel canvas matrix, so a pattern reads as a fixed on-screen density at any zoom, the same visual goal the old pixel-space design got for free.
- **Placement name positioning/sizing** (`hierarchy_resolver_stage.hpp`) - a Placement's own name label is now a separate Shape on its own dedicated PLACEMENT_NAME ViewLayer (already defined in `view_style.hpp`, previously unused), independently colorable/toggleable from the PLACEMENT_BOUNDARY outline it's drawn alongside. Font size is a fraction of the placement's own on-screen height (floored at a fixed minimum), bottom-left-anchored with a constant on-screen padding, and truncated to fit its own on-screen width (`truncate_text_to_width`, binary search over how many leading characters to drop behind an ellipsis).
- **Path extensions** (`draw_view_shapes`) - a routed Path's own centerline is now expanded into a real, square-ended buffered outline (`Geometry::path_to_polygons`, already existed and already extension-aware, just never called by the new pipeline) and filled/stroked with the layer's own pattern, instead of a plain butt-capped Skia stroke. A sub-pixel-on-screen (or deliberately zero-width, Track/GCellGrid) Path still collapses to a single hairline centerline stroke, matching the old design's own degenerate-case handling.
- **Via rendering** (`pipelines/via_shapes.hpp`, ported near-verbatim from `pipelines.old/stages/via_shapes.hpp`) - `Shape.vias`/`.via_iterates` now resolve into real drawable geometry (explicit LAYER/RECT vias, ViaRuleReference-driven arrays, or a bare VIARULE GENERATE name fit to the enclosing routed path's own width), appended into the same per-ViewLayer shape map instead of being silently skipped as before.

All four are covered by the existing `pipelines_tests` suite (several pre-existing tests needed updating, not just new ones added, since they'd hardcoded single-pixel-sample assumptions that a real per-object-type fill pattern now legitimately violates - e.g. a routing TERMINAL's own fill is no longer a flat translucent color anywhere on screen, only a patterned one) - 37/37 pass; `backend_tests` still shows exactly the same 54 pre-existing, accepted `ApiFixture` selection-hit-testing failures as before this change (nothing new broken).

Rasterize regressed sharply, as the user suspected it might ("I think complex fill patterns and transparency may have an affect on rasterization performance") - though the actual dominant cause turned out not to be fill patterns/transparency themselves:

| Pipeline | Stage                          | 1x1    | 2x1     | 2x2     | 3x2     | 3x3     | 5x5     | Comments |
| -------- | ------------------------------ | ------ | ------- | ------- | ------- | ------- | ------- | -------- |
| Warm     | Rasterize (everything visible) | 432 ms | 974 ms  | 2.14 s  | 2.59 s  | 3.75 s  | 8.27 s  | Was 100ms/143ms/278ms/362ms/532ms/1.40s - a consistent 4-8x slowdown across every size, not concentrated at scale |
| Warm     | Rasterize (default visibility) | 408 ms | 952 ms  | 2.11 s  | 2.51 s  | 3.69 s  | 8.18 s  | Was 74.6ms/116ms/226ms/314ms/461ms/1.28s - same 4-8x pattern |

Two real bugs found and fixed while chasing this, both about *caching granularity*, not the features' own inherent cost:

1. `Geometry::path_to_polygons` (real Boost.Geometry buffering, not cheap) was being recomputed from scratch for every routed Path on *every* Rasterize call - the pre-restart pipeline avoided exactly this by caching it once per Shape at shape-generation/Cold time (`RenderedShape::path_outlines`); the new pipeline's `Shape` (a `schema.py` type, reused directly rather than duplicated) has no field of its own to cache into, so a schema change was deliberately avoided in favor of a pipeline-local cache instead (`RasterizeStage::path_outline_cache_by_node_`, keyed per node/HierarchyId, holding the node's own current `data.shapes` handle to detect a real upstream recompute).
2. The *first* version of that cache was keyed on `culled` (RasterizeStage's own whole InputData) rather than on each individual node's own `data.shapes` - ViewportCullStage sits between HierarchyResolverStage and RasterizeStage and hands back a fresh wrapper object on *every single call*, regardless of whether the viewport change actually affected a given node, so that keying invalidated the entire cache on every pan tick and measured providing no benefit at all. Re-keying per node (5x5: 11.3-12.4s) recovered most, but not all, of the gap (5x5: 8.27s) - see the numbers above, still the current baseline.

The remaining ~4-8x is real, not a bug: via rendering now genuinely adds a large amount of drawable geometry that didn't exist before (a routed net's own via placements were previously silently skipped entirely), the pattern-shader/placement-label-truncation work is real per-shape/per-label cost with no equivalent in the old numbers, and - most importantly - RasterizeStage still has no per-shape viewport culling for the top-level node (ViewportCullStage only decides which *nodes* are visible, not which *shapes within* the huge top-level node's own flat shape map actually fall inside the current viewport window) - `draw_view_shapes` walks every shape in the whole design on every single tick regardless of pan position, a pre-existing architectural gap this work made significantly more expensive to keep paying, not something it introduced. Worth a dedicated follow-up; not attempted here.

Commit: f835615, continued

Re-introduced `UprightTextCanvas` (`pipelines/upright_text_canvas.hpp`/`.cpp`, ported near-verbatim from `pipelines.old/`, needing the same `-fno-rtti`-on-one-TU CMake treatment the old module used - this vendored Skia build still has no RTTI for `SkPaintFilterCanvas`/`SkCanvas`) wrapping each node's own canvas in `RasterizeStage::compute`, replacing `draw_view_shapes`' own manual per-label `save()/translate()/scale(1/scale,-1/scale)/drawString()/restore()` counter-transform with a plain `translate()/drawString()` - `UprightTextCanvas` intercepts every text draw and replaces the CTM with a translation+uniform-scale-only matrix at that exact moment instead, decomposing the *current* accumulated matrix rather than needing `scale` handed to it explicitly. Behaviorally equivalent for this pipeline's own transform chain (translate+scale+reflection only, never shear) - confirmed via the full `pipelines_tests` suite (37/37 still pass unchanged). Measured in isolation against the row above (everything else, including the two caching fixes, held constant):

| Pipeline | Stage                          | 1x1    | 2x1     | 2x2     | 3x2     | 3x3     | 5x5     | Comments |
| -------- | ------------------------------ | ------ | ------- | ------- | ------- | ------- | ------- | -------- |
| Warm     | Rasterize (manual counter-scale) | 432 ms | 974 ms  | 2.14 s  | 2.59 s  | 3.75 s  | 8.27 s  | Same row as above - the "before" for this specific comparison |
| Warm     | Rasterize (UprightTextCanvas)     | 449 ms | 1.01 s  | 2.23 s  | 2.70 s  | 3.95 s  | 8.73 s  | +3.3-5.5% over the manual counter-scale, growing slightly with scale (more labels = more per-draw virtual-dispatch/matrix-decomposition calls) |

A real but modest overhead (under 6% even at 5x5) - not the dominant driver of the 4-8x regression above, which comes almost entirely from the four ported features' own real geometry/text work. Kept enabled (this is now the active code path) since it's the more robust, less error-prone mechanism (automatic CTM decomposition vs. a hand-derived counter-scale that would need re-deriving if this pipeline ever draws text under a real rotation, e.g. if per-node picture caching or per-shape instance transforms are revisited later) and the cost is small relative to what's already here.

Commit: uncommitted (pipeline-refactor branch, 2026-09-09)

Closed the real per-shape viewport culling gap the previous entries' own root-cause analysis identified: `ViewportCullStage`'s own doc comment already named it explicitly - that stage only culls placements/instances, never individual shapes within one node's own direct content, so `draw_view_shapes` walked *every* shape in the whole design on *every* Rasterize tick regardless of pan position. Mirrors `ViewportCullStage`'s own proven pattern one level down: `HierarchyResolverStage::compute()` now builds a Boost.Geometry Index R-tree per `ViewLayerId` (`ViewData::shapes_index`, built once alongside `shapes` itself - so it's free on every later pan/zoom-only tick, no separate invalidation logic needed), storing each shape's own bbox paired with its index into that layer's own vector; `ViewportCullStage` propagates it unchanged (a `shared_ptr` refcount bump, same as `shapes` itself already got); `draw_view_shapes` queries it against the node's own render bbox (`RasterizeStage::compute()`'s own `local_bbox`) instead of walking every shape, falling back to a full scan if the index is ever missing.

37 pre-existing `pipelines_tests` still pass unchanged, plus 2 new ones (`HierarchyResolverStageFixture.ShapesIndexQueryFindsOnlyOverlappingShapesOnTheRightLayer` - queries the index directly against known rects; `RasterizeStageFixture.ShapeFarOutsideTheRenderViewportIsCulledButTheOneInsideStillDraws` - an end-to-end wiring guard) - 39/39. `backend_tests` unchanged at 567/621 (same 54 pre-existing, accepted failures).

| Pipeline | Stage                          | 1x1    | 2x1    | 2x2    | 3x2    | 3x3    | 5x5    | Comments |
| -------- | ------------------------------ | ------ | ------ | ------ | ------ | ------ | ------ | -------- |
| Warm     | Rasterize (before culling)     | 449 ms | 1.01 s | 2.23 s | 2.70 s | 3.95 s | 8.73 s | Previous entry's own UprightTextCanvas row - the "before" here |
| Warm     | Rasterize (after culling)      | 182 ms | 469 ms | 1.17 s | 767 ms | 914 ms | 1.60 s | 2.5-5.4x faster - the win *grows* with design size, as expected (more off-screen content to skip) |
| Warm     | Rasterize (default visibility) | 154 ms | 439 ms | 1.07 s | 729 ms | 828 ms | 1.44 s | Same shape as the row above |

5x5 (1.60s) is now only ~14% slower than the *very first* post-restart Rasterize baseline (1.40s, "everything visible," before fill patterns/via-rendering/path-extensions/placement-labels/UprightTextCanvas existed at all) - this single change recovered nearly all of the 4-8x regression those features introduced, confirming the root-cause read was right: it was never the fill patterns or transparency themselves (the user's own first suspicion) that dominated, it was the sheer number of shapes now being walked unconditionally on every tick.

As the plan itself flagged going in, this doesn't help every shape kind equally - PLACEMENT_BOUNDARY/PLACEMENT_NAME/ROW/TRACK/GCELLGRID are each still batched into one giant `Shape` per node (an earlier allocation-cost optimization), so each one's own bbox spans the whole design and is always a query hit regardless of viewport; the win measured above comes entirely from ROUTE/TERMINAL/OBSTRUCTION/via-expanded shapes, which are individually small and numerous. The remaining ~14% gap to the original baseline is consistent with that - not chased further here, since the numbers no longer show an obvious dominant cost the way the original 4-8x regression did. Revisit only if a future benchmark shows the batched shapes becoming the bottleneck.

Commit: uncommitted (pipeline-refactor branch, 2026-09-09)

Even after closing most of the shape-count regression above, zoom-fit (viewport = the whole design's own bbox, where per-shape culling provides zero benefit) is still slow - the remaining cost is genuine per-shape draw-call throughput in Skia's own CPU rasterizer. Rather than keep tuning that, tried [Blend2D](https://blend2d.com) - a JIT-compiled 2D vector graphics engine with native multi-threaded tiled rendering and composition operators that skip alpha-blend math for opaque draws - as a side experiment: a real, working `RasterizeBlend2DStage` sibling to `RasterizeStage`, benchmarked against Skia in the exact 4 steps requested.

**Build**: fetched via CMake `FetchContent` (`URL` archives of a pinned commit SHA each, since neither Blend2D nor its own AsmJit dependency publish tagged releases) - `blend2d::blend2d` links cleanly into the existing `pipelines` CMake target (now `PUBLIC`-linked there, alongside `skia`), no new dependency-management approach needed beyond what `googletest`/`googlebenchmark`/`tracy` already established for this project.

**Architecture**: `ViewData`/`ViewLayerShapes`/`ShapeSpatialIndex`/`Geometry::path_to_polygons` (the per-shape viewport-culling index and path-outline cache from the entry above) have zero Skia dependency, so `RasterizeBlend2DStage`'s own `draw_view_shapes_blend2d` reuses them completely unchanged - only the actual draw calls (`BLContext` instead of `SkCanvas`) differ. `RasterizedImage`/`RasterizeOutput` moved out of `rasterize_stage.hpp` into a new shared `rasterize_output.hpp` so both backends produce the identical output type; `RasterizeBlend2DStage`'s own final `BLImage` gets wrapped into an `sk_sp<SkImage>` (`kBGRA_8888_SkColorType`, matching Blend2D's own `BL_FORMAT_PRGB32` in-memory byte order exactly - confirmed via `format.h`'s own "Format_ARGB32_Premultiplied" doc comment) so `ComposeStage` needs zero changes regardless of which backend rasterized a given node. `ViewRenderPipeline` is now `ViewRenderPipelineImpl<RasterizeStageT = RasterizeStage>` under the hood, with `ViewRenderPipeline` staying a plain alias (zero impact on existing callers) and a new `ViewRenderPipelineBlend2D` alias wiring the Blend2D stage into the identical graph shape - a real, working, end-to-end-tested swap, not just a benchmark-only stub. Text (`Shape.texts`) is deliberately not drawn by this first Blend2D pass - a small fraction of draw calls next to ROUTE/TERMINAL/OBSTRUCTION/via geometry, scoped out rather than adding Blend2D's own font-loading integration surface for this side experiment.

Two real Blend2D API gotchas found and fixed while building this (both by direct pixel inspection, not assumption - see the commit's own history):
1. A `BLPattern`'s own `set_transform()` is combined with the *current* `BLContext` transform by default (`BL_CONTEXT_STYLE_TRANSFORM_MODE_USER`) - manually compensating for the ambient dbu-to-pixel scale (the direct analog of Skia's own `pattern_shader`/`makeWithLocalMatrix` trick) then applied that same scale *twice*, shrinking the tile to a fraction of a pixel at any real zoom. Fixed with `BL_CONTEXT_STYLE_TRANSFORM_MODE_NONE` instead (the pattern's own transform is absolute, not combined with the CTM at all) and no manual compensation needed.
2. Blend2D has exactly one `BLRenderingQuality` value (always antialiased, no way to disable it, unlike Skia's per-paint toggle `pattern_shader` relies on) - a pattern's own thin-line ink pixels never quite reach full alpha (measured ~233/255 for a 45-degree stripe, ~191/255 for an axis-aligned brick line straddling a tile boundary) - a real, permanent characteristic, not a bug; `rasterize_blend2d_stage_test.cpp`'s own tests use a wider color tolerance to account for it.

39 new/existing `pipelines_tests` all pass (44/44 total) - 3 new `RasterizeBlend2DStageFixture` tests plus a `ViewRenderPipelineBlend2D` end-to-end test. `backend_tests` unchanged at 567/621.

**Results** (`aes_scaling` fixtures, same tile configs as every earlier entry in this file):

| Benchmark                       | 1x1     | 2x1    | 2x2    | 3x2    | 3x3    | 5x5    |
| -------------------------------- | ------- | ------ | ------ | ------ | ------ | ------ |
| BM_Rasterize (Skia, baseline)     | 139 ms  | 364 ms | 910 ms | 593 ms | 707 ms | 1.23 s |
| BM_RasterizeBlend2D (single-thread, step 2) | 65.8 ms | 155 ms | 315 ms | 320 ms | 413 ms | 907 ms |
| BM_RasterizeBlend2D_MT2 (step 3)  | 65.3 ms | 163 ms | 265 ms | 292 ms | 388 ms | 769 ms |
| BM_RasterizeBlend2D_MT4 (step 3)  | 62.3 ms | 127 ms | 214 ms | 235 ms | 308 ms | 635 ms |
| BM_RasterizeBlend2D_MT4Opaque (step 4) | 66.0 ms | 125 ms | 219 ms | 262 ms | 341 ms | 635 ms |

Reading this honestly, step by step:
- **Step 2 (single-threaded Blend2D vs. Skia) is the whole story** - already 1.35-2.9x faster than Skia with zero threading and zero opaque-fast-path tricks, purely from Blend2D's own JIT-compiled rendering pipeline being a faster CPU rasterizer for this workload than Skia's.
- **Step 3 (multi-threading) is a real but modest further win**, and it scales with work size as expected: negligible at 1x1 (67ms -> 62ms - too little work to amortize thread coordination), a genuine ~1.4x at 5x5 (904ms -> 635ms at 4 threads). 2 threads captures roughly half of 4 threads' own gain at every size - consistent with `BLContextCreateInfo::thread_count`'s own documented "N-1 worker threads acquired from a pool" behavior scaling sub-linearly, not a bug.
- **Step 4 (opaque `BL_COMP_OP_SRC_COPY` fast path) shows no measurable benefit** - within noise (+/-5%) at every size, sometimes marginally *slower*. This is the one place the request's own framing didn't pan out, and it's worth saying plainly rather than searching for a way to call it a win: this project's real `ViewLayerStyle`s keep *fills* translucent (`layer_color()`'s own `fill_color.a = 100` convention) and only *outlines*/strokes opaque, and stroke geometry (thin lines) covers far less pixel area than fill geometry - so even a genuinely free win on strokes alone barely moves a total dominated by translucent fill work. The optimization is real and Blend2D-documented, it's just not reaching enough pixels in this specific style palette to show up.

**Net**: for this workload, Blend2D single-threaded already beats Skia by 1.35-2.9x, multi-threading adds up to another ~1.4x on top at real design sizes, and the opaque fast-path adds nothing measurable given this project's own current color palette. A `ViewRenderPipelineBlend2D` is now a real, working, tested alternative in the codebase - not yet wired into `api.cpp`/`LeHandle` for actual GUI use (a natural, small next step if these numbers motivate switching the default backend), and text rendering remains a real, scoped-out gap to close first if that happens.

Commit: 9dbc294, bbde626

Wired `ViewRenderPipelineBlend2D` into `LeHandle`/`api.cpp` (9dbc294), then found and fixed a real bug (bbde626): `draw_view_shapes_blend2d`'s own `set_stroke()` left the stroke width at a literal `0.0` for every rect/polygon outline (only the sub-pixel `Path` fallback branch ever overrode it). Skia's `SkPaint` treats stroke width 0 as a hairline (always exactly 1 device pixel) - the convention `rasterize_stage.hpp`'s own `stroke` paint relies on by never setting a width at all - but Blend2D has no such convention, so a literal 0-width stroke there rendered nothing. Every outline (including the outline color that patterns render their own ink with) was silently invisible while fills kept working fine, which is how this surfaced - reported directly by inspecting real rendered output, not caught by the existing unit tests (which only assert fill color, not outline color). Fixed by using `1.0 / scale` (a real, on-screen-~1-pixel-wide line at the current zoom) everywhere the code relied on Skia's hairline default.

Since every earlier Blend2D benchmark row above was measured *before* this fix, Blend2D was doing measurably less real drawing work than it should have (no outlines at all) - re-ran the full `BM_Rasterize`/`BM_RasterizeBlend2D*` matrix after the fix to get honest numbers, same `aes_scaling` fixtures/tile configs, single run (no `--benchmark_repetitions`, matching how the row above was itself measured):

| Benchmark                       | 1x1     | 2x1    | 2x2    | 3x2    | 3x3    | 5x5    |
| -------------------------------- | ------- | ------ | ------ | ------ | ------ | ------ |
| BM_Rasterize (Skia, baseline)     | 165 ms  | 430 ms | 1.06 s | 690 ms | 844 ms | 1.63 s |
| BM_RasterizeBlend2D (single-thread) | 87.3 ms | 234 ms | 440 ms | 426 ms | 575 ms | 1.24 s |
| BM_RasterizeBlend2D_MT2           | 83.8 ms | 199 ms | 325 ms | 378 ms | 538 ms | 1.04 s |
| BM_RasterizeBlend2D_MT4           | 84.5 ms | 178 ms | 276 ms | 353 ms | 483 ms | 926 ms |
| BM_RasterizeBlend2D_MT4Opaque     | 89.5 ms | 178 ms | 277 ms | 356 ms | 517 ms | 902 ms |

The Skia baseline itself also moved up from the previous entry's own numbers (139ms->165ms at 1x1, up to 1.23s->1.63s at 5x5) despite no Skia-side code change at all - this run-to-run machine noise (this environment is a shared/virtualized host, not a dedicated benchmarking box) is why the comparison below is done same-run (this table's own Blend2D rows against this table's own Skia row), not against the prior entry's numbers directly:

- **Blend2D single-threaded is still faster than Skia everywhere**, but by a smaller margin now that it's doing the real outline work it was skipping before: 1.89x at 1x1 (was 2.11x), 2.41x at 2x2 (was 2.89x), 1.31x at 5x5 (was 1.36x) - a real, expected, honest shrink, not a regression in the fix itself; outlines are extra draw calls that weren't happening at all in the numbers this file previously reported.
- **Multi-threading's own win is essentially unchanged in shape**: MT4 vs. single-thread is still a real, size-scaling win (84.5ms->same at 1x1 since there's too little work to amortize thread setup, up to 1.24s->926ms, ~1.34x, at 5x5) tracking closely with the pre-fix entry's own ~1.4x at 5x5.
- **The opaque fast path still shows no measurable, reliable benefit**, even now that outlines (mostly opaque strokes) are actually being drawn: MT4Opaque is within noise of plain MT4 at every size (89.5 vs 84.5 at 1x1, 902 vs 926 at 5x5 - if anything marginally *faster* at 5x5, ~2.6%, but well inside typical run-to-run variance on this machine). Thin outline strokes still don't cover enough pixel area to make `BL_COMP_OP_SRC_COPY` show up against a workload still dominated by translucent fill geometry.

**Net, updated**: the headline conclusion from the entry above still holds - Blend2D beats Skia at every size tested, multi-threading adds a further real win that grows with design size, and the opaque fast path doesn't move the needle for this project's current color palette - just with more honest margins now that outlines are actually part of what's being measured on both sides. `pipelines_tests` 44/44, `backend_tests` 567/621 (same accepted baseline) after the fix.

Follow-up experiment: is the opaque fast path's own lack of benefit really explained by translucent fills (`layer_style()`'s `fill.a = 100` convention, `view_style.hpp`), as the entries above kept hypothesizing rather than confirming? Tested directly - temporarily forced every fill fully opaque (`layer_style()`'s `fill.a = 255`, plus `placement_blockage_style()`'s own DOTS fill) and re-ran `BM_RasterizeBlend2D_MT4` vs. `_MT4Opaque` (same `aes_scaling` fixtures), then reverted both lines back to their real `100` values - this was a throwaway measurement, not a real palette change, and nothing in `view_style.hpp` is different after this entry than before it:

| Benchmark (all fills opaque)      | 1x1    | 2x1    | 2x2    | 3x2    | 3x3    | 5x5    |
| ---------------------------------- | ------ | ------ | ------ | ------ | ------ | ------ |
| BM_RasterizeBlend2D_MT4             | 81.3 ms | 168 ms | 282 ms | 337 ms | 446 ms | 853 ms |
| BM_RasterizeBlend2D_MT4Opaque       | 81.8 ms | 173 ms | 285 ms | 315 ms | 453 ms | 851 ms |

Still no reliable win - MT4Opaque ranges from ~4% slower (2x1) to ~7% faster (3x2), no consistent direction, all inside normal run-to-run noise on this machine. This is actually a *stronger* negative result than the outline-only case above: fills cover far more pixel area than thin outline strokes, so if `BL_COMP_OP_SRC_COPY` were going to show a real win from skipping alpha-blend math over a large opaque region, forcing every fill fully opaque is exactly the condition that should have revealed it, and it didn't. The earlier hypothesis (translucent fills masking a real win) doesn't hold up - something else (likely per-draw-call dispatch/JIT overhead, or memory bandwidth on the destination surface, dominating regardless of whether the blend math itself is skipped) is capping this optimization's real-world payoff for this workload, not fill alpha. Worth revisiting only if a future profiling pass identifies where Blend2D's own per-call time actually goes; not chased further here.

Commit: d13c1f6

Full-pipeline stage profile, Cold and Warm, using the exact same `aes_scaling` fixtures/tile configs/pan-position sequence `BM_RasterizeBlend2D_MT4` itself uses (`BM_LayerGeneration`/`BM_HierarchyResolver` for Cold, `BM_ViewportCull`/`BM_RasterizeBlend2D_MT4`/`BM_Compose` for Warm - `BM_Compose` itself still runs against Skia's `RasterizeStage` output, not Blend2D's, since `ComposeStage` only ever calls generic `sk_sp<SkImage>` methods regardless of which backend produced a node's image - its own cost is a function of image count/size, not which rasterizer drew them, so its existing numbers are directly reusable here unchanged):

| Tier | Stage             | 1x1     | 2x1    | 2x2    | 3x2    | 3x3    | 5x5     |
| ---- | ----------------- | ------- | ------ | ------ | ------ | ------ | ------- |
| Cold | LayerGeneration   | 0.195 ms | 0.191 ms | 0.177 ms | 0.180 ms | 0.179 ms | 0.175 ms |
| Cold | HierarchyResolver | 342 ms  | 856 ms | 1.84 s | 2.44 s | 4.00 s | 13.3 s  |
| Warm | ViewportCull      | 0.096 ms | 0.149 ms | 0.232 ms | 0.337 ms | 0.516 ms | 1.18 ms |
| Warm | Rasterize (Blend2D MT4) | 82.1 ms | 166 ms | 272 ms | 321 ms | 432 ms | 867 ms |
| Warm | Compose           | 5.43 ms | 3.83 ms | 7.12 ms | 6.55 ms | 9.43 ms | 17.7 ms |

Reading the ranking, not just the raw numbers:

- **Cold is completely dominated by `HierarchyResolver`** - `LayerGeneration` is flat/O(1) (Technology layer count alone, never design size, per its own long-standing finding) and worth under 0.2ms at every size, a rounding error next to `HierarchyResolver`'s 342ms-13.3s. `HierarchyResolver` alone accounts for >99.9% of Cold-tier time at every point in this matrix.
- **Warm is completely dominated by `Rasterize`** - `ViewportCull` stays under 1.2ms even at 5x5 (the per-shape spatial index doing its job), and `Compose` stays under 18ms (cheap `drawImage` calls, one per node). Rasterize alone is 93-98% of the whole Warm tier at every size (e.g. 5x5: 867ms of an 886ms Warm total).
- **Cold dwarfs Warm at every size, and the gap widens with scale**: Cold/Warm ratio goes from ~3.9x at 1x1 (342ms vs. 87.6ms) to ~15x at 5x5 (13.3s vs. 886ms). This matters for what to optimize next: Warm-tier tuning (this file's own Blend2D work, per-shape culling) only ever pays off on the 2nd-and-later pan/zoom tick against an *already-resolved* design - the first-ever render of a real, larger design is paying `HierarchyResolver`'s own Cold cost up front regardless, and that cost is now the far larger of the two, especially at scale.

**Net**: if a "why does opening/re-scaling a large design feel slow" investigation continues from here, `HierarchyResolver` (Cold, per-placement/per-Abstract shape collection) is the next real target, not further Warm-tier/Rasterize tuning - Rasterize is already the fast half of this picture. `HierarchyResolver`'s own scaling has been on record since early in this file's own history (see the `997e943`/`f5abfd9` entries above) and hasn't been revisited since the `MemoizingStage` shared_ptr-caching fix; that fix + this file's whole Rasterize-focused effort since has left Cold's own relative share of a first render *larger*, not smaller, simply because Warm got so much faster. Not chased further in this session - flagging it as the honest next bottleneck rather than continuing to narrow an already-small piece of the total.

Commit: b06c263

Real-world confirmation, not synthetic: the user reported that in the actual GUI (`le_shell`, `RasterizeBlend2DStage::thread_count_` defaulted locally to 4), zooming in is fast but zoom-fit still takes ~8s on a real design - `test_data/aes_scaling_4x4.def` (661,504 components, 401MB), read via `test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef`. This is a genuinely different, much larger fixture than anything in `aes_scaling_fixture.hpp`'s own synthetic matrix (that file's "5x5" is a small procedurally-generated repeat of a toy design, not a real chip) - "4x4" here means 16 real, full-size tiles of an actual AES core, not 16 tiny synthetic ones, so the two "5x5"/"4x4" labels aren't comparable in scale at all. Profiled directly with a throwaway harness reading the same LEF/DEF pair the user's own `tcl/aes_scaling_4x4.tcl` script uses, resolving Cold once, then timing a zoom-fit-shaped Warm tick (viewport = the Layout's own declared diearea bbox, a 1280x800 window, `RasterizeBlend2DStage` at `thread_count=4` matching the user's own local setting):

| Stage                              | Time     |
| ----------------------------------- | -------- |
| read_lef                            | 25.4 ms  |
| read_def                            | 18.1 s   |
| HierarchyResolver (Cold)            | 6.78 s   |
| ViewportCull (Warm, zoom-fit)       | 215 ms   |
| Rasterize (Blend2D MT4, Warm, zoom-fit) | **8.09 s** |
| Compose (Warm, zoom-fit)            | 602 ms   |

`Rasterize` alone at 8.09s matches the user's own reported ~8s almost exactly - direct, real-world confirmation of what every earlier entry in this file already predicted: per-shape viewport culling (`ViewData::shapes_index`, `HierarchyResolverStage`) provides no filtering benefit once the viewport *is* the whole design's own bbox - every shape's bbox intersects a full-design query, so `ViewportCull`'s own 215ms here is pure query overhead, not a win, and `Rasterize` has to walk and draw essentially every TERMINAL/OBSTRUCTION/ROUTE/via shape across all 661,504 placements regardless of backend. Blend2D + 4 threads makes the *per-shape* constant meaningfully smaller (this file's own single-thread/MT4 entries above), but doesn't change the fundamentally O(total-visible-shape-count) shape of the cost - at true zoom-fit on a design this large, "total visible" is "everything," and even a faster constant still adds up to real seconds. `ViewportCull`/`Compose` are both non-negligible here (215ms/602ms) in a way the synthetic benchmarks never showed at their own much smaller scale, but neither is remotely close to `Rasterize`'s own share - the ranking from the synthetic profile above (Rasterize dominates Warm) holds at real scale too, just with much larger absolute numbers across the board.

Also confirmed directly (`fit_scene_unlocked`, `api.cpp`): `le_fit_scene` itself is O(1) - it reads the Layout's declared `diearea` Shape (one DB lookup) and sets scale/pan, nothing more. The ~8s isn't in the fit call itself; it's paid on the *next* `le_render_pixel_buffer()` call once the viewport actually covers the whole design, exactly the Warm-tier cost measured above. `read_def` (18.1s) and `HierarchyResolver` Cold (6.78s) are separate, one-time costs paid once when the design is first opened (or after a real content mutation) - not part of zoom-fit's own recurring cost, though from a user's own perspective right after opening a design this large, all three (read, Cold resolve, first zoom-fit's Warm cost) can land close together and feel like one long wait.

**Net**: the ~8s zoom-fit is real, understood, and not a bug or regression - it's the direct, now-measured consequence of per-shape culling's own documented "no benefit at full-design zoom" limitation (flagged as far back as this file's own 164eb0e-era entries) meeting a design that's genuinely large (661K components). Closing this further needs a different technique than per-shape culling for the full-design-zoom case specifically - e.g. level-of-detail/simplified rendering below a certain on-screen pixel size, or a lower-resolution cached "overview" picture reused across zoom-fit views - rather than more Rasterize throughput tuning, which this file's own step-by-step Blend2D work has already substantially exhausted (single-thread + MT4 already applied here; the opaque fast path already tested and ruled out above).

Commit: 46d05f9

Reintroduced tiny-shape culling - the level-of-detail technique the entry above named as the actual next step - modeled on `pipelines.old/stages/viewport_filter_stage.hpp`'s own "drop a shape under 1 on-screen pixel in BOTH dimensions" test (not just one, so a long thin wire survives even if its width alone is sub-pixel), but *without* that stage's own `TinyViewportFilterStage`/`TinyShapeDot` companion - the user explicitly asked for the shape simply removed from the pipeline before rasterization, no dot drawn in its place at all. `bbox_is_sub_pixel`/`polygon_is_sub_pixel` (new, `draw_helpers.hpp` - shared by both Rasterize backends rather than duplicated) gate a Rect/Polygon's *entire* draw (fill, outline, pattern, via-cross) in `draw_view_shapes`/`draw_view_shapes_blend2d`'s own per-shape loop, checked *before* constructing a `BLPath`/`SkPath` at all, not just before the draw call, so a culled shape pays none of that construction cost either. Unlike the pre-restart pipeline, this couldn't be a separate Cold-tier stage - `HierarchyResolverStage`'s own shape collection is scale-independent and shared across every later zoom/pan tick, so a scale-dependent cull has to live in the Warm-tier draw path itself (where `scale` is actually known), the same reasoning `pipelines.old`'s own two-stage split (Cold generation, Warm `ViewportFilterStage`) already encoded. Applied to Rect/Polygon geometry only - a routed `Path` already has its own separate, pre-existing sub-pixel-width fast path (a cheap hairline stroke instead of the expensive buffered outline) that a long-but-thin wire needs to keep taking, matching the old code's own "don't drop a long thin wire" carve-out.

One existing test's own name and assertion described the *old*, dot-drawing behavior exactly (`ApiFixture.SubPixelShapeRendersAsASinglePixelDotAndIsNotSelectable`, `api_test.cpp`) - not a false positive, a real, intended behavior change this test needed updating for, renamed to `SubPixelShapeIsNotRenderedAndIsNotSelectable` and its own assertion flipped from `EXPECT_TRUE` to `EXPECT_FALSE` on the same pixel region. `backend_tests` back at the exact same accepted 567/621 baseline afterward (confirmed via a sorted diff of failing-test names against the pre-change list - identical set, this one rename aside); `pipelines_tests` unaffected, 44/44 (its own existing fixtures' shapes are all comfortably above 1px at the scales they're tested at).

**Real-world result**, re-measured with the exact same throwaway `aes_scaling_4x4.def` harness the entry above used (same LEF/DEF pair, same zoom-fit-shaped Warm tick, same `thread_count=4`):

| Stage                              | Before   | After    |
| ----------------------------------- | -------- | -------- |
| ViewportCull (Warm, zoom-fit)       | 215 ms   | 219 ms   |
| Rasterize (Blend2D MT4, Warm, zoom-fit) | 8.09 s | **2.92 s** |
| Compose (Warm, zoom-fit)            | 602 ms   | 607 ms   |

Rasterize dropped **2.77x** (8.09s -> 2.92s) - directly on the real design and the real scenario that motivated the whole investigation, not a synthetic proxy. `ViewportCull`/`Compose` are unaffected, as expected (neither one walks individual Rect/Polygon geometry). The synthetic `aes_scaling_fixture.hpp` benchmarks (`BM_RasterizeBlend2D_MT4`) barely moved by comparison (5x5: 867ms -> 738ms, ~15%) - expected and consistent, not a discrepancy: that benchmark's own pan sequence stays zoomed into a small window (10% of the design's own bbox) at every point, so relatively few of its own shapes were ever ending up sub-pixel in the first place; this optimization's real payoff is specific to the full-design-zoom case the synthetic benchmark's own methodology never actually exercises.

**Net**: zoom-fit on this real 661K-component design is now ~2.9s of Rasterize instead of ~8.1s - a real, substantial win, though still not "instant." Whatever remains past this point is genuinely-visible geometry that has to be drawn regardless of technique - the next lever, if this still isn't fast enough, is a coarser one than per-shape culling (a cached lower-resolution "overview" picture, or simplifying/merging geometry below some larger-than-1px threshold), not a further tweak to this same mechanism.

Commit: eaa0985

A direct per-function profile of `draw_view_shapes_blend2d` itself (single-threaded, to attribute cost accurately - Blend2D's own worker threads execute draw calls asynchronously, so timing individual calls under MT4 would misattribute real cost to `ctx.end()`'s own flush) found the per-shape geometry loops dominating almost completely, on both fixtures:

| Component | Synthetic `aes_scaling` 5x5 | Real `aes_scaling_4x4.def` |
| --------- | --------------------------- | --------------------------- |
| Rects (TERMINAL/OBSTRUCTION) | ~65% (2.2M rects) | 48% (8.26M rects) |
| Paths (routed wires) | ~34% (185K paths) | 43% (3.73M paths) |
| Polygons | ~0.5% | 5.6% |
| Spatial-index query | ~0.3% | 3.6% |
| Per-layer setup (style/paint) | ~0.1% | ~0.0% |

Per-layer setup/query were both already confirmed non-bottlenecks - but the setup number was suspicious on its own terms: `set_fill()`/`set_stroke()` (comp_op, fill/stroke color or pattern, stroke width, dash array) were being re-run on *every single shape* even though every one of those values is ViewLayerStyle-level, identical for every shape a layer holds - including a fresh `BLArray<double>` heap allocation per call for the dash array. Hoisted both to run once per layer instead (draw_cross_blend2d's own redundant pre-call state-setting removed too, since it already sets its own stroke_style/width internally) - BLContext keeps whatever style was last set until something changes it, so this is correct, not just faster.

Verified with proper repeated measurements (5 reps, cv <2.6%) against `BM_RasterizeBlend2D/5x5`:

| Benchmark | Before | After | Change |
| --------- | ------ | ----- | ------ |
| Single-threaded | 1050 ms | 912 ms | ~13% faster |
| MT4 | 736 ms | 449 ms | **~39% faster** |

The one real `aes_scaling_4x4.def` zoom-fit measurement (2919ms before, 2929-3117ms across 4 runs after) didn't show a clear win - within this machine's own already-documented noise band for that specific benchmark (see e.g. `BM_WarmTierColdStart`'s own 16-47% run-to-run swings earlier in this file), not a regression signal; the controlled, properly-repeated synthetic numbers above are the statistically reliable evidence here. `backend_tests`/`pipelines_tests` unaffected (same 567/621 baseline; 40/40, including `RasterizeBlend2DStageFixture`'s own real pixel-sampling assertions - confirms this is a pure perf change with no behavior difference).

Commit: 4733fd9

The `draw_view_shapes_blend2d` per-function profile above already found Paths (routed wires) at 43% of real-design time - the largest single remaining category after Rects. That number included every sub-pixel-width Path, which was drawn as a faint centerline hairline (`ctx.stroke_width(1.0/scale)`) rather than its real buffered outline - real, on-screen ink for a wire too thin to actually see clearly, but still a full draw call. Extended the same "not worth the draw call" reasoning `bbox_is_sub_pixel`/`polygon_is_sub_pixel` already apply to Rect/Polygon geometry to these: drop them entirely instead of drawing a hairline.

One real distinction had to be preserved carefully, not glossed over: this same branch also handles TRACK/GCellGrid's own deliberately zero-width synthetic Path (`p.width == 0` by construction, this function's own long-standing convention) - always sub-pixel regardless of zoom, not a real route that just happens to be thin right now. Dropping everything unconditionally would have deleted TRACK/GCellGrid rendering entirely, permanently, at every zoom level - not the intended change. `p.width == 0` (an exact dbu-integer comparison) identifies that case and keeps it drawing unconditionally; only a real route (`p.width > 0`) that computes sub-pixel at the current scale is dropped.

**Results** - by far the largest single win of this whole investigation:

| Benchmark | Before | After | Change |
| --------- | ------ | ----- | ------ |
| `BM_RasterizeBlend2D/5x5` (single-threaded, 5 reps) | 912 ms | 675 ms | ~26% faster |
| `BM_RasterizeBlend2D_MT4/5x5` (5 reps) | 449 ms | 303 ms | ~32% faster |
| Real `aes_scaling_4x4.def` zoom-fit (MT4, 3 fresh runs) | ~3030 ms mean | 850-918 ms | **~3.3x faster** |

The real-design number confirms directly what the per-function profile only implied: most of the real design's own remaining Rasterize cost at zoom-fit was millions of hairline route segments too thin to meaningfully see, not genuinely useful ink. `backend_tests` unchanged (567/621); `pipelines_tests` 40/40 - though no existing test currently exercises Path/route rendering directly, a real, pre-existing test-coverage gap this change didn't introduce and doesn't close either.

Commit: 089dd51

Two follow-ups after hoisting `set_fill()`/`set_stroke()` out of the per-shape loop (the `eaa0985` entry above). First, `has_outline` itself: every `ViewLayerStyle` this codebase actually constructs sets a nonzero `outline_color` (`layer_style()`'s own `base` always comes from a fully-opaque palette entry; every hand-written style literal sets one too), so the per-shape `if (has_outline)` checks throughout `draw_one_shape` never actually skipped a draw call in practice - dead branches, unlike `has_fill`'s own check (which genuinely varies). Removed them in both backends, drawing the outline/stroke unconditionally.

Second, re-evaluated `use_opaque_fast_path` (`BL_COMP_OP_SRC_COPY` vs. `SRC_OVER`) now that `comp_op` no longer gets re-set on every shape - the earlier "no benefit" finding (this file's own Blend2D-experiment entry) was measured *before* that hoist, so it seemed worth double-checking whether the redundant per-shape `set_comp_op` calls had been masking a real win. They weren't: `BM_RasterizeBlend2D_MT4` vs. `_MT4Opaque`, 5 reps, still show no measurable difference (308ms vs 314ms). Went further and tried removing the `color.a == 255` gate entirely - always `SRC_COPY`, even for a translucent color - per explicit direction: still no measurable benefit (300ms vs 302ms), and this time with a real cost attached, confirmed by directly sampling an overlapping-translucent-layers pixel (two TERMINAL shapes, M1 then M2, both using `layer_style()`'s own `fill.a = 100` convention) under each mode:

| Mode | Sampled pixel (M1 red, then M2 green, fully overlapping) |
| ---- | ---------------------------------------------------------- |
| SRC_OVER | `r=20 g=235 b=0 a=253` - M1's red still faintly present, alpha built up from real compositing |
| SRC_COPY (always) | `r=0 g=255 b=0 a=233` - M1 completely erased wherever M2 draws over it |

`SRC_COPY` doesn't blend with the destination at all - it overwrites it outright, alpha included. Given zero performance benefit either way, kept the gate (`color.a == 255`, the one case the two ops are actually equivalent) rather than accept that correctness cost for nothing. `use_opaque_fast_path` remains benchmark-only - `api.cpp` never enables it, so none of this affects live GUI/API rendering today, but it would be a real, silent regression if that ever changed without keeping this gate.

`backend_tests`/`pipelines_tests` unaffected (567/621, identical failing set; 40/40).

Commit: cc92972

Correction to the Blend2D-experiment entry above: "Blend2D has exactly one BLRenderingQuality value ... a pattern's own thin-line ink pixels never quite reach full alpha ... a real, permanent Blend2D characteristic, not a bug" - true for `DIAGONAL_STRIPES` (measured ~233/255, its 45-degree lines can't be pixel-aligned to avoid splitting AA coverage along their own length), but the `BRICK` half of that claim (~191/255) was wrong - it was a real, fixable bug, reported directly by the user as "the obstruction brick pattern is not rendering correctly," recognized as the same class of failure as an already-fixed `pipelines.old` bug (a hazy pattern instead of crisp joints), just a different root cause: `pattern_blend2d`'s own BRICK lines sat at exact integer coordinates (`y=0`, `y=s/2`, `x=s/2`, `x=0`), each straddling a pixel row/column boundary under Blend2D's mandatory antialiasing (no `setAntiAlias(false)` equivalent, unlike Skia's own `pattern_shader`). Fixed by offsetting each line's own cross-axis coordinate by +0.5, landing its AA-softened edges on the real integer boundaries instead of straddling them - a standalone tile-render check confirmed a solid 255/255 across the whole joint afterward, up from ~191/255.

`RasterizeBlend2DStageFixture`'s own color tolerance (introduced alongside the original, incomplete diagnosis) tightened from 70 to 30 to match - 70 was covering both BRICK's now-fixed gap and stripe's own still-real one; 30 is comfortable margin for stripe alone. `backend_tests`/`pipelines_tests` unaffected (567/621, identical failing set; 40/40, now passing at the tighter tolerance - proof of the fix, not just no-regression).

Added text (`Shape.texts`) drawing to `RasterizeBlend2DStage` - the generic per-shape TERMINAL/ROUTE label case only (`kLabelWidthRatio`-scaled, floored at `kMinLabelPixelSize`, centered at `text.location`, never truncated), mirroring `rasterize_stage.hpp`'s own equivalent loop; placement-name labels (the truncated, bottom-left-anchored `is_placement_name_layer` case) remain a scoped-out gap. Blend2D has no font-manager abstraction to scan a directory the way Skia's `SkFontMgr_New_Custom_Directory` does, so a new `default_blend2d_font_face()` (`blend2d_font.hpp`, implemented in `pipelines.cpp` alongside `default_typeface()`) loads one specific bundled file (`LE_FONT_DIR/DejaVuSans.ttf`) directly via `BLFontFace::create_from_file`, with the same executable-relative fallback rationale as `default_typeface()`'s own Linux path (for a release bundle where the compile-time `LE_FONT_DIR` path doesn't exist). `LE_FONT_DIR` (`CMakeLists.txt`) is now set unconditionally rather than only `if(NOT APPLE)`, since Blend2D needs it on every platform (no CoreText-style system-font fallback the way Skia has on macOS). No accumulated-instance-rotation concern exists for this backend (one `BLContext` draws directly into one node's own `BLImage` under a single, known translate+scale+flip transform - no picture recording/replay across nested hierarchy the way Skia's `UprightTextCanvas` defends against), so instead of decomposing/replacing the CTM per glyph run, each label's own `text.location` is mapped through the context's current `final_transform()` once to a real device-pixel point, then drawn under a plain identity transform at that point.

Visually verified (a small hand-built Root, one Terminal labeled "MYPIN" on M1, rendered via `RasterizeBlend2DStage` directly and dumped to PNG through a throwaway TEMP tool) - upright, correctly positioned, correctly sized text.

A real correctness issue surfaced immediately once text actually started rendering through `api.cpp` (`LeHandle::view_render_pipeline` is `le::ViewRenderPipelineBlend2D`, wired in an earlier commit - `api_test.cpp`'s `ApiFixture` tests exercise this backend directly, not Skia's): `ApiFixture.SubPixelShapeIsNotRenderedAndIsNotSelectable` started failing, because a Terminal's label floors at `kMinLabelPixelSize` (10px) regardless of whether its own rect survived `bbox_is_sub_pixel` culling - a fully sub-pixel, invisible pin was suddenly growing a 10px-tall floating label with nothing visible to anchor it to. This was consistent with `draw_helpers.hpp`'s own pre-existing `bbox_is_sub_pixel` doc comment ("Text is unaffected... never a label") and with Skia's own already-existing, identically-unconditional text loop in `rasterize_stage.hpp` - but that policy had never actually been exercised end-to-end before, since `RasterizeBlend2DStage` drew no text at all until now. Given the choice (keep labels unconditionally visible and update the test, vs. hide a shape's own label whenever none of its geometry survives the sub-pixel cull), the latter was chosen: both `draw_one_shape` lambdas (Blend2D and Skia) now track whether any of a shape's own rects/polygons/paths actually drew (survived their own cull check) and skip that shape's whole text block - both the placement-name case and the generic case - when none did. `bbox_is_sub_pixel`'s own doc comment updated to match (no longer claims text is unaffected).

**Benchmark** - real per-glyph cost, run with the standard `aes_scaling` fixture at `hierarchy_depth = 1` (already the benchmark's own existing setup, so terminal labels are resolved and visible):

| Benchmark | Before (no text) | After (with text) |
| --------- | ----------------- | ------------------ |
| `BM_RasterizeBlend2D/1x1` | ~66-90 ms | 185 ms |
| `BM_RasterizeBlend2D_MT4/1x1` | ~62-89 ms | 79 ms |
| `BM_RasterizeBlend2D/5x5` (5 reps) | 675 ms | 3785 ms |
| `BM_RasterizeBlend2D_MT4/5x5` (5 reps) | 303 ms | 1171 ms |

A real, substantial cost - not a rounding error. A standalone micro-benchmark (2,000,000 calls of `ctx.fill_utf8_text()` on a fixed 2-character string) isolated why: ~0.84 us/call for the draw itself (1.68s/2M), a further ~18% on top for this function's own `save()`/`set_transform(identity)`/`restore()` wrapper (1.98s/2M) - the wrapper is real but not the dominant cost. `fill_utf8_text` shapes and fills each glyph run as vector paths on every call; Blend2D has no bitmap-glyph-cache layer the way Skia's own text renderer does (a global cache keyed by typeface/size/glyph ID, so the same few characters repeated across millions of short pin labels become nearly free after the first few unique glyphs). MT4 scales this cost reasonably well (single-threaded text overhead at 5x5 is ~3110ms, MT4's own is ~868ms - close to the ~4x a 4-thread context should give), so this isn't a threading gap, just a real per-call API cost difference between the two libraries for this specific "millions of short repeated labels" workload. Not optimized further in this pass (a manual glyph-bitmap cache would close most of this gap but is real, separate engineering effort, out of scope for "add back text shapes") - flagged here as a known, quantified cost for whoever next tunes this backend's own text path.

`backend_tests`/`pipelines_tests` unaffected beyond the one real fix above (567/621, identical failing set once `SubPixelShapeIsNotRenderedAndIsNotSelectable` was fixed; 48/48).

