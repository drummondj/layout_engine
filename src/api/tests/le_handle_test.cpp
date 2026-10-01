#include "../le_handle.hpp"
#include <gtest/gtest.h>

using namespace le;

TEST(LeHandle, DefaultStateIsSensible)
{
    LeHandle handle;

    EXPECT_FALSE(handle.current_abstract().valid());
    EXPECT_EQ(handle.pan().x, 0);
    EXPECT_EQ(handle.pan().y, 0);
    EXPECT_DOUBLE_EQ(handle.scale(), 1.0);
    EXPECT_EQ(handle.viewport_width_px(), 0);
    EXPECT_EQ(handle.viewport_height_px(), 0);
    EXPECT_TRUE(handle.selection().empty());
    EXPECT_EQ(handle.viewport_version(), 0u);
    EXPECT_EQ(handle.visibility_version(), 0u);
    EXPECT_EQ(handle.minor_grid_spacing(), 5);
    EXPECT_EQ(handle.major_grid_spacing(), 50);
    EXPECT_FALSE(handle.current_layout().valid());
    EXPECT_EQ(handle.hierarchy_depth(), 0);
    EXPECT_EQ(handle.hierarchy_version(), 0u);
}

TEST(LeHandle, CurrentAbstractRoundTrips)
{
    LeHandle handle;
    AbstractId id{5, 1};
    handle.set_current_abstract(id);
    EXPECT_EQ(handle.current_abstract(), id);
}

TEST(LeHandle, SwitchingToADifferentAbstractClearsSelection)
{
    // Regression: TerminalId/ObstructionId/ShapeId are plain
    // {index,generation} pool handles, not namespaced by Abstract - a
    // selection left over from the old Abstract could otherwise
    // reference nothing (best case) or an unrelated object that happens
    // to reuse the same pool slot in the new Abstract (worst case).
    LeHandle handle;
    handle.set_current_abstract(AbstractId{1, 0});

    handle.select(ShapeId{1, 0});
    ASSERT_FALSE(handle.selection().empty());

    handle.set_current_abstract(AbstractId{2, 0});
    EXPECT_TRUE(handle.selection().empty());
}

TEST(LeHandle, SwitchingToADifferentAbstractClearsRulers)
{
    // Regression: rulers are plain dbu Points with no Abstract scoping
    // at all - left uncleared they'd go on being drawn, at the same raw
    // coordinates, over whatever design happens to occupy that part of
    // the new Abstract's own unrelated coordinate space.
    LeHandle handle;
    handle.set_current_abstract(AbstractId{1, 0});
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.set_mouse_position(10, 90); // dbu (10, 10)
    handle.add_ruler_point(false);
    ASSERT_FALSE(handle.rulers().empty());

    handle.set_current_abstract(AbstractId{2, 0});
    EXPECT_TRUE(handle.rulers().empty());
}

TEST(LeHandle, SwitchingToADifferentAbstractBumpsSelectionVersionOnlyIfSelectionWasNonEmpty)
{
    LeHandle handle;
    handle.set_current_abstract(AbstractId{1, 0});
    EXPECT_EQ(handle.selection_version(), 0u);

    // Nothing selected - switching must not bump the version for nothing.
    handle.set_current_abstract(AbstractId{2, 0});
    EXPECT_EQ(handle.selection_version(), 0u);

    handle.select(ShapeId{1, 0});
    ASSERT_EQ(handle.selection_version(), 1u);

    handle.set_current_abstract(AbstractId{3, 0});
    EXPECT_EQ(handle.selection_version(), 2u);
}

TEST(LeHandle, SettingTheSameCurrentAbstractAgainIsANoOp)
{
    LeHandle handle;
    handle.set_current_abstract(AbstractId{1, 0});
    handle.select(ShapeId{1, 0});
    ASSERT_EQ(handle.selection_version(), 1u);

    handle.set_current_abstract(AbstractId{1, 0}); // same Abstract already displayed
    EXPECT_FALSE(handle.selection().empty());
    EXPECT_EQ(handle.selection_version(), 1u);
}

TEST(LeHandle, CurrentLayoutRoundTrips)
{
    LeHandle handle;
    LayoutId id{5, 1};
    handle.set_current_layout(id);
    EXPECT_EQ(handle.current_layout(), id);
}

TEST(LeHandle, SwitchingToADifferentLayoutClearsSelectionAndRulers)
{
    // Mirrors SwitchingToADifferentAbstractClearsSelection/
    // ClearsRulers - same reasoning (see those tests' own comments)
    // applies to current_layout_ too.
    LeHandle handle;
    handle.set_current_layout(LayoutId{1, 0});
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.select(ShapeId{1, 0});
    handle.set_mouse_position(10, 90); // dbu (10, 10)
    handle.add_ruler_point(false);
    ASSERT_FALSE(handle.selection().empty());
    ASSERT_FALSE(handle.rulers().empty());

    handle.set_current_layout(LayoutId{2, 0});
    EXPECT_TRUE(handle.selection().empty());
    EXPECT_TRUE(handle.rulers().empty());
}

TEST(LeHandle, SettingTheSameCurrentLayoutAgainIsANoOp)
{
    LeHandle handle;
    handle.set_current_layout(LayoutId{1, 0});
    handle.select(ShapeId{1, 0});
    ASSERT_EQ(handle.selection_version(), 1u);

    handle.set_current_layout(LayoutId{1, 0}); // same Layout already displayed
    EXPECT_FALSE(handle.selection().empty());
    EXPECT_EQ(handle.selection_version(), 1u);
}

TEST(LeHandle, HierarchyDepthRoundTripsAndBumpsItsOwnVersion)
{
    LeHandle handle;
    handle.set_hierarchy_depth(2);
    EXPECT_EQ(handle.hierarchy_depth(), 2);
    EXPECT_EQ(handle.hierarchy_version(), 1u);

    handle.set_hierarchy_depth(2); // same value again - no-op
    EXPECT_EQ(handle.hierarchy_version(), 1u);

    handle.set_hierarchy_depth(0);
    EXPECT_EQ(handle.hierarchy_depth(), 0);
    EXPECT_EQ(handle.hierarchy_version(), 2u);
}

TEST(LeHandle, SetHierarchyDepthIgnoresNegativeValues)
{
    LeHandle handle;
    handle.set_hierarchy_depth(3);
    ASSERT_EQ(handle.hierarchy_depth(), 3);

    handle.set_hierarchy_depth(-1);
    EXPECT_EQ(handle.hierarchy_depth(), 3);
    EXPECT_EQ(handle.hierarchy_version(), 1u);
}

TEST(LeHandle, SelectRecordsTheShapeId)
{
    LeHandle handle;
    handle.select(ShapeId{1, 0});

    EXPECT_TRUE(handle.is_selected(ShapeId{1, 0}));
    ASSERT_FALSE(handle.selection().empty());
    EXPECT_EQ(std::get<LeHandle::ShapePiece>(handle.selection().front()).shape_id, (ShapeId{1, 0}));
}

