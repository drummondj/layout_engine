// Direct unit coverage for draw_group's own hairline-simplification
// branch (a rect or path sub-pixel in exactly one dimension draws as a
// single hairline instead of a full fill+outline/buffered-outline pass -
// see draw_helpers.hpp's own comments on that branch). No existing
// pipeline-level fixture exercises this: every shape in those fixtures
// is well above 1px in both dimensions. Tests draw_group directly rather
// than through a whole pipeline, since it's a plain free function over a
// canvas + shape list + style.
#include "../draw_helpers.hpp"
#include "../pixel_types.hpp"
#include "../../view_style/view_style.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#include <gtest/gtest.h>

namespace
{
    SkBitmap draw_group_to_bitmap(const std::vector<le::PixelShape> &group, const le::ViewLayerStyle &style, int width, int height, bool antialiasing_enabled = true)
    {
        sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        le::draw_group(*surface->getCanvas(), group, style, antialiasing_enabled);

        SkBitmap bitmap;
        bitmap.allocPixels(SkImageInfo::MakeN32Premul(width, height));
        surface->readPixels(bitmap, 0, 0);
        return bitmap;
    }

    // A small column/row tolerance band, not an exact pixel - hairline
    // placement can land on either side of a pixel boundary depending on
    // exact sub-pixel AA rasterization, which isn't the property under
    // test here. What distinguishes a hairline draw from a real (if
    // sub-pixel-wide) AA fill is opacity strength: a genuine fill of a
    // <1px-wide rect can only ever achieve partial (fractional-coverage)
    // alpha, while a hairline stroke is a full-strength line - so a
    // "high enough alpha somewhere nearby" check is both robust to exact
    // placement and still a real assertion that a hairline (not a faint
    // sub-pixel fill, not nothing) was drawn.
    bool band_has_strong_opaque_pixel(const SkBitmap &bitmap, int x0, int y0, int x1, int y1, uint8_t min_alpha = 200)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (SkColorGetA(bitmap.getColor(x, y)) >= min_alpha)
                    return true;
        return false;
    }

    bool region_has_opaque_pixel(const SkBitmap &bitmap, int x0, int y0, int x1, int y1)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (SkColorGetA(bitmap.getColor(x, y)) > 0)
                    return true;
        return false;
    }
}

TEST(DrawGroupHairline, SubPixelWidthRectDrawsAContinuousVerticalHairlineAtFullStrength)
{
    // width 0.3px (sub-pixel), height 40px (well above 1px) - the "long
    // thin wire" case ViewportFilterStage's own doc comment describes:
    // must stay visible, not culled, and not rasterized as a faint,
    // barely-there AA sliver either.
    // Centered on column 10's own pixel center (10.5, not the 10.0
    // boundary) - an AA hairline centered exactly on a pixel boundary
    // legitimately splits ~50/50 across the two adjacent columns, which
    // would fail a single-pixel full-strength check for the wrong
    // reason (test geometry, not draw_group's own behavior).
    le::PixelShape shape;
    shape.rects.push_back(le::PixelRect{.ll = {.x = 10.35, .y = 5.0}, .ur = {.x = 10.65, .y = 45.0}});

    le::ViewLayerStyle style;
    style.fill_color = le::Color{.r = 200, .g = 50, .b = 50, .a = 255};
    style.outline_color = le::Color{.r = 0, .g = 0, .b = 0, .a = 255};

    const SkBitmap bitmap = draw_group_to_bitmap({shape}, style, 20, 50);

    for (int y = 6; y < 45; ++y)
        EXPECT_TRUE(band_has_strong_opaque_pixel(bitmap, 8, y, 12, y)) << "row " << y;
}

TEST(DrawGroupHairline, SubPixelHeightRectDrawsAContinuousHorizontalHairlineAtFullStrength)
{
    // Centered on row 10's own pixel center (10.5, not the 10.0
    // boundary) - see the width-hairline test's own comment for why.
    le::PixelShape shape;
    shape.rects.push_back(le::PixelRect{.ll = {.x = 5.0, .y = 10.35}, .ur = {.x = 45.0, .y = 10.65}});

    le::ViewLayerStyle style;
    style.fill_color = le::Color{.r = 200, .g = 50, .b = 50, .a = 255};
    style.outline_color = le::Color{.r = 0, .g = 0, .b = 0, .a = 255};

    const SkBitmap bitmap = draw_group_to_bitmap({shape}, style, 50, 20);

    for (int x = 6; x < 45; ++x)
        EXPECT_TRUE(band_has_strong_opaque_pixel(bitmap, x, 8, x, 12)) << "col " << x;
}

