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
    /// @brief Records the mouse-driven overlay chrome - the live
    /// rubber-band drag-select rectangle if a drag is in progress
    /// (see draw_drag_rect), the grid-snap indicator box (see
    /// draw_cursor), a hovered shape's yellow outline (see
    /// draw_hover_outline), the live ruler ghost segment trailing
    /// from an active (unfinished) ruler in Ruler mode (UPDATES.md item
    /// 13, see draw_ruler_segment), and the live Move ghost preview
    /// while a move is armed and anchored in Edit mode (UPDATES.md item
    /// 21, see draw_move_ghost) - into its own small SkPicture,
    /// cached independently of the (potentially design-sized, expensive)
    /// design picture - keyed on viewport_version()/visibility_version()
    /// (grid spacing lives there), mouse_version(), and ruler_version(),
    /// deliberately *not* AbstractId or selection_version(): this
    /// overlay is a pure viewport/mouse/live-ruler concern, not design
    /// or selection content, so switching Abstracts or changing the
    /// selection doesn't need to recompute it. ruler_version() is
    /// included (unlike a pure mouse concern would need) because the
    /// ghost segment's anchor is the active ruler's *last committed*
    /// point, which changes on click, not mouse move - without this key
    /// the ghost would render stale for one frame after a click with no
    /// subsequent mouse move, and wouldn't disappear immediately when a
    /// ruler finishes (e.g. via double-click) with no mouse movement.
    /// Selected-piece outlines and finalized rulers are each a separate
    /// picture (see BuildSelectionOverlayPictureStage/
    /// BuildRulerOverlayPictureStage) precisely so a mouse move alone
    /// never has to re-walk the selection list or every ruler's tick
    /// geometry - see those classes' own comments for why those splits
    /// exist. Root of its own chain - reads Scene's drag/cursor/hover/
    /// ruler state directly, no upstream Renderer stage to compose from.
    class BuildOverlayPictureStage
    {
    public:
        const sk_sp<SkPicture> &run(const Scene &scene, std::optional<double> dbu_per_um)
        {
            const auto key = std::tuple{scene.viewport_version(), scene.visibility_version(), scene.mouse_version(), scene.ruler_version()};
            return stage_.get(key, [&]
            {
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
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> stage_;
    };
}
