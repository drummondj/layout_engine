// Direct unit coverage for UprightTextCanvas (BUGS_AND_ENHANCEMENTS.md E22)
// - built against a small, self-contained SkPicture rather than through the
// whole HierarchyResolver pipeline: an earlier attempt at a pipeline-level
// test (a rotated Placement's own terminal/name-label text) turned out
// unable to isolate the label's own glyph pixels reliably from the
// surrounding shape's own fill/outline color, and worse, initially "passed"
// even with UprightTextCanvas's own onDrawPicture override deliberately
// reverted - not because the fix wasn't needed, but because the specific
// content picked (a Placement's own name label, draw_placement_labels) is
// drawn axis-aligned at the top level regardless of Placement orientation,
// never actually exercising the bug. This file instead builds the exact
// nested-picture shape BuildLayoutPictureStage's own per-instance
// `canvas->concat(instance_transform); canvas->drawPicture(child)` idiom
// produces (a child picture drawn upright-in-local-space, replayed through
// a rotating outer transform) and gives the outer picture more than one
// top-level op (SkCanvasPriv.h's kMaxPictureOpsToUnrollInsteadOfRef == 1) -
// the exact condition under which SkPaintFilterCanvas's own default
// onDrawPicture forwards straight to the wrapped proxy canvas instead of
// replaying through `this` (see upright_text_canvas.hpp's own doc comment),
// so this test only reliably reproduces the bug's absence when reverted.
#include "../upright_text_canvas.hpp"
#include "../draw_helpers.hpp"
#include "include/core/SkBitmap.h"
#include "include/core/SkFont.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkSurface.h"
#include <gtest/gtest.h>

namespace
{
    // A small "leaf" picture drawing one line of text upright in its own
    // local space - exactly what draw_group's own text loop
    // (draw_helpers.hpp) records for a Terminal's name label inside an
    // Abstract's own picture, before any instance transform is applied.
    sk_sp<SkPicture> build_leaf_picture()
    {
        SkPictureRecorder recorder;
        SkCanvas *canvas = recorder.beginRecording(SkRect::MakeWH(200, 60));
        SkFont font(le::default_typeface(), 24);
        SkPaint paint;
        paint.setColor(SK_ColorBLACK);
        canvas->drawString("WIDE_TEXT_LABEL", 0, 40, font, paint);
        return recorder.finishRecordingAsPicture();
    }

    // The "parent" picture: a filler rect (so approximateOpCount() > 1,
    // matching every real picture in this codebase - see this file's own
    // header comment) plus one nested instance drawn via
    // concat(instance_transform) + drawPicture(leaf), mirroring
    // BuildLayoutPictureStage::run's own per-Placement idiom exactly.
    sk_sp<SkPicture> build_parent_picture(const sk_sp<SkPicture> &leaf, const SkMatrix &instance_transform)
    {
        SkPictureRecorder recorder;
        SkCanvas *canvas = recorder.beginRecording(SkRect::MakeWH(400, 400));
        SkPaint filler;
        filler.setColor(SK_ColorLTGRAY);
        canvas->drawRect(SkRect::MakeWH(400, 400), filler);
        canvas->save();
        canvas->concat(instance_transform);
        canvas->drawPicture(leaf.get());
        canvas->restore();
        return recorder.finishRecordingAsPicture();
    }

    // Tight bounding box of black (the label text's own color, distinct
    // from the light-gray filler) pixels in a bitmap. Returns false if
    // none were found.
    bool black_pixel_bbox(const SkBitmap &bitmap, int &out_minx, int &out_miny, int &out_maxx, int &out_maxy)
    {
        bool found = false;
        int minx = 0, miny = 0, maxx = 0, maxy = 0;
        for (int y = 0; y < bitmap.height(); ++y)
            for (int x = 0; x < bitmap.width(); ++x)
            {
                const SkColor c = bitmap.getColor(x, y);
                if (SkColorGetA(c) > 200 && SkColorGetR(c) < 40 && SkColorGetG(c) < 40 && SkColorGetB(c) < 40)
                {
                    if (!found)
                    {
                        minx = maxx = x;
                        miny = maxy = y;
                        found = true;
                    }
                    else
                    {
                        minx = std::min(minx, x);
                        maxx = std::max(maxx, x);
                        miny = std::min(miny, y);
                        maxy = std::max(maxy, y);
                    }
                }
            }
        if (found)
        {
            out_minx = minx;
            out_miny = miny;
            out_maxx = maxx;
            out_maxy = maxy;
        }
        return found;
    }
}

