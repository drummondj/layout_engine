#include "pipeline_benchmarks.hpp"

#include <benchmark/benchmark.h>

int main(int argc, char **argv)
{
    le::benchmarks::register_layer_generation_benchmarks();
    le::benchmarks::register_hierarchy_resolver_benchmarks();
    le::benchmarks::register_viewport_cull_benchmarks();
    le::benchmarks::register_rasterize_blend2d_benchmarks();
    le::benchmarks::register_compose_benchmarks();
    le::benchmarks::register_warm_tier_benchmarks();
    le::benchmarks::register_shape_ops_benchmarks();

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
