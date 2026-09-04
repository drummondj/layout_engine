
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

Commit: (pending)

ViewRenderOptions::antialiasing_enabled added, default false (was unconditionally on) - RasterizeStage's own SkPaint fill/stroke/font antialiasing now opt-in. Every benchmark below leaves it at the new default (off), so this is a direct before/after of that one change alone:

| Pipeline | Stage      | 1x1    | 2x1    | 2x2    | 3x2    | 3x3    | 5x5    | Comments                                                                 |
| -------- | ---------- | ------ | ------ | ------ | ------ | ------ | ------ | ------------------------------------------------------------------------- |
| Warm     | Rasterize  | 93.2 ms | 121 ms | 247 ms | 314 ms | 449 ms | 1.24 s | ~1.7-2.4x faster than with AA (227ms-2.06s) - real, but not close to closing the gap to 500ms |
| Warm     | Full tier  | 82.6 ms | 120 ms | 232 ms | 313 ms | 470 ms | 1.19 s | 1x1/2x1/2x2 now under the 500ms tier budget; 3x2 upward still over it |

AA was a real, measurable contributor (roughly half of Rasterize's own cost) but not the dominant one - something else in RasterizeStage's own per-shape drawing accounts for the rest. Not yet investigated further.

