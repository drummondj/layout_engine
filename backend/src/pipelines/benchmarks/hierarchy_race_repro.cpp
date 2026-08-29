// Dev-only investigative repro for BUGS_AND_ENHANCEMENTS.md B2 - not a
// benchmark or a test, not run by ctest. Builds a Layout with real scale
// (many placements + many real tracks/gcellgrid spanning several layers,
// tracks/gcellgrid left invisible by default per E2) and repeatedly calls
// HierarchyResolver::render_layout_frame, the exact call chain the real
// ispd19_test10 crash (EXC_BAD_ACCESS/SIGBUS inside
// LayerVisibilityFilterStage::compute, on a TBB worker thread) was
// reported from. Meant to be built under ThreadSanitizer (a fresh
// -fsanitize=thread build tree) for a fast, precise repro instead of the
// ~15-minute Flutter/Xcode integration-test cycle each attempt otherwise
// costs.
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "../frame_render_pipeline.hpp"
#include "../hierarchy_resolver.hpp"
#include <cstdio>
#include <string>

namespace
{
    constexpr int kLayerCount = 8;
    constexpr int kPlacementGridColumns = 200;
    constexpr int kPlacementCount = kPlacementGridColumns * kPlacementGridColumns; // 40,000
    constexpr int64_t kPlacementStepDbu = 20'000;
    constexpr int kTrackCountPerLayer = 400;
    constexpr int64_t kTrackStepDbu = 5'000;
    constexpr int kFrameCount = 30;

    le::LayoutId build_stress_layout(le::Root &root, le::TechnologyId technology_id, le::LibraryId library_id, const std::vector<le::LayerId> &layers)
    {
        const le::DesignId leaf_design_id = root.create_design(le::DesignData{.library = library_id, .name = "leaf"});
        const le::AbstractId leaf_abstract_id = root.create_abstract(le::AbstractData{.design = leaf_design_id, .size = le::Point{10'000, 10'000}});
        const le::ObstructionId obstruction_id = root.create_obstruction(le::ObstructionData{.abstract = leaf_abstract_id});
        le::Shape leaf_shape;
        leaf_shape.obstruction = obstruction_id;
        leaf_shape.layer = layers[0];
        leaf_shape.rects.push_back(le::Rect{.ll = {0, 0}, .ur = {10'000, 10'000}});
        root.create_shape(std::move(leaf_shape));

        const int64_t die_size = static_cast<int64_t>(kPlacementGridColumns) * kPlacementStepDbu + 10'000;

        const le::DesignId top_design_id = root.create_design(le::DesignData{.library = library_id, .name = "top"});
        const le::LayoutId layout_id = root.create_layout(le::LayoutData{.design = top_design_id});
        le::Shape diearea;
        diearea.layout = layout_id;
        diearea.purpose = le::ShapePurpose::BOUNDARY;
        diearea.rects.push_back(le::Rect{.ll = {0, 0}, .ur = {die_size, die_size}});
        root.create_shape(std::move(diearea));

        for (int row = 0; row < kPlacementGridColumns; ++row)
        {
            for (int col = 0; col < kPlacementGridColumns; ++col)
            {
                root.create_placement(le::PlacementData{
                    .layout = layout_id,
                    .name = "U" + std::to_string(row) + "_" + std::to_string(col),
                    .reference_design = leaf_design_id,
                    .location = le::Point{static_cast<int64_t>(col) * kPlacementStepDbu, static_cast<int64_t>(row) * kPlacementStepDbu},
                    .orientation = le::Orientation::N,
                });
            }
        }

        // Every layer gets its own track set spanning every layer name at
        // once (TrackData.layer_names), same shape DEFReader produces for a
        // real TRACKS statement covering several layers - the exact input
        // append_track_shapes (BUGS_AND_ENHANCEMENTS.md E2) resolves into
        // TRACK_PREFERRED/TRACK_NON_PREFERRED per layer.
        std::vector<std::string> layer_names;
        for (le::LayerId layer_id : layers)
            layer_names.push_back(root.get_layer(layer_id)->name);

        root.create_track(le::TrackData{.layout = layout_id, .is_x = true, .start = 0, .count = kTrackCountPerLayer, .step = kTrackStepDbu, .layer_names = layer_names});
        root.create_track(le::TrackData{.layout = layout_id, .is_x = false, .start = 0, .count = kTrackCountPerLayer, .step = kTrackStepDbu, .layer_names = layer_names});

        root.create_g_cell_grid(le::GCellGridData{.layout = layout_id, .is_x = true, .start = 0, .count = kTrackCountPerLayer, .step = kTrackStepDbu});
        root.create_g_cell_grid(le::GCellGridData{.layout = layout_id, .is_x = false, .start = 0, .count = kTrackCountPerLayer, .step = kTrackStepDbu});

        return layout_id;
    }
}

int main()
{
    le::Root root;
    const le::TechnologyId technology_id = root.create_technology(le::TechnologyData{.database_units_microns = 1000.0});

    std::vector<le::LayerId> layers;
    for (int i = 0; i < kLayerCount; ++i)
    {
        const le::RoutingDirection direction = (i % 2 == 0) ? le::RoutingDirection::H : le::RoutingDirection::V;
        layers.push_back(root.create_layer(le::LayerData{.technology = technology_id, .name = "M" + std::to_string(i + 1), .type = "ROUTING", .direction = direction}));
    }

    const le::LibraryId library_id = root.create_library(le::LibraryData{.name = "LIB"});
    le::ViewLayerSet view_layers = le::ViewLayerSet::build_for_technology(root, technology_id);
    const le::LayoutId layout_id = build_stress_layout(root, technology_id, library_id, layers);

    le::Scene scene;
    scene.set_viewport_size(1920, 1080);
    scene.set_scale(0.05);
    scene.set_pan(le::Point{0, 0});
    // Leave TRACK_PREFERRED/TRACK_NON_PREFERRED/ROW/GCELLGRID at their real
    // E2 default (invisible) - the exact condition that makes
    // LayerVisibilityFilterStage::compute() erase the majority of the
    // groups it just built on real data, per BUGS_AND_ENHANCEMENTS.md B2.

    le::HierarchyResolver resolver;
    le::FrameRenderPipeline frame;

    std::printf("Placements: %d, layers: %d, tracks/layer: %d, frames: %d\n", kPlacementCount, kLayerCount, kTrackCountPerLayer, kFrameCount);

    for (int i = 0; i < kFrameCount; ++i)
    {
        // Pan by a tiny amount each frame so render_layout_frame's own
        // top-level cache (keyed partly on viewport_version) can't just
        // return the previous frame's cached result untouched - forces a
        // real recompute (and therefore a fresh nested SynchronousStageRunner
        // construction inside build_layout_picture_uncached) every time,
        // same as a real user panning/zooming through the design.
        scene.set_pan(le::Point{static_cast<int64_t>(i), 0});
        const le::PixelBuffer &buffer = resolver.render_layout_frame(root, layout_id, /*hierarchy_depth=*/1, view_layers, scene, frame);
        std::printf("frame %d: %dx%d\n", i, buffer.width, buffer.height);
    }

    std::printf("Done - no crash.\n");
    return 0;
}
