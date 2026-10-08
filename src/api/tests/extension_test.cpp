#include "le/extension.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace
{
    // Runs `f` on its own thread and returns what it returned, or nullopt if
    // it hasn't finished in a few seconds - so a deadlock fails the test
    // instead of hanging it. A thread that never finishes is left running.
    // An exception (libstdc++ throws EDEADLK for a same-thread re-lock)
    // is rethrown here, where gtest reports it.
    template <class F>
    auto within_timeout(F f) -> std::optional<decltype(f())>
    {
        auto result = std::make_shared<std::promise<decltype(f())>>();
        std::future<decltype(f())> future = result->get_future();
        std::thread([result, f]() {
            try
            {
                result->set_value(f());
            }
            catch (...)
            {
                result->set_exception(std::current_exception());
            }
        }).detach();
        if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            return std::nullopt;
        return future.get();
    }

    LeLayoutId add_layout(LeHandle *handle)
    {
        return le_create_layout(handle, le_create_design(handle, le_create_library(handle, "lib"), "top"));
    }

    le::AbstractId add_abstract(le::ext::WriteView &view)
    {
        const le::LibraryId library = view.create_library({.name = "lib"}).value();
        const le::DesignId design = view.create_design({.library = library, .name = "cell"}).value();
        return view.create_abstract({.design = design}).value();
    }
}

TEST(ExtensionIds, RoundTripBetweenTheCApiAndTheDatabase)
{
    const le::LayoutId id{.index = 7, .generation = 3};
    const LeLayoutId c = le::ext::to_c(id);
    EXPECT_EQ(c.index, 7u);
    EXPECT_EQ(c.generation, 3u);
    EXPECT_EQ(le::ext::from_c(c), id);
}

TEST(ExtensionIds, AnInvalidIdStaysInvalid)
{
    EXPECT_EQ(le::ext::to_c(le::DesignId{}).index, UINT32_MAX);
    EXPECT_FALSE(le::ext::from_c(LeDesignId{UINT32_MAX, 0}).valid());
}

TEST(ExtensionContext, CurrentObjectsAreInvalidUntilSet)
{
    LeHandle *handle = le_create();
    const le::ext::ExtensionContext ctx(handle, "test_ext");
    EXPECT_FALSE(ctx.current_technology().valid());
    EXPECT_FALSE(ctx.current_abstract().valid());
    EXPECT_FALSE(ctx.current_schematic().valid());
    EXPECT_FALSE(ctx.current_layout().valid());
    le_destroy(handle);
}

TEST(ExtensionContext, CurrentObjectsMatchWhatWasSet)
{
    LeHandle *handle = le_create();
    const LeLayoutId layout = add_layout(handle);
    const LeSchematicId schematic = le_create_schematic(handle, le_create_design(handle, le_create_library(handle, "logic"), "top"));
    ASSERT_EQ(le_set_current_layout(handle, layout), 0);
    ASSERT_EQ(le_set_current_schematic(handle, schematic), 0);

    const le::ext::ExtensionContext ctx(handle, "test_ext");
    EXPECT_EQ(ctx.current_layout(), le::ext::from_c(layout));
    EXPECT_EQ(ctx.current_schematic(), le::ext::from_c(schematic));
    le_destroy(handle);
}

TEST(ExtensionContext, CurrentObjectsAreReadableInsideViewsAndTransactions)
{
    LeHandle *handle = le_create();
    const LeLayoutId layout = add_layout(handle);
    ASSERT_EQ(le_set_current_layout(handle, layout), 0);
    const le::LayoutId expected = le::ext::from_c(layout);

    const auto in_read = within_timeout([handle]() {
        le::ext::ExtensionContext ctx(handle, "test_ext");
        const le::ext::ReadView view = ctx.read();
        return view.current_layout();
    });
    ASSERT_TRUE(in_read.has_value()) << "ReadView::current_layout deadlocked";
    EXPECT_EQ(*in_read, expected);

    const auto in_write = within_timeout([handle]() {
        le::ext::ExtensionContext ctx(handle, "test_ext");
        const le::ext::WriteView view = ctx.write();
        return view.current_layout();
    });
    ASSERT_TRUE(in_write.has_value()) << "WriteView::current_layout deadlocked";
    EXPECT_EQ(*in_write, expected);

    const auto in_transaction = within_timeout([handle]() {
        le::ext::ExtensionContext ctx(handle, "test_ext");
        const le::ext::Transaction transaction = ctx.transaction("test");
        return ctx.current_layout();
    });
    ASSERT_TRUE(in_transaction.has_value()) << "ExtensionContext::current_layout deadlocked in a transaction";
    EXPECT_EQ(*in_transaction, expected);

    le_destroy(handle);
}

