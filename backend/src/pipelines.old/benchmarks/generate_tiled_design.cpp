#include "../../io/def_reader.hpp"
#include "../../io/def_writer.hpp"
#include "../../io/lef_reader.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace le;

// Dev-only data-generation tool, not part of the render pipeline itself
// (BUGS_AND_ENHANCEMENTS.md E31): reads one real LEF+DEF design and tiles
// its own Layout content (COMPONENTS/NETS/SPECIALNETS/ROW, TRACKS/
// GCELLGRID extended rather than duplicated - see below) across an
// tile_x x tile_y grid, translating every coordinate by each tile's own
// (dx, dy) offset, to produce one large *flat* DEF file (real duplicated
// geometry, not DEF's own hierarchical MACRO-placement mechanism E7's
// aes_5x5.def already exercises) for render-performance stress testing/
// Tracy profiling at a scale no real single design in this repo reaches.
//
// All tiled content stays entirely within its own tile - the source
// design's own NETS/SPECIALNETS are already fully self-contained (no
// nets connect outside the die), so there is no cross-tile routing to
// reconcile after translation; the result is tile_x*tile_y independent
// copies of the same design side by side, not one larger interconnected
// design. Placement/Route/Row names get a "_t<tx>_<ty>" suffix per tile
// (DEF names are unique_per_parent) - reference Designs (macro cells)
// and Layers are shared as-is (same Root, no need to re-resolve).
//
//   ./generate_tiled_design <tech.lef> <macro.lef> <design.def> <tile_x> <tile_y> <output.def>
//
// Usage: `cmake --build build_release --target generate_tiled_design && \
//         ./build_release/generate_tiled_design \
//           test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef \
//           test_data/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef \
//           test_data/ISPD22__final_benchmarks/AES_1/design_original.def \
//           10 10 test_data/aes_4M.def`
// (Release build strongly recommended - this pushes several million
// objects through the database; a Debug build's own bounds-checked STL
// iterators make that meaningfully slower, not just less optimized.)

namespace
{
    Point translate(const Point &p, int64_t dx, int64_t dy)
    {
        return Point{p.x + dx, p.y + dy};
    }

    Rect translate(const Rect &r, int64_t dx, int64_t dy)
    {
        return Rect{.ll = translate(r.ll, dx, dy), .ur = translate(r.ur, dx, dy)};
    }

    Polygon translate(const Polygon &poly, int64_t dx, int64_t dy)
    {
        Polygon result;
        result.points.reserve(poly.points.size());
        for (const Point &p : poly.points)
            result.points.push_back(translate(p, dx, dy));
        return result;
    }
}

