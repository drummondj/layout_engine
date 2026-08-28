#pragma once
#include "../../render/draw_helpers.hpp"
#include "../../scene/scene.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <optional>

namespace le
{
    /// @brief oneTBB port of src/render/stages/build_overlay_picture_stage.hpp
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan; "mouse
    /// target and movement layer" in the user's own migration outline) -
    /// records the mouse-driven overlay chrome (drag-select rectangle,
    /// grid-snap cursor box, hover outline, live ruler ghost segment, live
    /// Move ghost preview). compute()'s own body is unchanged from the
    /// original stage's run() lambda - see that file's own doc comment for
    /// why each piece belongs here rather than in SelectionOverlayStage/
    /// RulerOverlayStage.
    ///
    /// Root of its own chain (reads Scene state directly, no upstream
    /// stage to compose a data_version from) - unlike every other
    /// converted stage so far, this one has no real "changing payload" at
    /// all, only options-driven triggers, so InputData is a dummy `int`
    /// always submitted as `0` with `data_version` always `0`; every
    /// recompute is driven by options_did_change instead, comparing the
    /// same four fields the original stage's key was `{viewport_version,
    /// visibility_version, mouse_version, ruler_version}` - deliberately
    /// *not* selection_version, matching the original's own reasoning
    /// (switching selection alone shouldn't recompute mouse-only chrome).
    class MouseOverlayStage : public MemoizingStage<int, sk_sp<SkPicture>, PipelineOptions>
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

            draw_drag_rect(*canvas, scene);

            draw_cursor(*canvas, scene);

            if (const auto &hover = scene.hover())
                draw_hover_outline(*canvas, scene, *hover);

            if (scene.mode() == Scene::Mode::RULER && !scene.rulers().empty() && !scene.rulers().back().finished && !scene.rulers().back().points.empty())
            {
                if (const std::optional<Point> ghost = scene.ruler_next_point(scene.ruler_free_form()))
                    draw_ruler_segment(*canvas, scene, dbu_per_um, scene.rulers().back().points.back(), *ghost, /*is_ghost=*/true);
            }

            if (scene.mode() == Scene::Mode::EDIT && scene.move().armed && scene.move().anchor)
            {
                if (const std::optional<Point> delta = scene.move_delta(scene.move_free_form()))
                    for (const Shape &piece : scene.move().moving_geometry)
                        draw_move_ghost(*canvas, scene, piece, *delta);
            }

            return recorder.finishRecordingAsPicture();
        }

        bool options_did_change(const PipelineOptions &last, const PipelineOptions &current) const override
        {
            return last.viewport.viewport_version != current.viewport.viewport_version ||
                   last.viewport.visibility_version != current.viewport.visibility_version ||
                   last.interaction.mouse_version != current.interaction.mouse_version ||
                   last.interaction.ruler_version != current.interaction.ruler_version;
        }
    };
}