TEST(LeHandle, SelectingADifferentShapeAddsASecondEntry)
{
    // The actual reported bug's own regression test: shift-clicking a
    // second shape must add a second selection entry - both shapes end
    // up independently selected/highlighted/reportable - not replace the
    // first one or no-op against it.
    LeHandle handle;
    handle.select(ShapeId{1, 0});
    ASSERT_EQ(handle.selection_version(), 1u);
    ASSERT_EQ(handle.selection().size(), 1u);

    handle.select(ShapeId{2, 0}); // shift-clicking a different shape
    EXPECT_EQ(handle.selection_version(), 2u);
    ASSERT_EQ(handle.selection().size(), 2u); // both shapes are now separately selected

    EXPECT_EQ(std::get<LeHandle::ShapePiece>(handle.selection()[0]).shape_id, (ShapeId{1, 0}));
    EXPECT_EQ(std::get<LeHandle::ShapePiece>(handle.selection()[1]).shape_id, (ShapeId{2, 0}));
}

TEST(LeHandle, ReselectingTheSameShapeIsANoOp)
{
    LeHandle handle;
    handle.select(ShapeId{1, 0});
    ASSERT_EQ(handle.selection_version(), 1u);

    handle.select(ShapeId{1, 0});
    EXPECT_EQ(handle.selection_version(), 1u);
    EXPECT_EQ(handle.selection().size(), 1u);
}

TEST(LeHandle, SelectDedupsCorrectlyAcrossManyDistinctShapes)
{
    // Regression/refactor-safety test for select()'s O(1)-average dedup
    // (selected_ids_, a plain unordered_set<ShapeId>) - this matters
    // because le_mouse_up's drag-select branch (api.cpp) calls select()
    // once per enclosed piece, and a real design can put hundreds of
    // thousands of pieces under one shared Obstruction's OBS block (see
    // BENCHMARKS.md). Mixes distinct new ids with re-selecting already-
    // selected ones (in original and reverse order) and checks the exact
    // resulting count/version.
    LeHandle handle;

    std::vector<ShapeId> ids;
    for (uint32_t i = 0; i < 20; ++i)
        ids.push_back(ShapeId{i, 0});

    for (const ShapeId &id : ids)
        handle.select(id);
    ASSERT_EQ(handle.selection().size(), 20u);
    ASSERT_EQ(handle.selection_version(), 20u);

    // Re-select every id again, in reverse order - every one should be
    // recognized as an existing duplicate (no-op).
    for (auto it = ids.rbegin(); it != ids.rend(); ++it)
        handle.select(*it);
    EXPECT_EQ(handle.selection().size(), 20u);
    EXPECT_EQ(handle.selection_version(), 20u);

    // One genuinely new id still gets added correctly afterward.
    handle.select(ShapeId{1000, 0});
    EXPECT_EQ(handle.selection().size(), 21u);
    EXPECT_EQ(handle.selection_version(), 21u);
}

TEST(LeHandle, DeselectRemovesAnEntry)
{
    LeHandle handle;
    handle.select(ShapeId{1, 0});
    ASSERT_TRUE(handle.is_selected(ShapeId{1, 0}));

    handle.deselect(ShapeId{1, 0});
    EXPECT_FALSE(handle.is_selected(ShapeId{1, 0}));
    EXPECT_TRUE(handle.selection().empty());
}

// E1 (BUGS_AND_ENHANCEMENTS.md) - Row/Placement/Region are bare-id
// LeHandle::SelectedObject alternatives (no backing Shape - see LeHandle::ShapePiece's own
// comment), so they get their own select()/deselect()/is_selected()
// overload set rather than riding the ShapeId+piece one. Same dedup/
// version-bump contract as the ShapeId overload above.
TEST(LeHandle, SelectRowRecordsItAndDedups)
{
    LeHandle handle;
    handle.select(RowId{1, 0});

    EXPECT_TRUE(handle.is_selected(RowId{1, 0}));
    ASSERT_EQ(handle.selection().size(), 1u);
    EXPECT_EQ(std::get<RowId>(handle.selection().front()), (RowId{1, 0}));
    EXPECT_EQ(handle.selection_version(), 1u);

    handle.select(RowId{1, 0}); // re-selecting the same row is a no-op
    EXPECT_EQ(handle.selection().size(), 1u);
    EXPECT_EQ(handle.selection_version(), 1u);
}

TEST(LeHandle, SelectPlacementAndRegionAlsoWork)
{
    LeHandle handle;
    handle.select(PlacementId{1, 0});
    handle.select(RegionId{1, 0});

    EXPECT_TRUE(handle.is_selected(PlacementId{1, 0}));
    EXPECT_TRUE(handle.is_selected(RegionId{1, 0}));
    EXPECT_FALSE(handle.is_selected(PlacementId{2, 0}));
    ASSERT_EQ(handle.selection().size(), 2u);
    EXPECT_EQ(handle.selection_version(), 2u);
}

TEST(LeHandle, DeselectRowRemovesIt)
{
    LeHandle handle;
    handle.select(RowId{1, 0});
    ASSERT_TRUE(handle.is_selected(RowId{1, 0}));

    handle.deselect(RowId{1, 0});
    EXPECT_FALSE(handle.is_selected(RowId{1, 0}));
    EXPECT_TRUE(handle.selection().empty());
}

// A ShapeId and a bare-id kind with the same numeric index/generation
// must never collide - they're different alternatives of the same
// variant, and std::set<LeHandle::SelectedObject>'s own ordering (operator<=>)
// distinguishes alternatives before comparing their payload.
TEST(LeHandle, ShapePieceAndBareIdSelectionsCoexistWithoutColliding)
{
    LeHandle handle;
    handle.select(ShapeId{1, 0});
    handle.select(RowId{1, 0});
    handle.select(PlacementId{1, 0});
    handle.select(RegionId{1, 0});

    EXPECT_EQ(handle.selection().size(), 4u);
    EXPECT_EQ(handle.selection_version(), 4u);
    EXPECT_TRUE(handle.is_selected(ShapeId{1, 0}));
    EXPECT_TRUE(handle.is_selected(RowId{1, 0}));
    EXPECT_TRUE(handle.is_selected(PlacementId{1, 0}));
    EXPECT_TRUE(handle.is_selected(RegionId{1, 0}));
}

TEST(LeHandle, PanAndViewportRoundTrip)
{
    LeHandle handle;
    handle.set_pan(Point{100, -200});
    handle.set_viewport_size(1920, 1080);

    EXPECT_EQ(handle.pan().x, 100);
    EXPECT_EQ(handle.pan().y, -200);
    EXPECT_EQ(handle.viewport_width_px(), 1920);
    EXPECT_EQ(handle.viewport_height_px(), 1080);
}

TEST(LeHandle, SetScaleIgnoresNonPositiveValues)
{
    LeHandle handle;
    handle.set_scale(2.5);
    ASSERT_DOUBLE_EQ(handle.scale(), 2.5);

    handle.set_scale(0.0);
    EXPECT_DOUBLE_EQ(handle.scale(), 2.5); // unchanged

    handle.set_scale(-1.0);
    EXPECT_DOUBLE_EQ(handle.scale(), 2.5); // unchanged
}

