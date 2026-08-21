#include "../scene.hpp"
#include <gtest/gtest.h>

using namespace le;

TEST(Scene, DefaultStateIsSensible)
{
    Scene scene;

    EXPECT_FALSE(scene.current_abstract().valid());
    EXPECT_EQ(scene.pan().x, 0);
    EXPECT_EQ(scene.pan().y, 0);
    EXPECT_DOUBLE_EQ(scene.scale(), 1.0);
    EXPECT_EQ(scene.viewport_width_px(), 0);
    EXPECT_EQ(scene.viewport_height_px(), 0);
    EXPECT_TRUE(scene.selection().empty());
    EXPECT_EQ(scene.viewport_version(), 0u);
    EXPECT_EQ(scene.visibility_version(), 0u);
    EXPECT_EQ(scene.minor_grid_spacing(), 5);
    EXPECT_EQ(scene.major_grid_spacing(), 50);
}

TEST(Scene, CurrentAbstractRoundTrips)
{
    Scene scene;
    AbstractId id{5, 1};
    scene.set_current_abstract(id);
    EXPECT_EQ(scene.current_abstract(), id);
}

TEST(Scene, SwitchingToADifferentAbstractClearsSelectionAndHover)
{
    // Regression: TerminalId/ObstructionId/ShapeId are plain
    // {index,generation} pool handles, not namespaced by Abstract - a
    // selection/hover left over from the old Abstract could otherwise
    // reference nothing (best case) or an unrelated object that happens
    // to reuse the same pool slot in the new Abstract (worst case).
    Scene scene;
    scene.set_current_abstract(AbstractId{1, 0});

    Shape outline;
    outline.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});
    scene.select(ShapeId{1, 0});
    scene.set_hover(HoverTarget{.origin = TerminalId{1, 0}, .outline = outline});
    ASSERT_FALSE(scene.selection().empty());
    ASSERT_TRUE(scene.hover().has_value());

    scene.set_current_abstract(AbstractId{2, 0});
    EXPECT_TRUE(scene.selection().empty());
    EXPECT_FALSE(scene.hover().has_value());
}

TEST(Scene, SwitchingToADifferentAbstractClearsRulers)
{
    // Regression: rulers are plain dbu Points with no Abstract scoping
    // at all - left uncleared they'd go on being drawn, at the same raw
    // coordinates, over whatever design happens to occupy that part of
    // the new Abstract's own unrelated coordinate space.
    Scene scene;
    scene.set_current_abstract(AbstractId{1, 0});
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.set_mouse_position(10, 90); // dbu (10, 10)
    scene.add_ruler_point(false);
    ASSERT_FALSE(scene.rulers().empty());

    scene.set_current_abstract(AbstractId{2, 0});
    EXPECT_TRUE(scene.rulers().empty());
}

TEST(Scene, SwitchingToADifferentAbstractBumpsSelectionVersionOnlyIfSelectionWasNonEmpty)
{
    Scene scene;
    scene.set_current_abstract(AbstractId{1, 0});
    EXPECT_EQ(scene.selection_version(), 0u);

    // Nothing selected - switching must not bump the version for nothing.
    scene.set_current_abstract(AbstractId{2, 0});
    EXPECT_EQ(scene.selection_version(), 0u);

    scene.select(ShapeId{1, 0});
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.set_current_abstract(AbstractId{3, 0});
    EXPECT_EQ(scene.selection_version(), 2u);
}

TEST(Scene, SettingTheSameCurrentAbstractAgainIsANoOp)
{
    Scene scene;
    scene.set_current_abstract(AbstractId{1, 0});
    scene.select(ShapeId{1, 0});
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.set_current_abstract(AbstractId{1, 0}); // same Abstract already displayed
    EXPECT_FALSE(scene.selection().empty());
    EXPECT_EQ(scene.selection_version(), 1u);
}

TEST(Scene, SelectRecordsTheShapeId)
{
    Scene scene;
    scene.select(ShapeId{1, 0});

    EXPECT_TRUE(scene.is_selected(ShapeId{1, 0}));
    ASSERT_FALSE(scene.selection().empty());
    EXPECT_EQ(scene.selection().front().shape_id, (ShapeId{1, 0}));
}

TEST(Scene, SelectingADifferentShapeAddsASecondEntry)
{
    // The actual reported bug's own regression test: shift-clicking a
    // second shape must add a second selection entry - both shapes end
    // up independently selected/highlighted/reportable - not replace the
    // first one or no-op against it.
    Scene scene;
    scene.select(ShapeId{1, 0});
    ASSERT_EQ(scene.selection_version(), 1u);
    ASSERT_EQ(scene.selection().size(), 1u);

    scene.select(ShapeId{2, 0}); // shift-clicking a different shape
    EXPECT_EQ(scene.selection_version(), 2u);
    ASSERT_EQ(scene.selection().size(), 2u); // both shapes are now separately selected

    EXPECT_EQ(scene.selection()[0].shape_id, (ShapeId{1, 0}));
    EXPECT_EQ(scene.selection()[1].shape_id, (ShapeId{2, 0}));
}

TEST(Scene, ReselectingTheSameShapeIsANoOp)
{
    Scene scene;
    scene.select(ShapeId{1, 0});
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.select(ShapeId{1, 0});
    EXPECT_EQ(scene.selection_version(), 1u);
    EXPECT_EQ(scene.selection().size(), 1u);
}

TEST(Scene, SelectDedupsCorrectlyAcrossManyDistinctShapes)
{
    // Regression/refactor-safety test for select()'s O(1)-average dedup
    // (selected_ids_, a plain unordered_set<ShapeId>) - this matters
    // because le_mouse_up's drag-select branch (api.cpp) calls select()
    // once per enclosed piece, and a real design can put hundreds of
    // thousands of pieces under one shared Obstruction's OBS block (see
    // BENCHMARKS.md). Mixes distinct new ids with re-selecting already-
    // selected ones (in original and reverse order) and checks the exact
    // resulting count/version.
    Scene scene;

    std::vector<ShapeId> ids;
    for (uint32_t i = 0; i < 20; ++i)
        ids.push_back(ShapeId{i, 0});

    for (const ShapeId &id : ids)
        scene.select(id);
    ASSERT_EQ(scene.selection().size(), 20u);
    ASSERT_EQ(scene.selection_version(), 20u);

    // Re-select every id again, in reverse order - every one should be
    // recognized as an existing duplicate (no-op).
    for (auto it = ids.rbegin(); it != ids.rend(); ++it)
        scene.select(*it);
    EXPECT_EQ(scene.selection().size(), 20u);
    EXPECT_EQ(scene.selection_version(), 20u);

    // One genuinely new id still gets added correctly afterward.
    scene.select(ShapeId{1000, 0});
    EXPECT_EQ(scene.selection().size(), 21u);
    EXPECT_EQ(scene.selection_version(), 21u);
}

