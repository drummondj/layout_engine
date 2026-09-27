// resolver_profile - dev-only tool (incremental HierarchyResolver plan, step
// 0): where a cold HierarchyResolverStage::compute() spends its time on one
// real design, and what an edit costs downstream today. One design per
// process, so the reported peak RSS is that design's alone:
//
//   resolver_profile <aes_scaling label, e.g. 4x4> [repeats=3]
//
// Loads NangateOpenCellLibrary.lef + test_data/aes_scaling_<label>.def,
// then reports (one "metric value" line each, times in ms, medians over
// `repeats`):
//   - load time and RSS after load;
//   - the cold resolve's total and per-phase times (ResolverPhaseProfile),
//     and the time to free a previous output (paid on every recompute);
//   - output size: RenderShapes per purpose, rtree entries, estimated bytes;
//   - per viewport (zoom-fit and zoomed-in): ViewportCull cold (its per-node
//     placement index rebuilt, as after every resolve) vs warm, and the
//     first Rasterize after a resolve (route-outline cache empty) vs a warm
//     one;
//   - per edit (a placement move, a route shape edit, near the die center):
//     the resolve after it (incremental when the Root change log allows),
//     and the first zoomed-in ViewportCull/Rasterize after it.
// Not run by ctest.

#include "../../core/placement_geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../stages/hierarchy_resolver_stage.hpp"
#include "../stages/rasterize_blend2d_stage.hpp"
#include "../stages/viewport_cull_stage.hpp"
#include "../tests/synchronous_stage_runner.hpp"
#include "aes_scaling_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace le;
using namespace le::benchmarks;

namespace
{
    using HierarchyResolverRunner = SynchronousStageRunner<HierarchyResolverStage, ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using ViewportCullRunner = SynchronousStageRunner<ViewportCullStage, HierarchyResolverStage::OutputHandle, HierarchyResolverOutput, ViewRenderOptions>;
    using RasterizeRunner = SynchronousStageRunner<RasterizeBlend2DStage, HierarchyResolverStage::OutputHandle, RasterizeOutput, ViewRenderOptions>;

