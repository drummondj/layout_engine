#pragma once
#include "transform_to_pixels_stage.hpp"
#include "../draw_helpers.hpp"
#include "../pixel_types.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <map>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Record the pixel-space shapes into an SkPicture, sized to
    /// the Scene's viewport, drawn in map order (bottom-up, see
    /// Pipeline::filter_by_layer_visibility's comment) on top of the
    /// background dot grid + axis lines (draw_grid) drawn first, and the
    /// currently displayed Abstract's own origin marker (draw_origin_marker,
    /// UPDATES.md 5.4) drawn just after - `root` is only needed to look up
    /// that Abstract's AbstractData::origin; skipped entirely if
    /// scene.current_abstract() doesn't resolve (no Design selected). Each
    /// group's ViewLayerStyle (outline/fill Color) comes from `view_layers`;
    /// a ViewLayerId that doesn't resolve to a known ViewLayer (see
    /// Pipeline's layer filter comment on why that's kept, not dropped) has
    /// its whole group skipped here - there's no style to draw it with.
    ///
    /// Deliberately has no selection-dependent content: piece-level
    /// selection outlines are drawn by BuildSelectionOverlayPictureStage
    /// instead (see its own doc comment for the history of why that split
    /// exists).
    ///
    /// Renamed from BuildPictureStage (Migration Step 3 Phase B) once a
    /// Layout-side sibling (BuildLayoutPictureStage) existed too - this
    /// class's own Abstract-specific behavior isn't just a name, it really
    /// does look up `root.get_abstract(scene.current_abstract())` for the
    /// origin marker, unlike the four filter/tiny-shapes pipeline stages
    /// that turned out not to need a rename at all (see the Step 3 plan's
    /// own Naming section). Its shared shape-drawing core is factored out
    /// as draw_shape_groups (draw_helpers.hpp) so BuildLayoutPictureStage
    /// can reuse it without inheriting this class's own grid/origin-marker
    /// chrome, which is current-view-only and wrong to bake into a picture
    /// meant to be cached and replayed at arbitrary positions elsewhere in
    /// a hierarchy.
    ///
    /// Key: `{upstream TransformToPixelsStage's version()}` - composes via
    /// the upstream stage's own version (see VersionedStage's own doc
    /// comment) instead of independently re-deriving AbstractId/
    /// viewport_version/visibility_version/root.mutation_version() -
    /// TransformToPixelsStage's own key already covers exactly the same
    /// triggers this stage would otherwise need to re-list by hand.
    class BuildAbstractPictureStage
    {
    public:
        const sk_sp<SkPicture> &run(TransformToPixelsStage &upstream, const std::map<ViewLayerId, std::vector<PixelShape>> &shapes, const Scene &scene, const ViewLayerSet &view_layers, const Root &root)
        {
            const auto key = std::tuple{upstream.version()};
            return stage_.get(key, [&]
            {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                // Grid first, so it sits underneath the real design
                // geometry drawn below rather than obscuring it.
                draw_grid(*canvas, scene);

                if (const AbstractData *abstract = root.get_abstract(scene.current_abstract()))
                    draw_origin_marker(*canvas, scene, abstract->origin.value_or(Point{}));

                // Cancels the tiled fill pattern's own pan-swim - see
                // draw_group's own doc comment (BUGS_AND_ENHANCEMENTS.md
                // B1) for why this multiplication is needed at all.
                const SkPoint pattern_phase_px = SkPoint::Make(
                    static_cast<SkScalar>(scene.pan().x * scene.scale()),
                    static_cast<SkScalar>(scene.pan().y * scene.scale()));
                draw_shape_groups(*canvas, shapes, view_layers, pattern_phase_px);

                return recorder.finishRecordingAsPicture();
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t>, sk_sp<SkPicture>> stage_;
    };
}
