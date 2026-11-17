#include "session_test.hpp"
#include "../le_handle.hpp"
#include <gtest/gtest.h>

#include <string>

using namespace le;

namespace
{
    using PowerGridPortMarkers = le::test::SessionTest;
    using le::test::fixture_path;
}

// power_grid_pins.def's VDD/VSS pins are grids of die-spanning stripes.
// Each gets one marker, on its first 2um M2 stripe (its largest piece,
// where the M2 label sits), beyond that stripe's end - not one stretched
// across the block.
TEST_F(PowerGridPortMarkers, EachPinGetsOneSmallMarkerOnItsLabelledStripe)
{
    ASSERT_TRUE(read_lef(fixture_path("via_cell.lef").c_str(), "via_cell"));
    ASSERT_TRUE(read_def(fixture_path("power_grid_pins.def").c_str(), "top"));
    le::DesignId top_design{.index = UINT32_MAX, .generation = 0};
    for (int32_t l = 0; l < library_count(); ++l)
        for (int32_t d = 0; d < library_design_count(l); ++d)
            if (std::string(library_design_at(l, d).name) == "GRIDTOP")
                top_design = library_design_at(l, d).id;
    ASSERT_TRUE(open_layout(top_design));
    write().set_viewport_size(100, 100);
    render();

    const auto resolved = handle->view_render_pipeline.resolved_output();
    ASSERT_NE(resolved, nullptr);
    const ViewData &top = resolved->view_data.at(HierarchyId{handle->current_layout()});
    const ViewLayerId marker_layer = handle->view_layers.port_marker_view_layer();

    std::vector<Rect> marker_bboxes;
    std::vector<std::string> labels;
    for (const ViewShapeChunk &chunk : top.chunks)
        if (chunk.shapes)
            for (const auto &[layer, shapes] : *chunk.shapes)
                for (const RenderShape &shape : shapes)
                {
                    for (const Text &text : shape.texts)
                        labels.push_back(text.label);
                    if (layer == marker_layer)
                    {
                        const std::optional<Rect> bbox = Geometry::bbox(shape);
                        ASSERT_TRUE(bbox.has_value());
                        marker_bboxes.push_back(*bbox);
                    }
                }
    // One label per pin per layer (M1, M2).
    std::ranges::sort(labels);
    EXPECT_EQ(labels, (std::vector<std::string>{"VDD", "VDD", "VSS", "VSS"}));

    // INOUT: both triangles under the stripe's bottom end, as wide (2um)
    // and as deep as the stripe.
    ASSERT_EQ(marker_bboxes.size(), 2u);
    std::ranges::sort(marker_bboxes, {}, [](const Rect &r)
                      { return r.ll.x; });
    for (const auto &[bbox, stripe_x] : {std::pair{marker_bboxes[0], 10000}, std::pair{marker_bboxes[1], 20000}})
    {
        EXPECT_EQ(bbox.ll.x, stripe_x - 1000);
        EXPECT_EQ(bbox.ur.x, stripe_x + 1000);
        EXPECT_EQ(bbox.ur.y, 0);
        EXPECT_EQ(bbox.ll.y, -2000);
    }
}