TEST(LeHandle, ViewportVersionBumpsOnPanScaleAndViewportSizeChangesOnly)
{
    LeHandle handle;
    EXPECT_EQ(handle.viewport_version(), 0u);

    handle.set_pan(Point{1, 1});
    EXPECT_EQ(handle.viewport_version(), 1u);

    handle.set_scale(2.0);
    EXPECT_EQ(handle.viewport_version(), 2u);

    handle.set_viewport_size(100, 100);
    EXPECT_EQ(handle.viewport_version(), 3u);

    // A rejected (non-positive) scale must not bump the version - nothing
    // about the viewport actually changed.
    handle.set_scale(-1.0);
    EXPECT_EQ(handle.viewport_version(), 3u);

    // Unrelated state (visibility) must not bump it either.
    handle.set_layer_name_visible("M1", false);
    EXPECT_EQ(handle.viewport_version(), 3u);
}

TEST(LeHandle, GridSpacingRoundTrips)
{
    LeHandle handle;
    handle.set_minor_grid_spacing(10);
    handle.set_major_grid_spacing(100);

    EXPECT_EQ(handle.minor_grid_spacing(), 10);
    EXPECT_EQ(handle.major_grid_spacing(), 100);
}

TEST(LeHandle, GridSpacingIgnoresNonPositiveValues)
{
    LeHandle handle;
    handle.set_minor_grid_spacing(10);
    handle.set_major_grid_spacing(100);

    handle.set_minor_grid_spacing(0);
    handle.set_minor_grid_spacing(-5);
    EXPECT_EQ(handle.minor_grid_spacing(), 10); // unchanged

    handle.set_major_grid_spacing(0);
    handle.set_major_grid_spacing(-5);
    EXPECT_EQ(handle.major_grid_spacing(), 100); // unchanged
}

TEST(LeHandle, GridSpacingSettersBumpVisibilityVersion)
{
    // The grid is part of the rendered picture (see Renderer::draw_grid),
    // so changing its spacing must invalidate the same render cache
    // layer visibility does - unlike selectability, which doesn't.
    LeHandle handle;
    EXPECT_EQ(handle.visibility_version(), 0u);

    handle.set_minor_grid_spacing(10);
    EXPECT_EQ(handle.visibility_version(), 1u);

    handle.set_major_grid_spacing(100);
    EXPECT_EQ(handle.visibility_version(), 2u);

    // A rejected (non-positive) value must not bump it.
    handle.set_minor_grid_spacing(-5);
    EXPECT_EQ(handle.visibility_version(), 2u);
}

TEST(LeHandle, RulerLabelSizeDefaultsToElevenPixels)
{
    LeHandle handle;
    EXPECT_DOUBLE_EQ(handle.ruler_label_size_px(), 11.0);
}

TEST(LeHandle, RulerLabelSizeRoundTrips)
{
    LeHandle handle;
    handle.set_ruler_label_size_px(20.0);
    EXPECT_DOUBLE_EQ(handle.ruler_label_size_px(), 20.0);
}

TEST(LeHandle, RulerLabelSizeIgnoresNonPositiveValues)
{
    LeHandle handle;
    handle.set_ruler_label_size_px(20.0);

    handle.set_ruler_label_size_px(0.0);
    handle.set_ruler_label_size_px(-5.0);
    EXPECT_DOUBLE_EQ(handle.ruler_label_size_px(), 20.0); // unchanged
}

TEST(LeHandle, RulerLabelSizeSetterBumpsVisibilityVersionOnlyOnAnActualChange)
{
    // Both new/changed ruler overlay stages already key on
    // visibility_version() alongside ruler_version() - reusing this
    // signal (like grid spacing does) invalidates them with no new
    // plumbing.
    LeHandle handle;
    EXPECT_EQ(handle.visibility_version(), 0u);

    handle.set_ruler_label_size_px(20.0);
    EXPECT_EQ(handle.visibility_version(), 1u);

    // A rejected (non-positive) value must not bump it.
    handle.set_ruler_label_size_px(-5.0);
    EXPECT_EQ(handle.visibility_version(), 1u);
}

TEST(LeHandle, MousePositionDefaultsToUnset)
{
    LeHandle handle;
    EXPECT_FALSE(handle.has_mouse_position());
    EXPECT_EQ(handle.mouse_version(), 0u);
    EXPECT_FALSE(handle.mouse_dbu_position().has_value());
    EXPECT_FALSE(handle.snapped_mouse_position().has_value());
}

TEST(LeHandle, SetMousePositionBumpsMouseVersionNotViewportOrVisibilityVersion)
{
    // A mouse move must invalidate only the cheap overlay-picture cache
    // (see Renderer::build_overlay_picture/compose_with_overlays), not the
    // expensive design rasterize cache keyed on viewport/visibility
    // version - see handle.hpp's own comment on mouse_version_.
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_viewport_size(100, 100);
    const uint64_t viewport_version_before = handle.viewport_version();
    const uint64_t visibility_version_before = handle.visibility_version();

    handle.set_mouse_position(10, 20);
    EXPECT_TRUE(handle.has_mouse_position());
    EXPECT_EQ(handle.mouse_version(), 1u);
    EXPECT_EQ(handle.viewport_version(), viewport_version_before);
    EXPECT_EQ(handle.visibility_version(), visibility_version_before);

    handle.set_mouse_position(11, 20);
    EXPECT_EQ(handle.mouse_version(), 2u);
}

TEST(LeHandle, MouseDbuPositionUndoesPanScaleAndYFlip)
{
    // Pixel space is top-left origin/y-down (matches le_render_pixel_buffer's
    // output image and le_zoom's x/y); dbu space is y-up - see handle.hpp's
    // mouse_dbu_position comment for the exact inverse formula.
    LeHandle handle;
    handle.set_pan(Point{100, 200});
    handle.set_scale(2.0);
    handle.set_viewport_size(50, 40);

    handle.set_mouse_position(10, 10);
    const std::optional<Point> dbu = handle.mouse_dbu_position();
    ASSERT_TRUE(dbu.has_value());
    EXPECT_EQ(dbu->x, 100 + 10 / 2); // pan.x + x_px / scale
    EXPECT_EQ(dbu->y, 200 + (40 - 10) / 2); // pan.y + (height - y_px) / scale
}

TEST(LeHandle, SnappedMousePositionRoundsToNearestMinorGridMultiple)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(10);

    // dbu (23, 100 - 27) = (23, 73); nearest multiples of 10 are 20 and 70.
    handle.set_mouse_position(23, 27);
    const std::optional<Point> snapped = handle.snapped_mouse_position();
    ASSERT_TRUE(snapped.has_value());
    EXPECT_EQ(snapped->x, 20);
    EXPECT_EQ(snapped->y, 70);
}

