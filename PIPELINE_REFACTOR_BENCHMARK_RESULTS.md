
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

Commit: (pending)

Added BM_WarmTierColdStart - the number the row above can't give: the TRUE first-render cost (Cold running for real, immediately followed by one Warm tick, no untimed warm-up priming any cache first - a fresh ViewRenderPipeline per timed iteration, `->Iterations(1)` forced since Google Benchmark's own calibration would otherwise invoke the whole thing, construction included, more than once):

| Pipeline  | Stage                    | Time                    | Peak RSS      | Comments                                                                 |
| --------- | ------------------------ | ------------------------ | ------------- | --------------------------------------------------------------------------- |
| Cold+Warm | First render (cold start) | 3.21 s mean (2.89-3.73 s, 3 runs) | 3.65-3.73 GB | Consistent with Cold (2.14s) + Warm (1.38s) summed independently (~3.5s) - confirms the two numbers above really do add up for a genuine first render |