TEST(DrawGroupHairline, RectSubPixelInBothDimensionsFallsBackToTheOrdinaryFillPath)
{
    // Defensive-only case: ViewportFilterStage already drops anything
    // sub-pixel in BOTH dimensions upstream (in favor of the tiny-shape-
    // dot mechanism), so draw_group should never actually see one in
    // practice - but the hairline branch's own (width<1) != (height<1)
    // check must not misfire and treat this as the single-dimension
    // case. No crash, no assertion on exact pixels - just confirms the
    // ordinary fill path (not the hairline one) is what runs.
    le::PixelShape shape;
    shape.rects.push_back(le::PixelRect{.ll = {.x = 9.9, .y = 9.9}, .ur = {.x = 10.1, .y = 10.1}});

    le::ViewLayerStyle style;
    style.fill_color = le::Color{.r = 200, .g = 50, .b = 50, .a = 255};

    EXPECT_NO_FATAL_FAILURE(draw_group_to_bitmap({shape}, style, 20, 20));
}

TEST(DrawGroupHairline, NormalSizedRectStillFillsItsWholeAreaNotJustACenterline)
{
    // Regression guard: a rect well above 1px in both dimensions must
    // NOT take the hairline branch - its full interior should be
    // opaque, including well off any hairline-through-the-middle line.
    le::PixelShape shape;
    shape.rects.push_back(le::PixelRect{.ll = {.x = 10.0, .y = 10.0}, .ur = {.x = 30.0, .y = 30.0}});

    le::ViewLayerStyle style;
    style.fill_color = le::Color{.r = 200, .g = 50, .b = 50, .a = 255};

    const SkBitmap bitmap = draw_group_to_bitmap({shape}, style, 40, 40);

    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 12, 12, 14, 28)); // near the left edge, off the centerline
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 26, 12, 28, 28)); // near the right edge, off the centerline
}

TEST(DrawGroupHairline, SubPixelWidthPathDrawsACenterlineHairlineAtFullStrength)
{
    // Centered on row 10's own pixel center (10.5, not the 10.0
    // boundary) - see the rect hairline tests' own comment for why.
    le::PixelShape shape;
    le::PixelPath path;
    path.width = 0.4; // sub-pixel
    path.polygon.points = {{.x = 5.0, .y = 10.5}, {.x = 35.0, .y = 10.5}};
    // Deliberately left empty - mirrors transform_shapes_to_pixel_space's
    // own "sub-pixel paths skip transforming buffered_outline" behavior,
    // so this also confirms draw_group never dereferences it here.
    shape.paths.push_back(std::move(path));

    le::ViewLayerStyle style;
    style.fill_color = le::Color{.r = 200, .g = 50, .b = 50, .a = 255};
    style.outline_color = le::Color{.r = 0, .g = 0, .b = 0, .a = 255};

    const SkBitmap bitmap = draw_group_to_bitmap({shape}, style, 40, 20);

    for (int x = 6; x < 35; ++x)
        EXPECT_TRUE(band_has_strong_opaque_pixel(bitmap, x, 8, x, 12)) << "col " << x;
}

TEST(DrawGroupHairline, NormalWidthPathStillDrawsTheBufferedOutlineFill)
{
    // Regression guard: a path at/above the 1px threshold must NOT take
    // the hairline branch - its buffered_outline fill should cover a
    // real area around the centerline, not just a single-pixel line.
    le::PixelShape shape;
    le::PixelPath path;
    path.width = 6.0;
    path.polygon.points = {{.x = 5.0, .y = 10.0}, {.x = 35.0, .y = 10.0}};
    path.buffered_outline.push_back(le::PixelPolygon{.points = {
                                                           {.x = 5.0, .y = 7.0},
                                                           {.x = 35.0, .y = 7.0},
                                                           {.x = 35.0, .y = 13.0},
                                                           {.x = 5.0, .y = 13.0},
                                                       }});
    shape.paths.push_back(std::move(path));

    le::ViewLayerStyle style;
    style.fill_color = le::Color{.r = 200, .g = 50, .b = 50, .a = 255};

    const SkBitmap bitmap = draw_group_to_bitmap({shape}, style, 40, 20);

    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 15, 8, 17, 8));  // near the top edge of the buffered outline
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 15, 12, 17, 12)); // near the bottom edge of the buffered outline
}

