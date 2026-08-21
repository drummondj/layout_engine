#pragma once
#include "transform_tiny_shapes_to_pixels_stage.hpp"
#include "../draw_helpers.hpp"
#include "../pixel_types.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include <map>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Records the tiny-shapes dot picture (UPDATES.md item 6) -
    /// one batched SkCanvas::drawPoints call per ViewLayer group,
    /// hairline stroke width so each point rasterizes as exactly one
    /// device pixel. A group is skipped entirely if its ViewLayer's
    /// outline_color is fully transparent, matching draw_group's own
    /// has_outline convention - no color to draw a dot in. Deliberately a
    /// separate SkPicture from BuildPictureStage's, not folded into it or
    /// drawn via the existing PixelShape/RenderedShape plumbing - see
    /// TinyShapeDot's own comment: this keeps Pipeline::hit_test_point/
    /// hit_test_rect provably unaware of tiny shapes, so "not selectable"
    /// holds by construction, not by an exclusion check.
    ///
    /// Key: `{upstream TransformTinyShapesToPixelsStage's version()}` -
    /// sibling of BuildPictureStage, same composition reasoning.
    class BuildTinyShapesPictureStage
    {
    public:
        const sk_sp<SkPicture> &run(TransformTinyShapesToPixelsStage &upstream, const Root &root, const std::map<ViewLayerId, std::vector<PixelPoint>> &tiny_pixel_shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            (void)root;
            const auto key = std::tuple{upstream.version()};
            return stage_.get(key, [&]
            {
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
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t>, sk_sp<SkPicture>> stage_;
    };
}
