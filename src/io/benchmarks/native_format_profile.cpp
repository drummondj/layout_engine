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
// Not run by ctest.

#include "../../io/def_reader.hpp"
#include "../../io/lef_reader.hpp"
#include "../native_format.hpp"
#include "generated/database/native_tables.hpp"

#include <oneapi/tbb/global_control.h>
#include <memory>

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
