#pragma once
#include "../../database/database.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include <chrono>
#include <iostream>
#include <string>

// Migration Step 3's own stress fixture: a 2-level Placement hierarchy
// built directly via Root::create_* calls (not a DEF-text round trip -
// much cheaper to construct at this scale than parsing a generated file,
// unlike stress_data.hpp's own Abstract-shape-count fixture, which stresses
// a different axis entirely and has no Placement/Layout content at all).
// Matches the user's own worked example precisely: a sub-block with
// 1,000,000 placed instances of a small leaf cell, then a top-level with 4
// instances of that sub-block at different orientations.
namespace
{
    constexpr int kSubBlockPlacementCount = 1'000'000;
    constexpr int kTopPlacementCount = 4;

    // 1000x1000 grid, 20um pitch - the leaf's own 10x10um size leaves a
    // real (if small) gap between instances, matching stress_data.hpp's
    // own "spread across a grid" convention rather than tiling edge-to-edge.
    constexpr int kSubBlockGridColumns = 1'000;
    constexpr int64_t kSubBlockStepDbu = 20'000;
    // Sub-block's own diearea: grid extent plus the leaf's own trailing size.
    constexpr int64_t kSubBlockSizeDbu = (kSubBlockGridColumns - 1) * kSubBlockStepDbu + 10'000;
    // Top-level pitch: comfortably larger than the sub-block's own diearea
    // so its 4 instances (2x2, one per Orientation) never overlap.
    constexpr int64_t kTopStepDbu = 25'000'000;

    struct LayoutStressData
    {
        le::Root root;
        le::ViewLayerSet view_layers;
        le::LayoutId top_layout_id;
        le::LayoutId sub_layout_id;
        le::DesignId leaf_design_id;
        le::DesignId sub_design_id;
    };

    const LayoutStressData &layout_stress_data()
    {
        static const LayoutStressData data = [] {
            LayoutStressData d;

            const auto t0 = std::chrono::steady_clock::now();

            const le::TechnologyId technology_id = d.root.create_technology(le::TechnologyData{.database_units_microns = 1000.0});
            const le::LayerId m1 = d.root.create_layer(le::LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            d.view_layers = le::ViewLayerSet::build_for_technology(d.root, technology_id);

            const le::LibraryId library_id = d.root.create_library(le::LibraryData{.name = "STRESSLIB"});

            // Leaf: a small, fixed 10x10um cell with one M1 obstruction
            // filling its own bbox - modest, fixed content (not itself the
            // thing being stressed; the INSTANCE COUNT is).
            d.leaf_design_id = d.root.create_design(le::DesignData{.library = library_id, .name = "LEAF"});
            const le::AbstractId leaf_abstract_id = d.root.create_abstract(le::AbstractData{.design = d.leaf_design_id, .size = le::Point{10'000, 10'000}});
            const le::ObstructionId obstruction_id = d.root.create_obstruction(le::ObstructionData{.abstract = leaf_abstract_id});
            le::Shape leaf_shape;
            leaf_shape.obstruction = obstruction_id;
            leaf_shape.layer = m1;
            leaf_shape.rects.push_back(le::Rect{.ll = {0, 0}, .ur = {10'000, 10'000}});
            d.root.create_shape(std::move(leaf_shape));

            // Sub-block: a Layout containing kSubBlockPlacementCount
            // Placements of the leaf, grid-placed (mirrors stress_data.hpp's
            // own `x = (i % N) * step` style) - plus a real diearea (so
            // placement/orientation math for the top-level's own instances
            // of it doesn't fall back to a degenerate bbox) and a minimal
            // (size-only, no real shapes) Abstract, giving hierarchy_depth=0
            // a real, cheap fallback view instead of an unresolved one -
            // realistic for a real hard macro, which typically has both a
            // LEF abstract view and its own DEF/Layout physical view.
            d.sub_design_id = d.root.create_design(le::DesignData{.library = library_id, .name = "SUBBLOCK"});
            d.sub_layout_id = d.root.create_layout(le::LayoutData{.design = d.sub_design_id});
            d.root.create_abstract(le::AbstractData{.design = d.sub_design_id, .size = le::Point{kSubBlockSizeDbu, kSubBlockSizeDbu}});

            le::Shape sub_diearea;
            sub_diearea.layout = d.sub_layout_id;
            sub_diearea.purpose = le::ShapePurpose::BOUNDARY;
            sub_diearea.rects.push_back(le::Rect{.ll = {0, 0}, .ur = {kSubBlockSizeDbu, kSubBlockSizeDbu}});
            d.root.create_shape(std::move(sub_diearea));

            for (int i = 0; i < kSubBlockPlacementCount; ++i)
            {
                const int64_t x = (i % kSubBlockGridColumns) * kSubBlockStepDbu;
                const int64_t y = (i / kSubBlockGridColumns) * kSubBlockStepDbu;
                d.root.create_placement(le::PlacementData{
                    .layout = d.sub_layout_id,
                    .name = "U" + std::to_string(i),
                    .reference_design = d.leaf_design_id,
                    .placement_status = le::PlacementStatus::PLACED,
                    .location = le::Point{x, y},
                    .orientation = le::Orientation::N,
                });
            }

            // Top-level: a Layout containing exactly kTopPlacementCount
            // Placements of the sub-block, each a different Orientation -
            // matching the user's own spec precisely.
            const le::DesignId top_design_id = d.root.create_design(le::DesignData{.library = library_id, .name = "TOP"});
            d.top_layout_id = d.root.create_layout(le::LayoutData{.design = top_design_id});

            const le::Orientation top_orientations[kTopPlacementCount] = {le::Orientation::N, le::Orientation::W, le::Orientation::S, le::Orientation::E};
            for (int i = 0; i < kTopPlacementCount; ++i)
            {
                d.root.create_placement(le::PlacementData{
                    .layout = d.top_layout_id,
                    .name = "SUB" + std::to_string(i),
                    .reference_design = d.sub_design_id,
                    .placement_status = le::PlacementStatus::PLACED,
                    .location = le::Point{(i % 2) * kTopStepDbu, (i / 2) * kTopStepDbu},
                    .orientation = top_orientations[i],
                });
            }

            const auto t1 = std::chrono::steady_clock::now();
            std::cerr << "[setup] built " << kSubBlockPlacementCount << "-placement Layout stress fixture ("
                      << kTopPlacementCount << " top-level instances of it) in "
                      << std::chrono::duration<double>(t1 - t0).count() << "s\n";

            return d;
        }();
        return data;
    }

    // scale/viewport chosen so the whole top-level diearea-less extent
    // (4 sub-block instances, kTopStepDbu apart) roughly fits on screen -
    // real, non-trivial viewport-relative work for render_layout_frame's
    // own compose step, not an arbitrary/unrelated setting.
    le::Scene make_layout_scene(const LayoutStressData &data, int hierarchy_depth)
    {
        le::Scene scene;
        scene.set_current_layout(data.top_layout_id);
        scene.set_hierarchy_depth(hierarchy_depth);
        scene.set_pan(le::Point{0, 0});
        scene.set_scale(2000.0 / (2.0 * kTopStepDbu)); // ~2000px covers both columns of the 2x2 top-level grid
        scene.set_viewport_size(2000, 2000);
        return scene;
    }
}