TEST(LeHandle, ClearMousePositionResetsHasMousePositionAndBumpsVersionOnlyIfItWasSet)
{
    LeHandle handle;
    handle.set_mouse_position(5, 5);
    ASSERT_EQ(handle.mouse_version(), 1u);

    handle.clear_mouse_position();
    EXPECT_FALSE(handle.has_mouse_position());
    EXPECT_FALSE(handle.mouse_dbu_position().has_value());
    EXPECT_EQ(handle.mouse_version(), 2u);

    // Clearing an already-clear position is a no-op - must not bump the
    // version again (would otherwise invalidate caches for nothing).
    handle.clear_mouse_position();
    EXPECT_EQ(handle.mouse_version(), 2u);
}

TEST(LeHandle, PixelToDbuMatchesMouseDbuPositionsOwnFormula)
{
    // Same scenario as MouseDbuPositionUndoesPanScaleAndYFlip, but calling
    // pixel_to_dbu directly with an arbitrary x/y rather than going
    // through the stored mouse position - the two must agree exactly,
    // since mouse_dbu_position() is defined in terms of pixel_to_dbu.
    LeHandle handle;
    handle.set_pan(Point{100, 200});
    handle.set_scale(2.0);
    handle.set_viewport_size(50, 40);

    const Point dbu = handle.pixel_to_dbu(10, 10);
    EXPECT_EQ(dbu.x, 100 + 10 / 2);
    EXPECT_EQ(dbu.y, 200 + (40 - 10) / 2);

    handle.set_mouse_position(10, 10);
    ASSERT_TRUE(handle.mouse_dbu_position().has_value());
    EXPECT_EQ(handle.mouse_dbu_position()->x, dbu.x);
    EXPECT_EQ(handle.mouse_dbu_position()->y, dbu.y);
}

TEST(LeHandle, DragDefaultsToNotDragging)
{
    LeHandle handle;
    EXPECT_FALSE(handle.is_dragging());
    EXPECT_FALSE(handle.drag_rect_dbu().has_value());
}

TEST(LeHandle, BeginDragSetsStateAndBumpsMouseVersionNotViewportOrVisibilityVersion)
{
    LeHandle handle;
    const uint64_t viewport_version_before = handle.viewport_version();
    const uint64_t visibility_version_before = handle.visibility_version();

    handle.begin_drag(10, 20);
    EXPECT_TRUE(handle.is_dragging());
    EXPECT_EQ(handle.drag_start_x_px(), 10);
    EXPECT_EQ(handle.drag_start_y_px(), 20);
    EXPECT_EQ(handle.mouse_version(), 1u);
    EXPECT_EQ(handle.viewport_version(), viewport_version_before);
    EXPECT_EQ(handle.visibility_version(), visibility_version_before);
}

TEST(LeHandle, BeginDragDefaultsToSelectKind)
{
    // Regression guard for the defaulted third parameter (UPDATES.md
    // 9.3) - the existing le_mouse_down call site (begin_drag(x, y), no
    // third argument) must keep behaving exactly as before.
    LeHandle handle;
    handle.begin_drag(10, 20);
    EXPECT_EQ(handle.drag_kind(), LeHandle::DragKind::SELECT);
}

TEST(LeHandle, BeginDragAcceptsAnExplicitDragKind)
{
    LeHandle handle;
    handle.begin_drag(10, 20, LeHandle::DragKind::ZOOM);
    EXPECT_EQ(handle.drag_kind(), LeHandle::DragKind::ZOOM);
}

TEST(LeHandle, EndDragClearsDraggingAndBumpsMouseVersion)
{
    LeHandle handle;
    handle.begin_drag(10, 20);
    ASSERT_EQ(handle.mouse_version(), 1u);

    handle.end_drag();
    EXPECT_FALSE(handle.is_dragging());
    EXPECT_EQ(handle.mouse_version(), 2u);
}

TEST(LeHandle, DragRectDbuIsNulloptWithoutAMousePositionEvenWhileDragging)
{
    LeHandle handle;
    handle.begin_drag(10, 10);
    EXPECT_FALSE(handle.drag_rect_dbu().has_value()); // no mouse position set yet
}

TEST(LeHandle, DragRectDbuNormalizesRegardlessOfDragDirection)
{
    // Dragging from bottom-right up to top-left (in pixel space) must
    // still produce a rect with ll <= ur, not a rect with swapped/negative
    // extents.
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);

    handle.begin_drag(80, 80); // dbu (80, 20)
    handle.set_mouse_position(20, 20); // dbu (20, 80)

    const std::optional<Rect> rect = handle.drag_rect_dbu();
    ASSERT_TRUE(rect.has_value());
    EXPECT_EQ(rect->ll.x, 20);
    EXPECT_EQ(rect->ll.y, 20);
    EXPECT_EQ(rect->ur.x, 80);
    EXPECT_EQ(rect->ur.y, 80);
}

TEST(LeHandle, KeysDefaultToNotHeld)
{
    LeHandle handle;
    EXPECT_FALSE(handle.is_key_held(1));
}

TEST(LeHandle, PressKeyThenIsKeyHeldReturnsTrue)
{
    LeHandle handle;
    handle.press_key(1);
    EXPECT_TRUE(handle.is_key_held(1));
}

TEST(LeHandle, ReleaseKeyClearsAHeldKey)
{
    LeHandle handle;
    handle.press_key(1);
    ASSERT_TRUE(handle.is_key_held(1));

    handle.release_key(1);
    EXPECT_FALSE(handle.is_key_held(1));
}

TEST(LeHandle, ReleaseKeyWithoutAPrecedingPressIsANoOp)
{
    LeHandle handle;
    handle.release_key(1);
    EXPECT_FALSE(handle.is_key_held(1));
}

TEST(LeHandle, DifferentKeyCodesAreTrackedIndependently)
{
    LeHandle handle;
    handle.press_key(1);
    EXPECT_TRUE(handle.is_key_held(1));
    EXPECT_FALSE(handle.is_key_held(2));
}

TEST(LeHandle, ClearAllKeysReleasesEveryHeldKey)
{
    // The fix for a real reported bug: a modifier held at the moment a
    // widget loses keyboard focus never gets its matching release event,
    // so it would otherwise stay "held" forever, silently turning every
    // later plain click into a shift-click.
    LeHandle handle;
    handle.press_key(1);
    handle.press_key(2);
    ASSERT_TRUE(handle.is_key_held(1));
    ASSERT_TRUE(handle.is_key_held(2));

    handle.clear_all_keys();
    EXPECT_FALSE(handle.is_key_held(1));
    EXPECT_FALSE(handle.is_key_held(2));
}

TEST(LeHandle, ClearAllKeysWithNothingHeldIsANoOp)
{
    LeHandle handle;
    handle.clear_all_keys();
    EXPECT_FALSE(handle.is_key_held(1));
}

TEST(LeHandle, LayerNameVisibilityDefaultsToTrueUntilSet)
{
    LeHandle handle;

    EXPECT_TRUE(handle.is_layer_name_visible("M1"));

    handle.set_layer_name_visible("M1", false);
    EXPECT_FALSE(handle.is_layer_name_visible("M1"));

    handle.set_layer_name_visible("M1", true);
    EXPECT_TRUE(handle.is_layer_name_visible("M1"));

    // A different, never-toggled layer name is unaffected.
    EXPECT_TRUE(handle.is_layer_name_visible("M2"));
}

