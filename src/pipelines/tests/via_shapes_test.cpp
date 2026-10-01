// Ported from pipelines.old/tests/pipelines_test.cpp's own
// AbstractShapePipelineFixture via-resolution tests (git history) - see
// via_shapes.hpp's own doc comment for why this logic exists at all
// (BUGS_AND_ENHANCEMENTS.md B3, "Via arrays are not being rendered") and
// the three-tier resolution algorithm each test below exercises. Calls
// append_via_shapes directly rather than through a whole pipeline/stage
// (the old fixture's own AbstractShapePipeline no longer exists in this
// module) - a more precise unit test of exactly this function, and one
// that doesn't need a Placement/Layout hierarchy just to reach it. This
// also simplifies several assertions the old tests needed (e.g.
// disambiguating a via's own synthesized enclosure rect from the
// referencing Shape's own geometry by coordinate) since append_via_shapes
// only ever adds via-synthesized Shapes, never the referencing Shape's
// own rects/polygons.
#include "../via_shapes.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace le;

namespace
{
    struct ViaShapesFixture : public ::testing::Test
    {
        void SetUp() override
        {
            technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
            view_layers = ViewLayerSet::build_for_technology(root, technology_id);
        }

        Root root;
        TechnologyId technology_id;
        LayerId m1;
        LayerId m2;
        ViewLayerSet view_layers;
    };
}

TEST_F(ViaShapesFixture, ResolvesAnExplicitViaLayerOntoItsOwnPhysicalLayer)
{
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-5, -5}, .ur = {5, 5}}}});

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "VIA12", .origin = Point{50, 50}});

    std::unordered_map<ViewLayerId, std::vector<RenderShape>> shapes_by_layer;
    append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes_by_layer);

    const ViewLayerId expected = view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    const auto it = shapes_by_layer.find(expected);
    ASSERT_NE(it, shapes_by_layer.end());
    ASSERT_EQ(it->second.size(), 1u);
    ASSERT_EQ(it->second.front().rects.size(), 1u);
    EXPECT_EQ(it->second.front().rects[0].ll.x, 45);
    EXPECT_EQ(it->second.front().rects[0].ll.y, 45);
}

// BUGS_AND_ENHANCEMENTS.md B3 - a via with no explicit ViaLayer rects (a
// LEF 5.6 VIARULE-inside-VIA reference) but a real ROWCOL clause is a via
// *array*, synthesized into a real grid of cut rects rather than skipped
// entirely (via_shapes.hpp's own append_via_rule_array).
TEST_F(ViaShapesFixture, SynthesizesAViaRuleReferencesRowColIntoARealCutArray)
{
    const LayerId cut_layer = root.create_layer(LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});

    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIAARRAY"});
    root.create_via_rule_reference(ViaRuleReferenceData{
        .via = via_id,
        .via_rule_name = "ViaRule1",
        .cut_size = Point{.x = 2, .y = 2},
        .bot_layer_name = "M1",
        .cut_layer_name = "V1",
        .top_layer_name = "M2",
        .cut_spacing = Point{.x = 1, .y = 1},
        .bot_enclosure = Point{.x = 1, .y = 1},
        .top_enclosure = Point{.x = 1, .y = 1},
        .num_cut_rows = 2,
        .num_cut_cols = 3,
    });

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "VIAARRAY", .origin = Point{50, 50}});

    std::unordered_map<ViewLayerId, std::vector<RenderShape>> shapes_by_layer;
    append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes_by_layer);

    const ViewLayerId cut_view_layer = view_layers.find(cut_layer, ViewLayerPurpose::TERMINAL);
    const auto cut_it = shapes_by_layer.find(cut_view_layer);
    ASSERT_NE(cut_it, shapes_by_layer.end());
    ASSERT_EQ(cut_it->second.size(), 1u);                     // one synthesized Shape...
    EXPECT_EQ(cut_it->second.front().rects.size(), 6u);       // ...holding all 2 rows x 3 cols = 6 cuts

    // Bottom/top metal each get their own single enclosure rect (not
    // per-cut).
    const ViewLayerId bot_view_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const auto bot_it = shapes_by_layer.find(bot_view_layer);
    ASSERT_NE(bot_it, shapes_by_layer.end());
    ASSERT_EQ(bot_it->second.size(), 1u);
    EXPECT_EQ(bot_it->second.front().rects.size(), 1u);

    const ViewLayerId top_view_layer = view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    const auto top_it = shapes_by_layer.find(top_view_layer);
    ASSERT_NE(top_it, shapes_by_layer.end());
    ASSERT_EQ(top_it->second.size(), 1u);
    EXPECT_EQ(top_it->second.front().rects.size(), 1u);
}