TEST(UprightTextCanvas, BaselinePlainCanvasDrawsTextRotatedByTheInstanceTransform)
{
    // Sanity check that this test's own setup actually exercises rotation
    // in the first place - a plain (unwrapped) SkCanvas should draw the
    // leaf's text rotated 90 degrees along with everything else, same as
    // this codebase's real behavior before E22.
    SkMatrix instance_transform = SkMatrix::RotateDeg(90);
    instance_transform.postTranslate(100, 100);
    const sk_sp<SkPicture> leaf = build_leaf_picture();
    const sk_sp<SkPicture> parent = build_parent_picture(leaf, instance_transform);

    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(400, 400));
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    surface->getCanvas()->drawPicture(parent);

    SkBitmap bitmap;
    bitmap.allocPixels(SkImageInfo::MakeN32Premul(400, 400));
    surface->readPixels(bitmap, 0, 0);

    int minx, miny, maxx, maxy;
    ASSERT_TRUE(black_pixel_bbox(bitmap, minx, miny, maxx, maxy)) << "expected to find the leaf's own text somewhere";
    const int width = maxx - minx + 1;
    const int height = maxy - miny + 1;
    EXPECT_GT(height, width) << "rotated-90 text should read taller than wide on a plain canvas (width=" << width << " height=" << height << ")";
}

TEST(UprightTextCanvas, ForcesTextUprightThroughANestedInstanceRotation)
{
    // The real fix: the exact same nested picture as the baseline above,
    // rasterized through UprightTextCanvas instead - text should stay
    // wide (upright), not flip tall the way the baseline did.
    SkMatrix instance_transform = SkMatrix::RotateDeg(90);
    instance_transform.postTranslate(100, 100);
    const sk_sp<SkPicture> leaf = build_leaf_picture();
    const sk_sp<SkPicture> parent = build_parent_picture(leaf, instance_transform);

    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(400, 400));
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    le::UprightTextCanvas upright_canvas(surface->getCanvas());
    upright_canvas.drawPicture(parent);

    SkBitmap bitmap;
    bitmap.allocPixels(SkImageInfo::MakeN32Premul(400, 400));
    surface->readPixels(bitmap, 0, 0);

    int minx, miny, maxx, maxy;
    ASSERT_TRUE(black_pixel_bbox(bitmap, minx, miny, maxx, maxy)) << "expected to find the leaf's own text somewhere";
    const int width = maxx - minx + 1;
    const int height = maxy - miny + 1;
    EXPECT_GT(width, height * 2) << "upright text should read wide, not tall, despite the instance's own 90-degree rotation (width=" << width << " height=" << height << ")";
}

TEST(UprightTextCanvas, StaysUprightRegardlessOfWhichOrientationTheInstanceUses)
{
    // Same as the fix test above, but at two more rotation/reflection
    // transforms - 270 degrees, and a mirror-then-rotate-90 (determinant
    // -1, matching a LEF/DEF FW/FE-style orientation - a real
    // Geometry::orientation_linear case, unlike a bare mirror alone,
    // which this codebase's own real Placement orientations never
    // produce without an accompanying 90-family rotation). Deliberately
    // NOT 180 degrees or a bare mirror: both preserve a wide region's own
    // aspect ratio even when NOT forced upright (rotating or mirroring a
    // wide-not-tall glyph region by 180 degrees, or along one axis alone,
    // is still wide-not-tall), so the width>height*2 check below can't
    // actually distinguish "upright" from "upside-down"/"backwards" for
    // those two cases - confirmed by testing: both still passed with
    // onDrawTextBlob's override deliberately reverted, unlike the two
    // used here, which correctly failed reverted (swapping
    // width/height, the same real discriminator
    // ForcesTextUprightThroughANestedInstanceRotation above uses).
    for (const SkMatrix &raw_transform : {
             []
             { SkMatrix m = SkMatrix::RotateDeg(270); m.postTranslate(300, 100); return m; }(),
             []
             { SkMatrix m; m.setScale(-1, 1); m.postConcat(SkMatrix::RotateDeg(90)); m.postTranslate(300, 300); return m; }(),
         })
    {
        const sk_sp<SkPicture> leaf = build_leaf_picture();
        const sk_sp<SkPicture> parent = build_parent_picture(leaf, raw_transform);

        sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(400, 400));
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        le::UprightTextCanvas upright_canvas(surface->getCanvas());
        upright_canvas.drawPicture(parent);

        SkBitmap bitmap;
        bitmap.allocPixels(SkImageInfo::MakeN32Premul(400, 400));
        surface->readPixels(bitmap, 0, 0);

        int minx, miny, maxx, maxy;
        ASSERT_TRUE(black_pixel_bbox(bitmap, minx, miny, maxx, maxy)) << "expected to find the leaf's own text somewhere";
        const int width = maxx - minx + 1;
        const int height = maxy - miny + 1;
        EXPECT_GT(width, height * 2) << "upright text should read wide, not tall (width=" << width << " height=" << height << ")";
    }
}