TEST(WriteViewEdits, ALabelledViewIsOneUndoStep)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    le::LibraryId library;
    {
        le::ext::WriteView view = ctx.write("add library");
        library = view.create_library({.name = "lib"}).value();
        ASSERT_TRUE(view.update_library(library, {.name = "renamed"}));
    }
    ASSERT_EQ(le_command_history_count(handle), 1);
    EXPECT_STREQ(le_command_history_at(handle, 0), "add library");
    EXPECT_EQ(ctx.read().root().get_library(library)->name, "renamed");

    ASSERT_EQ(le_undo(handle), 1);
    EXPECT_TRUE(ctx.read().root().get_library_ids().empty());
    ASSERT_EQ(le_redo(handle), 1);
    {
        const le::ext::ReadView view = ctx.read();
        ASSERT_EQ(view.root().get_library_ids().size(), 1u);
        EXPECT_EQ(view.root().get_library(view.root().get_library_ids().front())->name, "renamed");
    }
    le_destroy(handle);
}

TEST(WriteViewEdits, RenamesKeepTheGlobalNameIndexThroughUndoAndRedo)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    le::LibraryId library;
    le::LayerId layer;
    {
        le::ext::WriteView view = ctx.write("create");
        library = view.create_library({.name = "lib"}).value();
        const le::TechnologyId technology = view.create_technology({.database_units_microns = 1000.0}).value();
        layer = view.create_layer({.technology = technology, .name = "M1", .type = "ROUTING"}).value();
    }
    {
        le::ext::WriteView view = ctx.write("rename");
        ASSERT_TRUE(view.update_library(library, {.name = "renamed"}));
        ASSERT_TRUE(view.update_layer(layer, {.name = "M9"}));
    }

    const auto expect_names = [&](const char *library_name, const char *stale_library_name, const char *layer_name, const char *stale_layer_name)
    {
        const le::ext::ReadView view = ctx.read();
        EXPECT_EQ(view.root().get_library_by_name(library_name), library);
        EXPECT_FALSE(view.root().get_library_by_name(stale_library_name).valid());
        EXPECT_EQ(view.root().get_layer_by_name(layer_name), layer);
        EXPECT_FALSE(view.root().get_layer_by_name(stale_layer_name).valid());
    };
    expect_names("renamed", "lib", "M9", "M1");
    ASSERT_EQ(le_undo(handle), 1);
    expect_names("lib", "renamed", "M1", "M9");
    ASSERT_EQ(le_redo(handle), 1);
    expect_names("renamed", "lib", "M9", "M1");
    le_destroy(handle);
}

TEST(WriteViewEdits, ADuplicateGlobalNameIsAnErrorAndRecordsNothing)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    le::LibraryId other;
    {
        le::ext::WriteView view = ctx.write("create");
        ASSERT_TRUE(view.create_library({.name = "lib"}));
        other = view.create_library({.name = "other"}).value();
    }
    {
        le::ext::WriteView view = ctx.write("clash");
        const auto created = view.create_library({.name = "lib"});
        ASSERT_FALSE(created.has_value());
        EXPECT_EQ(created.error(), "a Library with this name ('lib') already exists");
        const auto renamed = view.update_library(other, {.name = "lib"});
        ASSERT_FALSE(renamed.has_value());
        EXPECT_EQ(renamed.error(), "a Library with this name ('lib') already exists");
    }
    EXPECT_EQ(ctx.read().root().get_library_ids().size(), 2u);
    EXPECT_EQ(ctx.read().root().get_library(other)->name, "other");
    le_destroy(handle);
}