// A ViaRuleReference with no ROWCOL clause at all still means something
// real in LEF - a single cut at cut_size - not "nothing to draw" the way
// it was skipped before this fix.
TEST_F(ViaShapesFixture, SynthesizesASingleCutForAViaRuleReferenceWithNoRowCol)
{
    const LayerId cut_layer = root.create_layer(LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});

    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIASINGLE"});
    root.create_via_rule_reference(ViaRuleReferenceData{
        .via = via_id,
        .via_rule_name = "ViaRule1",
        .cut_size = Point{.x = 2, .y = 2},
        .bot_layer_name = "M1",
        .cut_layer_name = "V1",
        .top_layer_name = "M2",
    });

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "VIASINGLE", .origin = Point{50, 50}});

    std::unordered_map<ViewLayerId, std::vector<RenderShape>> shapes_by_layer;
    append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes_by_layer);

    const ViewLayerId cut_view_layer = view_layers.find(cut_layer, ViewLayerPurpose::TERMINAL);
    const auto cut_it = shapes_by_layer.find(cut_view_layer);
    ASSERT_NE(cut_it, shapes_by_layer.end());
    ASSERT_EQ(cut_it->second.size(), 1u);
    EXPECT_EQ(cut_it->second.front().rects.size(), 1u);
}

// BUGS_AND_ENHANCEMENTS.md B3 follow-up - ORIGIN shifts the whole cut
// array's own center away from the via's own placement point; OFFSET
// separately shifts each metal layer's own enclosure-rect center on top
// of that. rows=cols=1 (a single 2x2 cut) keeps the arithmetic small
// enough to hand-verify exactly, matching via_shapes.hpp's own
// synthesize_cut_array formulas: local start = -total/2 + origin, each
// enclosure rect = [start - enc + layer_offset, start + total + enc +
// layer_offset], both transformed by the via's own placement point (50,50).
TEST_F(ViaShapesFixture, AppliesOriginAndOffsetToAViaRuleReferencesCutArray)
{
    root.create_layer(LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});

    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIAORIGIN"});
    root.create_via_rule_reference(ViaRuleReferenceData{
        .via = via_id,
        .via_rule_name = "ViaRule1",
        .cut_size = Point{.x = 2, .y = 2},
        .bot_layer_name = "M1",
        .cut_layer_name = "V1",
        .top_layer_name = "M2",
        .bot_enclosure = Point{.x = 1, .y = 1},
        .top_enclosure = Point{.x = 1, .y = 1},
        .origin = Point{.x = 5, .y = -5},
        .bot_offset = Point{.x = 2, .y = 0},
        .top_offset = Point{.x = -2, .y = 0},
    });

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "VIAORIGIN", .origin = Point{50, 50}});

    std::unordered_map<ViewLayerId, std::vector<RenderShape>> shapes_by_layer;
    append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes_by_layer);

    const ViewLayerId cut_view_layer = view_layers.find(root.get_layer_by_name("V1"), ViewLayerPurpose::TERMINAL);
    const auto cut_it = shapes_by_layer.find(cut_view_layer);
    ASSERT_NE(cut_it, shapes_by_layer.end());
    ASSERT_EQ(cut_it->second.size(), 1u);
    ASSERT_EQ(cut_it->second.front().rects.size(), 1u);
    // ORIGIN (5,-5): local start = (-1+5, -1-5) = (4,-6); cut spans local
    // (4,-6)-(6,-4); + placement (50,50) -> world (54,44)-(56,46).
    const Rect &cut_rect = cut_it->second.front().rects[0];
    EXPECT_EQ(cut_rect.ll.x, 54);
    EXPECT_EQ(cut_rect.ll.y, 44);
    EXPECT_EQ(cut_rect.ur.x, 56);
    EXPECT_EQ(cut_rect.ur.y, 46);

    const ViewLayerId bot_view_layer = view_layers.find(m1, ViewLayerPurpose::TERMINAL);
    const auto bot_it = shapes_by_layer.find(bot_view_layer);
    ASSERT_NE(bot_it, shapes_by_layer.end());
    ASSERT_EQ(bot_it->second.size(), 1u);
    ASSERT_EQ(bot_it->second.front().rects.size(), 1u);
    // bot_offset (2,0): local (5,-7)-(9,-3) -> world (55,43)-(59,47).
    EXPECT_EQ(bot_it->second.front().rects[0].ll.x, 55);
    EXPECT_EQ(bot_it->second.front().rects[0].ll.y, 43);
    EXPECT_EQ(bot_it->second.front().rects[0].ur.x, 59);
    EXPECT_EQ(bot_it->second.front().rects[0].ur.y, 47);

    const ViewLayerId top_view_layer = view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    const auto top_it = shapes_by_layer.find(top_view_layer);
    ASSERT_NE(top_it, shapes_by_layer.end());
    ASSERT_EQ(top_it->second.size(), 1u);
    ASSERT_EQ(top_it->second.front().rects.size(), 1u);
    // top_offset (-2,0): local (1,-7)-(5,-3) -> world (51,43)-(55,47).
    EXPECT_EQ(top_it->second.front().rects[0].ll.x, 51);
    EXPECT_EQ(top_it->second.front().rects[0].ll.y, 43);
    EXPECT_EQ(top_it->second.front().rects[0].ur.x, 55);
    EXPECT_EQ(top_it->second.front().rects[0].ur.y, 47);
}