    double elapsed_ms(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    double median(std::vector<double> values)
    {
        if (values.empty())
            return 0.0;
        std::ranges::sort(values);
        return values[values.size() / 2];
    }

    void report(const std::string &metric, double value)
    {
        std::printf("%s %.3f\n", metric.c_str(), value);
    }

    const char *purpose_name(ViewLayerPurpose purpose)
    {
        switch (purpose)
        {
        case ViewLayerPurpose::TERMINAL: return "TERMINAL";
        case ViewLayerPurpose::OBSTRUCTION: return "OBSTRUCTION";
        case ViewLayerPurpose::BOUNDARY: return "BOUNDARY";
        case ViewLayerPurpose::TRACK_PREFERRED: return "TRACK_PREFERRED";
        case ViewLayerPurpose::TRACK_NON_PREFERRED: return "TRACK_NON_PREFERRED";
        case ViewLayerPurpose::ROUTING_BLOCKAGE: return "ROUTING_BLOCKAGE";
        case ViewLayerPurpose::ROW: return "ROW";
        case ViewLayerPurpose::GCELLGRID: return "GCELLGRID";
        case ViewLayerPurpose::PLACEMENT_BLOCKAGE: return "PLACEMENT_BLOCKAGE";
        case ViewLayerPurpose::ROUTE: return "ROUTE";
        case ViewLayerPurpose::REGION: return "REGION";
        case ViewLayerPurpose::PLACEMENT: return "PLACEMENT";
        case ViewLayerPurpose::CUSTOM_SHAPE: return "CUSTOM_SHAPE";
        case ViewLayerPurpose::DEBUG: return "DEBUG";
        case ViewLayerPurpose::FLIGHTLINE: return "FLIGHTLINE";
        case ViewLayerPurpose::PORT_MARKER: return "PORT_MARKER";
        }
        return "UNKNOWN";
    }

    // ViewRenderOptions for `viewport` in a 1280-px-wide window, with the
    // purposes the GUI hides by default hidden (LeHandle's own defaults).
    ViewRenderOptions view_options(const ViewRenderOptions &cold, Rect viewport)
    {
        ViewRenderOptions options = cold;
        options.viewport = viewport;
        options.scale = 1280.0 / static_cast<double>(std::max<int64_t>(viewport.ur.x - viewport.ll.x, 1));
        options.purpose_visible = {
            {ViewLayerPurpose::TRACK_PREFERRED, false},
            {ViewLayerPurpose::TRACK_NON_PREFERRED, false},
            {ViewLayerPurpose::ROW, false},
            {ViewLayerPurpose::GCELLGRID, false},
            {ViewLayerPurpose::FLIGHTLINE, false},
        };
        return options;
    }

    Rect shifted(Rect r, int64_t dx)
    {
        r.ll.x += dx;
        r.ur.x += dx;
        return r;
    }

    // ViewportCull and Rasterize timings for one viewport - "cold" is the
    // first run on a freshly resolved output (every per-node cache empty,
    // as after an edit today), "warm" a pan of 1% of the viewport width on
    // the same output.
    void profile_viewport(const std::string &name, const HierarchyResolverStage::OutputHandle &resolved,
                          const ViewRenderOptions &cold, Rect viewport, int repeats)
    {
        const int64_t pan = (viewport.ur.x - viewport.ll.x) / 100;
        std::vector<double> cull_cold, cull_warm, raster_cold, raster_warm;
        for (int r = 0; r < repeats; ++r)
        {
            ViewportCullRunner cull{"profile_cull"};
            RasterizeRunner raster{"profile_rasterize"};
            raster.stage().set_thread_count(4);

            const ViewRenderOptions first = view_options(cold, viewport);
            const ViewRenderOptions second = view_options(cold, shifted(viewport, pan));

            auto start = std::chrono::steady_clock::now();
            cull.run(resolved, 1, first);
            cull_cold.push_back(elapsed_ms(start));
            const HierarchyResolverStage::OutputHandle first_culled = cull.last_handle();

            start = std::chrono::steady_clock::now();
            cull.run(resolved, 1, second);
            cull_warm.push_back(elapsed_ms(start));
            const HierarchyResolverStage::OutputHandle second_culled = cull.last_handle();

            start = std::chrono::steady_clock::now();
            raster.run(first_culled, 1, first);
            raster_cold.push_back(elapsed_ms(start));

            start = std::chrono::steady_clock::now();
            raster.run(second_culled, 2, second);
            raster_warm.push_back(elapsed_ms(start));
        }
        report(name + ".cull_cold_ms", median(cull_cold));
        report(name + ".cull_warm_ms", median(cull_warm));
        report(name + ".rasterize_first_ms", median(raster_cold));
        report(name + ".rasterize_warm_ms", median(raster_warm));
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <aes_scaling label, e.g. 4x4> [repeats=3]\n", argv[0]);
        return 2;
    }
    const std::string label = argv[1];
    const int repeats = argc > 2 ? std::max(1, std::atoi(argv[2])) : 3;

    std::printf("design aes_scaling_%s\n", label.c_str());
    auto start = std::chrono::steady_clock::now();
    AesScalingFixture fixture = load_aes_scaling_fixture(TileConfig{0, 0, label.c_str()});
    report("load_ms", elapsed_ms(start));
    report("rss_after_load_mb", peak_rss_mb());
    report("placements", static_cast<double>(fixture.root.get_layout_placements(fixture.layout_id).size()));
    report("routes", static_cast<double>(fixture.root.get_layout_routes(fixture.layout_id).size()));

    const std::vector<TechnologyId> technology_ids = fixture.root.get_technology_ids();
    const ViewLayerSet view_layers = ViewLayerSet::build_for_technology(fixture.root, technology_ids.front());
    const ViewLayerSetHandle view_layers_handle = std::make_shared<const ViewLayerSet>(view_layers);
    const ViewRenderOptions cold{
        .root = &fixture.root,
        .root_mutation_version = fixture.root.mutation_version(),
        .top_level = HierarchyId{fixture.layout_id},
        .hierarchy_depth = 1,
    };

    // Cold resolves, each on a fresh runner. Every run's output but the
    // last is freed explicitly, timed - a real recompute frees the previous
    // output too.
    std::vector<double> totals, frees;
    std::map<std::string, std::vector<double>> phases;
    std::vector<std::string> phase_order;
    std::optional<HierarchyResolverRunner> runner;
    for (int r = 0; r < repeats; ++r)
    {
        if (runner)
        {
            start = std::chrono::steady_clock::now();
            runner.reset();
            frees.push_back(elapsed_ms(start));
        }
        runner.emplace("profile_resolver");

        ResolverPhaseProfile profile;
        g_resolver_phase_profile.store(&profile);
        start = std::chrono::steady_clock::now();
        runner->run(view_layers_handle, 0, cold);
        totals.push_back(elapsed_ms(start));
        g_resolver_phase_profile.store(nullptr);

        for (const auto &[name, ms] : profile.ms)
        {
            if (!phases.contains(name))
                phase_order.push_back(name);
            phases[name].push_back(ms);
        }
    }
    report("resolve_total_ms", median(totals));
    for (const std::string &name : phase_order)
        report("resolve." + name + "_ms", median(phases[name]));
    if (!frees.empty())
        report("free_previous_output_ms", median(frees));
    report("rss_after_resolve_mb", peak_rss_mb());

    // Output size.
    HierarchyResolverStage::OutputHandle resolved = runner->last_handle();
    const HierarchyResolverOutputStats stats = estimate_hierarchy_resolver_output_stats(*resolved);
    report("output.render_shapes", static_cast<double>(stats.shape_count));
    report("output.shape_bytes_mb", static_cast<double>(stats.shape_bytes) / (1024.0 * 1024.0));
    report("output.overhead_bytes_mb", static_cast<double>(stats.own_overhead_bytes) / (1024.0 * 1024.0));
    std::size_t index_entries = 0;
    std::map<std::string, std::size_t> shapes_by_purpose;
    for (const auto &[id, data] : resolved->view_data)
    {
        for (const ViewShapeChunk &chunk : data.chunks)
        {
            if (chunk.shapes_index)
                for (const auto &[layer, index] : *chunk.shapes_index)
                    index_entries += index.size();
            if (chunk.shapes)
                for (const auto &[layer, shapes] : *chunk.shapes)
                    if (const ViewLayerData *view_layer = view_layers.get(layer))
                        shapes_by_purpose[purpose_name(view_layer->purpose)] += shapes.size();
        }
    }
    report("output.index_entries", static_cast<double>(index_entries));
    for (const auto &[purpose, count] : shapes_by_purpose)
        report("output.shapes." + purpose, static_cast<double>(count));

    // Downstream, per viewport: zoom-fit, and a zoomed-in window 1/10 of the
    // die wide at its center (the editing case).
    const Rect die = layout_declared_bbox(fixture.root, fixture.layout_id);
    const int64_t width = die.ur.x - die.ll.x;
    const int64_t height = die.ur.y - die.ll.y;
    profile_viewport("fit", resolved, cold, die, repeats);
    const Point center{die.ll.x + width / 2, die.ll.y + height / 2};
    const Rect zoomed{.ll = Point{center.x - width / 20, center.y - height / 20}, .ur = Point{center.x + width / 20, center.y + height / 20}};
    profile_viewport("zoom", resolved, cold, zoomed, repeats);
    resolved.reset(); // edits below replace the output; don't keep the original alive

    // Edits near the die center (inside the zoomed-in window), each
    // followed by the resolve (incremental when the change log allows),
    // and the first ViewportCull/Rasterize of the zoomed-in window after
    // it - both warm on that window beforehand, as in an editing session.
    Root &root = fixture.root;
    auto distance = [&](const Rect &r)
    { return std::abs((r.ll.x + r.ur.x) / 2 - center.x) + std::abs((r.ll.y + r.ur.y) / 2 - center.y); };

    PlacementId placement;
    int64_t best = INT64_MAX;
    for (const PlacementId id : root.get_layout_placements(fixture.layout_id))
        if (const PlacementData *p = root.get_placement(id); p && p->location)
            if (const int64_t d = distance(Rect{.ll = *p->location, .ur = *p->location}); d < best)
            {
                best = d;
                placement = id;
            }
    ShapeId route_shape;
    best = INT64_MAX;
    for (const RouteId route : root.get_layout_routes(fixture.layout_id))
        for (const ShapeId id : root.get_route_shapes(route))
            if (const std::optional<Rect> box = Geometry::bbox(*root.get_shape(id)))
                if (const int64_t d = distance(*box); d < best)
                {
                    best = d;
                    route_shape = id;
                }

    auto profile_edit = [&](const std::string &name, auto &&apply_edit)
    {
        ViewRenderOptions zoom_options = view_options(cold, zoomed);
        ViewportCullRunner cull{"profile_edit_cull"};
        RasterizeRunner raster{"profile_edit_rasterize"};
        raster.stage().set_thread_count(4);
        std::uint64_t data_version = 1;
        cull.run(runner->last_handle(), data_version, zoom_options);
        raster.run(cull.last_handle(), data_version, zoom_options);

        std::vector<double> resolve, cull_first, raster_first, raster_second;
        bool incremental = true;
        for (int r = 0; r < repeats; ++r)
        {
            apply_edit(r);
            root.bump_mutation_version();
            ViewRenderOptions options = cold;
            options.root_mutation_version = root.mutation_version();

            auto t = std::chrono::steady_clock::now();
            runner->run(view_layers_handle, 0, options);
            resolve.push_back(elapsed_ms(t));
            incremental = incremental && runner->stage().last_compute_was_incremental();

            ++data_version;
            t = std::chrono::steady_clock::now();
            cull.run(runner->last_handle(), data_version, zoom_options);
            cull_first.push_back(elapsed_ms(t));
            t = std::chrono::steady_clock::now();
            raster.run(cull.last_handle(), data_version, zoom_options);
            raster_first.push_back(elapsed_ms(t));
            // The same frame again (forced): what's left once per-chunk caches refill.
            ++data_version;
            t = std::chrono::steady_clock::now();
            raster.run(cull.last_handle(), data_version, zoom_options);
            raster_second.push_back(elapsed_ms(t));
        }
        report("edit." + name + ".resolve_ms", median(resolve));
        report("edit." + name + ".incremental", incremental ? 1.0 : 0.0);
        report("edit." + name + ".zoom_cull_first_ms", median(cull_first));
        report("edit." + name + ".zoom_rasterize_first_ms", median(raster_first));
        report("edit." + name + ".zoom_rasterize_second_ms", median(raster_second));
    };

    // Move a placement by 2 um and back.
    const double dbu_per_um = root.get_technology(technology_ids.front())->database_units_microns;
    const Point home = *root.get_placement(placement)->location;
    profile_edit("placement_move", [&](int r)
                 {
        const Point to{home.x + ((r % 2 == 0) ? static_cast<int64_t>(2 * dbu_per_um) : 0), home.y};
        root.update_placement(placement, fixture.layout_id, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, to,
                              std::nullopt, std::nullopt, std::nullopt); });

    // Rewrite one route shape's geometry (unchanged values - the resolver
    // can't tell, so the cost is that of a real edit).
    profile_edit("route_shape", [&](int)
                 {
        const ShapeData current = *root.get_shape(route_shape);
        root.update_shape(route_shape, std::nullopt, std::nullopt, current.paths, current.polygons, current.rects, std::nullopt,
                          std::nullopt, std::nullopt); });

    report("rss_peak_mb", peak_rss_mb());
    return 0;
}
