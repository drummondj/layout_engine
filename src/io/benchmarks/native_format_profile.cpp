// native_format_profile - dev-only tool (plans/NATIVE_FILE_FORMAT_RESEARCH.md §7):
// how the native .led format compares with reading the DEF it came from,
// on one real aes_scaling design per process.
//
//   native_format_profile <aes_scaling label, e.g. 4x4> [zstd levels, default "1,3"] [max threads]
//
// Reports one "metric value" line each (times in ms, sizes in MB): the
// LEF+DEF read, then per zstd level the save time, file size and load
// time, and whether re-saving the loaded database reproduces the file.
// Memory (MB): process RSS after the read and after each load, and each
// pooled class's slot storage (capacity x sizeof(slot), not the heap its
// vectors own) - the classes holding at least 1 MB, plus Shape always.
// Route geometry (route_*): what the routed Wires and Shapes hold -
// segments, points, vias and the heap they own (estimated as glibc
// chunks: request + 8, rounded up to 16, at least 32 bytes).
// Not run by ctest.

#include "../../io/def_reader.hpp"
#include "../../io/lef_reader.hpp"
#include "../native_format.hpp"
#include "generated/database/native_tables.hpp"

#include <oneapi/tbb/global_control.h>
#include <memory>

#include <algorithm>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace le;

namespace
{
    double elapsed_ms(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void report(const std::string &metric, double value) { std::printf("%s %.3f\n", metric.c_str(), value); }

    // Resident set size, from /proc/self/status (VmRSS, in kB).
    double rss_mb()
    {
        std::ifstream status("/proc/self/status");
        for (std::string line; std::getline(status, line);)
            if (line.rfind("VmRSS:", 0) == 0)
                return std::stod(line.substr(6)) / 1024.0;
        return 0.0;
    }

    void report_memory(const std::string &prefix, const Root &root)
    {
        report(prefix + "rss_mb", rss_mb());
        native_tables::for_each_pooled([&]<class P>(P) {
            const auto &slots = P::pool(root).slots();
            const double mb = static_cast<double>(slots.capacity() * sizeof(slots[0])) / 1e6;
            const std::string name(P::name);
            if (mb >= 1.0 || name == "Shape")
            {
                report(prefix + "pool[" + name + "]_count", static_cast<double>(P::pool(root).alive_count()));
                report(prefix + "pool[" + name + "]_slot_bytes", static_cast<double>(sizeof(slots[0])));
                report(prefix + "pool[" + name + "]_mb", mb);
            }
        });
    }

    double malloc_chunk(size_t bytes) { return bytes == 0 ? 0.0 : static_cast<double>(std::max<size_t>(32, (bytes + 8 + 15) / 16 * 16)); }

    template <class T>
    double compact_heap(const CompactVector<T> &list) { return list.capacity() == 0 ? 0.0 : malloc_chunk(16 + list.capacity() * sizeof(T)); }

    void report_route_geometry(const Root &root)
    {
        double routes = 0, shapes = 0, shapes_with_paths = 0, shapes_with_vias = 0;
        double paths = 0, points = 0, two_point = 0, manhattan_two_point = 0, default_width = 0, max_shapes_per_route = 0;
        double vias = 0, via_iterates = 0, via_name_chars = 0, long_via_names = 0;
        double path_heap = 0, via_heap = 0, slot_bytes = 0;
        double wires = 0, wire_segments = 0, wire_vias = 0, wire_widths = 0, wire_heap = 0;
        for (RouteId route_id : root.pool_route().ids())
        {
            routes++;
            for (WireId wire_id : root.get_route_wires(route_id))
                if (const WireData *wire = root.get_wire(wire_id))
                {
                    wires++;
                    wire_segments += static_cast<double>(wire->segments.size());
                    wire_vias += static_cast<double>(wire->vias.size());
                    wire_widths += static_cast<double>(wire->widths.size());
                    wire_heap += compact_heap(wire->segments) + compact_heap(wire->vias) + compact_heap(wire->widths);
                }
            const auto &shape_ids = root.get_route_shapes(route_id);
            max_shapes_per_route = std::max(max_shapes_per_route, static_cast<double>(shape_ids.size()));
            for (ShapeId shape_id : shape_ids)
            {
                const ShapeData *shape = root.get_shape(shape_id);
                if (!shape)
                    continue;
                shapes++;
                slot_bytes += sizeof(ShapeData);
                const LayerData *layer = root.get_layer(shape->layer);
                const int64_t layer_width = layer ? layer->width.value_or(-1) : -1;
                shapes_with_paths += shape->paths.empty() ? 0 : 1;
                shapes_with_vias += shape->vias.empty() && shape->via_iterates.empty() ? 0 : 1;
                path_heap += compact_heap(shape->paths);
                for (const Path &path : shape->paths)
                {
                    paths++;
                    const auto &pts = path.polygon.points;
                    points += static_cast<double>(pts.size());
                    path_heap += malloc_chunk(pts.capacity() * sizeof(Point));
                    default_width += path.width == layer_width ? 1 : 0;
                    if (pts.size() == 2)
                    {
                        two_point++;
                        manhattan_two_point += pts[0].x == pts[1].x || pts[0].y == pts[1].y ? 1 : 0;
                    }
                }
                via_heap += compact_heap(shape->vias) + compact_heap(shape->via_iterates);
                for (const ShapeVia &via : shape->vias)
                {
                    vias++;
                    via_name_chars += static_cast<double>(via.via_name.size());
                    if (via.via_name.size() > 15)
                    {
                        long_via_names++;
                        via_heap += malloc_chunk(via.via_name.capacity() + 1);
                    }
                }
                via_iterates += static_cast<double>(shape->via_iterates.size());
            }
        }
        report("route_count", routes);
        report("route_shapes", shapes);
        report("route_shapes_per_route_max", max_shapes_per_route);
        report("route_shapes_with_paths", shapes_with_paths);
        report("route_shapes_with_vias", shapes_with_vias);
        report("route_shape_slot_mb", slot_bytes / 1e6);
        report("route_paths", paths);
        report("route_paths_per_shape", shapes ? paths / shapes : 0);
        report("route_points_per_path", paths ? points / paths : 0);
        report("route_paths_two_point_share", paths ? two_point / paths : 0);
        report("route_paths_manhattan_two_point_share", paths ? manhattan_two_point / paths : 0);
        report("route_paths_layer_default_width_share", paths ? default_width / paths : 0);
        report("route_path_heap_mb", path_heap / 1e6);
        report("route_path_heap_bytes_per_path", paths ? path_heap / paths : 0);
        report("route_vias", vias);
        report("route_via_iterates", via_iterates);
        report("route_vias_per_route", routes ? vias / routes : 0);
        report("route_via_name_mean_chars", vias ? via_name_chars / vias : 0);
        report("route_via_names_past_sso", long_via_names);
        report("route_via_heap_mb", via_heap / 1e6);
        report("route_via_heap_bytes_per_via", vias ? via_heap / vias : 0);
        report("route_wires", wires);
        report("route_wire_segments", wire_segments);
        report("route_wire_vias", wire_vias);
        report("route_wire_widths", wire_widths);
        report("route_wire_heap_mb", wire_heap / 1e6);
        report("sizeof_ShapeData", sizeof(ShapeData));
        report("sizeof_WireData", sizeof(WireData));
        report("sizeof_WireSegment", sizeof(WireSegment));
        report("sizeof_WireVia", sizeof(WireVia));
        report("sizeof_Path", sizeof(Path));
        report("sizeof_ShapeVia", sizeof(ShapeVia));
    }

    bool same_file(const std::string &a, const std::string &b)
    {
        std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
        return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(fb));
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: native_format_profile <aes_scaling label, e.g. 4x4> [zstd levels, e.g. 1,3]\n");
        return 2;
    }
    const std::string label = argv[1];
    std::vector<int> levels;
    {
        std::stringstream list(argc > 2 ? argv[2] : "1,3");
        for (std::string item; std::getline(list, item, ',');)
            levels.push_back(std::stoi(item));
    }
    std::unique_ptr<tbb::global_control> threads;
    if (argc > 3)
        threads = std::make_unique<tbb::global_control>(tbb::global_control::max_allowed_parallelism, std::stoul(argv[3]));
    const std::string data = REAL_DESIGN_TEST_DATA_DIR;
    const std::string def_path = data + "/aes_scaling_" + label + ".def";

