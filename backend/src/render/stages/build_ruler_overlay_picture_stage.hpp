#pragma once
#include "../draw_helpers.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../scene/scene.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <optional>
#include <tuple>

namespace le
{
    /// @brief Records every ruler (UPDATES.md item 13) - finished or
    /// still active, but not the live ghost segment trailing from an
    /// active one (see BuildOverlayPictureStage) - into its own small
    /// SkPicture, keyed on viewport_version()/visibility_version()/
    /// ruler_version(), deliberately *not* mouse_version(). Same split
    /// reasoning as BuildSelectionOverlayPictureStage: ruler tick/label
    /// geometry across potentially several rulers with several segments
    /// each should only recompute on an actual ruler change (a point
    /// added, a ruler finished, rulers cleared), not on every mouse move
    /// while panning/hovering. Root of its own chain - reads
    /// Scene::rulers() directly, no upstream Renderer stage to compose
    /// from.
    class BuildRulerOverlayPictureStage
    {
    public:
        const sk_sp<SkPicture> &run(const Scene &scene, std::optional<double> dbu_per_um)
        {
            const auto key = std::tuple{scene.viewport_version(), scene.visibility_version(), scene.ruler_version()};
            return stage_.get(key, [&]
            {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                for (const auto &ruler : scene.rulers())
                    draw_ruler_polyline(*canvas, scene, dbu_per_um, ruler);

                return recorder.finishRecordingAsPicture();
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> stage_;
    };
}
