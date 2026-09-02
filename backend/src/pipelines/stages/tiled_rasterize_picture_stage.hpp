#pragma once
#include "../pixel_types.hpp"
#include "../../scene/scene.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "../upright_text_canvas.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPicture.h"
#include "include/core/SkSurface.h"
#include <algorithm>
#include <utility>
#include <oneapi/tbb.h>

namespace le
{
    /// @brief Same sk_sp<SkPicture> -> RasterizedFrame contract as
    /// RasterizePictureStage, but rasterizes in parallel by splitting the
    /// destination viewport into row-band tiles rather than splitting the
    /// picture's own content - see RasterizeComposePipeline's own doc
    /// comment for why this is the axis that actually helps both the
    /// Abstract-view and Layout-view paths. A per-ViewLayerId content
    /// split (tried previously as BuildParallelDesignPictureStage/
    /// MultipleRasterizePictureStage/ComposePictures, since removed) only
    /// ever reached the Abstract-view path - a Layout view's design
    /// picture is one already-composited SkPicture (grid + cached
    /// hierarchy-node pictures merged through instance transforms) with
    /// no per-layer structure left to split by the time it reaches a
    /// rasterize stage. Splitting the OUTPUT space instead works
    /// regardless of how the picture's own content is organized.
    ///
    /// Each tile is a separate SkSurface wrapping (via
    /// SkSurfaces::WrapPixels - no allocation, no copy) a disjoint
    /// row-range of the SAME backing pixel buffer the final surface owns,
    /// so no merge/blit step is needed afterward - unlike ComposeStage's
    /// own N-frame blend, tiles never overlap so there's nothing to
    /// composite, just independent memory ranges each written by exactly
    /// one task. SkPicture::playback (what drawPicture triggers) is safe
    /// to call concurrently from multiple threads against independent
    /// canvases, per Skia's own documented thread-safety contract for a
    /// single immutable SkPicture read from multiple threads.
    ///
    /// Used for RasterizeComposePipeline's own design_rasterize_ only -
    /// the content-heavy slot Tracy identified as the actual bottleneck;
    /// the tiny/selection/ruler slots stay on plain RasterizePictureStage,
    /// where per-tile SkSurface/task overhead would outweigh the benefit
    /// on much lighter content.
    class TiledRasterizePictureStage : public MemoizingStage<sk_sp<SkPicture>, RasterizedFrame, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

    protected:
        RasterizedFrame compute(const sk_sp<SkPicture> &picture, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            const int width = scene.viewport_width_px();
            const int height = scene.viewport_height_px();

            if (width <= 0 || height <= 0)
                return RasterizedFrame{.empty = true};

            const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
            sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
            surface->getCanvas()->clear(SK_ColorTRANSPARENT);

            SkPixmap pixmap;
            surface->peekPixels(&pixmap);

            const int tile_count = std::clamp(static_cast<int>(tbb::info::default_concurrency()), 1, height);
            const int rows_per_tile = (height + tile_count - 1) / tile_count;

            tbb::parallel_for(0, tile_count, [&](int i)
                               {
                                   ZoneScopedN("TiledRasterizePictureStage: rasterize_tile");
                                   const int y0 = i * rows_per_tile;
                                   const int y1 = std::min(height, y0 + rows_per_tile);
                                   if (y0 >= y1)
                                       return;

                                   const SkImageInfo tile_info = SkImageInfo::Make(width, y1 - y0, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
                                   sk_sp<SkSurface> tile_surface = SkSurfaces::WrapPixels(tile_info, pixmap.writable_addr(0, y0), pixmap.rowBytes());

                                   // Same Y-flip as RasterizePictureStage's own untiled draw,
                                   // just re-anchored so this tile's own row range lands
                                   // correctly: a picture point at picture-y=py belongs at
                                   // full-surface screen row (height - py), i.e. at this
                                   // tile's own row ((height - py) - y0) = ((height - y0) -
                                   // py) - translating by (height - y0) instead of the full
                                   // height reproduces that offset within the tile's own
                                   // [0, y1-y0) row range.
                                   // BUGS_AND_ENHANCEMENTS.md E22 - wraps
                                   // the real tile canvas so every text
                                   // draw this picture's own replay
                                   // reaches (at any nesting depth -
                                   // UprightTextCanvas's own doc comment
                                   // for why that works transparently)
                                   // renders upright, regardless of
                                   // whatever Placement orientation or
                                   // accumulated flip is active at that
                                   // point. UprightTextCanvas forwards
                                   // every non-text draw straight through
                                   // to tile_canvas unchanged.
                                   SkCanvas *tile_canvas = tile_surface->getCanvas();
                                   UprightTextCanvas upright_canvas(tile_canvas);
                                   upright_canvas.translate(0, static_cast<SkScalar>(height - y0));
                                   upright_canvas.scale(1, -1);
                                   upright_canvas.drawPicture(picture);
                               });

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
