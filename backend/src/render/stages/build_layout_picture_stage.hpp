#pragma once
#include "../draw_helpers.hpp"
#include "../pixel_types.hpp"
#include "../../view_style/view_style.hpp"
#include "include/core/SkCanvas.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <map>
#include <vector>

namespace le
{
    /// @brief Records a Layout's own already-resolved, local-pixel-space
    /// direct content (Migration Step 3 Phase A's own shapes, transformed
    /// with no pan - see InstanceRenderer's own class comment for why)
    /// into an SkPicture, then draws each of `instances` through its own
    /// SkMatrix on top - the recursive child-instance
    /// canvas->concat()+drawPicture() work the Step 3 plan's own Naming
    /// section called for. `Geometry::instance_transform` +
    /// `to_instance_matrix` (draw_helpers.hpp) are what a caller uses to
    /// build each instance's own `ResolvedInstance::transform`.
    ///
    /// No origin marker (unlike BuildAbstractPictureStage) - Layout has no
    /// such field - and deliberately no grid/axis lines either: those are
    /// current-view-only chrome, wrong to bake into a picture meant to be
    /// cached and replayed at arbitrary positions elsewhere in the
    /// hierarchy (see BuildAbstractPictureStage's own updated doc comment
    /// for the same reasoning from its side).
    ///
    /// `tiny_instance_rects` - already-scaled local-pixel-space rects, one
    /// per Placement whose own transformed world bbox is sub-pixel at the
    /// caller's scale (see InstanceRenderer::build_layout_picture_uncached's
    /// own comment for why: recursing into/recording a full picture for an
    /// instance nobody could see anyway is real, avoidable per-instance
    /// cost at 1,000,000-placement scale). Drawn as an outline only (no
    /// fill, no internal content) in the BOUNDARY ViewLayer's own
    /// outline_color - a single collapsed-to-a-point dot would lose the
    /// instance's own real size/position once the threshold is set well
    /// above 1px (MIGRATION_REVIEW.md item 2's own follow-up: "what I
    /// really need is the boundary drawn and nothing else"), where its own
    /// rect outline still shows both, degrading gracefully to a hairline
    /// dot-like mark only once truly sub-pixel. Batched into one SkPath
    /// (addRect per instance) and stroked with a single drawPath call,
    /// same "one draw call regardless of instance count" reasoning
    /// drawPoints's own batching had - Skia has no direct batched
    /// multi-rect stroke API, so a path is the next-cheapest equivalent.
    ///
    /// Deliberately NOT VersionedStage-cached, unlike every sibling stage
    /// in this directory - a real, reasoned deviation, not an omission:
    /// caching here would need to be keyed per-{LayoutId, remaining_depth},
    /// but InstanceRenderer's own {DesignId/LayoutId, remaining_depth}
    /// picture cache (src/instancing/) already provides exactly that reuse
    /// one level up, and this class is only ever invoked once per
    /// InstanceRenderer cache miss - an inner single-slot cache here would
    /// just be evicted by the very next distinct Layout the recursive walk
    /// visits, providing zero benefit while adding a VersionedStage member
    /// nothing would ever hit twice.
    class BuildLayoutPictureStage
    {
    public:
        struct ResolvedInstance
        {
            SkMatrix transform;
            sk_sp<SkPicture> picture; // empty/null if unresolved - skipped, not drawn
        };

        static sk_sp<SkPicture> run(SkRect bounds, const std::map<ViewLayerId, std::vector<PixelShape>> &own_shapes, const std::vector<ResolvedInstance> &instances, const std::vector<PixelRect> &tiny_instance_rects, const ViewLayerSet &view_layers)
        {
            SkPictureRecorder recorder;
            SkCanvas *canvas = recorder.beginRecording(bounds);

            draw_shape_groups(*canvas, own_shapes, view_layers);

            for (const ResolvedInstance &instance : instances)
            {
                if (!instance.picture)
                    continue;

                canvas->save();
                canvas->concat(instance.transform);
                canvas->drawPicture(instance.picture);
                canvas->restore();
            }

            if (!tiny_instance_rects.empty())
            {
                const ViewLayerData *boundary_view_layer = view_layers.get(view_layers.boundary_view_layer());
                if (boundary_view_layer && boundary_view_layer->style.outline_color.a != 0)
                {
                    SkPaint paint;
                    paint.setAntiAlias(true);
                    paint.setStyle(SkPaint::kStroke_Style); // outline only, no fill, no internal content
                    paint.setColor(to_sk_color(boundary_view_layer->style.outline_color));

                    SkPathBuilder builder;
                    for (const PixelRect &r : tiny_instance_rects)
                        builder.addRect(SkRect::MakeLTRB(static_cast<SkScalar>(r.ll.x), static_cast<SkScalar>(r.ll.y), static_cast<SkScalar>(r.ur.x), static_cast<SkScalar>(r.ur.y)));

                    canvas->drawPath(builder.detach(), paint);
                }
            }

            return recorder.finishRecordingAsPicture();
        }
    };
}
