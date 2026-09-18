## Cold Start Zoom-Fit

| Stage             | 1x1      | 2x1      | 2x2      | 3x2      | 3x3      | 5x5 (d=0) | 5x5 (d=1) |
| ----------------- | -------- | -------- | -------- | -------- | -------- | --------- | --------- |
| LayerGeneration   | 33.1 us  | 26.5 us  | 222.1 us | 173.8 us | 47.2 us  | 26.0 us   | 18.5 us   |
| HierarchyResolver | 365.2 ms | 717.9 ms | 1.33 s   | 2.22 s   | 3.32 s   | 16.9 us   | 257.8 ms  |
| ViewportCull      | 2.2 us   | 2.5 us   | 10.2 us  | 2.3 us   | 2.6 us   | 2.0 us    | 6.6 us    |
| Rasterize         | 69.7 ms  | 118.4 ms | 236.9 ms | 391.0 ms | 545.5 ms | 4.8 ms    | 62.6 ms   |
| Compose           | 952.0 us | 941.6 us | 1.4 ms   | 2.0 ms   | 958.4 us | 1.1 ms    | 3.0 ms    |

### Cache objects

| Stage             | 1x1     | 2x1     | 2x2       | 3x2       | 3x3       | 5x5 (d=0) | 5x5 (d=1) |
| ----------------- | ------- | ------- | --------- | --------- | --------- | --------- | --------- |
| LayerGeneration   | 139     | 139     | 139       | 139       | 139       | 139       | 139       |
| HierarchyResolver | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1         | 594,002   |
| ViewportCull      | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1         | 594,002   |
| Rasterize         | 1       | 1       | 1         | 1         | 1         | 1         | 2         |
| Compose           | 1       | 1       | 1         | 1         | 1         | 1         | 1         |

### Cache bytes (approximate)

| Stage             | 1x1      | 2x1      | 2x2      | 3x2     | 3x3     | 5x5 (d=0) | 5x5 (d=1) |
| ----------------- | -------- | -------- | -------- | ------- | ------- | --------- | --------- |
| LayerGeneration   | 13.0 KB  | 13.0 KB  | 13.0 KB  | 13.0 KB | 13.0 KB | 13.0 KB   | 13.0 KB   |
| HierarchyResolver | 208.7 MB | 422.4 MB | 844.3 MB | 1.24 GB | 1.85 GB | 536 B     | 257.3 MB  |
| ViewportCull      | 208.7 MB | 422.4 MB | 844.3 MB | 1.24 GB | 1.85 GB | 536 B     | 257.3 MB  |
| Rasterize         | 3.8 MB   | 3.8 MB   | 3.8 MB   | 3.8 MB  | 3.8 MB  | 3.8 MB    | 3.9 MB    |
| Compose           | 3.8 MB   | 3.8 MB   | 3.8 MB   | 3.8 MB  | 3.8 MB  | 3.8 MB    | 3.8 MB    |

### Process memory

| Test case | Peak RSS  | Peak swap |
| --------- | --------- | --------- |
| 1x1       | 523.9 MB  | 0         |
| 2x1       | 931.0 MB  | 0         |
| 2x2       | 1812.8 MB | 0         |
| 3x2       | 2336.8 MB | 0         |
| 3x3       | 3475.1 MB | 0         |
| 5x5 (d=0) | 157.6 MB  | 0         |
| 5x5 (d=1) | 592.6 MB  | 0         |

## Zoom-In

| Stage             | 1x1           | 2x1           | 2x2           | 3x2           | 3x3           | 5x5 (d=0)     | 5x5 (d=1)     |
| ----------------- | ------------- | ------------- | ------------- | ------------- | ------------- | ------------- | ------------- |
| LayerGeneration   | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| HierarchyResolver | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| ViewportCull      | 3.6 us        | 4.1 us        | 18.7 us       | 11.6 us       | 4.3 us        | 2.1 us        | 7.6 us        |
| Rasterize         | 104.3 ms      | 88.7 ms       | 120.2 ms      | 223.3 ms      | 262.5 ms      | 580.5 us      | 59.7 ms       |
| Compose           | 1.7 ms        | 728.4 us      | 1.1 ms        | 2.0 ms        | 800.5 us      | 903.2 us      | 3.0 ms        |

### Cache objects

| Stage             | 1x1     | 2x1     | 2x2       | 3x2       | 3x3       | 5x5 (d=0) | 5x5 (d=1) |
| ----------------- | ------- | ------- | --------- | --------- | --------- | --------- | --------- |
| LayerGeneration   | 139     | 139     | 139       | 139       | 139       | 139       | 139       |
| HierarchyResolver | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1         | 594,002   |
| ViewportCull      | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1         | 593,986   |
| Rasterize         | 1       | 1       | 1         | 1         | 1         | 1         | 2         |
| Compose           | 1       | 1       | 1         | 1         | 1         | 1         | 1         |

