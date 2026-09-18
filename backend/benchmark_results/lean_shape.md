## Cold Start Zoom-Fit

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 18.9 us | 18.5 us | 18.5 us | 17.8 us | 17.5 us | 18.1 us | 18.1 us |
| HierarchyResolver | 175.4 ms | 305.5 ms | 753.2 ms | 962.7 ms | 1.67 s | 9.1 us | 140.2 ms |
| ViewportCull | 2.8 us | 8.4 us | 3.1 us | 9.6 us | 2.9 us | 2.2 us | 6.5 us |
| Rasterize | 68.5 ms | 114.9 ms | 214.7 ms | 306.7 ms | 459.8 ms | 2.3 ms | 45.4 ms |
| Compose | 2.7 ms | 2.8 ms | 793.1 us | 709.2 us | 795.1 us | 852.7 us | 2.5 ms |

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
| HierarchyResolver | 70.0 MB | 145.0 MB | 289.6 MB | 434.1 MB | 650.8 MB | 232 B | 85.1 MB |
| ViewportCull | 56 B | 56 B | 56 B | 56 B | 56 B | 56 B | 3.0 KB |
| Rasterize | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.9 MB |
| Compose | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB |

### Process memory

| Test case | Peak RSS | Peak swap |
| --- | --- | --- |
| 1x1 | 329.9 MB | 0 |
| 2x1 | 537.6 MB | 0 |
| 2x2 | 916.4 MB | 0 |
| 3x2 | 1309.4 MB | 0 |
| 3x3 | 1919.4 MB | 0 |
| 5x5 (d=0) | 149.9 MB | 0 |
| 5x5 (d=1) | 352.3 MB | 0 |

## Zoom-In

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| HierarchyResolver | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| ViewportCull | 3.2 us | 3.5 us | 3.4 us | 3.6 us | 3.6 us | 1.2 us | 5.8 us |
| Rasterize | 80.1 ms | 72.2 ms | 93.5 ms | 175.0 ms | 203.7 ms | 260.4 us | 45.5 ms |
| Compose | 595.7 us | 551.4 us | 606.1 us | 608.7 us | 3.5 ms | 923.9 us | 2.6 ms |

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
| HierarchyResolver | 70.0 MB | 145.0 MB | 289.6 MB | 434.1 MB | 650.8 MB | 232 B | 85.1 MB |
| ViewportCull | 56 B | 56 B | 56 B | 56 B | 56 B | 56 B | 1.2 KB |
| Rasterize | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 4.3 MB |
| Compose | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB |

### Process memory

| Test case | Peak RSS | Peak swap |
| --- | --- | --- |
| 1x1 | 336.8 MB | 0 |
| 2x1 | 553.6 MB | 0 |
| 2x2 | 922.4 MB | 0 |
| 3x2 | 1321.4 MB | 0 |
| 3x3 | 1919.4 MB | 0 |
| 5x5 (d=0) | 153.9 MB | 0 |
| 5x5 (d=1) | 376.3 MB | 0 |

## Final Zoom-Fit

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| HierarchyResolver | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| ViewportCull | 3.4 us | 3.5 us | 3.5 us | 3.5 us | 3.5 us | 1.0 us | 8.0 us |
| Rasterize | 52.1 ms | 84.6 ms | 184.1 ms | 272.6 ms | 396.4 ms | 261.2 us | 45.0 ms |
| Compose | 597.4 us | 583.4 us | 606.1 us | 662.0 us | 603.6 us | 455.3 us | 2.1 ms |

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
| HierarchyResolver | 70.0 MB | 145.0 MB | 289.6 MB | 434.1 MB | 650.8 MB | 232 B | 85.1 MB |
| ViewportCull | 56 B | 56 B | 56 B | 56 B | 56 B | 56 B | 3.0 KB |
| Rasterize | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.9 MB |
| Compose | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB | 3.8 MB |

### Process memory

| Test case | Peak RSS | Peak swap |
| --- | --- | --- |
| 1x1 | 352.8 MB | 0 |
| 2x1 | 555.6 MB | 0 |
| 2x2 | 948.8 MB | 0 |
| 3x2 | 1321.4 MB | 0 |
| 3x3 | 1944.1 MB | 0 |
| 5x5 (d=0) | 153.9 MB | 0 |
| 5x5 (d=1) | 376.3 MB | 0 |