TEST(Scene, DeselectRemovesAnEntry)
{
    Scene scene;
    scene.select(ShapeId{1, 0});
    ASSERT_TRUE(scene.is_selected(ShapeId{1, 0}));

    scene.deselect(ShapeId{1, 0});
    EXPECT_FALSE(scene.is_selected(ShapeId{1, 0}));
    EXPECT_TRUE(scene.selection().empty());
}

TEST(Scene, PanAndViewportRoundTrip)
{
    Scene scene;
    scene.set_pan(Point{100, -200});
    scene.set_viewport_size(1920, 1080);

    EXPECT_EQ(scene.pan().x, 100);
    EXPECT_EQ(scene.pan().y, -200);
    EXPECT_EQ(scene.viewport_width_px(), 1920);
    EXPECT_EQ(scene.viewport_height_px(), 1080);
}

TEST(Scene, SetScaleIgnoresNonPositiveValues)
{
    Scene scene;
    scene.set_scale(2.5);
    ASSERT_DOUBLE_EQ(scene.scale(), 2.5);

    scene.set_scale(0.0);
    EXPECT_DOUBLE_EQ(scene.scale(), 2.5); // unchanged

    scene.set_scale(-1.0);
    EXPECT_DOUBLE_EQ(scene.scale(), 2.5); // unchanged
}

TEST(Scene, ViewportVersionBumpsOnPanScaleAndViewportSizeChangesOnly)
{
    Scene scene;
    EXPECT_EQ(scene.viewport_version(), 0u);

    scene.set_pan(Point{1, 1});
    EXPECT_EQ(scene.viewport_version(), 1u);

    scene.set_scale(2.0);
    EXPECT_EQ(scene.viewport_version(), 2u);

    scene.set_viewport_size(100, 100);
    EXPECT_EQ(scene.viewport_version(), 3u);

    // A rejected (non-positive) scale must not bump the version - nothing
    // about the viewport actually changed.
    scene.set_scale(-1.0);
    EXPECT_EQ(scene.viewport_version(), 3u);

    // Unrelated state (visibility) must not bump it either.
    scene.set_layer_name_visible("M1", false);
    EXPECT_EQ(scene.viewport_version(), 3u);
}

TEST(Scene, GridSpacingRoundTrips)
{
    Scene scene;
    scene.set_minor_grid_spacing(10);
    scene.set_major_grid_spacing(100);

    EXPECT_EQ(scene.minor_grid_spacing(), 10);
    EXPECT_EQ(scene.major_grid_spacing(), 100);
}

TEST(Scene, GridSpacingIgnoresNonPositiveValues)
{
    Scene scene;
    scene.set_minor_grid_spacing(10);
    scene.set_major_grid_spacing(100);

    scene.set_minor_grid_spacing(0);
    scene.set_minor_grid_spacing(-5);
    EXPECT_EQ(scene.minor_grid_spacing(), 10); // unchanged

    scene.set_major_grid_spacing(0);
    scene.set_major_grid_spacing(-5);
    EXPECT_EQ(scene.major_grid_spacing(), 100); // unchanged
}

TEST(Scene, GridSpacingSettersBumpVisibilityVersion)
{
    // The grid is part of the rendered picture (see Renderer::draw_grid),
    // so changing its spacing must invalidate the same render cache
    // layer visibility does - unlike selectability, which doesn't.
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_minor_grid_spacing(10);
    EXPECT_EQ(scene.visibility_version(), 1u);

    scene.set_major_grid_spacing(100);
    EXPECT_EQ(scene.visibility_version(), 2u);

    // A rejected (non-positive) value must not bump it.
    scene.set_minor_grid_spacing(-5);
    EXPECT_EQ(scene.visibility_version(), 2u);
}

TEST(Scene, RulerLabelSizeDefaultsToElevenPixels)
{
    Scene scene;
    EXPECT_DOUBLE_EQ(scene.ruler_label_size_px(), 11.0);
}

TEST(Scene, RulerLabelSizeRoundTrips)
{
    Scene scene;
    scene.set_ruler_label_size_px(20.0);
    EXPECT_DOUBLE_EQ(scene.ruler_label_size_px(), 20.0);
}

TEST(Scene, RulerLabelSizeIgnoresNonPositiveValues)
{
    Scene scene;
    scene.set_ruler_label_size_px(20.0);

    scene.set_ruler_label_size_px(0.0);
    scene.set_ruler_label_size_px(-5.0);
    EXPECT_DOUBLE_EQ(scene.ruler_label_size_px(), 20.0); // unchanged
}

TEST(Scene, RulerLabelSizeSetterBumpsVisibilityVersionOnlyOnAnActualChange)
{
    // Both new/changed ruler overlay stages already key on
    // visibility_version() alongside ruler_version() - reusing this
    // signal (like grid spacing does) invalidates them with no new
    // plumbing.
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_ruler_label_size_px(20.0);
    EXPECT_EQ(scene.visibility_version(), 1u);

    // A rejected (non-positive) value must not bump it.
    scene.set_ruler_label_size_px(-5.0);
    EXPECT_EQ(scene.visibility_version(), 1u);
}

TEST(Scene, MousePositionDefaultsToUnset)
{
    Scene scene;
    EXPECT_FALSE(scene.has_mouse_position());
    EXPECT_EQ(scene.mouse_version(), 0u);
    EXPECT_FALSE(scene.mouse_dbu_position().has_value());
    EXPECT_FALSE(scene.snapped_mouse_position().has_value());
}

