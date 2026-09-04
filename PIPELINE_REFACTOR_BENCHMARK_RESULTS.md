
Commit: 12bd236a4e8399eede4f4722e59d9928fdecd639

| Pipeline | Stage           | 1x1     | 2x1     | 2x2     | 3x2     | 3x3     | Comments                          |
| -------- | --------------- | ------- | ------- | ------- | ------- | ------- | --------------------------------- |
| Cold     | LayerGeneration | 79.7 us | 79.2 us | 79.0 us | 78.6 us | 79.4 us | Flat/O(1) across all 5 tile sizes |

Commit: e61a8ab

| Pipeline | Stage             | 1x1    | 2x1    | 2x2    | 3x2    | 3x3    | Comments                    |
| -------- | ----------------- | ------ | ------ | ------ | ------ | ------ | --------------------------- |
| Cold     | HierarchyResolver | 104 ms | 216 ms | 554 ms | 829 ms | 1202 ms | Scales with design size, roughly linear |