// BUGS_AND_ENHANCEMENTS.md B3 follow-up - a via_name resolving only to a
// top-level VIARULE ... GENERATE rule (no Via/LayoutVia, no
// ViaRuleReference anywhere) synthesizes a cut array fit to the
// available routing width (ShapeVia.width) - via_shapes.hpp's own
// append_generate_via_array. cut_w=cut_h=2 (RECT -1 -1 1 1), spacing=1,
// margin=1 on both metal layers (bot/top both ENCLOSURE 1 1, so
// margin_x=margin_y=max(1,1)=1) and width=13 is chosen so the fit is
// exact: floor((13 - 2*1 + 1) / (2+1)) = floor(12/3) = 4 cuts per axis,
// consuming exactly 4*2 + 3*1 + 2*1 = 13 of the available width - no
// slack, so this also confirms the fit isn't off-by-one in either
// direction.
TEST_F(ViaShapesFixture, FitsAGenerateViaRulesCutArrayToTheAvailableRoutingWidth)
{
    const LayerId cut_layer = root.create_layer(LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});

    const ViaRuleId via_rule_id = root.create_via_rule(ViaRuleData{.technology = technology_id, .name = "GENRULE", .is_generate = true});
    root.create_via_rule_layer(ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "M1", .enclosure_overhang1 = 1, .enclosure_overhang2 = 1});
    root.create_via_rule_layer(ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "M2", .enclosure_overhang1 = 1, .enclosure_overhang2 = 1});
    root.create_via_rule_layer(ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "V1", .spacing_step_x = 1, .spacing_step_y = 1, .rect = Rect{.ll = {-1, -1}, .ur = {1, 1}}});

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "GENRULE", .origin = Point{50, 50}, .width = 13});

    std::unordered_map<ViewLayerId, std::vector<RenderShape>> shapes_by_layer;
    append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes_by_layer);

    const ViewLayerId cut_view_layer = view_layers.find(cut_layer, ViewLayerPurpose::TERMINAL);
    const auto cut_it = shapes_by_layer.find(cut_view_layer);
    ASSERT_NE(cut_it, shapes_by_layer.end());
    ASSERT_EQ(cut_it->second.size(), 1u);
    EXPECT_EQ(cut_it->second.front().rects.size(), 16u); // 4 rows x 4 cols

    EXPECT_NE(shapes_by_layer.find(view_layers.find(m1, ViewLayerPurpose::TERMINAL)), shapes_by_layer.end());
    EXPECT_NE(shapes_by_layer.find(view_layers.find(m2, ViewLayerPurpose::TERMINAL)), shapes_by_layer.end());
}

