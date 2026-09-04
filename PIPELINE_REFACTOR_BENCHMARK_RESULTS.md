
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