TEST(DrawGroupAntiAliasing, DisablingAntiAliasingProducesAHardEdgeInsteadOfAFractionalCoverageOne)
{
    // A rect whose own edge lands mid-pixel (not aligned to a whole
    // pixel boundary) - with AA on, Skia blends partial coverage into
    // the boundary pixel (an alpha strictly between 0 and 255); with AA
    // off, every pixel is either fully covered or not at all, so that
    // same boundary pixel must land on one side or the other with no
    // fractional value. This is the actual, directly observable effect
    // Scene::antialiasing_enabled()/draw_group's own new parameter
    // controls - not just "does it compile with a bool added".
    //
    // outline_color is explicitly transparent: ViewLayerStyle's own
    // default is an *opaque* outline (Color's own a=255 default), which
    // would stroke a solid hairline right over this same boundary pixel
    // and swamp the fill's own fractional-coverage edge under test here.
    le::PixelShape shape;
    shape.rects.push_back(le::PixelRect{.ll = {.x = 10.0, .y = 10.0}, .ur = {.x = 20.5, .y = 20.0}});

    le::ViewLayerStyle style;
    style.fill_color = le::Color{.r = 200, .g = 50, .b = 50, .a = 255};
    style.outline_color = le::Color{.r = 0, .g = 0, .b = 0, .a = 0};

    const SkBitmap aa_on = draw_group_to_bitmap({shape}, style, 30, 30, /*antialiasing_enabled=*/true);
    const SkBitmap aa_off = draw_group_to_bitmap({shape}, style, 30, 30, /*antialiasing_enabled=*/false);

    // Column 20 is the boundary pixel (rect edge at x=20.5, mid-pixel).
    const uint8_t aa_on_alpha = SkColorGetA(aa_on.getColor(20, 15));
    const uint8_t aa_off_alpha = SkColorGetA(aa_off.getColor(20, 15));
    EXPECT_GT(aa_on_alpha, 0);
    EXPECT_LT(aa_on_alpha, 255);
    EXPECT_TRUE(aa_off_alpha == 0 || aa_off_alpha == 255) << "got " << static_cast<int>(aa_off_alpha);
}

// BUGS_AND_ENHANCEMENTS.md E19 - text_paint/label_origin_paint used to be
// hardcoded setAntiAlias(true), unlike fill/stroke just above (which
// already respected antialiasing_enabled) - a real, accidental
// inconsistency (draw_placement_labels' own text_paint already got this
// right; draw_group's own terminal/route-label text_paint, in the same
// function, didn't) rather than a deliberate design boundary. Same
// "fractional vs. binary coverage" observable effect as the rect case
// above, applied to glyph edges instead of a rect edge: counts pixels
// with strictly-fractional alpha (neither fully transparent nor fully
// opaque) across the whole label's own bounding region - AA-on text has
// many (smoothed glyph curves/diagonals), AA-off text should have none
// (every pixel is either in or out of the rasterized glyph outline).
TEST(DrawGroupAntiAliasing, DisablingAntiAliasingAlsoProducesHardEdgesForText)
{
    le::PixelShape shape;
    shape.texts.push_back(le::PixelText{.label = "AV", .location = {.x = 10, .y = 10}, .size = 40.0});

    le::ViewLayerStyle style;
    style.fill_color = le::Color{.r = 0, .g = 0, .b = 0, .a = 0};
    style.outline_color = le::Color{.r = 0, .g = 0, .b = 0, .a = 255};

    const SkBitmap aa_on = draw_group_to_bitmap({shape}, style, 60, 60, /*antialiasing_enabled=*/true);
    const SkBitmap aa_off = draw_group_to_bitmap({shape}, style, 60, 60, /*antialiasing_enabled=*/false);

    auto count_fractional_alpha_pixels = [](const SkBitmap &bitmap)
    {
        int count = 0;
        for (int y = 0; y < bitmap.height(); ++y)
            for (int x = 0; x < bitmap.width(); ++x)
            {
                const uint8_t a = SkColorGetA(bitmap.getColor(x, y));
                if (a > 0 && a < 255)
                    ++count;
            }
        return count;
    };

    const int aa_on_fractional = count_fractional_alpha_pixels(aa_on);
    const int aa_off_fractional = count_fractional_alpha_pixels(aa_off);
    EXPECT_GT(aa_on_fractional, 0) << "AA-on text should have real fractional-coverage edge pixels";
    EXPECT_EQ(aa_off_fractional, 0) << "AA-off text should have no fractional-coverage pixels at all";
}