TEST(Scene, SetMousePositionBumpsMouseVersionNotViewportOrVisibilityVersion)
{
    // A mouse move must invalidate only the cheap overlay-picture cache
    // (see Renderer::build_overlay_picture/compose_with_overlays), not the
    // expensive design rasterize cache keyed on viewport/visibility
    // version - see scene.hpp's own comment on mouse_version_.
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_viewport_size(100, 100);
    const uint64_t viewport_version_before = scene.viewport_version();
    const uint64_t visibility_version_before = scene.visibility_version();

    scene.set_mouse_position(10, 20);
    EXPECT_TRUE(scene.has_mouse_position());
    EXPECT_EQ(scene.mouse_version(), 1u);
    EXPECT_EQ(scene.viewport_version(), viewport_version_before);
    EXPECT_EQ(scene.visibility_version(), visibility_version_before);

    scene.set_mouse_position(11, 20);
    EXPECT_EQ(scene.mouse_version(), 2u);
}

TEST(Scene, MouseDbuPositionUndoesPanScaleAndYFlip)
{
    // Pixel space is top-left origin/y-down (matches le_render_pixel_buffer's
    // output image and le_zoom's x/y); dbu space is y-up - see scene.hpp's
    // mouse_dbu_position comment for the exact inverse formula.
    Scene scene;
    scene.set_pan(Point{100, 200});
    scene.set_scale(2.0);
    scene.set_viewport_size(50, 40);

    scene.set_mouse_position(10, 10);
    const std::optional<Point> dbu = scene.mouse_dbu_position();
    ASSERT_TRUE(dbu.has_value());
    EXPECT_EQ(dbu->x, 100 + 10 / 2); // pan.x + x_px / scale
    EXPECT_EQ(dbu->y, 200 + (40 - 10) / 2); // pan.y + (height - y_px) / scale
}

TEST(Scene, SnappedMousePositionRoundsToNearestMinorGridMultiple)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(10);

    // dbu (23, 100 - 27) = (23, 73); nearest multiples of 10 are 20 and 70.
    scene.set_mouse_position(23, 27);
    const std::optional<Point> snapped = scene.snapped_mouse_position();
    ASSERT_TRUE(snapped.has_value());
    EXPECT_EQ(snapped->x, 20);
    EXPECT_EQ(snapped->y, 70);
}

TEST(Scene, ClearMousePositionResetsHasMousePositionAndBumpsVersionOnlyIfItWasSet)
{
    Scene scene;
    scene.set_mouse_position(5, 5);
    ASSERT_EQ(scene.mouse_version(), 1u);

    scene.clear_mouse_position();
    EXPECT_FALSE(scene.has_mouse_position());
    EXPECT_FALSE(scene.mouse_dbu_position().has_value());
    EXPECT_EQ(scene.mouse_version(), 2u);

    // Clearing an already-clear position is a no-op - must not bump the
    // version again (would otherwise invalidate caches for nothing).
    scene.clear_mouse_position();
    EXPECT_EQ(scene.mouse_version(), 2u);
}

TEST(Scene, PixelToDbuMatchesMouseDbuPositionsOwnFormula)
{
    // Same scenario as MouseDbuPositionUndoesPanScaleAndYFlip, but calling
    // pixel_to_dbu directly with an arbitrary x/y rather than going
    // through the stored mouse position - the two must agree exactly,
    // since mouse_dbu_position() is defined in terms of pixel_to_dbu.
    Scene scene;
    scene.set_pan(Point{100, 200});
    scene.set_scale(2.0);
    scene.set_viewport_size(50, 40);

    const Point dbu = scene.pixel_to_dbu(10, 10);
    EXPECT_EQ(dbu.x, 100 + 10 / 2);
    EXPECT_EQ(dbu.y, 200 + (40 - 10) / 2);

    scene.set_mouse_position(10, 10);
    ASSERT_TRUE(scene.mouse_dbu_position().has_value());
    EXPECT_EQ(scene.mouse_dbu_position()->x, dbu.x);
    EXPECT_EQ(scene.mouse_dbu_position()->y, dbu.y);
}

TEST(Scene, DragDefaultsToNotDragging)
{
    Scene scene;
    EXPECT_FALSE(scene.is_dragging());
    EXPECT_FALSE(scene.drag_rect_dbu().has_value());
}

TEST(Scene, BeginDragSetsStateAndBumpsMouseVersionNotViewportOrVisibilityVersion)
{
    Scene scene;
    const uint64_t viewport_version_before = scene.viewport_version();
    const uint64_t visibility_version_before = scene.visibility_version();

    scene.begin_drag(10, 20);
    EXPECT_TRUE(scene.is_dragging());
    EXPECT_EQ(scene.drag_start_x_px(), 10);
    EXPECT_EQ(scene.drag_start_y_px(), 20);
    EXPECT_EQ(scene.mouse_version(), 1u);
    EXPECT_EQ(scene.viewport_version(), viewport_version_before);
    EXPECT_EQ(scene.visibility_version(), visibility_version_before);
}

TEST(Scene, BeginDragDefaultsToSelectKind)
{
    // Regression guard for the defaulted third parameter (UPDATES.md
    // 9.3) - the existing le_mouse_down call site (begin_drag(x, y), no
    // third argument) must keep behaving exactly as before.
    Scene scene;
    scene.begin_drag(10, 20);
    EXPECT_EQ(scene.drag_kind(), Scene::DragKind::SELECT);
}

TEST(Scene, BeginDragAcceptsAnExplicitDragKind)
{
    Scene scene;
    scene.begin_drag(10, 20, Scene::DragKind::ZOOM);
    EXPECT_EQ(scene.drag_kind(), Scene::DragKind::ZOOM);
}

TEST(Scene, EndDragClearsDraggingAndBumpsMouseVersion)
{
    Scene scene;
    scene.begin_drag(10, 20);
    ASSERT_EQ(scene.mouse_version(), 1u);

    scene.end_drag();
    EXPECT_FALSE(scene.is_dragging());
    EXPECT_EQ(scene.mouse_version(), 2u);
}

TEST(Scene, DragRectDbuIsNulloptWithoutAMousePositionEvenWhileDragging)
{
    Scene scene;
    scene.begin_drag(10, 10);
    EXPECT_FALSE(scene.drag_rect_dbu().has_value()); // no mouse position set yet
}

