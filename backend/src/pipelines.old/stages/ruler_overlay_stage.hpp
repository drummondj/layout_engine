#pragma once
#include "../../database/database.hpp"
#include "../draw_helpers.hpp"
#include "../../scene/scene.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <optional>

namespace le
{
    /// @brief oneTBB port of src/render/stages/build_ruler_overlay_picture_stage.hpp
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - records
    /// every finished-or-active ruler, but not the live ghost segment (see
    /// MouseOverlayStage). compute()'s own body is unchanged from the
    /// original stage's run() lambda.
    ///
    /// Same "no real payload, options-only" shape as MouseOverlayStage -
    /// InputData is a dummy `int`, always `0`; options_did_change compares
    /// viewport_version/visibility_version/ruler_version, matching the
    /// original key `{viewport_version, visibility_version,
    /// ruler_version}` - deliberately *not* mouse_version (a mouse move
    /// alone shouldn't re-walk every ruler's own tick geometry).
    class RulerOverlayStage : public MemoizingStage<int, sk_sp<SkPicture>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

    protected:
        sk_sp<SkPicture> compute(const int &, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            const std::optional<double> dbu_per_um = technology_dbu_per_um(*options.ctx.root);

            SkPictureRecorder recorder;
            SkCanvas *canvas = recorder.beginRecording(
                SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

            for (const auto &ruler : scene.rulers())
                draw_ruler_polyline(*canvas, scene, dbu_per_um, ruler);

            return recorder.finishRecordingAsPicture();
        }

        bool options_did_change(const PipelineOptions &last, const PipelineOptions &current) const override
        {
            return last.viewport.viewport_version != current.viewport.viewport_version ||
                   last.viewport.visibility_version != current.viewport.visibility_version ||
                   last.interaction.ruler_version != current.interaction.ruler_version;
        }
    };
}