    Root root;
    auto start = std::chrono::steady_clock::now();
    if (LEFReader().read_lef(data + "/ISPD22__final_benchmarks/__Nangate/NangateOpenCellLibrary.lef", root, "tech") != 0 ||
        DEFReader().read_def(def_path, root, "aes_scaling") != 0)
    {
        std::fprintf(stderr, "failed to read the design\n");
        return 1;
    }
    report("lef_def_read_ms", elapsed_ms(start));
    report("def_file_mb", static_cast<double>(std::filesystem::file_size(def_path)) / 1e6);
    report_memory("read_", root);
    report_route_geometry(root);

    const auto dir = std::filesystem::temp_directory_path();
    for (int level : levels)
    {
        const std::string prefix = "level" + std::to_string(level) + "_";
        const std::string path = (dir / ("native_format_profile_" + label + ".led")).string();
        start = std::chrono::steady_clock::now();
        const auto saved = persistence::save_native(root, path, persistence::SaveOptions{.compression_level = level});
        if (!saved.ok())
        {
            std::fprintf(stderr, "save failed: %s\n", saved.error.c_str());
            return 1;
        }
        report(prefix + "save_ms", elapsed_ms(start));
        report(prefix + "file_mb", static_cast<double>(saved.file_bytes) / 1e6);
        report(prefix + "objects", static_cast<double>(saved.objects));

        Root loaded;
        start = std::chrono::steady_clock::now();
        const auto load = persistence::load_native(loaded, path);
        if (!load.ok())
        {
            std::fprintf(stderr, "load failed: %s\n", load.error.c_str());
            return 1;
        }
        report(prefix + "load_ms", elapsed_ms(start));
        report_memory(prefix + "loaded_", loaded);
        for (const auto &[phase, ms] : saved.phase_ms)
            if (ms >= 50)
                report(prefix + "save_phase[" + phase + "]_ms", ms);
        for (const auto &[phase, ms] : load.phase_ms)
            if (ms >= 50)
                report(prefix + "load_phase[" + phase + "]_ms", ms);

        const std::string again = path + ".again";
        persistence::save_native(loaded, again, persistence::SaveOptions{.compression_level = level});
        report(prefix + "resave_identical", same_file(path, again) ? 1 : 0);
        std::filesystem::remove(path);
        std::filesystem::remove(again);
    }
    return 0;
}
