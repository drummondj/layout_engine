#pragma once

// One benchmark executable (pipeline_benchmarks), one real main()
// (benchmark_main.cpp) - each stage gets its own .cpp registering its own
// benchmarks via benchmark::RegisterBenchmark rather than the BENCHMARK()
// macro (which would need its own translation-unit-local static
// registration, fine for one file but this executable's whole point is
// growing to cover every Cold-tier stage in one binary).

namespace le::benchmarks
{
    void register_layer_generation_benchmarks();
    void register_hierarchy_resolver_benchmarks();
    void register_viewport_cull_benchmarks();
    void register_rasterize_benchmarks();
    void register_rasterize_blend2d_benchmarks();
    void register_compose_benchmarks();
    void register_warm_tier_benchmarks();
}
