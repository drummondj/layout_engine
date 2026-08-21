#include "../../geometry/geometry.hpp"
#include "../../io/lef_reader.hpp"
#include "../../render/render.hpp"
#include "../pipeline.hpp"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include <cstdio>
#include <filesystem>

using namespace le;

namespace
{
    constexpr int kOutputSize = 1000;
    constexpr int kPaddingPx = 20;

    // Fits abstract_id's content bbox into a kOutputSize x kOutputSize image
    // with kPaddingPx of margin on every side - see Scene::fit_to_content
    // (also used by api.cpp's le_fit_scene) for the actual scale/pan math.
    Scene fit_scene(const Root &root, AbstractId abstract_id, Pipeline &pipeline, const ViewLayerSet &view_layers)
    {
        Scene scene;
        scene.set_current_abstract(abstract_id);
        scene.set_viewport_size(kOutputSize, kOutputSize);

        const auto &generated = pipeline.generate_shapes(root, abstract_id, view_layers);

        std::vector<const Shape *> shape_ptrs;
        shape_ptrs.reserve(generated.size());
        for (const auto &rs : generated)
            shape_ptrs.push_back(&rs.shape);

        scene.fit_to_content(Geometry::bbox(shape_ptrs), kPaddingPx);
        return scene;
    }

    // Encodes Renderer::rasterize()'s own output directly - using the same
    // production rasterization path (RGBA8888, Y-flip applied) this preview
    // tool exists to sanity-check, rather than a separate ad hoc
    // SkSurface/drawPicture of its own. The background is therefore fully
    // transparent wherever nothing was drawn (Renderer's real "nothing
    // here" state), not a dark-gray preview convenience like this used to
    // clear to - most image viewers render PNG transparency fine.
    bool write_png(const PixelBuffer &buffer, const std::string &output_path)
    {
        const SkImageInfo info = SkImageInfo::Make(buffer.width, buffer.height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        SkPixmap pixmap(info, buffer.data, buffer.row_bytes);

        SkFILEWStream stream(output_path.c_str());
        if (!stream.isValid())
        {
            fprintf(stderr, "Failed to open '%s' for writing\n", output_path.c_str());
            return false;
        }

        if (!SkPngEncoder::Encode(&stream, pixmap, SkPngEncoder::Options{}))
        {
            fprintf(stderr, "Failed to encode PNG for '%s'\n", output_path.c_str());
            return false;
        }

        printf("Wrote %dx%d PNG to %s\n", buffer.width, buffer.height, output_path.c_str());
        return true;
    }
}

// Dev-only visualization tool, not part of the render pipeline itself: reads
// every given LEF file into one shared Root, runs Pipeline + Renderer
// against every Design's Abstract found across all of them (Scene fitted to
// that Abstract's own content bbox, since real macros don't share the
// stress data's hardcoded scene - see fit_scene above), and writes one PNG
// per Design into preview/ - a quick way to sanity-check what real LEF
// files render as (layer colors, bottom-up z-order, Terminal labels)
// without waiting for Flutter texture wiring.
//
// A single shared Root (not one per file) matters because LEF is commonly
// split across a tech file (LAYER definitions, no macros) and one or more
// macro/cell files (MACRO/PIN definitions referencing those layers by
// name) - LEFReader::read_lef already supports this: it reuses an existing
// Technology instead of creating a new one when the Root already has one,
// so passing the tech file first lets later macro files' LAYER references
// resolve. Order matters for that reason - pass the tech file before any
// macro file that depends on it.
//
//   ./render_preview file1.lef [file2.lef ...]
//
// Usage: `cmake --build build --target render_preview && ./build/render_preview <lef files>`
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <lef file> [lef file ...]\n", argv[0]);
        return 1;
    }

    const std::filesystem::path preview_dir = "preview";
    std::filesystem::create_directories(preview_dir);

    bool all_ok = true;
    Root root;

    for (int i = 1; i < argc; ++i)
    {
        const std::filesystem::path lef_path = argv[i];

        LEFReader reader;
        if (reader.read_lef(lef_path.string(), root, lef_path.stem().string()) != 0)
        {
            fprintf(stderr, "Failed to parse '%s'\n", lef_path.string().c_str());
            all_ok = false;
        }
    }

    const auto technology_ids = root.get_technology_ids();
    if (technology_ids.empty())
    {
        fprintf(stderr, "No technology declared across the given LEF files\n");
        return 1;
    }
    const ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_ids.front());

    const auto design_ids = root.get_design_ids();
    if (design_ids.empty())
    {
        fprintf(stderr, "No designs declared across the given LEF files\n");
        return 1;
    }

    for (const DesignId design_id : design_ids)
    {
        const DesignData *design = root.get_design(design_id);
        const AbstractId abstract_id = root.get_design_abstract(design_id);

        Pipeline pipeline;
        Renderer renderer;
        Scene scene = fit_scene(root, abstract_id, pipeline, view_layers);

        const auto &shapes = pipeline.run(root, scene, view_layers);
        const auto &tiny_shapes = pipeline.run_tiny_shapes(root, scene, view_layers);
        const auto &buffer = renderer.render(root, shapes, tiny_shapes, scene, view_layers);

        // The design's own library name (derived from whichever file's
        // read_lef call actually declared its MACRO - see the file-level
        // comment above) rather than a fixed input file's stem, since a
        // design here may come from any of the given files.
        const LibraryData *library = root.get_library(design->library);
        const std::string prefix = library ? library->name : "unknown";
        const std::string output_name = prefix + "__" + design->name + ".png";
        const std::filesystem::path output_path = preview_dir / output_name;

        if (!write_png(buffer, output_path.string()))
            all_ok = false;
    }

    return all_ok ? 0 : 1;
}
