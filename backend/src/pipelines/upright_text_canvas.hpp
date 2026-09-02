#pragma once
#include "include/core/SkCanvas.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"
#include "include/core/SkRect.h"
#include "include/core/SkTextBlob.h"
#include "include/utils/SkPaintFilterCanvas.h"
#include <cmath>

namespace le
{
    /// @brief A canvas that wraps a real target canvas and forwards every
    /// draw call to it unchanged, EXCEPT text - BUGS_AND_ENHANCEMENTS.md
    /// E22, terminal/route label text should always render upright, never
    /// rotated/mirrored by whatever Placement orientation (or accumulated
    /// scale/flip) is active at the point a picture's own recorded text
    /// draw replays.
    ///
    /// Why this works transparently for arbitrarily nested hierarchy:
    /// `SkPicture::playback` (what `canvas->drawPicture(picture)`
    /// eventually triggers) replays every recorded op through the TARGET
    /// canvas's own public `drawXxx()` methods, which route internally
    /// through the matching protected `onDrawXxx()` virtual - including a
    /// nested instance's own `canvas->concat(instance_transform);
    /// canvas->drawPicture(child_picture);` (BuildLayoutPictureStage::run) -
    /// so a custom canvas passed as the REPLAY target intercepts text
    /// draws at every nesting level, not just the top one, with no picture
    /// re-recording needed - PROVIDED playback is actually routed through
    /// `this` at every level, which needs one correction to
    /// `SkPaintFilterCanvas`'s own default behavior (see `onDrawPicture`
    /// below) to actually hold.
    ///
    /// The public `SkCanvas::drawPicture(picture, matrix, paint)` entry
    /// point (src/core/SkCanvas.cpp, this project's own vendored Skia
    /// checkout) only calls `picture->playback(this)` directly when
    /// `picture->approximateOpCount() <= kMaxPictureOpsToUnrollInsteadOfRef`
    /// (`SkCanvasPriv.h`, value `1`) - true only for a near-empty picture,
    /// never for real content. Otherwise it dispatches to the virtual
    /// `onDrawPicture(...)`, whose *default* `SkPaintFilterCanvas`
    /// implementation (`SkPaintFilterCanvas.cpp`) forwards straight to the
    /// one WRAPPED proxy canvas (`SkNWayCanvas::onDrawPicture` ->
    /// `fCanvas->drawPicture(...)`), not back through `this` - meaning
    /// every picture with real content (every picture in this codebase)
    /// would silently escape this class's own filtering the instant it's
    /// drawn, including every nested instance picture inside it, with no
    /// compile error and no crash - just every text draw quietly going
    /// unfiltered. `onDrawPicture` is overridden below specifically to
    /// close this - it replays `picture->playback(this)` directly instead
    /// of delegating to the base's forward-to-proxy behavior, mirroring
    /// `SkAutoCanvasMatrixPaint`'s own save/concat/restore RAII shape
    /// (`src/core/SkCanvasPriv.cpp` - a private Skia header, so
    /// reimplemented here against public `SkCanvas` methods rather than
    /// included) so a nested `drawPicture(child, matrix, paint)` call
    /// keeps replaying through this same override at every depth. Found
    /// and fixed only by tracing this project's own vendored Skia source
    /// end to end rather than assuming the doc-comment claim above without
    /// checking it - the initial implementation compiled, linked, and
    /// even benchmarked identically before/after (unsurprising in
    /// hindsight: with this bug, nothing was actually being intercepted),
    /// which is exactly the shape of bug that survives a build+benchmark
    /// pass without a real behavioral test - see this module's own tests
    /// for the pixel-sampling coverage that actually catches it.
    ///
    /// `onDrawTextBlob`, not `onDrawGlyphRunList`, is the real interception
    /// point for every text draw this class actually needs to catch - the
    /// opposite of this file's own first (wrong) assumption, caught only
    /// by a failing pixel-sampling test (see this module's own tests),
    /// not by the earlier "confirmed against vendored Skia source" claim
    /// this replaces, which checked only a LIVE, non-recording canvas's
    /// own call path: `SkCanvas::drawSimpleText` does call
    /// `this->onDrawGlyphRunList(...)` directly on such a canvas, true -
    /// but `SkPictureRecord` (the canvas type actually recording every
    /// picture in this codebase) overrides `onDrawTextBlob` only, never
    /// `onDrawGlyphRunList` (confirmed by grepping for a
    /// "DrawGlyphRunList" record kind in `SkRecords.h` - there isn't one),
    /// so a live `drawString` call made *while recording* is always
    /// converted to, and stored as, a `DrawTextBlob` record. Replaying
    /// that recorded picture later - the only way text ever reaches this
    /// class, since it exists specifically to wrap the REPLAY target, not
    /// to receive live draws - always arrives as `canvas->drawTextBlob(...)`,
    /// dispatching to `onDrawTextBlob`, never `onDrawGlyphRunList`.
    /// `SkPaintFilterCanvas::onDrawTextBlob`'s own default implementation
    /// (`SkPaintFilterCanvas.cpp`) forwards straight to
    /// `SkNWayCanvas::onDrawTextBlob` -> the wrapped proxy canvas, exactly
    /// the same "bypasses `this` entirely" pitfall `onDrawPicture` above
    /// has - it does NOT call `this->onDrawGlyphRunList(...)` the way this
    /// file originally assumed. Overriding `onDrawTextBlob` directly below
    /// (with `onDrawGlyphRunList` also overridden, identically, as a
    /// defensive second interception point for the unlikely case of a
    /// live, non-recorded draw reaching this canvas some other way) is
    /// what actually closes this.
    ///
    /// Built on `SkPaintFilterCanvas` (a proxy base already designed for
    /// "forward everything to one wrapped canvas, with hooks to
    /// intercept specific draw types") rather than hand-rolling a
    /// forward-every-onDrawXxx-method subclass - far less surface area to
    /// get wrong, and forwarding-by-default is exactly the semantics
    /// wanted for every non-text draw.
    ///
    /// The interception itself: read the CURRENT accumulated CTM
    /// (`getTotalMatrix()`) at the exact moment a glyph run would draw,
    /// decompose it into a translation (where local (0,0) currently maps
    /// to in device space - this must stay put, so the label's own
    /// position still tracks its Placement/pan/zoom exactly like every
    /// other draw) and a uniform scale magnitude
    /// (`sqrt(|det(linear_2x2)|)`), discarding the rotation/reflection
    /// component entirely - exact, not an approximation, for this
    /// codebase's own transform chain specifically: pan/zoom is always
    /// translate+uniform-scale (Scene::set_pan/set_scale, never sheared),
    /// and a Placement's own orientation (Geometry::orientation_linear) is
    /// always a pure rotation/reflection (determinant exactly +-1) - any
    /// composition of these is still translate+uniform-scale+rotation/
    /// reflection, never shear, so "uniform scale magnitude" is
    /// well-defined and exact regardless of how many ancestor instances
    /// contributed to the accumulated CTM. Replaces the CTM with a fresh
    /// translation+scale-only matrix (no rotation/reflection at all) for
    /// the duration of this one glyph run's own draw, so glyphs render
    /// upright and correctly sized/positioned, then restores.
    ///
    /// Deliberately does NOT try to preserve/interact with the existing
    /// local per-label counter-flip idiom draw_group/draw_placement_labels/
    /// draw_ruler_label already record (`canvas.translate(...);
    /// canvas.scale(1,-1); canvas.drawString(...)` - countering
    /// RasterizePictureStage's own whole-canvas Y-flip at RECORD time) -
    /// this class discards rotation/reflection unconditionally regardless
    /// of what's already baked into the CTM by the time it intercepts a
    /// given draw, so that now-redundant local counter-flip is harmless
    /// (same net result either way) but no longer strictly needed. Left
    /// in place rather than removed in this same pass - a separate,
    /// optional cleanup, not required for correctness.
    class UprightTextCanvas : public SkPaintFilterCanvas
    {
    public:
        explicit UprightTextCanvas(SkCanvas *target) : SkPaintFilterCanvas(target) {}