// No routing-width context at all (ShapeVia.width unset - e.g. a LEF
// PORT/OBS VIA, which has no enclosing routed path) falls back to a
// single cut, the same "nothing to size an array from" meaning a
// ViaRuleReference with no ROWCOL clause already uses.
TEST_F(ViaShapesFixture, FallsBackToASingleCutForAGenerateViaRuleWithNoWidthContext)
{
    const LayerId cut_layer = root.create_layer(LayerData{.technology = technology_id, .name = "V1", .type = "CUT"});

    const ViaRuleId via_rule_id = root.create_via_rule(ViaRuleData{.technology = technology_id, .name = "GENRULE", .is_generate = true});
    root.create_via_rule_layer(ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "M1", .enclosure_overhang1 = 1, .enclosure_overhang2 = 1});
    root.create_via_rule_layer(ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "M2", .enclosure_overhang1 = 1, .enclosure_overhang2 = 1});
    root.create_via_rule_layer(ViaRuleLayerData{.via_rule = via_rule_id, .layer_name = "V1", .spacing_step_x = 1, .spacing_step_y = 1, .rect = Rect{.ll = {-1, -1}, .ur = {1, 1}}});

    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "GENRULE", .origin = Point{50, 50}}); // width left unset

    std::unordered_map<ViewLayerId, std::vector<RenderShape>> shapes_by_layer;
    append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes_by_layer);

    const ViewLayerId cut_view_layer = view_layers.find(cut_layer, ViewLayerPurpose::TERMINAL);
    const auto cut_it = shapes_by_layer.find(cut_view_layer);
    ASSERT_NE(cut_it, shapes_by_layer.end());
    ASSERT_EQ(cut_it->second.size(), 1u);
    EXPECT_EQ(cut_it->second.front().rects.size(), 1u);
}

// A via_name resolving to neither an explicit Via/LayoutVia, nor a
// ViaRuleReference, nor a GENERATE ViaRule is still skipped, not an
// error - the pre-existing "no explicit geometry to draw" behavior for
// every other case must survive tier 3's own addition.
TEST_F(ViaShapesFixture, SkipsAViaNameResolvingToNothingAtAll)
{
    Shape shape{.layer = m1, .rects = {Rect{.ll = {0, 0}, .ur = {10, 10}}}};
    shape.vias.push_back(ShapeVia{.via_name = "NO_SUCH_VIA_OR_RULE", .origin = Point{50, 50}});

    std::unordered_map<ViewLayerId, std::vector<RenderShape>> shapes_by_layer;
    append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes_by_layer);

    EXPECT_TRUE(shapes_by_layer.empty());
}

// New (not a port - pipelines.old's own suite never had one either,
// found while porting the tests above): a DEF routed path's own "VIA ...
// DO n BY m STEP x y" array placement (or LEF VIA ITERATE) - the *other*
// "via array" concept via_shapes.hpp handles, distinct from a single
// via's own internal ROWCOL cut array (the tests above) - is expanded
// into one independently-resolved via placement per grid position, each
// via_iterates.hpp comment's own append_one_via call reusing the exact
// same tier-1/2/3 resolution as a plain ShapeVia.
TEST_F(ViaShapesFixture, ExpandsAViaIterateIntoOneViaPerGridPosition)
{
    const ViaId via_id = root.create_via(ViaData{.technology = technology_id, .name = "VIA12"});
    root.create_via_layer(ViaLayerData{.via = via_id, .layer_name = "M2", .rects = {Rect{.ll = {-1, -1}, .ur = {1, 1}}}});

    Shape shape{.layer = m1};
    shape.via_iterates.push_back(ShapeViaIterate{.via_name = "VIA12", .origin = Point{0, 0}, .num_x = 2, .num_y = 3, .space_x = 10, .space_y = 20});

    std::unordered_map<ViewLayerId, std::vector<RenderShape>> shapes_by_layer;
    append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes_by_layer);

    const ViewLayerId expected = view_layers.find(m2, ViewLayerPurpose::TERMINAL);
    const auto it = shapes_by_layer.find(expected);
    ASSERT_NE(it, shapes_by_layer.end());
    ASSERT_EQ(it->second.size(), 6u); // 2 x 3 grid = 6 separate via placements

    std::vector<std::pair<int64_t, int64_t>> positions;
    for (const RenderShape &via_shape : it->second)
    {
        ASSERT_EQ(via_shape.rects.size(), 1u);
        positions.emplace_back(via_shape.rects[0].ll.x, via_shape.rects[0].ll.y);
    }
    std::sort(positions.begin(), positions.end());
    const std::vector<std::pair<int64_t, int64_t>> expected_positions = {
        {-1, -1}, {-1, 19}, {-1, 39}, {9, -1}, {9, 19}, {9, 39},
    };
    EXPECT_EQ(positions, expected_positions);
}