TEST(WriteViewEdits, AnUnlabelledViewRecordsNothing)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    {
        le::ext::WriteView view = ctx.write();
        ASSERT_TRUE(view.create_library({.name = "lib"}));
    }
    EXPECT_EQ(le_command_history_count(handle), 0);
    EXPECT_EQ(le_can_undo(handle), 0);
    le_destroy(handle);
}

TEST(WriteViewEdits, ALabelledViewJoinsAnOpenStep)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    le_begin_command(handle, "outer");
    {
        le::ext::WriteView view = ctx.write("inner");
        ASSERT_TRUE(view.create_library({.name = "lib"}));
    }
    EXPECT_EQ(le_is_command_running(handle), 1);
    le_end_command(handle, 1);
    ASSERT_EQ(le_command_history_count(handle), 1);
    EXPECT_STREQ(le_command_history_at(handle, 0), "outer");
    ASSERT_EQ(le_undo(handle), 1);
    EXPECT_TRUE(ctx.read().root().get_library_ids().empty());
    le_destroy(handle);
}

TEST(WriteViewEdits, ErrorsComeBackAsValues)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    {
        le::ext::WriteView view = ctx.write("errors");

        const auto orphan = view.create_design({.library = le::LibraryId{}, .name = "cell"});
        ASSERT_FALSE(orphan);
        EXPECT_EQ(orphan.error(), "unknown library - no such Library exists");

        const le::AbstractId abstract = add_abstract(view);
        ASSERT_TRUE(view.create_terminal({.abstract = abstract, .name = "A"}));
        const auto clash = view.create_terminal({.abstract = abstract, .name = "A"});
        ASSERT_FALSE(clash);
        EXPECT_EQ(clash.error(), "a sibling Terminal with this name ('A') already exists");

        const auto unknown_update = view.update_library(le::LibraryId{}, {.name = "x"});
        ASSERT_FALSE(unknown_update);
        EXPECT_EQ(unknown_update.error(), "unknown id");
        const auto bad_move = view.update_abstract(abstract, {.design = le::DesignId{}});
        ASSERT_FALSE(bad_move);
        EXPECT_EQ(bad_move.error(), "unknown design - no such Design exists");
        EXPECT_FALSE(view.delete_library(le::LibraryId{}));

        const auto ownerless = view.create_shape({.rects = {le::Rect{{0, 0}, {10, 10}}}});
        ASSERT_FALSE(ownerless);
        EXPECT_EQ(ownerless.error(), "an owner is required");
    }
    le_destroy(handle);
}

TEST(WriteViewEdits, ACascadingDeleteUndoes)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    {
        le::ext::WriteView view = ctx.write("setup");
        const le::AbstractId abstract = add_abstract(view);
        ASSERT_TRUE(view.create_terminal({.abstract = abstract, .name = "A"}));
        ASSERT_TRUE(view.create_terminal({.abstract = abstract, .name = "B"}));
    }
    const auto abstract_of = [&ctx]() {
        const le::ext::ReadView view = ctx.read();
        const le::DesignId design = view.root().get_library_designs(view.root().get_library_by_name("lib")).front();
        return view.root().get_design_abstract(design);
    };
    {
        const le::AbstractId abstract = abstract_of();
        le::ext::WriteView view = ctx.write("delete");
        ASSERT_TRUE(view.delete_abstract(abstract));
    }
    EXPECT_FALSE(abstract_of().valid());

    ASSERT_EQ(le_undo(handle), 1);
    const le::AbstractId restored = abstract_of();
    ASSERT_TRUE(restored.valid());
    EXPECT_EQ(ctx.read().root().get_abstract_terminals(restored).size(), 2u);
    le_destroy(handle);
}

namespace
{
    // A Technology (1000 dbu per micron), layers M1/M2 and a Layout holding
    // two free-standing M1 rects, all made through the WriteView API.
    struct ShapeScene
    {
        le::LayerId m1;
        le::LayerId m2;
        le::LayoutId layout;
        std::vector<le::ShapeId> rects;
    };