TEST(Scene, DragRectDbuNormalizesRegardlessOfDragDirection)
{
    // Dragging from bottom-right up to top-left (in pixel space) must
    // still produce a rect with ll <= ur, not a rect with swapped/negative
    // extents.
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);

    scene.begin_drag(80, 80); // dbu (80, 20)
    scene.set_mouse_position(20, 20); // dbu (20, 80)

    const std::optional<Rect> rect = scene.drag_rect_dbu();
    ASSERT_TRUE(rect.has_value());
    EXPECT_EQ(rect->ll.x, 20);
    EXPECT_EQ(rect->ll.y, 20);
    EXPECT_EQ(rect->ur.x, 80);
    EXPECT_EQ(rect->ur.y, 80);
}

TEST(Scene, KeysDefaultToNotHeld)
{
    Scene scene;
    EXPECT_FALSE(scene.is_key_held(1));
}

TEST(Scene, PressKeyThenIsKeyHeldReturnsTrue)
{
    Scene scene;
    scene.press_key(1);
    EXPECT_TRUE(scene.is_key_held(1));
}

TEST(Scene, ReleaseKeyClearsAHeldKey)
{
    Scene scene;
    scene.press_key(1);
    ASSERT_TRUE(scene.is_key_held(1));

    scene.release_key(1);
    EXPECT_FALSE(scene.is_key_held(1));
}

TEST(Scene, ReleaseKeyWithoutAPrecedingPressIsANoOp)
{
    Scene scene;
    scene.release_key(1);
    EXPECT_FALSE(scene.is_key_held(1));
}

TEST(Scene, DifferentKeyCodesAreTrackedIndependently)
{
    Scene scene;
    scene.press_key(1);
    EXPECT_TRUE(scene.is_key_held(1));
    EXPECT_FALSE(scene.is_key_held(2));
}

TEST(Scene, ClearAllKeysReleasesEveryHeldKey)
{
    // The fix for a real reported bug: a modifier held at the moment a
    // widget loses keyboard focus never gets its matching release event,
    // so it would otherwise stay "held" forever, silently turning every
    // later plain click into a shift-click.
    Scene scene;
    scene.press_key(1);
    scene.press_key(2);
    ASSERT_TRUE(scene.is_key_held(1));
    ASSERT_TRUE(scene.is_key_held(2));

    scene.clear_all_keys();
    EXPECT_FALSE(scene.is_key_held(1));
    EXPECT_FALSE(scene.is_key_held(2));
}

TEST(Scene, ClearAllKeysWithNothingHeldIsANoOp)
{
    Scene scene;
    scene.clear_all_keys();
    EXPECT_FALSE(scene.is_key_held(1));
}

TEST(Scene, HoverDefaultsToUnset)
{
    Scene scene;
    EXPECT_FALSE(scene.hover().has_value());
}

TEST(Scene, HoverRoundTrips)
{
    Scene scene;
    Shape outline;
    outline.rects.push_back(Rect{.ll = {0, 0}, .ur = {10, 10}});

    scene.set_hover(HoverTarget{.origin = TerminalId{5, 1}, .outline = outline});

    ASSERT_TRUE(scene.hover().has_value());
    ASSERT_TRUE(std::holds_alternative<TerminalId>(scene.hover()->origin));
    EXPECT_EQ(std::get<TerminalId>(scene.hover()->origin), (TerminalId{5, 1}));
    ASSERT_EQ(scene.hover()->outline.rects.size(), 1u);
    EXPECT_EQ(scene.hover()->outline.rects.front().ur.x, 10);
}

TEST(Scene, ClearHoverResetsToUnset)
{
    Scene scene;
    Shape outline;
    scene.set_hover(HoverTarget{.origin = ObstructionId{2, 0}, .outline = outline});
    ASSERT_TRUE(scene.hover().has_value());

    scene.clear_hover();
    EXPECT_FALSE(scene.hover().has_value());
}

TEST(Scene, SetModeLeavingSelectClearsHover)
{
    // Regression: the hover outline is a Select-mode-only affordance
    // (see set_mode's own comment) - a stale highlight from just before
    // switching mode must not linger.
    Scene scene;
    Shape outline;
    scene.set_hover(HoverTarget{.origin = ObstructionId{2, 0}, .outline = outline});
    ASSERT_TRUE(scene.hover().has_value());

    scene.set_mode(Scene::Mode::RULER);
    EXPECT_FALSE(scene.hover().has_value());
}

TEST(Scene, SetModeStayingInOrReturningToSelectDoesNotClearHover)
{
    Scene scene;
    Shape outline;
    scene.set_hover(HoverTarget{.origin = ObstructionId{2, 0}, .outline = outline});

    scene.set_mode(Scene::Mode::SELECT); // already SELECT - no-op, no change
    EXPECT_TRUE(scene.hover().has_value());
}

TEST(Scene, SetHoverAndClearHoverDoNotBumpViewportOrVisibilityVersion)
{
    // Hover is driven by every pointer-move event (see le_set_mouse_position)
    // - it must not invalidate the expensive design rasterize cache, only
    // the small mouse-overlay picture (already keyed on mouse_version,
    // which set_mouse_position bumps independently - see scene.hpp's own
    // comment on why set_hover needs no version counter of its own).
    Scene scene;
    const uint64_t viewport_version_before = scene.viewport_version();
    const uint64_t visibility_version_before = scene.visibility_version();

    Shape outline;
    scene.set_hover(HoverTarget{.origin = TerminalId{1, 0}, .outline = outline});
    scene.clear_hover();

    EXPECT_EQ(scene.viewport_version(), viewport_version_before);
    EXPECT_EQ(scene.visibility_version(), visibility_version_before);
}

TEST(Scene, LayerNameVisibilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_layer_name_visible("M1"));

    scene.set_layer_name_visible("M1", false);
    EXPECT_FALSE(scene.is_layer_name_visible("M1"));

    scene.set_layer_name_visible("M1", true);
    EXPECT_TRUE(scene.is_layer_name_visible("M1"));

    // A different, never-toggled layer name is unaffected.
    EXPECT_TRUE(scene.is_layer_name_visible("M2"));
}

