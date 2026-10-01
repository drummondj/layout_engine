#include "../shape_ops.hpp"
#include <gtest/gtest.h>

using namespace le;

namespace
{
    class ShapeOps : public ::testing::Test
    {
    protected:
        Root root;
        LayerId m1;
        LayerId m2;
        AbstractId abstract_id;
        ObstructionId obstruction_id;

        void SetUp() override
        {
            const TechnologyId technology_id = root.create_technology(TechnologyData{.database_units_microns = 1000.0});
            m1 = root.create_layer(LayerData{.technology = technology_id, .name = "M1", .type = "ROUTING"});
            m2 = root.create_layer(LayerData{.technology = technology_id, .name = "M2", .type = "ROUTING"});
            const LibraryId library_id = root.create_library(LibraryData{.name = "LIB"});
            const DesignId design_id = root.create_design(DesignData{.library = library_id, .name = "CELL"});
            abstract_id = root.create_abstract(AbstractData{.design = design_id});
            obstruction_id = root.create_obstruction(ObstructionData{.abstract = abstract_id});
        }

        ShapeId free_rect(LayerId layer, Rect rect)
        {
            return root.create_shape(ShapeData{.in_abstract = abstract_id, .layer = layer, .rects = {rect}});
        }
    };
}

TEST_F(ShapeOps, CopyCreatesOneShapePerInputOnTheNewLayer)
{
    const ShapeId a = free_rect(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});
    const ShapeId b = free_rect(m1, Rect{.ll = {20, 0}, .ur = {30, 10}});

    const shape_ops::Result result = shape_ops::copy(root, {a, b}, shape_ops::LayerOrPurpose{.layer = m2}, abstract_id);

    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->size(), 2u);
    for (ShapeId id : *result)
    {
        const Shape *copy = root.get_shape(id);
        ASSERT_NE(copy, nullptr);
        EXPECT_EQ(copy->layer, m2);
        EXPECT_EQ(copy->in_abstract, abstract_id);
        EXPECT_EQ(copy->rects.size(), 1u);
    }
    EXPECT_EQ(root.get_shape(a)->layer, m1); // the original is untouched
    EXPECT_EQ(root.get_abstract_free_shapes(abstract_id).size(), 4u);
}

TEST_F(ShapeOps, BooleanDefaultsToTheFirstInputsLayer)
{
    const ShapeId a = free_rect(m2, Rect{.ll = {0, 0}, .ur = {10, 10}});
    const ShapeId b = free_rect(m1, Rect{.ll = {5, 0}, .ur = {15, 10}});

    const shape_ops::Result result = shape_ops::boolean(root, {a}, {b}, BooleanOp::Or, std::nullopt, abstract_id);

    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->size(), 1u);
    const Shape *merged = root.get_shape(result->front());
    EXPECT_EQ(merged->layer, m2);
    ASSERT_EQ(merged->rects.size(), 1u);
    EXPECT_EQ(merged->rects[0].ur.x, 15);
}

TEST_F(ShapeOps, AnEmptyResultCreatesNothing)
{
    const ShapeId a = free_rect(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});
    const ShapeId b = free_rect(m1, Rect{.ll = {20, 20}, .ur = {30, 30}});
    const uint64_t before = root.get_shape_size();

    const shape_ops::Result result = shape_ops::boolean(root, {a}, {b}, BooleanOp::And, std::nullopt, abstract_id);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
    EXPECT_EQ(root.get_shape_size(), before);
}

TEST_F(ShapeOps, ResultsCanJoinARealParentsShapes)
{
    const ShapeId a = free_rect(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});

    const shape_ops::Result result = shape_ops::to_rects(root, {a}, FractureDirection::Horizontal, std::nullopt, obstruction_id);

    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(root.get_shape(result->front())->obstruction, obstruction_id);
    EXPECT_FALSE(root.get_shape(result->front())->in_abstract.valid());
    ASSERT_EQ(root.get_obstruction_shapes(obstruction_id).size(), 1u);
}

TEST_F(ShapeOps, UnknownInputsFailWithoutCreatingAnything)
{
    const uint64_t before = root.get_shape_size();

    EXPECT_FALSE(shape_ops::copy(root, {ShapeId{}}, shape_ops::LayerOrPurpose{.layer = m2}, abstract_id).has_value());
    EXPECT_FALSE(shape_ops::copy(root, {}, shape_ops::LayerOrPurpose{.layer = m2}, abstract_id).has_value());
    EXPECT_EQ(root.get_shape_size(), before);
}

TEST_F(ShapeOps, UnknownParentOrLayerFails)
{
    const ShapeId a = free_rect(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});

    EXPECT_FALSE(shape_ops::to_polygons(root, {a}, std::nullopt, AbstractId{}).has_value());
    EXPECT_FALSE(shape_ops::to_polygons(root, {a}, shape_ops::LayerOrPurpose{.layer = LayerId{}}, abstract_id).has_value());
}