TEST(LeHandle, PurposeVisibilityDefaultsToTrueUntilSet)
{
    LeHandle handle;

    EXPECT_TRUE(handle.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    handle.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(handle.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    handle.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, true);
    EXPECT_TRUE(handle.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    // A different, never-toggled purpose is unaffected.
    EXPECT_TRUE(handle.is_purpose_visible(ViewLayerPurpose::TERMINAL));
}

TEST(LeHandle, TrackRowAndGCellGridDefaultToInvisibleUnlikeEveryOtherPurpose)
{
    // BUGS_AND_ENHANCEMENTS.md E2 - these four are pre-seeded false
    // (everything else keeps the ordinary "unknown key -> visible"
    // default PurposeVisibilityDefaultsToTrueUntilSet above covers).
    LeHandle handle;
    EXPECT_FALSE(handle.is_purpose_visible(ViewLayerPurpose::TRACK_PREFERRED));
    EXPECT_FALSE(handle.is_purpose_visible(ViewLayerPurpose::TRACK_NON_PREFERRED));
    EXPECT_FALSE(handle.is_purpose_visible(ViewLayerPurpose::ROW));
    EXPECT_FALSE(handle.is_purpose_visible(ViewLayerPurpose::GCELLGRID));

    // Still toggleable like any other purpose, both directions.
    handle.set_purpose_visible(ViewLayerPurpose::TRACK_PREFERRED, true);
    EXPECT_TRUE(handle.is_purpose_visible(ViewLayerPurpose::TRACK_PREFERRED));
    // TRACK_NON_PREFERRED is unaffected by TRACK_PREFERRED's own toggle -
    // that's the whole point of splitting them (E2's "toggled by
    // preferred and non-preferred routing direction" independently).
    EXPECT_FALSE(handle.is_purpose_visible(ViewLayerPurpose::TRACK_NON_PREFERRED));
}

TEST(LeHandle, IsViewLayerVisibleIsTheAndOfBothAxes)
{
    LeHandle handle;

    // Both axes default true -> visible.
    EXPECT_TRUE(handle.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));

    // Layer name off, purpose still on -> not visible.
    handle.set_layer_name_visible("M1", false);
    EXPECT_FALSE(handle.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
    // A different layer name is unaffected by M1's toggle.
    EXPECT_TRUE(handle.is_view_layer_visible("M2", ViewLayerPurpose::TERMINAL));

    // Layer name back on, but now the purpose is off -> still not visible.
    handle.set_layer_name_visible("M1", true);
    handle.set_purpose_visible(ViewLayerPurpose::TERMINAL, false);
    EXPECT_FALSE(handle.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
    // OBSTRUCTION on the same layer is unaffected by the TERMINAL toggle.
    EXPECT_TRUE(handle.is_view_layer_visible("M1", ViewLayerPurpose::OBSTRUCTION));

    // Both axes on again -> visible.
    handle.set_purpose_visible(ViewLayerPurpose::TERMINAL, true);
    EXPECT_TRUE(handle.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
}

TEST(LeHandle, VisibilityVersionBumpsOnSetLayerNameVisibleAndSetPurposeVisible)
{
    LeHandle handle;
    EXPECT_EQ(handle.visibility_version(), 0u);

    handle.set_layer_name_visible("M1", false);
    EXPECT_EQ(handle.visibility_version(), 1u);

    handle.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_EQ(handle.visibility_version(), 2u);

    // Unrelated state (viewport) must not bump it.
    handle.set_pan(Point{5, 5});
    EXPECT_EQ(handle.visibility_version(), 2u);
}

TEST(LeHandle, LayerNameSelectabilityDefaultsToTrueUntilSet)
{
    LeHandle handle;

    EXPECT_TRUE(handle.is_layer_name_selectable("M1"));

    handle.set_layer_name_selectable("M1", false);
    EXPECT_FALSE(handle.is_layer_name_selectable("M1"));

    handle.set_layer_name_selectable("M1", true);
    EXPECT_TRUE(handle.is_layer_name_selectable("M1"));

    EXPECT_TRUE(handle.is_layer_name_selectable("M2"));
}

TEST(LeHandle, PurposeSelectabilityDefaultsToTrueUntilSet)
{
    LeHandle handle;

    EXPECT_TRUE(handle.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    handle.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(handle.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    handle.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, true);
    EXPECT_TRUE(handle.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    EXPECT_TRUE(handle.is_purpose_selectable(ViewLayerPurpose::TERMINAL));
}

TEST(LeHandle, TrackAndGCellGridDefaultToNonSelectableButRowStaysSelectable)
{
    // BUGS_AND_ENHANCEMENTS.md E2 ("not selectable") - hit_test_point/
    // hit_test_rect already skip these regardless (no `origin` set on a
    // track/gcellgrid RenderedShape - see LayoutGeometryStage::
    // append_track_shapes/append_gcell_grid_shapes), this just keeps the
    // visibility widget's own selectable-checkbox default consistent
    // with that. ROW stays selectable by default (E1 - rows are meant to
    // be selectable), unlike its own visibility default above.
    LeHandle handle;
    EXPECT_FALSE(handle.is_purpose_selectable(ViewLayerPurpose::TRACK_PREFERRED));
    EXPECT_FALSE(handle.is_purpose_selectable(ViewLayerPurpose::TRACK_NON_PREFERRED));
    EXPECT_FALSE(handle.is_purpose_selectable(ViewLayerPurpose::GCELLGRID));
    EXPECT_TRUE(handle.is_purpose_selectable(ViewLayerPurpose::ROW));
}

TEST(LeHandle, IsViewLayerSelectableIsTheAndOfBothAxes)
{
    LeHandle handle;
    EXPECT_TRUE(handle.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));

    handle.set_layer_name_selectable("M1", false);
    EXPECT_FALSE(handle.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));

    handle.set_layer_name_selectable("M1", true);
    handle.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(handle.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));
}

TEST(LeHandle, SetLayerNameSelectableAndSetPurposeSelectableDoNotBumpVisibilityVersion)
{
    // Selectability isn't consumed by Pipeline/Renderer caching (unlike
    // visibility) - it must not bump visibility_version(), or every
    // selectability toggle would force an unnecessary re-render.
    LeHandle handle;
    EXPECT_EQ(handle.visibility_version(), 0u);

    handle.set_layer_name_selectable("M1", false);
    EXPECT_EQ(handle.visibility_version(), 0u);

    handle.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_EQ(handle.visibility_version(), 0u);
}

TEST(LeHandle, ModeDefaultsToSelect)
{
    LeHandle handle;
    EXPECT_EQ(handle.mode(), LeHandle::Mode::SELECT);
}

TEST(LeHandle, SetModeChangesMode)
{
    LeHandle handle;
    handle.set_mode(LeHandle::Mode::EDIT);
    EXPECT_EQ(handle.mode(), LeHandle::Mode::EDIT);

    handle.set_mode(LeHandle::Mode::SELECT);
    EXPECT_EQ(handle.mode(), LeHandle::Mode::SELECT);
}

TEST(LeHandle, SetModeOnlyBumpsMouseVersionOnAnActualChange)
{
    LeHandle handle;
    const uint64_t baseline = handle.mouse_version();

    handle.set_mode(LeHandle::Mode::SELECT); // already SELECT - no-op
    EXPECT_EQ(handle.mouse_version(), baseline);

    handle.set_mode(LeHandle::Mode::EDIT);
    EXPECT_NE(handle.mouse_version(), baseline);
}

// --- Rulers (UPDATES.md item 13) ---

TEST(LeHandle, RulerModeDefaultsToNoRulers)
{
    LeHandle handle;
    EXPECT_TRUE(handle.rulers().empty());
    EXPECT_EQ(handle.ruler_version(), 0u);
    EXPECT_FALSE(handle.ruler_free_form());
}

TEST(LeHandle, AddRulerPointWithNoMousePositionIsNoOp)
{
    LeHandle handle;
    handle.add_ruler_point(false);
    EXPECT_TRUE(handle.rulers().empty());
    EXPECT_EQ(handle.ruler_version(), 0u);
}

TEST(LeHandle, AddRulerPointAppendsTheSnappedMousePosition)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.set_mouse_position(23, 27); // dbu (23, 73)
    handle.add_ruler_point(false);

    ASSERT_EQ(handle.rulers().size(), 1u);
    ASSERT_EQ(handle.rulers()[0].points.size(), 1u);
    EXPECT_EQ(handle.rulers()[0].points[0].x, 23);
    EXPECT_EQ(handle.rulers()[0].points[0].y, 73);
    EXPECT_FALSE(handle.rulers()[0].finished);
    EXPECT_NE(handle.ruler_version(), 0u);
}

TEST(LeHandle, AddRulerPointIdenticalToTheLastCommittedPointIsANoOp)
{
    // Regression: a double-click's second click can easily snap to the
    // exact same grid point as its first (the double-click distance
    // threshold is typically smaller than the grid spacing on screen) -
    // without this guard, that appended a zero-length final segment,
    // which broke the "total: " label's own perpendicular-direction math
    // (see draw_ruler_polyline in draw_helpers.hpp).
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.set_mouse_position(23, 27); // dbu (23, 73)
    handle.add_ruler_point(false);
    const uint64_t after_first = handle.ruler_version();

    handle.set_mouse_position(23, 27); // same snapped point again
    handle.add_ruler_point(false);

    EXPECT_EQ(handle.rulers()[0].points.size(), 1u); // not appended
    EXPECT_EQ(handle.ruler_version(), after_first);
}

TEST(LeHandle, AddRulerPointConstrainsOrthogonalByDefault)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.set_mouse_position(10, 90); // dbu (10, 10)
    handle.add_ruler_point(false);

    handle.set_mouse_position(40, 70); // dbu (40, 30) - diagonal from (10,10)
    handle.add_ruler_point(false);

    ASSERT_EQ(handle.rulers()[0].points.size(), 2u);
    const Point &second = handle.rulers()[0].points[1];
    // dx (30) > dy (20), so the second point pins y to the first point's y.
    EXPECT_EQ(second.x, 40);
    EXPECT_EQ(second.y, 10);
}

TEST(LeHandle, AddRulerPointFreeFormIgnoresOrthogonalConstraint)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.set_mouse_position(10, 90); // dbu (10, 10)
    handle.add_ruler_point(true);

    handle.set_mouse_position(40, 70); // dbu (40, 30)
    handle.add_ruler_point(true);

    ASSERT_EQ(handle.rulers()[0].points.size(), 2u);
    const Point &second = handle.rulers()[0].points[1];
    EXPECT_EQ(second.x, 40);
    EXPECT_EQ(second.y, 30);
}