        // Declared (not defined) here so this is the class's only
        // out-of-line virtual - its "key function" - and the vtable/
        // typeinfo are emitted exactly once, in upright_text_canvas.cpp,
        // instead of vaguely-linked in every TU that constructs one of
        // these. That one TU is compiled with -fno-rtti (CMakeLists.txt)
        // to match this vendored Skia build: SkPaintFilterCanvas/SkCanvas
        // have vtables but no `typeinfo for` symbol anywhere in
        // libskia.a (confirmed via `nm` - Skia's own official build
        // disables RTTI for its core classes), so a normal
        // RTTI-enabled derived class fails to link ("Undefined symbols:
        // typeinfo for SkPaintFilterCanvas, referenced from: typeinfo
        // for le::UprightTextCanvas") the moment any TU actually
        // instantiates it - -fno-rtti here means no typeinfo is ever
        // attempted for either class, matching the base's own ABI.
        ~UprightTextCanvas() override;

    protected:
        // No paint filtering needed - onDrawTextBlob/onDrawGlyphRunList
        // below are overridden for real; this pure virtual still needs a
        // body.
        bool onFilter(SkPaint &) const override { return true; }

        // Replaces the CTM with a translation+uniform-scale-only matrix
        // (see this class's own doc comment for why that decomposition is
        // exact, not approximate, for this codebase) for the duration of
        // one call to `draw_text`, so whatever it draws renders upright
        // and correctly sized/positioned regardless of any rotation/
        // reflection baked into the CTM by an enclosing instance
        // transform, then restores. Shared by both onDrawTextBlob (the
        // real interception point - see this class's own doc comment) and
        // onDrawGlyphRunList (a defensive second one, for a live draw
        // that reaches this canvas some other way than picture replay).
        template <typename DrawTextFn>
        void with_upright_matrix(DrawTextFn &&draw_text)
        {
            const SkMatrix ctm = this->getTotalMatrix();
            const SkPoint origin_device = ctm.mapOrigin();
            const double det = static_cast<double>(ctm.getScaleX()) * static_cast<double>(ctm.getScaleY()) -
                                static_cast<double>(ctm.getSkewX()) * static_cast<double>(ctm.getSkewY());
            const double scale = std::sqrt(std::abs(det));

            this->save();
            SkMatrix upright = SkMatrix::Scale(static_cast<SkScalar>(scale), static_cast<SkScalar>(scale));
            upright.postTranslate(origin_device.x(), origin_device.y());
            this->setMatrix(upright);
            draw_text();
            this->restore();
        }