### Cache bytes (approximate)

| Stage             | 1x1      | 2x1      | 2x2      | 3x2     | 3x3     | 5x5 (d=0) | 5x5 (d=1) |
| ----------------- | -------- | -------- | -------- | ------- | ------- | --------- | --------- |
| LayerGeneration   | 13.0 KB  | 13.0 KB  | 13.0 KB  | 13.0 KB | 13.0 KB | 13.0 KB   | 13.0 KB   |
| HierarchyResolver | 208.7 MB | 422.4 MB | 844.3 MB | 1.24 GB | 1.85 GB | 536 B     | 257.3 MB  |
| ViewportCull      | 208.7 MB | 422.4 MB | 844.3 MB | 1.24 GB | 1.85 GB | 536 B     | 257.3 MB  |
| Rasterize         | 3.8 MB   | 3.8 MB   | 3.8 MB   | 3.8 MB  | 3.8 MB  | 3.8 MB    | 4.3 MB    |
| Compose           | 3.8 MB   | 3.8 MB   | 3.8 MB   | 3.8 MB  | 3.8 MB  | 3.8 MB    | 3.8 MB    |

### Process memory

| Test case | Peak RSS  | Peak swap |
| --------- | --------- | --------- |
| 1x1       | 535.9 MB  | 0         |
| 2x1       | 931.2 MB  | 0         |
| 2x2       | 1812.8 MB | 0         |
| 3x2       | 2350.1 MB | 0         |
| 3x3       | 3496.1 MB | 0         |
| 5x5 (d=0) | 163.7 MB  | 0         |
| 5x5 (d=1) | 616.9 MB  | 0         |

## Final Zoom-Fit

| Stage             | 1x1           | 2x1           | 2x2           | 3x2           | 3x3           | 5x5 (d=0)     | 5x5 (d=1)     |
| ----------------- | ------------- | ------------- | ------------- | ------------- | ------------- | ------------- | ------------- |
| LayerGeneration   | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| HierarchyResolver | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| ViewportCull      | 3.2 us        | 3.2 us        | 4.3 us        | 3.4 us        | 3.4 us        | 1.2 us        | 7.6 us        |
| Rasterize         | 63.3 ms       | 107.5 ms      | 248.9 ms      | 344.6 ms      | 537.2 ms      | 493.8 us      | 57.3 ms       |
| Compose           | 804.1 us      | 819.0 us      | 882.9 us      | 804.6 us      | 910.6 us      | 724.4 us      | 2.9 ms        |

### Cache objects

| Stage             | 1x1     | 2x1     | 2x2       | 3x2       | 3x3       | 5x5 (d=0) | 5x5 (d=1) |
| ----------------- | ------- | ------- | --------- | --------- | --------- | --------- | --------- |
| LayerGeneration   | 139     | 139     | 139       | 139       | 139       | 139       | 139       |
| HierarchyResolver | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1         | 594,002   |
| ViewportCull      | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1         | 594,002   |
| Rasterize         | 1       | 1       | 1         | 1         | 1         | 1         | 2         |
| Compose           | 1       | 1       | 1         | 1         | 1         | 1         | 1         |

### Cache bytes (approximate)

| Stage             | 1x1      | 2x1      | 2x2      | 3x2     | 3x3     | 5x5 (d=0) | 5x5 (d=1) |
| ----------------- | -------- | -------- | -------- | ------- | ------- | --------- | --------- |
| LayerGeneration   | 13.0 KB  | 13.0 KB  | 13.0 KB  | 13.0 KB | 13.0 KB | 13.0 KB   | 13.0 KB   |
| HierarchyResolver | 208.7 MB | 422.4 MB | 844.3 MB | 1.24 GB | 1.85 GB | 536 B     | 257.3 MB  |
| ViewportCull      | 208.7 MB | 422.4 MB | 844.3 MB | 1.24 GB | 1.85 GB | 536 B     | 257.3 MB  |
| Rasterize         | 3.8 MB   | 3.8 MB   | 3.8 MB   | 3.8 MB  | 3.8 MB  | 3.8 MB    | 3.9 MB    |
| Compose           | 3.8 MB   | 3.8 MB   | 3.8 MB   | 3.8 MB  | 3.8 MB  | 3.8 MB    | 3.8 MB    |

### Process memory

| Test case | Peak RSS  | Peak swap |
| --------- | --------- | --------- |
| 1x1       | 548.1 MB  | 0         |
| 2x1       | 931.2 MB  | 0         |
| 2x2       | 1812.8 MB | 0         |
| 3x2       | 2366.1 MB | 0         |
| 3x3       | 3496.9 MB | 0         |
| 5x5 (d=0) | 163.7 MB  | 0         |
| 5x5 (d=1) | 628.9 MB  | 0         |