TEST_F(ShapeOps, AFailingInputLeavesTheDatabaseUntouched)
{
    // The rect alone could be sized, but the triangle can't take different
    // X/Y amounts - nothing may be created for either.
    const ShapeId rect = free_rect(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});
    const ShapeId triangle = root.create_shape(ShapeData{
        .in_abstract = abstract_id,
        .layer = m1,
        .polygons = {Polygon{.points = {{0, 0}, {100, 0}, {0, 100}, {0, 0}}}},
    });
    const uint64_t before = root.get_shape_size();

    const shape_ops::Result result = shape_ops::size(root, {rect, triangle}, 1, 2, std::nullopt, abstract_id);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(root.get_shape_size(), before);
}

TEST_F(ShapeOps, IteratesAreExpandedBeforeOperating)
{
    const ShapeId iterated = root.create_shape(ShapeData{
        .in_abstract = abstract_id,
        .layer = m1,
        .rect_iterates = {RectIterate{.rect = Rect{.ll = {0, 0}, .ur = {10, 10}}, .num_x = 3, .num_y = 1, .space_x = 20, .space_y = 0}},
    });

    const shape_ops::Result result = shape_ops::to_rects(root, {iterated}, FractureDirection::Horizontal, std::nullopt, abstract_id);

    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(root.get_shape(result->front())->rects.size(), 3u);
}

TEST_F(ShapeOps, PathRequiresAPositiveWidth)
{
    const ShapeId a = free_rect(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});

    EXPECT_FALSE(shape_ops::outline_paths(root, {a}, 0, std::nullopt, abstract_id).has_value());

    const shape_ops::Result result = shape_ops::outline_paths(root, {a}, 2, std::nullopt, abstract_id);
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(root.get_shape(result->front())->paths.size(), 1u);
}

TEST_F(ShapeOps, BboxSpansEveryInput)
{
    const ShapeId a = free_rect(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});
    const ShapeId b = free_rect(m2, Rect{.ll = {50, -5}, .ur = {60, 5}});

    const auto box = shape_ops::bbox(root, {a, b});

    ASSERT_TRUE(box.has_value()) << box.error();
    EXPECT_EQ(box->ll.x, 0);
    EXPECT_EQ(box->ll.y, -5);
    EXPECT_EQ(box->ur.x, 60);
    EXPECT_EQ(box->ur.y, 10);
}

TEST_F(ShapeOps, ADebugTargetCreatesLayerlessDebugShapes)
{
    const ShapeId a = free_rect(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});

    const shape_ops::Result result = shape_ops::copy(root, {a}, shape_ops::LayerOrPurpose{.purpose = ShapePurpose::DEBUG}, abstract_id);

    ASSERT_TRUE(result.has_value()) << result.error();
    const Shape *debug = root.get_shape(result->front());
    EXPECT_FALSE(debug->layer.valid());
    EXPECT_EQ(debug->purpose, ShapePurpose::DEBUG);

    // A per-input op without an explicit target inherits the debug purpose.
    const shape_ops::Result grown = shape_ops::size(root, {result->front()}, 1, 1, std::nullopt, abstract_id);
    ASSERT_TRUE(grown.has_value()) << grown.error();
    EXPECT_EQ(root.get_shape(grown->front())->purpose, ShapePurpose::DEBUG);
}

TEST_F(ShapeOps, ChangeLayerOntoAndOffTheDebugLayerKeepsExactlyOneOfLayerAndPurpose)
{
    const ShapeId a = free_rect(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});

    const auto to_debug = shape_ops::change_layer(root, {a}, shape_ops::LayerOrPurpose{.purpose = ShapePurpose::DEBUG});
    ASSERT_TRUE(to_debug.has_value()) << to_debug.error();
    EXPECT_FALSE(root.get_shape(a)->layer.valid());
    EXPECT_EQ(root.get_shape(a)->purpose, ShapePurpose::DEBUG);
    EXPECT_EQ(to_debug->front().before.layer, m1);

    // Back onto a real layer: the purpose must be cleared, not left behind.
    const auto to_m2 = shape_ops::change_layer(root, {a}, shape_ops::LayerOrPurpose{.layer = m2});
    ASSERT_TRUE(to_m2.has_value()) << to_m2.error();
    EXPECT_EQ(root.get_shape(a)->layer, m2);
    EXPECT_FALSE(root.get_shape(a)->purpose.has_value());
    EXPECT_EQ(root.get_shape(a)->rects.size(), 1u); // geometry untouched
}

TEST_F(ShapeOps, ChangeLayerIsAllOrNothing)
{
    const ShapeId a = free_rect(m1, Rect{.ll = {0, 0}, .ur = {10, 10}});

    EXPECT_FALSE(shape_ops::change_layer(root, {a, ShapeId{}}, shape_ops::LayerOrPurpose{.layer = m2}).has_value());
    EXPECT_EQ(root.get_shape(a)->layer, m1);
    EXPECT_FALSE(shape_ops::change_layer(root, {a}, shape_ops::LayerOrPurpose{}).has_value()); // neither layer nor purpose
}