TEST(Scene, PurposeVisibilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(scene.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, true);
    EXPECT_TRUE(scene.is_purpose_visible(ViewLayerPurpose::OBSTRUCTION));

    // A different, never-toggled purpose is unaffected.
    EXPECT_TRUE(scene.is_purpose_visible(ViewLayerPurpose::TERMINAL));
}

TEST(Scene, IsViewLayerVisibleIsTheAndOfBothAxes)
{
    Scene scene;

    // Both axes default true -> visible.
    EXPECT_TRUE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));

    // Layer name off, purpose still on -> not visible.
    scene.set_layer_name_visible("M1", false);
    EXPECT_FALSE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
    // A different layer name is unaffected by M1's toggle.
    EXPECT_TRUE(scene.is_view_layer_visible("M2", ViewLayerPurpose::TERMINAL));

    // Layer name back on, but now the purpose is off -> still not visible.
    scene.set_layer_name_visible("M1", true);
    scene.set_purpose_visible(ViewLayerPurpose::TERMINAL, false);
    EXPECT_FALSE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
    // OBSTRUCTION on the same layer is unaffected by the TERMINAL toggle.
    EXPECT_TRUE(scene.is_view_layer_visible("M1", ViewLayerPurpose::OBSTRUCTION));

    // Both axes on again -> visible.
    scene.set_purpose_visible(ViewLayerPurpose::TERMINAL, true);
    EXPECT_TRUE(scene.is_view_layer_visible("M1", ViewLayerPurpose::TERMINAL));
}

TEST(Scene, VisibilityVersionBumpsOnSetLayerNameVisibleAndSetPurposeVisible)
{
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_layer_name_visible("M1", false);
    EXPECT_EQ(scene.visibility_version(), 1u);

    scene.set_purpose_visible(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_EQ(scene.visibility_version(), 2u);

    // Unrelated state (viewport) must not bump it.
    scene.set_pan(Point{5, 5});
    EXPECT_EQ(scene.visibility_version(), 2u);
}

TEST(Scene, LayerNameSelectabilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_layer_name_selectable("M1"));

    scene.set_layer_name_selectable("M1", false);
    EXPECT_FALSE(scene.is_layer_name_selectable("M1"));

    scene.set_layer_name_selectable("M1", true);
    EXPECT_TRUE(scene.is_layer_name_selectable("M1"));

    EXPECT_TRUE(scene.is_layer_name_selectable("M2"));
}

TEST(Scene, PurposeSelectabilityDefaultsToTrueUntilSet)
{
    Scene scene;

    EXPECT_TRUE(scene.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(scene.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, true);
    EXPECT_TRUE(scene.is_purpose_selectable(ViewLayerPurpose::OBSTRUCTION));

    EXPECT_TRUE(scene.is_purpose_selectable(ViewLayerPurpose::TERMINAL));
}

TEST(Scene, IsViewLayerSelectableIsTheAndOfBothAxes)
{
    Scene scene;
    EXPECT_TRUE(scene.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));

    scene.set_layer_name_selectable("M1", false);
    EXPECT_FALSE(scene.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));

    scene.set_layer_name_selectable("M1", true);
    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_FALSE(scene.is_view_layer_selectable("M1", ViewLayerPurpose::OBSTRUCTION));
}

TEST(Scene, SetLayerNameSelectableAndSetPurposeSelectableDoNotBumpVisibilityVersion)
{
    // Selectability isn't consumed by Pipeline/Renderer caching (unlike
    // visibility) - it must not bump visibility_version(), or every
    // selectability toggle would force an unnecessary re-render.
    Scene scene;
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_layer_name_selectable("M1", false);
    EXPECT_EQ(scene.visibility_version(), 0u);

    scene.set_purpose_selectable(ViewLayerPurpose::OBSTRUCTION, false);
    EXPECT_EQ(scene.visibility_version(), 0u);
}

TEST(Scene, ModeDefaultsToSelect)
{
    Scene scene;
    EXPECT_EQ(scene.mode(), Scene::Mode::SELECT);
}

TEST(Scene, SetModeChangesMode)
{
    Scene scene;
    scene.set_mode(Scene::Mode::EDIT);
    EXPECT_EQ(scene.mode(), Scene::Mode::EDIT);

    scene.set_mode(Scene::Mode::SELECT);
    EXPECT_EQ(scene.mode(), Scene::Mode::SELECT);
}

TEST(Scene, SetModeOnlyBumpsMouseVersionOnAnActualChange)
{
    Scene scene;
    const uint64_t baseline = scene.mouse_version();

    scene.set_mode(Scene::Mode::SELECT); // already SELECT - no-op
    EXPECT_EQ(scene.mouse_version(), baseline);

    scene.set_mode(Scene::Mode::EDIT);
    EXPECT_NE(scene.mouse_version(), baseline);
}

// --- Rulers (UPDATES.md item 13) ---

TEST(Scene, RulerModeDefaultsToNoRulers)
{
    Scene scene;
    EXPECT_TRUE(scene.rulers().empty());
    EXPECT_EQ(scene.ruler_version(), 0u);
    EXPECT_FALSE(scene.ruler_free_form());
}

TEST(Scene, AddRulerPointWithNoMousePositionIsNoOp)
{
    Scene scene;
    scene.add_ruler_point(false);
    EXPECT_TRUE(scene.rulers().empty());
    EXPECT_EQ(scene.ruler_version(), 0u);
}

TEST(Scene, AddRulerPointAppendsTheSnappedMousePosition)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.set_mouse_position(23, 27); // dbu (23, 73)
    scene.add_ruler_point(false);

    ASSERT_EQ(scene.rulers().size(), 1u);
    ASSERT_EQ(scene.rulers()[0].points.size(), 1u);
    EXPECT_EQ(scene.rulers()[0].points[0].x, 23);
    EXPECT_EQ(scene.rulers()[0].points[0].y, 73);
    EXPECT_FALSE(scene.rulers()[0].finished);
    EXPECT_NE(scene.ruler_version(), 0u);
}

