
Commit: 12bd236a4e8399eede4f4722e59d9928fdecd639

| Pipeline | Stage | 1x1 | 2x1 | 2x2 | 3x2 | 3x3 | Comments |
| -------- | ----- | --- | --- | --- | --- | --- | -------- |
| Cold     | LayerGeneration | 79.7 us | 79.2 us | 79.0 us | 78.6 us | 79.4 us | Flat/O(1) across all 5 tile sizes, as expected - this stage rebuilds ViewLayerSet from the Technology's own layer count alone (ViewLayerSet::build_for_technology), never from Layout/design size, so a 9x larger DEF (1x1 -> 3x3) costs nothing extra. Forced cold recompute per iteration (fresh SynchronousStageRunner each time, see layer_generation_benchmark.cpp); a Warm-tier cache hit is far cheaper still. Release build, `./build_release/pipeline_benchmarks --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`, mean of 5 reps. |