    ShapeScene add_shape_scene(le::ext::WriteView &view)
    {
        ShapeScene scene;
        const le::TechnologyId technology = view.create_technology({.database_units_microns = 1000}).value();
        scene.m1 = view.create_layer({.technology = technology, .name = "M1", .type = "ROUTING"}).value();
        scene.m2 = view.create_layer({.technology = technology, .name = "M2", .type = "ROUTING"}).value();
        const le::LibraryId library = view.create_library({.name = "lib"}).value();
        const le::DesignId design = view.create_design({.library = library, .name = "top"}).value();
        scene.layout = view.create_layout({.design = design}).value();
        for (const le::Rect rect : {le::Rect{{0, 0}, {1000, 2000}}, le::Rect{{500, 500}, {4000, 4000}}})
            scene.rects.push_back(view.create_shape({.owner = le::ShapeOwner::in_layout(scene.layout), .layer = scene.m1, .rects = {rect}}).value());
        return scene;
    }
}

TEST(ViewShapeOps, BboxMatchesTheCApi)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    ShapeScene scene;
    {
        le::ext::WriteView view = ctx.write();
        scene = add_shape_scene(view);
        const auto box = view.shape_bbox(scene.rects);
        ASSERT_TRUE(box);
        EXPECT_EQ(box->ll.x, 0);
        EXPECT_EQ(box->ll.y, 0);
        EXPECT_EQ(box->ur.x, 4000);
        EXPECT_EQ(box->ur.y, 4000);
    }
    const auto one = ctx.read().shape_bbox({scene.rects[0]});
    ASSERT_TRUE(one);
    EXPECT_EQ(one->ur.x, 1000);
    EXPECT_EQ(one->ur.y, 2000);

    const std::vector<LeShapeId> c_ids{le::ext::to_c(scene.rects[0]), le::ext::to_c(scene.rects[1])};
    const LeShapeBbox c_box = le_shape_bbox(handle, c_ids.data(), 2);
    ASSERT_EQ(c_box.valid, 1);
    EXPECT_DOUBLE_EQ(c_box.ur_x_um, 4.0);
    EXPECT_DOUBLE_EQ(c_box.ur_y_um, 4.0);

    const auto none = ctx.read().shape_bbox({});
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error(), "no shapes given");
    le_destroy(handle);
}

TEST(ViewShapeOps, CreatingOpsReturnTheirIdsAndUndo)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    ShapeScene scene;
    {
        le::ext::WriteView view = ctx.write();
        scene = add_shape_scene(view);
    }
    ASSERT_EQ(le_set_current_layout(handle, le::ext::to_c(scene.layout)), 0);
    {
        le::ext::WriteView view = ctx.write("shape ops");
        // No parent: the open Layout's free shapes.
        const auto copies = view.shape_copy(scene.rects, {.layer = scene.m2});
        ASSERT_TRUE(copies) << copies.error();
        ASSERT_EQ(copies->size(), 2u);
        EXPECT_EQ(view.root().get_shape(copies->front())->layer, scene.m2);

        const auto merged = view.shape_boolean({scene.rects[0]}, {scene.rects[1]}, le::BooleanOp::Or, std::nullopt, scene.layout);
        ASSERT_TRUE(merged) << merged.error();
        ASSERT_EQ(merged->size(), 1u);
        EXPECT_EQ(view.root().get_shape(merged->front())->layer, scene.m1);

        ASSERT_TRUE(view.shape_change_layer({scene.rects[0]}, {.layer = scene.m2}));
        EXPECT_EQ(view.root().get_layout_free_shapes(scene.layout).size(), 5u);
    }
    ASSERT_EQ(le_undo(handle), 1);
    {
        const le::ext::ReadView view = ctx.read();
        EXPECT_EQ(view.root().get_layout_free_shapes(scene.layout).size(), 2u);
        EXPECT_EQ(view.root().get_shape(scene.rects[0])->layer, scene.m1);
    }
    le_destroy(handle);
}