TEST(LeHandle, FinishActiveRulerMarksItFinishedAndBumpsVersion)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.set_mouse_position(10, 90);
    handle.add_ruler_point(false);
    const uint64_t before = handle.ruler_version();

    handle.finish_active_ruler();
    EXPECT_TRUE(handle.rulers()[0].finished);
    EXPECT_NE(handle.ruler_version(), before);
}

TEST(LeHandle, FinishActiveRulerWithNoneActiveIsNoOp)
{
    LeHandle handle;
    handle.finish_active_ruler();
    EXPECT_EQ(handle.ruler_version(), 0u);
    EXPECT_TRUE(handle.rulers().empty());
}

TEST(LeHandle, ClickAfterFinishFarEnoughAwayStartsASecondRulerLeavingTheFirstIntact)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.set_mouse_position(10, 90); // dbu (10, 10)
    handle.add_ruler_point(false);
    handle.set_mouse_position(20, 90); // dbu (20, 10)
    handle.add_ruler_point(false);
    handle.finish_active_ruler();
    ASSERT_EQ(handle.rulers().size(), 1u);
    ASSERT_EQ(handle.rulers()[0].points.size(), 2u);

    // Well beyond kNewRulerMinDistancePx (20px, scale 1.0) from (20, 10).
    handle.set_mouse_position(80, 90); // dbu (80, 10)
    handle.add_ruler_point(false);

    ASSERT_EQ(handle.rulers().size(), 2u);
    EXPECT_EQ(handle.rulers()[0].points.size(), 2u); // first ruler untouched
    EXPECT_TRUE(handle.rulers()[0].finished);
    ASSERT_EQ(handle.rulers()[1].points.size(), 1u);
    EXPECT_FALSE(handle.rulers()[1].finished);
}

TEST(LeHandle, ClickTooCloseToAJustFinishedRulerIsANoOp)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.set_mouse_position(10, 90); // dbu (10, 10)
    handle.add_ruler_point(false);
    handle.finish_active_ruler();
    ASSERT_EQ(handle.rulers().size(), 1u);
    const uint64_t before = handle.ruler_version();

    // Only 5 dbu (= 5px at scale 1.0) away - under kNewRulerMinDistancePx.
    handle.set_mouse_position(15, 90); // dbu (15, 10)
    handle.add_ruler_point(false);

    EXPECT_EQ(handle.rulers().size(), 1u); // no second ruler started
    EXPECT_EQ(handle.rulers()[0].points.size(), 1u);
    EXPECT_EQ(handle.ruler_version(), before);
}

TEST(LeHandle, ResetRulerModeFinishesAnInProgressRulerAndEnsuresRulerMode)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.reset_ruler_mode();
    EXPECT_EQ(handle.mode(), LeHandle::Mode::RULER);

    handle.set_mouse_position(10, 90);
    handle.add_ruler_point(false);
    ASSERT_FALSE(handle.rulers()[0].finished);

    // Calling again while already in Ruler mode with an in-progress
    // ruler finishes it - this is what lets 'r' double as "abandon the
    // current ruler".
    handle.reset_ruler_mode();
    EXPECT_EQ(handle.mode(), LeHandle::Mode::RULER);
    EXPECT_TRUE(handle.rulers()[0].finished);
}

