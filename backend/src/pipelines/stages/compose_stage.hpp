#pragma once
#include "../../render/pixel_types.hpp"
#include "../../scene/scene.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "../version_utils.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPicture.h"
#include "include/core/SkSurface.h"
#include <utility>

namespace le
{
    /// @brief oneTBB port of src/render/stages/compose_with_overlays_stage.hpp
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - composites
    /// the design/tiny-shapes/selection/ruler frames' already-rasterized
    /// pixels with the mouse overlay picture into one final RGBA8888
    /// buffer. compute()'s own body is unchanged from the original stage's
    /// run() lambda.
    ///
    /// The final 5-way merge across DesignRenderPipeline/
    /// MouseTargetLayerPipeline/SelectionGhostLayerPipeline's own outputs -
    /// deliberately plain code (FrameRenderPipeline calls
    /// data_version_for/ComposeInput itself), not FanInCollectStage: the
    /// arity here is fixed and known at compile time (five specific
    /// pictures/frames, not a runtime-determined layer count), so
    /// FanInCollectStage's spin-mutex/accumulator machinery would be pure
    /// overhead for no benefit (see the migration plan's own Threading
    /// model section, decision 4).
    struct ComposeInput
    {
        RasterizedFrame design_frame;
        RasterizedFrame tiny_shapes_frame;
        RasterizedFrame selection_frame;
        RasterizedFrame ruler_frame;
        sk_sp<SkPicture> overlay_picture;
    };

    class ComposeStage : public MemoizingStage<ComposeInput, RasterizedFrame, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

        /// @brief The original stage's key was `{design_rasterizer.version(),
        /// tiny_rasterizer.version(), selection_rasterizer.version(),
        /// ruler_rasterizer.version(), overlay_stage.version()}` -
        /// FrameRenderPipeline calls this with each of those five upstream
        /// stages' own output data_version (the StageData each one last
        /// emitted) to derive this stage's own.
        static uint64_t data_version_for(uint64_t design_version, uint64_t tiny_shapes_version, uint64_t selection_version, uint64_t ruler_version, uint64_t overlay_version)
        {
            return combine_versions(design_version, tiny_shapes_version, selection_version, ruler_version, overlay_version);
        }

    protected:
        RasterizedFrame compute(const ComposeInput &in, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            const int width = scene.viewport_width_px();
            const int height = scene.viewport_height_px();
            if (width <= 0 || height <= 0 || !in.design_frame.surface || !in.tiny_shapes_frame.surface || !in.selection_frame.surface || !in.ruler_frame.surface)
                return RasterizedFrame{};

            const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
            sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
            SkCanvas *canvas = surface->getCanvas();
            canvas->clear(SK_ColorTRANSPARENT);

            // Cheap blits of the already-rasterized design/tiny-shapes/
            // selection/ruler pixels - no re-walk of any of their
            // underlying (potentially large) draw commands. All four
            // source surfaces already have their own Y-flip baked in, so
            // no further transform is needed for these.
            canvas->drawImage(in.design_frame.surface->makeImageSnapshot(), 0, 0);
            canvas->drawImage(in.tiny_shapes_frame.surface->makeImageSnapshot(), 0, 0);
            canvas->drawImage(in.selection_frame.surface->makeImageSnapshot(), 0, 0);
            canvas->drawImage(in.ruler_frame.surface->makeImageSnapshot(), 0, 0);

            canvas->translate(0, static_cast<SkScalar>(height));
            canvas->scale(1, -1);
            canvas->drawPicture(in.overlay_picture);

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
