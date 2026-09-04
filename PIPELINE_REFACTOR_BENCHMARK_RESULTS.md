
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
| -------- | ----------------- | ------ | ------ | ------ | ------- | ------- | --------------------------------------------- |
| Cold     | HierarchyResolver | 156 ms | 289 ms | 762 ms | 1187 ms | 2017 ms | +PLACEMENT_BOUNDARY per-placement label cost |

Commit: bcf6293

| Pipeline | Stage             | 1x1    | 2x1    | 2x2    | 3x2    | 3x3     | Comments                                    |
| -------- | ----------------- | ------ | ------ | ------ | ------ | ------- | -------------------------------------------- |
| Cold     | HierarchyResolver | 121 ms | 262 ms | 609 ms | 996 ms | 1571 ms | Batched PLACEMENT_BOUNDARY shapes, ~9-22% faster |