// BUGS_AND_ENHANCEMENTS.md E16 - the small "+" marker drawn at each
// label's own anchor point (kLabelOriginMarkerSizeRatio) used to be a
// fixed pixel size regardless of the label's own text size; now it
// scales with it (half the text's own pixel height/size). Two labels
// with very different PixelText::size, anchored far enough apart that
// their own crosses can't overlap - the larger text's cross must reach
// noticeably farther from its own anchor than the smaller text's does.
TEST(DrawGroupLabelOriginMarker, CrossMarkerSizeScalesWithTheLabelsOwnTextSize)
{
    le::PixelShape small_text_shape;
    small_text_shape.texts.push_back(le::PixelText{.label = "", .location = {.x = 50, .y = 50}, .size = 20.0});

    le::PixelShape large_text_shape;
    large_text_shape.texts.push_back(le::PixelText{.label = "", .location = {.x = 150, .y = 150}, .size = 100.0});

    le::ViewLayerStyle style;
    style.fill_color = le::Color{.r = 0, .g = 0, .b = 0, .a = 0};
    style.outline_color = le::Color{.r = 0, .g = 0, .b = 0, .a = 255};

    const SkBitmap bitmap = draw_group_to_bitmap({small_text_shape, large_text_shape}, style, 200, 200);

    // Half-length is size * kLabelOriginMarkerSizeRatio / 2 = size / 4:
    // small (20px) -> 5px, large (100px) -> 25px.
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 46, 50, 54, 50)) << "small label's own cross should be present near its anchor";
    EXPECT_FALSE(region_has_opaque_pixel(bitmap, 65, 50, 65, 50)) << "small label's own cross should not reach 15px out";

    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 146, 150, 154, 150)) << "large label's own cross should be present near its anchor";
    EXPECT_TRUE(region_has_opaque_pixel(bitmap, 165, 150, 165, 150)) << "large label's own cross should reach 15px out (half-length 25px)";
}

// BUGS_AND_ENHANCEMENTS.md E13 - truncate_text_to_width backs Placement
// name labels (replaces characters at the *beginning* with "...", unlike
// a typical trailing-ellipsis truncation).
TEST(TruncateTextToWidth, TextThatAlreadyFitsIsReturnedUnchanged)
{
    const SkFont font(le::default_typeface(), 20.0f);
    const SkScalar full_width = font.measureText("U1", 2, SkTextEncoding::kUTF8);

    EXPECT_EQ(le::truncate_text_to_width("U1", font, full_width + 10.0f), "U1");
}

TEST(TruncateTextToWidth, TooLongTextIsTruncatedFromTheFrontWithAnEllipsisPrefix)
{
    const SkFont font(le::default_typeface(), 20.0f);
    const std::string name = "some_very_long_hierarchical_instance_name_42";
    const SkScalar full_width = font.measureText(name.c_str(), name.size(), SkTextEncoding::kUTF8);

    // Half the full width - definitely too narrow for the whole name,
    // definitely wide enough for a real (non-empty) truncated remainder.
    const SkScalar target_width = full_width / 2.0f;
    const std::string truncated = le::truncate_text_to_width(name, font, target_width);

    ASSERT_FALSE(truncated.empty());
    EXPECT_EQ(truncated.substr(0, 3), "...");
    // The *end* of the original name survives - the whole point of
    // front-truncation is keeping a shared-prefix name's own
    // distinguishing suffix visible.
    EXPECT_EQ(truncated.substr(truncated.size() - 2), "42");
    EXPECT_LE(font.measureText(truncated.c_str(), truncated.size(), SkTextEncoding::kUTF8), target_width);
}

TEST(TruncateTextToWidth, WidthTooNarrowForEvenTheEllipsisReturnsEmpty)
{
    const SkFont font(le::default_typeface(), 20.0f);
    EXPECT_EQ(le::truncate_text_to_width("U1", font, 0.5f), "");
}
