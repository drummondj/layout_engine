## Cold Start Zoom-Fit

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 19.6 us | 19.6 us | 21.0 us | 19.0 us | 18.3 us | 18.6 us | 19.4 us |
| HierarchyResolver | 297.0 ms | 584.4 ms | 1.21 s | 1.64 s | 2.67 s | 9.4 us | 302.5 ms |
| ViewportCull | 2.2 us | 10.0 us | 2.4 us | 2.4 us | 2.3 us | 2.0 us | 6.4 us |
| Rasterize | 64.3 ms | 101.6 ms | 194.1 ms | 294.5 ms | 439.1 ms | 2.6 ms | 51.8 ms |
| Compose | 693.3 us | 1.1 ms | 789.1 us | 711.2 us | 760.9 us | 885.9 us | 3.4 ms |

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
| 1x1 | 506.6 MB | 0 |
| 2x1 | 959.4 MB | 0 |
| 2x2 | 1829.7 MB | 0 |
| 3x2 | 2298.3 MB | 0 |
| 3x3 | 3468.7 MB | 0 |
| 5x5 (d=0) | 149.8 MB | 0 |
| 5x5 (d=1) | 544.1 MB | 0 |

## Zoom-In

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| HierarchyResolver | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| ViewportCull | 3.3 us | 3.9 us | 4.5 us | 2.9 us | 3.9 us | 1.7 us | 7.8 us |
| Rasterize | 83.6 ms | 72.7 ms | 92.1 ms | 171.3 ms | 208.7 ms | 262.7 us | 47.4 ms |
| Compose | 3.1 ms | 586.4 us | 638.1 us | 593.1 us | 680.5 us | 746.5 us | 2.6 ms |

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
| 1x1 | 528.6 MB | 0 |
| 2x1 | 959.4 MB | 0 |
| 2x2 | 1829.7 MB | 0 |
| 3x2 | 2312.8 MB | 0 |
| 3x3 | 3471.2 MB | 0 |
| 5x5 (d=0) | 153.8 MB | 0 |
| 5x5 (d=1) | 564.1 MB | 0 |

## Final Zoom-Fit

| Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | 5x5 (d=0) | 5x5 (d=1) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LayerGeneration | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| HierarchyResolver | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) | 0 ms (cached) |
| ViewportCull | 3.7 us | 4.4 us | 4.0 us | 3.5 us | 3.6 us | 1.0 us | 8.0 us |
| Rasterize | 51.1 ms | 89.6 ms | 195.0 ms | 275.0 ms | 421.1 ms | 259.9 us | 46.6 ms |
| Compose | 1.4 ms | 726.7 us | 752.7 us | 616.4 us | 757.9 us | 476.1 us | 2.1 ms |

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
| 1x1 | 540.6 MB | 0 |
| 2x1 | 959.4 MB | 0 |
| 2x2 | 1829.7 MB | 0 |
| 3x2 | 2312.8 MB | 0 |
| 3x3 | 3471.2 MB | 0 |
| 5x5 (d=0) | 153.8 MB | 0 |
| 5x5 (d=1) | 576.1 MB | 0 |