TEST(Scene, AddRulerPointIdenticalToTheLastCommittedPointIsANoOp)
{
    // Regression: a double-click's second click can easily snap to the
    // exact same grid point as its first (the double-click distance
    // threshold is typically smaller than the grid spacing on screen) -
    // without this guard, that appended a zero-length final segment,
    // which broke the "total: " label's own perpendicular-direction math
    // (see draw_ruler_polyline in draw_helpers.hpp).
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.set_mouse_position(23, 27); // dbu (23, 73)
    scene.add_ruler_point(false);
    const uint64_t after_first = scene.ruler_version();

    scene.set_mouse_position(23, 27); // same snapped point again
    scene.add_ruler_point(false);

    EXPECT_EQ(scene.rulers()[0].points.size(), 1u); // not appended
    EXPECT_EQ(scene.ruler_version(), after_first);
}

TEST(Scene, AddRulerPointConstrainsOrthogonalByDefault)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.set_mouse_position(10, 90); // dbu (10, 10)
    scene.add_ruler_point(false);

    scene.set_mouse_position(40, 70); // dbu (40, 30) - diagonal from (10,10)
    scene.add_ruler_point(false);

    ASSERT_EQ(scene.rulers()[0].points.size(), 2u);
    const Point &second = scene.rulers()[0].points[1];
    // dx (30) > dy (20), so the second point pins y to the first point's y.
    EXPECT_EQ(second.x, 40);
    EXPECT_EQ(second.y, 10);
}

TEST(Scene, AddRulerPointFreeFormIgnoresOrthogonalConstraint)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.set_mouse_position(10, 90); // dbu (10, 10)
    scene.add_ruler_point(true);

    scene.set_mouse_position(40, 70); // dbu (40, 30)
    scene.add_ruler_point(true);

    ASSERT_EQ(scene.rulers()[0].points.size(), 2u);
    const Point &second = scene.rulers()[0].points[1];
    EXPECT_EQ(second.x, 40);
    EXPECT_EQ(second.y, 30);
}

TEST(Scene, FinishActiveRulerMarksItFinishedAndBumpsVersion)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.set_mouse_position(10, 90);
    scene.add_ruler_point(false);
    const uint64_t before = scene.ruler_version();

    scene.finish_active_ruler();
    EXPECT_TRUE(scene.rulers()[0].finished);
    EXPECT_NE(scene.ruler_version(), before);
}

TEST(Scene, FinishActiveRulerWithNoneActiveIsNoOp)
{
    Scene scene;
    scene.finish_active_ruler();
    EXPECT_EQ(scene.ruler_version(), 0u);
    EXPECT_TRUE(scene.rulers().empty());
}

TEST(Scene, ClickAfterFinishFarEnoughAwayStartsASecondRulerLeavingTheFirstIntact)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.set_mouse_position(10, 90); // dbu (10, 10)
    scene.add_ruler_point(false);
    scene.set_mouse_position(20, 90); // dbu (20, 10)
    scene.add_ruler_point(false);
    scene.finish_active_ruler();
    ASSERT_EQ(scene.rulers().size(), 1u);
    ASSERT_EQ(scene.rulers()[0].points.size(), 2u);

    // Well beyond kNewRulerMinDistancePx (20px, scale 1.0) from (20, 10).
    scene.set_mouse_position(80, 90); // dbu (80, 10)
    scene.add_ruler_point(false);

    ASSERT_EQ(scene.rulers().size(), 2u);
    EXPECT_EQ(scene.rulers()[0].points.size(), 2u); // first ruler untouched
    EXPECT_TRUE(scene.rulers()[0].finished);
    ASSERT_EQ(scene.rulers()[1].points.size(), 1u);
    EXPECT_FALSE(scene.rulers()[1].finished);
}

TEST(Scene, ClickTooCloseToAJustFinishedRulerIsANoOp)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.set_mouse_position(10, 90); // dbu (10, 10)
    scene.add_ruler_point(false);
    scene.finish_active_ruler();
    ASSERT_EQ(scene.rulers().size(), 1u);
    const uint64_t before = scene.ruler_version();

    // Only 5 dbu (= 5px at scale 1.0) away - under kNewRulerMinDistancePx.
    scene.set_mouse_position(15, 90); // dbu (15, 10)
    scene.add_ruler_point(false);

    EXPECT_EQ(scene.rulers().size(), 1u); // no second ruler started
    EXPECT_EQ(scene.rulers()[0].points.size(), 1u);
    EXPECT_EQ(scene.ruler_version(), before);
}

TEST(Scene, ResetRulerModeFinishesAnInProgressRulerAndEnsuresRulerMode)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.reset_ruler_mode();
    EXPECT_EQ(scene.mode(), Scene::Mode::RULER);

    scene.set_mouse_position(10, 90);
    scene.add_ruler_point(false);
    ASSERT_FALSE(scene.rulers()[0].finished);

    // Calling again while already in Ruler mode with an in-progress
    // ruler finishes it - this is what lets 'r' double as "abandon the
    // current ruler".
    scene.reset_ruler_mode();
    EXPECT_EQ(scene.mode(), Scene::Mode::RULER);
    EXPECT_TRUE(scene.rulers()[0].finished);
}

TEST(Scene, SetModeLeavingRulerFinishesTheActiveRuler)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.reset_ruler_mode();
    scene.set_mouse_position(10, 90);
    scene.add_ruler_point(false);
    ASSERT_FALSE(scene.rulers()[0].finished);

    scene.set_mode(Scene::Mode::SELECT);
    EXPECT_TRUE(scene.rulers()[0].finished);
}

TEST(Scene, RulerVersionIsNotBumpedByMouseMoveAlone)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.set_mouse_position(10, 90);
    scene.add_ruler_point(false);
    const uint64_t after_add = scene.ruler_version();

    scene.set_mouse_position(50, 50);
    scene.set_mouse_position(20, 20);
    EXPECT_EQ(scene.ruler_version(), after_add);
}

TEST(Scene, SetRulerFreeFormDedupsItsVersionBump)
{
    Scene scene;
    const uint64_t baseline = scene.mouse_version();

    scene.set_ruler_free_form(true);
    const uint64_t after_first = scene.mouse_version();
    EXPECT_NE(after_first, baseline);

    scene.set_ruler_free_form(true); // same value again - no-op
    EXPECT_EQ(scene.mouse_version(), after_first);

    scene.set_ruler_free_form(false);
    EXPECT_NE(scene.mouse_version(), after_first);
}

