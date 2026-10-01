#include "../../core/placement_geometry.hpp"
#include "../../geometry/geometry.hpp"
#include "../../io/def_reader.hpp"
#include "../../io/lef_reader.hpp"
#include <boost/geometry/index/rtree.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace le;
namespace bgi = boost::geometry::index;

namespace
{
    using Clock = std::chrono::steady_clock;
    double elapsed_ms(Clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    // area_fraction of the die's own area, centered on it - a stand-in
    // for "the visible viewport in dbu space" at a given zoom level (1.0
    // == fully zoomed out, the whole die visible; 0.001 == zoomed way
    // in, a tenth of a percent of the die's own area visible - a
    // realistic deep-pan/zoom state on a large tiled design).
    Rect centered_window(const Rect &die, double area_fraction)
    {
        const double side_fraction = std::sqrt(area_fraction);
        const int64_t cx = (die.ll.x + die.ur.x) / 2;
        const int64_t cy = (die.ll.y + die.ur.y) / 2;
        const int64_t hw = static_cast<int64_t>(static_cast<double>(die.ur.x - die.ll.x) * side_fraction / 2.0);
        const int64_t hh = static_cast<int64_t>(static_cast<double>(die.ur.y - die.ll.y) * side_fraction / 2.0);
        return Rect{.ll = Point{cx - hw, cy - hh}, .ur = Point{cx + hw, cy + hh}};
    }
}

// Dev-only benchmark (BUGS_AND_ENHANCEMENTS.md E31 zoom/pan-tick
// follow-up): quantifies the win of a spatial index (Boost.Geometry
// Index rtree, bulk-loaded from Placement world bboxes) over
// HierarchyResolver::discover_layout_children's own current unconditional
// per-placement linear scan (hierarchy_resolver.hpp) - the walk shown to
// dominate every scale/pan-only "zoom tick" render, since it reruns in
// full on every such call regardless of ensure_epoch's own scale-drift
// tolerance.
//
// Deliberately measures the two costs discover_layout_children's own
// per-tick budget would actually split into under a spatial-index
// design: an rtree only pays off if it's built ONCE (keyed on
// root.mutation_version(), rebuilt only on a real database edit - Root
// never changes between pan/zoom ticks) and reused across every
// view-only call, so "build" and "query" are reported as two separate
// numbers, not combined - the real per-tick win is the query number
// alone, compared against the linear scan's own per-tick cost (which
// pays the equivalent of a fresh "build" every single tick today).
//
//   ./spatial_index_benchmark <tech.lef> <design.def>
int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <tech.lef> <design.def>\n", argv[0]);
        return 1;
    }

    Root root;
    LEFReader lef_reader;
    if (lef_reader.read_lef(argv[1], root, "tech") != 0)
    {
        fprintf(stderr, "Failed to read LEF '%s'\n", argv[1]);
        return 1;
    }
    DEFReader def_reader;
    if (def_reader.read_def(argv[2], root, "design_lib") != 0)
    {
        fprintf(stderr, "Failed to read DEF '%s'\n", argv[2]);
        return 1;
    }

    LayoutId layout_id;
    for (const DesignId design_id : root.get_design_ids())
    {
        const LayoutId id = root.get_design_layout(design_id);
        if (id.valid())
        {
            layout_id = id;
            break;
        }
    }
    if (!layout_id.valid())
    {
        fprintf(stderr, "No Design with a Layout found in '%s'\n", argv[2]);
        return 1;
    }

    // remaining_depth=0 - matches render_layout_frame's own top-level
    // discover_layout_children call at hierarchy_depth=1 (profile_tiled_render's
    // own default), the case the zoom-tick benchmark actually measured.
    constexpr int kRemainingDepth = 0;

    const auto &placement_ids = root.get_layout_placements(layout_id);

    auto t0 = Clock::now();
    std::vector<std::pair<Rect, PlacementId>> entries;
    entries.reserve(placement_ids.size());
    for (PlacementId pid : placement_ids)
        if (const std::optional<Rect> bbox = placement_world_bbox(root, pid, kRemainingDepth))
            entries.push_back({*bbox, pid});
    const double bbox_compute_ms = elapsed_ms(t0);

    t0 = Clock::now();
    const bgi::rtree<std::pair<Rect, PlacementId>, bgi::rstar<16>> rtree(entries);
    const double rtree_build_ms = elapsed_ms(t0);

    printf("Placements: %zu (resolved: %zu), bbox_compute_ms: %.3f, rtree_build_ms: %.3f, rtree_build_total_ms: %.3f\n",
           placement_ids.size(), entries.size(), bbox_compute_ms, rtree_build_ms, bbox_compute_ms + rtree_build_ms);

    const Rect die = layout_declared_bbox(root, layout_id);

    constexpr int kReps = 5;
    printf("CSV,area_fraction,linear_matches,rtree_matches,linear_scan_ms,rtree_query_ms,speedup\n");
    for (const double fraction : {1.0, 0.25, 0.05, 0.01, 0.001, 0.0001})
    {
        const Rect window = centered_window(die, fraction);

        // Linear scan - mirrors discover_layout_children's own current
        // per-tick cost: recompute-or-read every placement's bbox and
        // test it against the viewport, unconditionally, every call.
        double linear_total_ms = 0.0;
        size_t linear_matches = 0;
        for (int r = 0; r < kReps; r++)
        {
            t0 = Clock::now();
            size_t count = 0;
            for (const auto &[bbox, pid] : entries)
                if (bg::intersects(window, bbox))
                    count++;
            linear_total_ms += elapsed_ms(t0);
            linear_matches = count;
        }
        const double linear_avg_ms = linear_total_ms / kReps;

        // rtree query against the ALREADY-BUILT index - the real per-tick
        // cost under a spatial-index design (build happens once, off the
        // per-tick path entirely).
        double rtree_total_ms = 0.0;
        size_t rtree_matches = 0;
        for (int r = 0; r < kReps; r++)
        {
            std::vector<std::pair<Rect, PlacementId>> results;
            t0 = Clock::now();
            rtree.query(bgi::intersects(window), std::back_inserter(results));
            rtree_total_ms += elapsed_ms(t0);
            rtree_matches = results.size();
        }
        const double rtree_avg_ms = rtree_total_ms / kReps;

        printf("CSV,%.4f,%zu,%zu,%.4f,%.4f,%.2f\n",
               fraction, linear_matches, rtree_matches, linear_avg_ms, rtree_avg_ms,
               rtree_avg_ms > 0.0 ? linear_avg_ms / rtree_avg_ms : 0.0);
    }

    return 0;
}