TEST(Units, ConvertAtTheTechnologysScaleRoundingToTheNearestDbu)
{
    const le::ext::Units units(1000.0);
    EXPECT_EQ(units.to_dbu(0.1), 100);
    EXPECT_EQ(units.to_dbu(-0.0016), -2);
    EXPECT_DOUBLE_EQ(units.to_um(int64_t{2500}), 2.5);
    const le::Rect rect = units.to_dbu(le::ext::RectUm{{0.5, 1.0}, {2.0, 3.25}});
    EXPECT_EQ(rect.ll.x, 500);
    EXPECT_EQ(rect.ur.y, 3250);
    const le::ext::RectUm back = units.to_um(rect);
    EXPECT_DOUBLE_EQ(back.ll.x, 0.5);
    EXPECT_DOUBLE_EQ(back.ur.y, 3.25);
}

TEST(Units, NeedATechnology)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    const auto units = ctx.read().units();
    ASSERT_FALSE(units);
    EXPECT_EQ(units.error(), "no Technology with a DATABASE MICRONS scale has been read yet");
    {
        le::ext::WriteView view = ctx.write();
        ASSERT_TRUE(view.create_technology({.database_units_microns = 2000}));
        ASSERT_TRUE(view.units());
        EXPECT_DOUBLE_EQ(view.units()->dbu_per_um(), 2000.0);
    }
    le_destroy(handle);
}

TEST(ShapeBuilder, BuildsAShapeInMicronsAsOneUndoStep)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    ShapeScene scene;
    {
        le::ext::WriteView view = ctx.write();
        scene = add_shape_scene(view);
    }
    le::ShapeId shape;
    {
        le::ext::WriteView view = ctx.write("build");
        const auto built = view.build_shape(le::ShapeOwner::in_layout(scene.layout))
                               .layer(scene.m2)
                               .rect(0, 0, 2, 1)
                               .rect({{3, 3}, {4, 4}})
                               .polygon({{0, 0}, {1, 0}, {0.5, 0.75}})
                               .path(0.1, {{0, 5}, {10, 5}})
                               .create();
        ASSERT_TRUE(built) << built.error();
        shape = *built;

        const le::ShapeData &data = *view.root().get_shape(shape);
        EXPECT_EQ(data.layer, scene.m2);
        EXPECT_EQ(data.owner.kind, le::ShapeOwnerKind::InLayout);
        ASSERT_EQ(data.rects.size(), 2u);
        EXPECT_EQ(data.rects[0].ur.x, 2000);
        EXPECT_EQ(data.rects[0].ur.y, 1000);
        EXPECT_EQ(data.rects[1].ll.x, 3000);
        ASSERT_EQ(data.polygons.size(), 1u);
        EXPECT_EQ(data.polygons[0].points[2].y, 750);
        ASSERT_EQ(data.paths.size(), 1u);
        EXPECT_EQ(data.paths[0].width, 100);
        EXPECT_EQ(data.paths[0].polygon.points[1].x, 10000);

        // Reading back in microns; the path reaches half its width past its ends.
        const le::ext::RectUm box = view.units()->to_um(view.shape_bbox({shape}).value());
        EXPECT_DOUBLE_EQ(box.ll.x, -0.05);
        EXPECT_DOUBLE_EQ(box.ll.y, 0.0);
        EXPECT_DOUBLE_EQ(box.ur.x, 10.05);
        EXPECT_DOUBLE_EQ(box.ur.y, 5.05);
        EXPECT_DOUBLE_EQ(view.units()->to_um(data.polygons[0])[1].x, 1.0);
    }
    ASSERT_EQ(le_undo(handle), 1);
    EXPECT_EQ(ctx.read().root().get_shape(shape), nullptr);
    le_destroy(handle);
}

TEST(ShapeBuilder, ALayerlessShapeTakesAPurpose)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    {
        le::ext::WriteView view = ctx.write();
        const ShapeScene scene = add_shape_scene(view);
        const auto built = view.build_shape(le::ShapeOwner::in_layout(scene.layout)).layer(scene.m1).purpose(le::ShapePurpose::DEBUG).rect(0, 0, 1, 1).create();
        ASSERT_TRUE(built) << built.error();
        EXPECT_FALSE(view.root().get_shape(*built)->layer.valid());
        EXPECT_EQ(view.root().get_shape(*built)->purpose, le::ShapePurpose::DEBUG);
    }
    le_destroy(handle);
}