TEST(Scene, ClearRulersEmptiesAndBumpsVersion)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);

    scene.set_mouse_position(10, 90);
    scene.add_ruler_point(false);
    ASSERT_FALSE(scene.rulers().empty());
    const uint64_t before = scene.ruler_version();

    scene.clear_rulers();
    EXPECT_TRUE(scene.rulers().empty());
    EXPECT_NE(scene.ruler_version(), before);
}

TEST(Scene, ClearRulersOnAnEmptyListIsANoOp)
{
    Scene scene;
    scene.clear_rulers();
    EXPECT_EQ(scene.ruler_version(), 0u);
}

TEST(Scene, ArmMoveWithEmptySelectionIsNoOp)
{
    Scene scene;
    scene.arm_move({});
    EXPECT_FALSE(scene.move().armed);
    EXPECT_TRUE(scene.move().moving_pieces.empty());
}

TEST(Scene, ArmMoveSnapshotsSelectionAndGeometry)
{
    Scene scene;
    ShapeId shape_id{3, 0};
    scene.select(shape_id, PieceKind::POLYGON, 2);

    Shape geometry;
    geometry.layer_name = "M1";
    const uint64_t before = scene.mouse_version();

    scene.arm_move({geometry});
    EXPECT_TRUE(scene.move().armed);
    ASSERT_EQ(scene.move().moving_pieces.size(), 1u);
    EXPECT_EQ(scene.move().moving_pieces[0].shape_id, shape_id);
    EXPECT_EQ(scene.move().moving_pieces[0].piece_kind, PieceKind::POLYGON);
    EXPECT_EQ(scene.move().moving_pieces[0].piece_index, 2u);
    ASSERT_EQ(scene.move().moving_geometry.size(), 1u);
    EXPECT_EQ(scene.move().moving_geometry[0].layer_name, "M1");
    EXPECT_GT(scene.mouse_version(), before);
}

TEST(Scene, RefreshMoveGeometryReplacesTheGhostSnapshotWithoutTouchingAnchorOrArmedState)
{
    // Regression: committing a move re-arms for a follow-up move (see
    // api.cpp's move_click_unlocked), keeping the ghost snapshot from the
    // moment of that re-arm - if something *else* changes the moving
    // shapes' geometry afterward (an external undo/redo), that snapshot
    // goes stale unless explicitly refreshed. refresh_move_geometry is
    // that explicit refresh - unlike arm_move, it must not touch
    // anchor/armed, since an undo/redo can happen mid-gesture too.
    Scene scene;
    scene.select(ShapeId{1, 0});
    Shape original;
    original.layer_name = "M1";
    scene.arm_move({original});
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_mouse_position(10, 10);
    ASSERT_TRUE(scene.move_set_anchor());
    ASSERT_TRUE(scene.move().anchor.has_value());
    const uint64_t before = scene.mouse_version();

    Shape refreshed;
    refreshed.layer_name = "M2";
    scene.refresh_move_geometry({refreshed});

    EXPECT_TRUE(scene.move().armed); // untouched
    ASSERT_TRUE(scene.move().anchor.has_value()); // untouched - not cleared like arm_move would
    ASSERT_EQ(scene.move().moving_geometry.size(), 1u);
    EXPECT_EQ(scene.move().moving_geometry[0].layer_name, "M2");
    EXPECT_GT(scene.mouse_version(), before);
}

TEST(Scene, RefreshMoveGeometryIsANoOpWhenNotArmed)
{
    Scene scene;
    const uint64_t before = scene.mouse_version();
    Shape geometry;
    scene.refresh_move_geometry({geometry});
    EXPECT_FALSE(scene.move().armed);
    EXPECT_TRUE(scene.move().moving_geometry.empty());
    EXPECT_EQ(scene.mouse_version(), before);
}

TEST(Scene, MoveSetAnchorRequiresArmedAndAMousePosition)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);
    scene.select(ShapeId{1, 0});

    // Not armed yet - no-op.
    scene.set_mouse_position(10, 90);
    EXPECT_FALSE(scene.move_set_anchor());

    scene.arm_move({Shape{}});
    // Armed, but requires a mouse position too - already set above, so
    // this should succeed now.
    EXPECT_TRUE(scene.move_set_anchor());
    ASSERT_TRUE(scene.move().anchor.has_value());
    EXPECT_EQ(scene.move().anchor->x, 10);
    EXPECT_EQ(scene.move().anchor->y, 10);

    // A second call while an anchor already exists is a no-op.
    EXPECT_FALSE(scene.move_set_anchor());
}

TEST(Scene, MoveDeltaOrthogonalConstrainsToTheLargerMagnitudeAxis)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);
    scene.select(ShapeId{1, 0});
    scene.arm_move({Shape{}});

    scene.set_mouse_position(10, 90); // dbu (10, 10)
    ASSERT_TRUE(scene.move_set_anchor());

    scene.set_mouse_position(25, 85); // dbu (25, 15) - dx=15, dy=5, x wins
    const std::optional<Point> delta = scene.move_delta(/*free_form=*/false);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->x, 15);
    EXPECT_EQ(delta->y, 0);
}

TEST(Scene, MoveDeltaFreeFormReturnsTheRawOffset)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_minor_grid_spacing(1);
    scene.select(ShapeId{1, 0});
    scene.arm_move({Shape{}});

    scene.set_mouse_position(10, 90); // dbu (10, 10)
    ASSERT_TRUE(scene.move_set_anchor());

    scene.set_mouse_position(25, 85); // dbu (25, 15)
    const std::optional<Point> delta = scene.move_delta(/*free_form=*/true);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->x, 15);
    EXPECT_EQ(delta->y, 5);
}

TEST(Scene, MoveDeltaIsNulloptBeforeAnAnchorIsSet)
{
    Scene scene;
    scene.select(ShapeId{1, 0});
    scene.arm_move({Shape{}});
    EXPECT_FALSE(scene.move_delta(false).has_value());
}

TEST(Scene, EndMoveClearsAllMoveState)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.select(ShapeId{1, 0});
    scene.arm_move({Shape{}});
    scene.set_mouse_position(10, 10);
    scene.move_set_anchor();

    scene.end_move();
    EXPECT_FALSE(scene.move().armed);
    EXPECT_FALSE(scene.move().anchor.has_value());
    EXPECT_TRUE(scene.move().moving_pieces.empty());
}

