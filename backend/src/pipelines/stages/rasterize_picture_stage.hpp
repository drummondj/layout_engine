#pragma once
#include "../pixel_types.hpp"
#include "../../scene/scene.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPicture.h"
#include "include/core/SkSurface.h"
#include <utility>

namespace le
{
    /// @brief oneTBB port of src/render/stages/rasterize_stage.hpp (Phase 3,
    /// backend/ONETBB_INTEGRATION.md migration plan) - rasterizes an
    /// SkPicture into a raw RGBA8888 PixelBuffer, applying the Y-flip that
    /// corrects dbu-space's "y increases upward" convention to Skia's
    /// y-down canvas. compute()'s own body is unchanged from the original
    /// stage's run() lambda.
    ///
    /// Renamed from the original's own `RasterizeStage` (kept unique from
    /// that still-live class during the migration's coexistence period,
    /// per this module's own established naming convention - see e.g.
    /// AbstractGeometryStage vs. GenerateAbstractShapesStage). Instantiated
    /// multiple times per pipeline (design/tiny-shapes/selection/ruler),
    /// same as the original - each instance wired downstream of its own
    /// picture-building stage via make_edge, so its own data_version
    /// arrives as that stage's version(), matching the original's
    /// `{upstream_version}` key.
    class RasterizePictureStage : public MemoizingStage<sk_sp<SkPicture>, RasterizedFrame, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

    protected:
        RasterizedFrame compute(const sk_sp<SkPicture> &picture, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            const int width = scene.viewport_width_px();
            const int height = scene.viewport_height_px();

            // SkSurfaces::Raster returns null for non-positive dimensions
            // (Scene's default-constructed viewport size, or simply not
            // having called set_viewport_size() yet) - an empty (all-null/
            // zero) PixelBuffer rather than a null surface->getCanvas()
            // dereference, matching this project's "degrade gracefully
            // rather than crash on unset/invalid state" convention.
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
        }
    };
}
