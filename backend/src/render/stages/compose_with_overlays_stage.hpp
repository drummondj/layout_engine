#pragma once
#include "build_overlay_picture_stage.hpp"
#include "rasterize_stage.hpp"
#include "../pixel_types.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../scene/scene.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPicture.h"
#include "include/core/SkSurface.h"
#include <tuple>
#include <utility>

namespace le
{
    /// @brief Composites the design frame's cached, already-rasterized
    /// pixels (from a RasterizeStage instance - the expensive step for a
    /// large design) with the tiny-shapes dot frame (see
    /// BuildTinyShapesPictureStage, UPDATES.md item 6), the finalized-ruler
    /// frame (UPDATES.md item 13), and the two cheap overlay pictures
    /// (mouse chrome + selection outline) into one final RGBA8888
    /// buffer, without re-rasterizing the design's own vector content on
    /// every call. Reuses the design's/tiny-shapes'/selection's/rulers'
    /// cached raster surfaces via a cheap SkImage snapshot + blit
    /// (`SkCanvas::drawImage`, a bitmap copy - not a re-walk of either
    /// one's draw ops). The mouse overlay picture is drawn through the
    /// same whole-canvas Y-flip applied to the design (translate +
    /// scale(1,-1)), since it records pre-flip pixel-space coordinates,
    /// the same convention BuildPictureStage's own shapes use.
    ///
    /// Finalized rulers are rasterized (like selection), not drawn
    /// directly like the mouse overlay picture - same reasoning as
    /// BuildSelectionOverlayPictureStage's own split: potentially many
    /// rulers with many segments/ticks each is real, non-trivial replay
    /// cost, so it's rasterized once per ruler_version() change and
    /// blitted every frame instead. Blitted after selection (rulers are
    /// design-adjacent annotation, not chrome) and before the live mouse
    /// overlay - drawn last of all - so the ghost segment (part of
    /// overlay_picture) always renders on top of every finalized ruler,
    /// never occluded while a new point is being placed.
    ///
    /// Key: `{design_rasterizer.version(), tiny_rasterizer.version(),
    /// selection_rasterizer.version(), ruler_rasterizer.version(),
    /// overlay_stage.version()}` - composes via all five upstream stages'
    /// own versions instead of hand-copying their individual triggers -
    /// every real caller (traced exhaustively: api.cpp, render_preview.cpp,
    /// every relevant benchmark) rebuilds every picture fresh, in order,
    /// on the same Renderer instance immediately before every call here,
    /// so composing is strictly correct, not just shorter - and any
    /// *future* new trigger added to any upstream stage propagates
    /// automatically instead of needing another manual edit to this key.
    ///
    /// The four RasterizeStage upstreams are `run()` unconditionally here
    /// (not only inside a cache-miss lambda, unlike the pre-refactor
    /// version) so their versions are guaranteed current before this
    /// stage's own key is built - mirrors Pipeline's TinyShapesByViewportStage
    /// calling its own upstream unconditionally. `overlay_stage`'s
    /// `.version()` is read without re-running it - every real caller
    /// already calls BuildOverlayPictureStage fresh immediately before this
    /// stage, confirmed via full trace of every call site.
    class ComposeWithOverlaysStage
    {
    public:
        const PixelBuffer &run(RasterizeStage &design_rasterizer, RasterizeStage &tiny_rasterizer, RasterizeStage &selection_rasterizer, RasterizeStage &ruler_rasterizer, BuildOverlayPictureStage &overlay_stage,
                                uint64_t design_upstream_version, uint64_t tiny_upstream_version, uint64_t selection_upstream_version, uint64_t ruler_upstream_version,
                                const sk_sp<SkPicture> &design_picture, const sk_sp<SkPicture> &tiny_shapes_picture, const sk_sp<SkPicture> &overlay_picture, const sk_sp<SkPicture> &selection_overlay_picture, const sk_sp<SkPicture> &ruler_overlay_picture,
                                const Scene &scene)
        {
            const RasterizedFrame &design_frame = design_rasterizer.run(design_upstream_version, design_picture, scene);
            const RasterizedFrame &tiny_shapes_frame = tiny_rasterizer.run(tiny_upstream_version, tiny_shapes_picture, scene);
            const RasterizedFrame &selection_frame = selection_rasterizer.run(selection_upstream_version, selection_overlay_picture, scene);
            const RasterizedFrame &ruler_frame = ruler_rasterizer.run(ruler_upstream_version, ruler_overlay_picture, scene);

            const auto key = std::tuple{design_rasterizer.version(), tiny_rasterizer.version(), selection_rasterizer.version(), ruler_rasterizer.version(), overlay_stage.version()};
            return stage_.get(key, [&]
            {
                const int width = scene.viewport_width_px();
                const int height = scene.viewport_height_px();
                if (width <= 0 || height <= 0 || !design_frame.surface || !tiny_shapes_frame.surface || !selection_frame.surface || !ruler_frame.surface)
                    return RasterizedFrame{};

                const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
                sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
                SkCanvas *canvas = surface->getCanvas();
                canvas->clear(SK_ColorTRANSPARENT);

                // Cheap blits of the already-rasterized design/tiny-shapes/
                // selection/ruler pixels - no re-walk of any of their
                // underlying (potentially large) draw commands. All four
                // source surfaces already have their own Y-flip baked in,
                // so no further transform is needed for these. Tiny
                // shapes are design content (UPDATES.md item 6 - they
                // stand in for shapes too small to otherwise render), so
                // they're blitted right after the design itself and
                // before the selection/ruler/mouse overlay chrome on top.
                canvas->drawImage(design_frame.surface->makeImageSnapshot(), 0, 0);
                canvas->drawImage(tiny_shapes_frame.surface->makeImageSnapshot(), 0, 0);
                canvas->drawImage(selection_frame.surface->makeImageSnapshot(), 0, 0);
                canvas->drawImage(ruler_frame.surface->makeImageSnapshot(), 0, 0);

                canvas->translate(0, static_cast<SkScalar>(height));
                canvas->scale(1, -1);
                canvas->drawPicture(overlay_picture);

                SkPixmap pixmap;
                surface->peekPixels(&pixmap);

                return RasterizedFrame{
                    .surface = std::move(surface),
                    .buffer = PixelBuffer{
                        .data = static_cast<const uint8_t *>(pixmap.addr()),
                        .width = width,
                        .height = height,
                        .row_bytes = pixmap.rowBytes(),
                    },
                };
            }).buffer;
        }

        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t, uint64_t>, RasterizedFrame> stage_;
    };
}