int main(int argc, char **argv)
{
    if (argc != 7)
    {
        fprintf(stderr, "Usage: %s <tech.lef> <macro.lef> <design.def> <tile_x> <tile_y> <output.def>\n", argv[0]);
        return 1;
    }

    const std::string tech_lef_path = argv[1];
    const std::string macro_lef_path = argv[2];
    const std::string def_path = argv[3];
    const int tile_x = std::atoi(argv[4]);
    const int tile_y = std::atoi(argv[5]);
    const std::string output_path = argv[6];

    if (tile_x <= 0 || tile_y <= 0)
    {
        fprintf(stderr, "tile_x/tile_y must both be positive (got %d, %d)\n", tile_x, tile_y);
        return 1;
    }

    Root root;

    LEFReader tech_reader;
    if (tech_reader.read_lef(tech_lef_path, root, "tech") != 0)
    {
        fprintf(stderr, "Failed to read tech LEF '%s'\n", tech_lef_path.c_str());
        return 1;
    }
    LEFReader macro_reader;
    if (macro_lef_path != tech_lef_path && macro_reader.read_lef(macro_lef_path, root, "macro") != 0)
    {
        fprintf(stderr, "Failed to read macro LEF '%s'\n", macro_lef_path.c_str());
        return 1;
    }

    DEFReader def_reader;
    if (def_reader.read_def(def_path, root, "source_lib") != 0)
    {
        fprintf(stderr, "Failed to read DEF '%s'\n", def_path.c_str());
        for (const std::string &m : def_reader.messages())
            fprintf(stderr, "  %s\n", m.c_str());
        return 1;
    }

    const auto design_ids = root.get_design_ids();
    DesignId source_design_id;
    for (const DesignId id : design_ids)
    {
        if (root.get_design_layout(id).valid())
        {
            source_design_id = id;
            break;
        }
    }
    if (!source_design_id.valid())
    {
        fprintf(stderr, "No Design with a Layout found after reading '%s'\n", def_path.c_str());
        return 1;
    }
    const LayoutId source_layout_id = root.get_design_layout(source_design_id);
    const DesignData *source_design = root.get_design(source_design_id);
    printf("Source design '%s': %zu placements, %zu routes, %zu rows, %zu tracks, %zu gcell grids\n",
           source_design->name.c_str(),
           root.get_layout_placements(source_layout_id).size(),
           root.get_layout_routes(source_layout_id).size(),
           root.get_layout_rows(source_layout_id).size(),
           root.get_layout_tracks(source_layout_id).size(),
           root.get_layout_gcell_grids(source_layout_id).size());

    // DIEAREA is stored as a polygon (DEFReader::polygon_from_die_area) -
    // even a plain 2-corner DIEAREA (this fixture's own shape) expands to
    // a real closed rectangle polygon via Geometry::rect_to_polygon, not
    // 2 bare points, so this computes the bounding box directly rather
    // than assuming a specific point count.
    const ShapeId diearea_shape_id = root.get_layout_diearea(source_layout_id);
    const ShapeData *diearea_shape = root.get_shape(diearea_shape_id);
    if (!diearea_shape || diearea_shape->polygons.empty() || diearea_shape->polygons.front().points.empty())
    {
        fprintf(stderr, "Source Layout has no DIEAREA - can't compute a tile size\n");
        return 1;
    }
    Point die_ll = diearea_shape->polygons.front().points.front();
    Point die_ur = die_ll;
    for (const Point &p : diearea_shape->polygons.front().points)
    {
        die_ll.x = std::min(die_ll.x, p.x);
        die_ll.y = std::min(die_ll.y, p.y);
        die_ur.x = std::max(die_ur.x, p.x);
        die_ur.y = std::max(die_ur.y, p.y);
    }
    const int64_t tile_width = die_ur.x - die_ll.x;
    const int64_t tile_height = die_ur.y - die_ll.y;
    printf("Source die: %lldx%lld dbu -> %dx%d tile grid -> %lldx%lld dbu\n",
           static_cast<long long>(tile_width), static_cast<long long>(tile_height),
           tile_x, tile_y,
           static_cast<long long>(tile_width) * tile_x, static_cast<long long>(tile_height) * tile_y);

    const LibraryId out_library_id = root.create_library(LibraryData{.name = "tiled_lib"});
    const DesignId out_design_id = root.create_design(DesignData{.library = out_library_id, .name = "tiled"});
    const LayoutId out_layout_id = root.create_layout(LayoutData{.design = out_design_id});

    // DIEAREA must be a polygon, not a rect - DEFWriter::write_die_area
    // only ever reads shape->polygons.front() (matching the reader's own
    // polygon_from_die_area, which always produces a polygon even for a
    // plain 2-corner DIEAREA), so a .rects-only diearea Shape here
    // silently writes no DIEAREA statement at all - confirmed directly:
    // an earlier version of this tool did exactly that, and the
    // resulting DEF's own missing DIEAREA made a downstream zoom-fit
    // silently no-op (Scene::fit_to_content(std::nullopt, ...) leaves
    // scale/pan at their default 1.0/(0,0) - a real, all-white render at
    // full-chip zoom, not an error).
    root.create_shape(ShapeData{
        .layout = out_layout_id,
        .purpose = ShapePurpose::BOUNDARY,
        .polygons = {Polygon{.points = {Point{0, 0}, Point{tile_width * tile_x, tile_height * tile_y}}}},
    });

    // TRACKS/GCELLGRID: extended to cover the new die (same start/step,
    // recomputed count), not duplicated per tile - a track/gcell-grid
    // pattern has no inherent per-tile extent of its own in DEF's model
    // (each entry just marks repeating grid lines with no clip region),
    // so duplicating one per tile would just produce redundant/
    // overlapping entries, not a real per-tile pattern the way Placement/
    // Route/Row genuinely have.
    for (const TrackId id : root.get_layout_tracks(source_layout_id))
    {
        const TrackData *track = root.get_track(id);
        if (!track)
            continue;
        TrackData copy = *track;
        copy.layout = out_layout_id;
        const int64_t span = track->is_x ? tile_width * tile_x : tile_height * tile_y;
        copy.count = track->step > 0 ? static_cast<int32_t>((span - track->start) / track->step) + 1 : track->count;
        root.create_track(copy);
    }
    for (const GCellGridId id : root.get_layout_gcell_grids(source_layout_id))
    {
        const GCellGridData *grid = root.get_g_cell_grid(id);
        if (!grid)
            continue;
        GCellGridData copy = *grid;
        copy.layout = out_layout_id;
        const int64_t span = grid->is_x ? tile_width * tile_x : tile_height * tile_y;
        copy.count = grid->step > 0 ? static_cast<int32_t>((span - grid->start) / grid->step) + 1 : grid->count;
        root.create_g_cell_grid(copy);
    }

    const auto &source_placements = root.get_layout_placements(source_layout_id);
    const auto &source_routes = root.get_layout_routes(source_layout_id);
    const auto &source_rows = root.get_layout_rows(source_layout_id);

    int64_t total_placements = 0, total_routes = 0, total_shapes = 0;

    for (int ty = 0; ty < tile_y; ty++)
    {
        for (int tx = 0; tx < tile_x; tx++)
        {
            const int64_t dx = tile_width * tx;
            const int64_t dy = tile_height * ty;
            const std::string suffix = "_t" + std::to_string(tx) + "_" + std::to_string(ty);

            for (const PlacementId id : source_placements)
            {
                const PlacementData *src = root.get_placement(id);
                if (!src)
                    continue;
                PlacementData copy = *src;
                copy.layout = out_layout_id;
                copy.name = src->name + suffix;
                if (copy.location)
                    copy.location = translate(*src->location, dx, dy);
                root.create_placement(copy);
                total_placements++;
            }

            for (const RouteId id : source_routes)
            {
                const RouteData *src_route = root.get_route(id);
                if (!src_route)
                    continue;
                RouteData route_copy = *src_route;
                route_copy.layout = out_layout_id;
                route_copy.name = src_route->name + suffix;
                const RouteId new_route_id = root.create_route(route_copy);
                total_routes++;

                for (const ShapeId shape_id : root.get_route_shapes(id))
                {
                    const ShapeData *src_shape = root.get_shape(shape_id);
                    if (!src_shape)
                        continue;
                    ShapeData shape_copy = *src_shape;
                    shape_copy.route = new_route_id;

                    for (Rect &r : shape_copy.rects)
                        r = translate(r, dx, dy);
                    for (Polygon &p : shape_copy.polygons)
                        p = translate(p, dx, dy);
                    for (Path &path : shape_copy.paths)
                        path.polygon = translate(path.polygon, dx, dy);
                    for (ShapeVia &via : shape_copy.vias)
                        via.origin = translate(via.origin, dx, dy);
                    for (ShapeViaIterate &via_iter : shape_copy.via_iterates)
                        via_iter.origin = translate(via_iter.origin, dx, dy);

                    root.create_shape(shape_copy);
                    total_shapes++;
                }
            }

            for (const RowId id : source_rows)
            {
                const RowData *src = root.get_row(id);
                if (!src)
                    continue;
                RowData copy = *src;
                copy.layout = out_layout_id;
                copy.name = src->name + suffix;
                if (copy.origin)
                    copy.origin = translate(*src->origin, dx, dy);
                root.create_row(copy);
            }
        }
        printf("Row of tiles %d/%d done (%lld placements, %lld routes, %lld shapes so far)\n",
               ty + 1, tile_y, static_cast<long long>(total_placements), static_cast<long long>(total_routes), static_cast<long long>(total_shapes));
    }

    printf("Generated: %lld placements, %lld routes, %lld shapes. Writing '%s'...\n",
           static_cast<long long>(total_placements), static_cast<long long>(total_routes), static_cast<long long>(total_shapes), output_path.c_str());

    DEFWriter writer;
    const int status = writer.write_def(output_path, root, out_layout_id);
    if (status != 0)
    {
        fprintf(stderr, "write_def failed with status %d\n", status);
        for (const std::string &m : writer.messages())
            fprintf(stderr, "  %s\n", m.c_str());
        return 1;
    }

    printf("Wrote '%s'\n", output_path.c_str());
    return 0;
}
