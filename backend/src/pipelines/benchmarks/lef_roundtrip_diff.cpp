#include "../../io/lef_reader.hpp"
#include "../../io/lef_writer.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace le;

namespace
{
    // lefdiff itself doesn't diff anything - its real main() (differLef.cpp)
    // takes `fileName1 fileName2 lefOut1 lefOut2` and dumps a normalized
    // text representation of each input to lefOut1/lefOut2; the caller is
    // expected to sort and compare those two dumps itself. Feeding the same
    // path for both fileName args and only reading lefOut1 back is just a
    // convenient way to get one normalized dump of one file - the same
    // mechanism src/io/tests/lef_writer_test.cpp's own lefdiff-based test
    // uses for a real two-file comparison.
    std::vector<std::string> lefdiff_dump(const std::string &lefdiff_bin, const std::string &lef_path)
    {
        const std::filesystem::path dump_path = std::filesystem::temp_directory_path() / "le_roundtrip_diff_dump.txt";
        const std::filesystem::path unused_path = std::filesystem::temp_directory_path() / "le_roundtrip_diff_unused.txt";

        const std::string command = "\"" + lefdiff_bin + "\" \"" + lef_path + "\" \"" + lef_path + "\" \"" + dump_path.string() + "\" \"" + unused_path.string() + "\" > /dev/null 2>&1";
        if (std::system(command.c_str()) != 0)
        {
            fprintf(stderr, "lefdiff failed on '%s'\n", lef_path.c_str());
            return {};
        }

        std::ifstream in(dump_path);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line))
            lines.push_back(std::move(line));
        return lines;
    }
}

// Dev-only progress-tracking tool for UPDATES.md item 12 (not a benchmark,
// not run by ctest - mirrors render_preview's own "dev-only" precedent):
// reads a LEF file, writes every Design's Abstract back out via LEFWriter
// (each to its own file, plus one for the Technology's own layers -
// LEFWriter::write_lef only ever writes one AbstractId at a time, per
// UPDATES.md item 12 step 1's own spec, so there's no single "write the
// whole file back out" call to make), gets the vendored lefdiff tool's own
// normalized dump of each piece, concatenates them, and reports which
// lines from the *original* file's own lefdiff dump are missing from that
// concatenation - i.e. what LEFWriter/LEFReader still can't round-trip.
//
// Usage: `cmake --build build --target lef_roundtrip_diff &&
//         ./build/lef_roundtrip_diff [lef_file]`
// Defaults to src/lefdef/lef/TEST/complete.5.8.lef (LEFDEF_TEST_DIR) - the
// file item 12 names as the full-syntax target.
int main(int argc, char **argv)
{
    const std::string lef_path = argc > 1 ? argv[1] : std::string(LEFDEF_TEST_DIR) + "/complete.5.8.lef";
    const std::string lefdiff_bin = LEFDIFF_BIN;

    Root root;
    LEFReader reader;
    if (reader.read_lef(lef_path, root, "roundtrip") != 0)
    {
        fprintf(stderr, "Failed to parse '%s'\n", lef_path.c_str());
        for (const auto &msg : reader.messages())
            fprintf(stderr, "  %s\n", msg.c_str());
        return 1;
    }

    const std::filesystem::path out_dir = "roundtrip";
    std::filesystem::create_directories(out_dir);

    std::vector<std::string> written_dump;

    const auto technology_ids = root.get_technology_ids();
    if (!technology_ids.empty())
    {
        const std::string tech_path = (out_dir / "technology.lef").string();
        LEFWriter tech_writer;
        if (tech_writer.write_lef(tech_path, root, AbstractId{}, LEFWriter::LayerWriteMode::TechnologyOnly) == 0)
        {
            const auto dump = lefdiff_dump(lefdiff_bin, tech_path);
            written_dump.insert(written_dump.end(), dump.begin(), dump.end());
        }
        else
        {
            fprintf(stderr, "Failed to write Technology layers: %s\n", tech_writer.messages().empty() ? "(no message)" : tech_writer.messages().front().c_str());
        }
    }

    for (const DesignId design_id : root.get_design_ids())
    {
        const DesignData *design = root.get_design(design_id);
        const AbstractId abstract_id = root.get_design_abstract(design_id);
        if (!design || !abstract_id.valid())
            continue;

        const std::string macro_path = (out_dir / (design->name + ".lef")).string();
        LEFWriter macro_writer;
        if (macro_writer.write_lef(macro_path, root, abstract_id, LEFWriter::LayerWriteMode::None) == 0)
        {
            const auto dump = lefdiff_dump(lefdiff_bin, macro_path);
            written_dump.insert(written_dump.end(), dump.begin(), dump.end());
        }
        else
        {
            fprintf(stderr, "Failed to write MACRO %s: %s\n", design->name.c_str(), macro_writer.messages().empty() ? "(no message)" : macro_writer.messages().front().c_str());
        }
    }

    const std::vector<std::string> original_dump = lefdiff_dump(lefdiff_bin, lef_path);

    const std::set<std::string> written_set(written_dump.begin(), written_dump.end());

    std::vector<std::string> missing;
    for (const std::string &line : original_dump)
        if (!written_set.contains(line))
            missing.push_back(line);
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());

    printf("Original '%s': %zu normalized statement lines.\n", lef_path.c_str(), original_dump.size());
    printf("Round-tripped (Technology + %zu MACROs written via LEFWriter): %zu normalized statement lines.\n", root.get_design_ids().size(), written_dump.size());
    printf("%zu distinct lines present in the original but missing after round-tripping:\n", missing.size());
    for (const std::string &line : missing)
        printf("  %s\n", line.c_str());

    return 0;
}
