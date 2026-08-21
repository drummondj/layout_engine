#pragma once
#include "../pixel_types.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../scene/scene.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPicture.h"
#include "include/core/SkSurface.h"
#include <cstdint>
#include <tuple>
#include <utility>

namespace le
{
    /// @brief Rasterizes an SkPicture into a raw RGBA8888 PixelBuffer,
    /// applying the Y-flip that corrects dbu-space's "y increases upward"
    /// convention to Skia's y-down canvas (see RasterizedFrame's own doc
    /// comment for the full byte-format/Y-flip rationale). The one generic
    /// reusable transform in the render module (UPDATES.md item 16 point
    /// 4): the design/tiny-shapes/selection-overlay frames all use this
    /// exact same procedure - build an RGBA8888 surface, Y-flip, draw, peek
    /// pixels - differing only in *which* picture is drawn and which
    /// upstream stage's version() feeds this stage's key. Renderer owns
    /// three independent instances (one per picture) rather than sharing
    /// one, since VersionedStage is single-slot - reusing one instance
    /// across three different pictures would have each one's rasterization
    /// evict the other's on every alternating call.
    ///
    /// Key: `{upstream_version}` - the caller passes in whichever upstream
    /// build-picture stage's own `.version()` applies to this instance
    /// (BuildPictureStage for the design frame, BuildTinyShapesPictureStage
    /// for the tiny-shapes frame, BuildSelectionOverlayPictureStage for the
    /// selection-overlay frame) - composing via that stage's version
    /// instead of re-deriving its triggers by hand.
    class RasterizeStage
    {
    public:
        const RasterizedFrame &run(uint64_t upstream_version, const sk_sp<SkPicture> &picture, const Scene &scene)
        {
            const auto key = std::tuple{upstream_version};
            return stage_.get(key, [&]
            {
                const int width = scene.viewport_width_px();
                const int height = scene.viewport_height_px();

                // SkSurfaces::Raster returns null for non-positive
                // dimensions (Scene's default-constructed viewport size,
                // or simply not having called set_viewport_size() yet) -
                // an empty (all-null/zero) PixelBuffer rather than a null
                // surface->getCanvas() dereference, matching this
                // project's "degrade gracefully rather than crash on
                // unset/invalid state" convention elsewhere.
                if (width <= 0 || height <= 0)
                    return RasterizedFrame{};

                const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
                sk_sp<SkSurface> surface = SkSurfaces::Raster(info);

                SkCanvas *canvas = surface->getCanvas();
                canvas->clear(SK_ColorTRANSPARENT);
                canvas->translate(0, static_cast<SkScalar>(height));
                canvas->scale(1, -1);
                canvas->drawPicture(picture);

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
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t>, RasterizedFrame> stage_;
    };
}
