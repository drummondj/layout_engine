#pragma once
#include "../../database/database.hpp"
#include "../draw_helpers.hpp"
#include "../pixel_types.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "include/core/SkBBHFactory.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <map>
#include <vector>

namespace le
{
    /// @brief oneTBB port of src/render/stages/build_abstract_picture_stage.hpp
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - records
    /// the pixel-space shapes into an SkPicture, sized to the Scene's
    /// viewport, drawn on top of the background grid + axis lines and the
    /// current Abstract's own origin marker. compute()'s own body is
    /// unchanged from the original stage's run() lambda - see that file's
    /// own doc comment for the full drawing-order rationale.
    ///
    /// Records with an SkRTreeFactory-backed bounding box hierarchy
    /// (BUGS_AND_ENHANCEMENTS.md E25) - this picture is the one
    /// RasterizeComposePipeline's design_rasterize_ tile-splits across
    /// several row-band clips every rasterize (TiledRasterizePictureStage),
    /// and a BBH lets playback skip recorded ops outside each tile's own
    /// clip instead of walking/clip-testing all of them per tile.
    /// Benchmarked (BM_TiledRasterizePlayback_NoBBH vs. _WithRTree,
    /// BM_RecordPicture_NoBBH vs. _WithRTree, see BENCHMARKS.md): ~2.7x
    /// faster tiled playback against the 1,000,000-shape stress fixture,
    /// for ~2% extra one-time recording cost - a clear win since playback
    /// happens far more often (every pan/zoom/rasterize) than recording
    /// (every real content change).
    ///
    /// Wired downstream of PixelTransformStage via make_edge (within
    /// DesignRenderPipeline's own graph) - its own data_version arrives
    /// automatically as PixelTransformStage's own version(), matching the
    /// original stage's key composing via `upstream TransformToPixelsStage's
    /// version()`.
    class BuildDesignPictureStage : public MemoizingStage<std::map<ViewLayerId, std::vector<PixelShape>>, sk_sp<SkPicture>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

    protected:
        sk_sp<SkPicture> compute(const std::map<ViewLayerId, std::vector<PixelShape>> &shapes, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            const ViewLayerSet &view_layers = *options.ctx.view_layers;
            const Root &root = *options.ctx.root;

            SkRTreeFactory rtree_factory;
            SkPictureRecorder recorder;
            SkCanvas *canvas = recorder.beginRecording(
                SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())), &rtree_factory);

            // Grid first, so it sits underneath the real design geometry
            // drawn below rather than obscuring it.
            draw_grid(*canvas, scene);

            if (const AbstractData *abstract = root.get_abstract(scene.current_abstract()))
                draw_origin_marker(*canvas, scene, abstract->origin.value_or(Point{}));

            // Cancels the tiled fill pattern's own pan-swim - see
            // draw_group's own doc comment (BUGS_AND_ENHANCEMENTS.md B1)
            // for why this multiplication is needed at all.
            const SkPoint pattern_phase_px = SkPoint::Make(
                static_cast<SkScalar>(scene.pan().x * scene.scale()),
                static_cast<SkScalar>(scene.pan().y * scene.scale()));
            draw_shape_groups(*canvas, shapes, view_layers, scene.antialiasing_enabled(), pattern_phase_px);

            return recorder.finishRecordingAsPicture();
        }
    };
}