TEST(LeHandle, SetModeLeavingRulerFinishesTheActiveRuler)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.reset_ruler_mode();
    handle.set_mouse_position(10, 90);
    handle.add_ruler_point(false);
    ASSERT_FALSE(handle.rulers()[0].finished);

    handle.set_mode(LeHandle::Mode::SELECT);
    EXPECT_TRUE(handle.rulers()[0].finished);
}

TEST(LeHandle, RulerVersionIsNotBumpedByMouseMoveAlone)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.set_mouse_position(10, 90);
    handle.add_ruler_point(false);
    const uint64_t after_add = handle.ruler_version();

    handle.set_mouse_position(50, 50);
    handle.set_mouse_position(20, 20);
    EXPECT_EQ(handle.ruler_version(), after_add);
}

TEST(LeHandle, SetRulerFreeFormDedupsItsVersionBump)
{
    LeHandle handle;
    const uint64_t baseline = handle.mouse_version();

    handle.set_ruler_free_form(true);
    const uint64_t after_first = handle.mouse_version();
    EXPECT_NE(after_first, baseline);

    handle.set_ruler_free_form(true); // same value again - no-op
    EXPECT_EQ(handle.mouse_version(), after_first);

    handle.set_ruler_free_form(false);
    EXPECT_NE(handle.mouse_version(), after_first);
}

TEST(LeHandle, ClearRulersEmptiesAndBumpsVersion)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);

    handle.set_mouse_position(10, 90);
    handle.add_ruler_point(false);
    ASSERT_FALSE(handle.rulers().empty());
    const uint64_t before = handle.ruler_version();

    handle.clear_rulers();
    EXPECT_TRUE(handle.rulers().empty());
    EXPECT_NE(handle.ruler_version(), before);
}

TEST(LeHandle, ClearRulersOnAnEmptyListIsANoOp)
{
    LeHandle handle;
    handle.clear_rulers();
    EXPECT_EQ(handle.ruler_version(), 0u);
}

TEST(LeHandle, ArmMoveWithEmptySelectionIsNoOp)
{
    LeHandle handle;
    handle.arm_move({});
    EXPECT_FALSE(handle.move().armed);
    EXPECT_TRUE(handle.move().moving_pieces.empty());
}

TEST(LeHandle, ArmMoveSnapshotsSelectionAndGeometry)
{
    LeHandle handle;
    ShapeId shape_id{3, 0};
    handle.select(shape_id, PieceKind::POLYGON, 2);

    const LayerId m1{1, 1};
    Shape geometry;
    geometry.layer = m1;
    const uint64_t before = handle.mouse_version();

    handle.arm_move({geometry});
    EXPECT_TRUE(handle.move().armed);
    ASSERT_EQ(handle.move().moving_pieces.size(), 1u);
    const auto &moving_piece = std::get<LeHandle::ShapePiece>(handle.move().moving_pieces[0]);
    EXPECT_EQ(moving_piece.shape_id, shape_id);
    EXPECT_EQ(moving_piece.piece_kind, PieceKind::POLYGON);
    EXPECT_EQ(moving_piece.piece_index, 2u);
    ASSERT_EQ(handle.move().moving_geometry.size(), 1u);
    EXPECT_EQ(handle.move().moving_geometry[0].layer, m1);
    EXPECT_GT(handle.mouse_version(), before);
}

TEST(LeHandle, RefreshMoveGeometryReplacesTheGhostSnapshotWithoutTouchingAnchorOrArmedState)
{
    // Regression: committing a move re-arms for a follow-up move (see
    // api.cpp's move_click_unlocked), keeping the ghost snapshot from the
    // moment of that re-arm - if something *else* changes the moving
    // shapes' geometry afterward (an external undo/redo), that snapshot
    // goes stale unless explicitly refreshed. refresh_move_geometry is
    // that explicit refresh - unlike arm_move, it must not touch
    // anchor/armed, since an undo/redo can happen mid-gesture too.
    LeHandle handle;
    handle.select(ShapeId{1, 0});
    const LayerId m1{1, 1};
    const LayerId m2{2, 1};
    Shape original;
    original.layer = m1;
    handle.arm_move({original});
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_mouse_position(10, 10);
    ASSERT_TRUE(handle.move_set_anchor());
    ASSERT_TRUE(handle.move().anchor.has_value());
    const uint64_t before = handle.mouse_version();

    Shape refreshed;
    refreshed.layer = m2;
    handle.refresh_move_geometry({refreshed});

    EXPECT_TRUE(handle.move().armed); // untouched
    ASSERT_TRUE(handle.move().anchor.has_value()); // untouched - not cleared like arm_move would
    ASSERT_EQ(handle.move().moving_geometry.size(), 1u);
    EXPECT_EQ(handle.move().moving_geometry[0].layer, m2);
    EXPECT_GT(handle.mouse_version(), before);
}

TEST(LeHandle, RefreshMoveGeometryIsANoOpWhenNotArmed)
{
    LeHandle handle;
    const uint64_t before = handle.mouse_version();
    Shape geometry;
    handle.refresh_move_geometry({geometry});
    EXPECT_FALSE(handle.move().armed);
    EXPECT_TRUE(handle.move().moving_geometry.empty());
    EXPECT_EQ(handle.mouse_version(), before);
}

TEST(LeHandle, MoveSetAnchorRequiresArmedAndAMousePosition)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);
    handle.select(ShapeId{1, 0});

    // Not armed yet - no-op.
    handle.set_mouse_position(10, 90);
    EXPECT_FALSE(handle.move_set_anchor());

    handle.arm_move({Shape{}});
    // Armed, but requires a mouse position too - already set above, so
    // this should succeed now.
    EXPECT_TRUE(handle.move_set_anchor());
    ASSERT_TRUE(handle.move().anchor.has_value());
    EXPECT_EQ(handle.move().anchor->x, 10);
    EXPECT_EQ(handle.move().anchor->y, 10);

    // A second call while an anchor already exists is a no-op.
    EXPECT_FALSE(handle.move_set_anchor());
}

TEST(LeHandle, MoveDeltaOrthogonalConstrainsToTheLargerMagnitudeAxis)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);
    handle.select(ShapeId{1, 0});
    handle.arm_move({Shape{}});

    handle.set_mouse_position(10, 90); // dbu (10, 10)
    ASSERT_TRUE(handle.move_set_anchor());

    handle.set_mouse_position(25, 85); // dbu (25, 15) - dx=15, dy=5, x wins
    const std::optional<Point> delta = handle.move_delta(/*free_form=*/false);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->x, 15);
    EXPECT_EQ(delta->y, 0);
}

TEST(LeHandle, MoveDeltaFreeFormReturnsTheRawOffset)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_minor_grid_spacing(1);
    handle.select(ShapeId{1, 0});
    handle.arm_move({Shape{}});

    handle.set_mouse_position(10, 90); // dbu (10, 10)
    ASSERT_TRUE(handle.move_set_anchor());

    handle.set_mouse_position(25, 85); // dbu (25, 15)
    const std::optional<Point> delta = handle.move_delta(/*free_form=*/true);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->x, 15);
    EXPECT_EQ(delta->y, 5);
}

