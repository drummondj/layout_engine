#include "../le_handle.hpp"
#include <gtest/gtest.h>

#include <string>

using namespace le;

namespace
{
    std::string fixture_path(const std::string &name) { return std::string(API_TEST_FIXTURES_DIR) + "/" + name; }
}

// power_grid_pins.def's VDD/VSS pins are grids of die-spanning stripes.
// Each gets one marker, on its first 2um M2 stripe (its largest piece,
// where the M2 label sits), beyond that stripe's end - not one stretched
// across the block.
TEST(PowerGridPortMarkers, EachPinGetsOneSmallMarkerOnItsLabelledStripe)
{
    LeHandle *handle = le_create();
    ASSERT_EQ(le_read_lef(handle, fixture_path("via_cell.lef").c_str(), "via_cell"), 0);
    ASSERT_EQ(le_read_def(handle, fixture_path("power_grid_pins.def").c_str(), "top"), 0);
    LeDesignId top_design{.index = UINT32_MAX, .generation = 0};
    for (int32_t l = 0; l < le_library_count(handle); ++l)
        for (int32_t d = 0; d < le_library_design_count(handle, l); ++d)
            if (std::string(le_library_design_at(handle, l, d).name) == "GRIDTOP")
                top_design = le_library_design_at(handle, l, d).id;
    ASSERT_EQ(le_set_current_design_layout_by_id(handle, top_design), 0);
    le_set_viewport_size(handle, 100, 100);
    le_render_pixel_buffer(handle);

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
    le_destroy(handle);
}