TEST(ShapeBuilder, ReportsTheFirstErrorAndCreatesNothing)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    {
        le::ext::WriteView view = ctx.write();
        const auto no_technology = view.build_shape(le::ShapeOwner{}).rect(0, 0, 1, 1).create();
        ASSERT_FALSE(no_technology);
        EXPECT_EQ(no_technology.error(), "no Technology with a DATABASE MICRONS scale has been read yet");

        const ShapeScene scene = add_shape_scene(view);
        const size_t shapes = view.root().get_shape_ids().size();
        const auto owner = le::ShapeOwner::in_layout(scene.layout);
        const auto flat = view.build_shape(owner).layer(scene.m1).polygon({{0, 0}, {1, 1}}).path(0.1, {{0, 0}}).create();
        ASSERT_FALSE(flat);
        EXPECT_EQ(flat.error(), "a polygon needs at least 3 points");
        const auto short_path = view.build_shape(owner).layer(scene.m1).path(0.1, {{0, 0}}).create();
        ASSERT_FALSE(short_path);
        EXPECT_EQ(short_path.error(), "a path needs at least 2 points");
        const auto thin_path = view.build_shape(owner).layer(scene.m1).path(0, {{0, 0}, {1, 0}}).create();
        ASSERT_FALSE(thin_path);
        EXPECT_EQ(thin_path.error(), "a path's width must be positive");
        const auto ownerless = view.build_shape(le::ShapeOwner{}).layer(scene.m1).rect(0, 0, 1, 1).create();
        ASSERT_FALSE(ownerless);
        EXPECT_EQ(ownerless.error(), "an owner is required");
        EXPECT_EQ(view.root().get_shape_ids().size(), shapes);
    }
    le_destroy(handle);
}

TEST(WriteViewEdits, RemovingAShapePieceTakesItsMaskAndUndoes)
{
    LeHandle *handle = le_create();
    le::ext::ExtensionContext ctx(handle, "test_ext");
    le::ShapeId shape;
    {
        le::ext::WriteView view = ctx.write();
        const ShapeScene scene = add_shape_scene(view);
        le::ShapeData data{.owner = le::ShapeOwner::in_layout(scene.layout), .layer = scene.m1};
        data.rects = {le::Rect{{0, 0}, {1, 1}}, le::Rect{{2, 2}, {3, 3}}, le::Rect{{4, 4}, {5, 5}}};
        data.rect_masks = {1, 2, 3};
        shape = view.create_shape(data).value();
    }
    {
        le::ext::WriteView view = ctx.write("remove");
        ASSERT_TRUE(view.remove_shape_piece(shape, le::PieceKind::RECT, 1));
        const le::ShapeData &data = *view.root().get_shape(shape);
        ASSERT_EQ(data.rects.size(), 2u);
        EXPECT_EQ(data.rects[1].ll.x, 4);
        EXPECT_EQ(data.rect_masks, (std::vector<int>{1, 3}));

        const auto out_of_range = view.remove_shape_piece(shape, le::PieceKind::RECT, 5);
        ASSERT_FALSE(out_of_range);
        EXPECT_EQ(out_of_range.error(), "index out of range");
        EXPECT_FALSE(view.remove_shape_piece(shape, le::PieceKind::VIA, 0));
        EXPECT_FALSE(view.remove_shape_piece(le::ShapeId{}, le::PieceKind::RECT, 0));
    }
    ASSERT_EQ(le_undo(handle), 1);
    {
        const le::ext::ReadView view = ctx.read();
        EXPECT_EQ(view.root().get_shape(shape)->rects.size(), 3u);
        EXPECT_EQ(view.root().get_shape(shape)->rect_masks, (std::vector<int>{1, 2, 3}));
    }
    le_destroy(handle);
}
