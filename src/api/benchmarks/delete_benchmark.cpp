// Deleting many children of one parent through the generated edit ops,
// as a Tcl foreach or a selection delete does, and a cascade:
//
//  - BM_DeleteEveryRoute: every Route of one Layout, oldest first.
//  - BM_DeleteEveryOtherRoute: half of them, scattered through the list.
//  - BM_DeleteLayoutCascade: delete_layout over N Routes and N Placements.
//
// Each runs inside one recording transaction, as one typed command would.
// The time per object should stay flat as N grows; a quadratic child-list
// removal shows as time per object growing with N.

#include "../edit_ops.hpp"
#include "../le_handle.hpp"

#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

using namespace le;

namespace
{
    struct Design
    {
        std::unique_ptr<LeHandle> handle = std::make_unique<LeHandle>();
        LayoutId layout;
        std::vector<RouteId> routes;

        explicit Design(int64_t n)
        {
            Root &root = handle->root;
            const LibraryId library = root.create_library(LibraryData{.name = "lib"});
            const DesignId cell = root.create_design(DesignData{.library = library, .name = "cell"});
            layout = root.create_layout(LayoutData{.design = root.create_design(DesignData{.library = library, .name = "top"})});
            routes.reserve(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i)
            {
                routes.push_back(root.create_route(RouteData{.layout = layout, .name = "n" + std::to_string(i)}));
                root.create_placement(PlacementData{.layout = layout, .name = "u" + std::to_string(i), .reference_design = cell});
            }
        }
    };

    template <class Body>
    void run(benchmark::State &state, Body &&body)
    {
        for (auto _ : state)
        {
            state.PauseTiming();
            auto design = std::make_unique<Design>(state.range(0));
            design->handle->command_history.begin("delete");
            state.ResumeTiming();
            body(*design);
            state.PauseTiming();
            design->handle->command_history.end(true);
            design.reset();
            state.ResumeTiming();
        }
        state.SetItemsProcessed(state.iterations() * state.range(0));
    }

    void BM_DeleteEveryRoute(benchmark::State &state)
    {
        run(state, [](Design &d) {
            for (const RouteId id : d.routes)
                benchmark::DoNotOptimize(edit::delete_route(*d.handle, id));
        });
    }

    void BM_DeleteEveryOtherRoute(benchmark::State &state)
    {
        run(state, [](Design &d) {
            for (size_t i = 0; i < d.routes.size(); i += 2)
                benchmark::DoNotOptimize(edit::delete_route(*d.handle, d.routes[i]));
        });
    }

    void BM_DeleteLayoutCascade(benchmark::State &state)
    {
        run(state, [](Design &d) { benchmark::DoNotOptimize(edit::delete_layout(*d.handle, d.layout)); });
    }
}

BENCHMARK(BM_DeleteEveryRoute)->Arg(10'000)->Arg(40'000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_DeleteEveryOtherRoute)->Arg(10'000)->Arg(40'000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_DeleteLayoutCascade)->Arg(10'000)->Arg(40'000)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