TEST(LeHandle, MoveDeltaIsNulloptBeforeAnAnchorIsSet)
{
    LeHandle handle;
    handle.select(ShapeId{1, 0});
    handle.arm_move({Shape{}});
    EXPECT_FALSE(handle.move_delta(false).has_value());
}

TEST(LeHandle, EndMoveClearsAllMoveState)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.select(ShapeId{1, 0});
    handle.arm_move({Shape{}});
    handle.set_mouse_position(10, 10);
    handle.move_set_anchor();

    handle.end_move();
    EXPECT_FALSE(handle.move().armed);
    EXPECT_FALSE(handle.move().anchor.has_value());
    EXPECT_TRUE(handle.move().moving_pieces.empty());
}

TEST(LeHandle, SetModeLeavingEditCancelsAnInProgressMove)
{
    LeHandle handle;
    handle.set_pan(Point{0, 0});
    handle.set_scale(1.0);
    handle.set_viewport_size(100, 100);
    handle.set_mode(LeHandle::Mode::EDIT);
    handle.select(ShapeId{1, 0});
    handle.arm_move({Shape{}});
    ASSERT_TRUE(handle.move().armed);

    handle.set_mode(LeHandle::Mode::SELECT);
    EXPECT_FALSE(handle.move().armed);
}

TEST(LeHandle, SetMoveFreeFormDedupsItsVersionBump)
{
    LeHandle handle;
    handle.set_move_free_form(true);
    const uint64_t after_first = handle.mouse_version();
    handle.set_move_free_form(true);
    EXPECT_EQ(handle.mouse_version(), after_first);

    handle.set_move_free_form(false);
    EXPECT_GT(handle.mouse_version(), after_first);
}

TEST(LeHandle, SelectDeselectAndClear)
{
    LeHandle handle;
    ShapeId first{1, 0};
    ShapeId second{2, 0};

    handle.select(first);
    handle.select(second);
    EXPECT_TRUE(handle.is_selected(first));
    EXPECT_TRUE(handle.is_selected(second));
    EXPECT_EQ(handle.selection().size(), 2u);

    // Selecting the same id again must not duplicate it.
    handle.select(first);
    EXPECT_EQ(handle.selection().size(), 2u);

    handle.deselect(first);
    EXPECT_FALSE(handle.is_selected(first));
    EXPECT_TRUE(handle.is_selected(second));
    EXPECT_EQ(handle.selection().size(), 1u);

    handle.clear_selection();
    EXPECT_TRUE(handle.selection().empty());
}

TEST(LeHandle, FitToContentCentersAndScalesToFillTheTighterAxis)
{
    LeHandle handle;
    handle.set_viewport_size(1000, 1000);

    // 100x50 dbu content, 20px padding on each side -> usable 960px, scale
    // bound by the wider (x) axis: 960 / 100 = 9.6.
    Rect bbox{.ll = Point{0, 0}, .ur = Point{100, 50}};
    handle.fit_to_content(bbox, 20);

    EXPECT_DOUBLE_EQ(handle.scale(), 9.6);

    // Content is centered: leftover space on y is (1000/9.6 - 50) ~= 54.17,
    // half of that offsets pan below the bbox's own ll.y.
    const double expected_pan_y = 0 - (1000.0 / 9.6 - 50.0) / 2.0;
    EXPECT_EQ(handle.pan().x, 0 - static_cast<int64_t>((1000.0 / 9.6 - 100.0) / 2.0));
    EXPECT_EQ(handle.pan().y, static_cast<int64_t>(expected_pan_y));
}

TEST(LeHandle, FitToContentWithNoBboxFallsBackToDefaultScaleAndPan)
{
    LeHandle handle;
    handle.set_viewport_size(500, 500);
    handle.set_scale(3.0);
    handle.set_pan(Point{10, 10});

    handle.fit_to_content(std::nullopt, 10);

    EXPECT_DOUBLE_EQ(handle.scale(), 1.0);
    EXPECT_EQ(handle.pan().x, 0);
    EXPECT_EQ(handle.pan().y, 0);
}

TEST(LeHandle, FitToContentWithZeroSizedViewportFallsBackToDefault)
{
    LeHandle handle;
    Rect bbox{.ll = Point{0, 0}, .ur = Point{100, 100}};
    handle.fit_to_content(bbox, 10);

    EXPECT_DOUBLE_EQ(handle.scale(), 1.0);
    EXPECT_EQ(handle.pan().x, 0);
    EXPECT_EQ(handle.pan().y, 0);
}

TEST(LeHandle, FitToContentWithDegenerateZeroWidthBboxDoesNotDivideByZero)
{
    LeHandle handle;
    handle.set_viewport_size(200, 200);

    // Zero-width content (e.g. a single vertical line) - scale must be
    // bounded by the y axis only, not blow up via a 1/0 on x.
    Rect bbox{.ll = Point{5, 0}, .ur = Point{5, 100}};
    handle.fit_to_content(bbox, 0);

    EXPECT_DOUBLE_EQ(handle.scale(), 2.0); // 200 / 100
}

TEST(LeHandle, SelectBumpsSelectionVersionOnlyOnAnActualChange)
{
    LeHandle handle;
    ShapeId id{5, 0};
    EXPECT_EQ(handle.selection_version(), 0u);

    handle.select(id);
    EXPECT_EQ(handle.selection_version(), 1u);

    // Already selected - a no-op, must not bump again.
    handle.select(id);
    EXPECT_EQ(handle.selection_version(), 1u);
}

TEST(LeHandle, DeselectBumpsSelectionVersionOnlyOnAnActualChange)
{
    LeHandle handle;
    ShapeId id{5, 0};
    handle.select(id);
    ASSERT_EQ(handle.selection_version(), 1u);

    handle.deselect(id);
    EXPECT_FALSE(handle.is_selected(id));
    EXPECT_EQ(handle.selection_version(), 2u);

    // Already gone - a no-op, must not bump again.
    handle.deselect(id);
    EXPECT_EQ(handle.selection_version(), 2u);
}

TEST(LeHandle, ClearSelectionBumpsSelectionVersionOnlyIfSelectionWasNonEmpty)
{
    LeHandle handle;
    ShapeId id{5, 0};

    handle.clear_selection(); // already empty - a no-op
    EXPECT_EQ(handle.selection_version(), 0u);

    handle.select(id);
    ASSERT_EQ(handle.selection_version(), 1u);

    handle.clear_selection();
    EXPECT_TRUE(handle.selection().empty());
    EXPECT_EQ(handle.selection_version(), 2u);
}

TEST(LeHandle, SelectionChangesDoNotBumpViewportOrVisibilityVersion)
{
    LeHandle handle;
    const uint64_t viewport_version_before = handle.viewport_version();
    const uint64_t visibility_version_before = handle.visibility_version();

    handle.select(ShapeId{5, 0});
    handle.clear_selection();

    EXPECT_EQ(handle.viewport_version(), viewport_version_before);
    EXPECT_EQ(handle.visibility_version(), visibility_version_before);
}