TEST(Scene, SetModeLeavingEditCancelsAnInProgressMove)
{
    Scene scene;
    scene.set_pan(Point{0, 0});
    scene.set_scale(1.0);
    scene.set_viewport_size(100, 100);
    scene.set_mode(Scene::Mode::EDIT);
    scene.select(ShapeId{1, 0});
    scene.arm_move({Shape{}});
    ASSERT_TRUE(scene.move().armed);

    scene.set_mode(Scene::Mode::SELECT);
    EXPECT_FALSE(scene.move().armed);
}

TEST(Scene, SetMoveFreeFormDedupsItsVersionBump)
{
    Scene scene;
    scene.set_move_free_form(true);
    const uint64_t after_first = scene.mouse_version();
    scene.set_move_free_form(true);
    EXPECT_EQ(scene.mouse_version(), after_first);

    scene.set_move_free_form(false);
    EXPECT_GT(scene.mouse_version(), after_first);
}

TEST(Scene, SelectDeselectAndClear)
{
    Scene scene;
    ShapeId first{1, 0};
    ShapeId second{2, 0};

    scene.select(first);
    scene.select(second);
    EXPECT_TRUE(scene.is_selected(first));
    EXPECT_TRUE(scene.is_selected(second));
    EXPECT_EQ(scene.selection().size(), 2u);

    // Selecting the same id again must not duplicate it.
    scene.select(first);
    EXPECT_EQ(scene.selection().size(), 2u);

    scene.deselect(first);
    EXPECT_FALSE(scene.is_selected(first));
    EXPECT_TRUE(scene.is_selected(second));
    EXPECT_EQ(scene.selection().size(), 1u);

    scene.clear_selection();
    EXPECT_TRUE(scene.selection().empty());
}

TEST(Scene, FitToContentCentersAndScalesToFillTheTighterAxis)
{
    Scene scene;
    scene.set_viewport_size(1000, 1000);

    // 100x50 dbu content, 20px padding on each side -> usable 960px, scale
    // bound by the wider (x) axis: 960 / 100 = 9.6.
    Rect bbox{.ll = Point{0, 0}, .ur = Point{100, 50}};
    scene.fit_to_content(bbox, 20);

    EXPECT_DOUBLE_EQ(scene.scale(), 9.6);

    // Content is centered: leftover space on y is (1000/9.6 - 50) ~= 54.17,
    // half of that offsets pan below the bbox's own ll.y.
    const double expected_pan_y = 0 - (1000.0 / 9.6 - 50.0) / 2.0;
    EXPECT_EQ(scene.pan().x, 0 - static_cast<int64_t>((1000.0 / 9.6 - 100.0) / 2.0));
    EXPECT_EQ(scene.pan().y, static_cast<int64_t>(expected_pan_y));
}

TEST(Scene, FitToContentWithNoBboxFallsBackToDefaultScaleAndPan)
{
    Scene scene;
    scene.set_viewport_size(500, 500);
    scene.set_scale(3.0);
    scene.set_pan(Point{10, 10});

    scene.fit_to_content(std::nullopt, 10);

    EXPECT_DOUBLE_EQ(scene.scale(), 1.0);
    EXPECT_EQ(scene.pan().x, 0);
    EXPECT_EQ(scene.pan().y, 0);
}

TEST(Scene, FitToContentWithZeroSizedViewportFallsBackToDefault)
{
    Scene scene;
    Rect bbox{.ll = Point{0, 0}, .ur = Point{100, 100}};
    scene.fit_to_content(bbox, 10);

    EXPECT_DOUBLE_EQ(scene.scale(), 1.0);
    EXPECT_EQ(scene.pan().x, 0);
    EXPECT_EQ(scene.pan().y, 0);
}

TEST(Scene, FitToContentWithDegenerateZeroWidthBboxDoesNotDivideByZero)
{
    Scene scene;
    scene.set_viewport_size(200, 200);

    // Zero-width content (e.g. a single vertical line) - scale must be
    // bounded by the y axis only, not blow up via a 1/0 on x.
    Rect bbox{.ll = Point{5, 0}, .ur = Point{5, 100}};
    scene.fit_to_content(bbox, 0);

    EXPECT_DOUBLE_EQ(scene.scale(), 2.0); // 200 / 100
}

TEST(Scene, SelectBumpsSelectionVersionOnlyOnAnActualChange)
{
    Scene scene;
    ShapeId id{5, 0};
    EXPECT_EQ(scene.selection_version(), 0u);

    scene.select(id);
    EXPECT_EQ(scene.selection_version(), 1u);

    // Already selected - a no-op, must not bump again.
    scene.select(id);
    EXPECT_EQ(scene.selection_version(), 1u);
}

TEST(Scene, DeselectBumpsSelectionVersionOnlyOnAnActualChange)
{
    Scene scene;
    ShapeId id{5, 0};
    scene.select(id);
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.deselect(id);
    EXPECT_FALSE(scene.is_selected(id));
    EXPECT_EQ(scene.selection_version(), 2u);

    // Already gone - a no-op, must not bump again.
    scene.deselect(id);
    EXPECT_EQ(scene.selection_version(), 2u);
}

TEST(Scene, ClearSelectionBumpsSelectionVersionOnlyIfSelectionWasNonEmpty)
{
    Scene scene;
    ShapeId id{5, 0};

    scene.clear_selection(); // already empty - a no-op
    EXPECT_EQ(scene.selection_version(), 0u);

    scene.select(id);
    ASSERT_EQ(scene.selection_version(), 1u);

    scene.clear_selection();
    EXPECT_TRUE(scene.selection().empty());
    EXPECT_EQ(scene.selection_version(), 2u);
}

TEST(Scene, SelectionChangesDoNotBumpViewportOrVisibilityVersion)
{
    Scene scene;
    const uint64_t viewport_version_before = scene.viewport_version();
    const uint64_t visibility_version_before = scene.visibility_version();

    scene.select(ShapeId{5, 0});
    scene.clear_selection();

    EXPECT_EQ(scene.viewport_version(), viewport_version_before);
    EXPECT_EQ(scene.visibility_version(), visibility_version_before);
}
