#pragma once
#include "../../render/draw_helpers.hpp"
#include "../../render/pixel_types.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include <map>
#include <vector>

namespace le
{
    /// @brief oneTBB port of src/render/stages/build_tiny_shapes_picture_stage.hpp
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - records
    /// the tiny-shapes dot picture, one batched SkCanvas::drawPoints call
    /// per ViewLayer group, hairline stroke so each point rasterizes as
    /// exactly one device pixel. compute()'s own body is unchanged from
    /// the original stage's run() lambda.
    ///
    /// Wired downstream of TinyPixelTransformStage via make_edge - its own
    /// data_version arrives as that stage's version().
    class BuildTinyDotsPictureStage : public MemoizingStage<std::map<ViewLayerId, std::vector<PixelPoint>>, sk_sp<SkPicture>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

    protected:
        sk_sp<SkPicture> compute(const std::map<ViewLayerId, std::vector<PixelPoint>> &tiny_pixel_shapes, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            const ViewLayerSet &view_layers = *options.ctx.view_layers;

            SkPictureRecorder recorder;
            SkCanvas *canvas = recorder.beginRecording(
                SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

            for (const auto &[view_layer_id, group] : tiny_pixel_shapes)
            {
                if (group.empty())
                    continue;

                const ViewLayerData *view_layer = view_layers.get(view_layer_id);
                if (!view_layer || view_layer->style.outline_color.a == 0)
                    continue;

                SkPaint paint;
                paint.setAntiAlias(false);
                paint.setStrokeWidth(0); // hairline - exactly one device pixel per point
                paint.setColor(to_sk_color(view_layer->style.outline_color));

                std::vector<SkPoint> points;
                points.reserve(group.size());
                for (const auto &p : group)
                    points.push_back(SkPoint::Make(static_cast<SkScalar>(p.x), static_cast<SkScalar>(p.y)));

                canvas->drawPoints(SkCanvas::kPoints_PointMode, {points.data(), points.size()}, paint);
            }

            return recorder.finishRecordingAsPicture();
        }
    };
}
