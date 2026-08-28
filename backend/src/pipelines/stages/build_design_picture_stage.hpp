#pragma once
#include "../../database/database.hpp"
#include "../../render/draw_helpers.hpp"
#include "../../render/pixel_types.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
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

            SkPictureRecorder recorder;
            SkCanvas *canvas = recorder.beginRecording(
                SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

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
            draw_shape_groups(*canvas, shapes, view_layers, pattern_phase_px);

            return recorder.finishRecordingAsPicture();
        }
    };
}