        void onDrawTextBlob(const SkTextBlob *blob, SkScalar x, SkScalar y, const SkPaint &paint) override
        {
            with_upright_matrix([&]
                                 { this->SkPaintFilterCanvas::onDrawTextBlob(blob, x, y, paint); });
        }

        void onDrawGlyphRunList(const sktext::GlyphRunList &glyph_run_list, const SkPaint &paint) override
        {
            with_upright_matrix([&]
                                 { this->SkPaintFilterCanvas::onDrawGlyphRunList(glyph_run_list, paint); });
        }

        // See this class's own doc comment above for why this override is
        // required (not optional/defensive) - without it, every picture
        // with real content bypasses this class's filtering entirely,
        // including onDrawTextBlob/onDrawGlyphRunList above. Mirrors
        // SkAutoCanvasMatrixPaint's own save/concat/restore shape
        // (SkCanvasPriv.cpp) against public SkCanvas methods only.
        void onDrawPicture(const SkPicture *picture, const SkMatrix *matrix, const SkPaint *paint) override
        {
            const int save_count = this->getSaveCount();
            if (paint)
            {
                SkRect bounds = picture->cullRect();
                if (matrix)
                    matrix->mapRect(&bounds);
                this->saveLayer(&bounds, paint);
            }
            else if (matrix)
            {
                this->save();
            }
            if (matrix)
                this->concat(*matrix);
            picture->playback(this);
            this->restoreToCount(save_count);
        }
    };
}
