## Cold Start Zoom-Fit

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 34.9 us | 40.2 us | 199.2 us | 1.1 ms | 17.8 us | 19.4 us | 44.9 us |
| HierarchyResolver | 374.7 ms | 743.7 ms | 1.35 s | 2.34 s | 3.68 s | 10.2 us | 246.8 ms |
| ViewportCull | 7.9 us | 2.9 us | 2.5 us | 8.3 us | 3.2 us | 1.7 us | 6.4 us |
| Rasterize | 73.2 ms | 122.7 ms | 235.1 ms | 398.2 ms | 641.3 ms | 4.4 ms | 60.3 ms |
| Compose | 1.1 ms | 929.1 us | 1.5 ms | 1.1 ms | 1.1 ms | 1.4 ms | 3.0 ms |

### Cache objects

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 139 | 139 | 139 | 139 | 139 | 139 | 139 |
| HierarchyResolver | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1 | 594,002 |
| ViewportCull | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1 | 594,002 |
| Rasterize | 1 | 1 | 1 | 1 | 1 | 1 | 2 |
| Compose | 1 | 1 | 1 | 1 | 1 | 1 | 1 |

### Cache bytes (approximate)

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB |
| HierarchyResolver | 208.7 MB | 422.4 MB | 844.3 MB | 1.24 GB | 1.85 GB | 536 B | 257.3 MB |
| ViewportCull | 56 B | 56 B | 56 B | 56 B | 56 B | 56 B | 3.0 KB |
| Rasterize | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.9 MB |
| Compose | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB |

### Process memory

| Test case | Peak RSS | Peak swap |
| --- | --- | --- |
| 1x1 | 532.2 MB | 0 |
| 2x1 | 938.8 MB | 0 |
| 2x2 | 1809.9 MB | 0 |
| 3x2 | 2340.8 MB | 0 |
| 3x3 | 3490.5 MB | 0 |
| 5x5 (d=0) | 155.5 MB | 0 |
| 5x5 (d=1) | 570.4 MB | 0 |

## Zoom-In

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| HierarchyResolver | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| ViewportCull | 17.9 us | 18.2 us | 4.5 us | 4.4 us | 3.6 us | 2.5 us | 8.2 us |
| Rasterize | 106.6 ms | 90.3 ms | 121.2 ms | 231.9 ms | 287.5 ms | 627.1 us | 59.0 ms |
| Compose | 3.5 ms | 801.4 us | 813.4 us | 908.4 us | 834.3 us | 978.4 us | 2.9 ms |

### Cache objects

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 139 | 139 | 139 | 139 | 139 | 139 | 139 |
| HierarchyResolver | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1 | 594,002 |
| ViewportCull | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1 | 593,986 |
| Rasterize | 1 | 1 | 1 | 1 | 1 | 1 | 2 |
| Compose | 1 | 1 | 1 | 1 | 1 | 1 | 1 |

### Cache bytes (approximate)

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB |
| HierarchyResolver | 208.7 MB | 422.4 MB | 844.3 MB | 1.24 GB | 1.85 GB | 536 B | 257.3 MB |
| ViewportCull | 56 B | 56 B | 56 B | 56 B | 56 B | 56 B | 1.2 KB |
| Rasterize | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 4.3 MB |
| Compose | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB |

### Process memory

| Test case | Peak RSS | Peak swap |
| --- | --- | --- |
| 1x1 | 544.2 MB | 0 |
| 2x1 | 939.3 MB | 0 |
| 2x2 | 1809.9 MB | 0 |
| 3x2 | 2356.7 MB | 0 |
| 3x3 | 3498.4 MB | 0 |
| 5x5 (d=0) | 161.5 MB | 0 |
| 5x5 (d=1) | 594.4 MB | 0 |

## Final Zoom-Fit

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| HierarchyResolver | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| ViewportCull | 3.6 us | 3.8 us | 4.2 us | 3.9 us | 12.1 us | 1.9 us | 7.7 us |
| Rasterize | 64.1 ms | 111.5 ms | 220.3 ms | 351.5 ms | 526.0 ms | 543.1 us | 56.2 ms |
| Compose | 827.1 us | 834.1 us | 790.6 us | 1.0 ms | 756.7 us | 619.9 us | 2.5 ms |

### Cache objects

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 139 | 139 | 139 | 139 | 139 | 139 | 139 |
| HierarchyResolver | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1 | 594,002 |
| ViewportCull | 478,380 | 956,735 | 1,913,445 | 2,870,155 | 4,305,220 | 1 | 594,002 |
| Rasterize | 1 | 1 | 1 | 1 | 1 | 1 | 2 |
| Compose | 1 | 1 | 1 | 1 | 1 | 1 | 1 |

### Cache bytes (approximate)

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB | 13.0 KB |
| HierarchyResolver | 208.7 MB | 422.4 MB | 844.3 MB | 1.24 GB | 1.85 GB | 536 B | 257.3 MB |
| ViewportCull | 56 B | 56 B | 56 B | 56 B | 56 B | 56 B | 3.0 KB |
| Rasterize | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.9 MB |
| Compose | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB |

### Process memory

| Test case | Peak RSS | Peak swap |
| --- | --- | --- |
| 1x1 | 556.7 MB | 0 |
| 2x1 | 947.8 MB | 0 |
| 2x2 | 1809.9 MB | 0 |
| 3x2 | 2372.0 MB | 0 |
| 3x3 | 3514.9 MB | 0 |
| 5x5 (d=0) | 161.5 MB | 0 |
| 5x5 (d=1) | 608.4 MB | 0 |
