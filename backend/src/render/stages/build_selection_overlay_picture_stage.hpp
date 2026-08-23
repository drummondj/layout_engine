#pragma once
#include "../draw_helpers.hpp"
#include "../../core/placement_geometry.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../core/row_geometry.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../scene/scene.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <map>
#include <tuple>
#include <variant>
#include <vector>

namespace le
{
    /// @brief Records a white outline for every selected *piece*
    /// (UPDATES.md item 21), into its own small SkPicture separate from
    /// BuildOverlayPictureStage's mouse-driven chrome - keyed on
    /// viewport_version()/visibility_version()/selection_version(),
    /// deliberately *not* mouse_version().
    ///
    /// This split exists because a selected PATH piece's outline is
    /// traced via Geometry::path_to_polygons (a real Boost.Geometry
    /// buffer op, not free) - before this split, that work lived in
    /// build_overlay_picture keyed together with mouse_version(), which
    /// bumps on *every* pointer-move event, meaning every mouse move
    /// re-buffered every selected path's outline from scratch even though
    /// the selection hadn't changed - a real, reported regression that got
    /// worse the more objects were selected (see BENCHMARKS.md).
    ///
    /// `Scene::SelectedObject` is a variant (E1, BUGS_AND_ENHANCEMENTS.md):
    /// a `ShapePiece` (`ShapeId` + `piece_kind`/`piece_index`) for
    /// anything with real backing Shape geometry - Terminal/Obstruction
    /// (Abstract view) and now also Blockage/Route/PhysicalPort (Layout
    /// view) - resolved with a fresh `Root::get_shape()` lookup plus
    /// `Geometry::extract_piece`, so the highlight always traces the
    /// piece exactly as it's actually stored - not a Pipeline-derived
    /// approximation - matching Move's own commit path, which mutates
    /// that same raw piece; or a bare `RowId`/`RegionId`/`PlacementId`
    /// for a kind with no backing Shape at all, resolved via
    /// `row_footprint_bbox`/`RegionData::rects`/`placement_world_bbox`
    /// (src/core/) and drawn through the exact same, unmodified
    /// `draw_selected_piece_outline` by wrapping the bbox as a transient
    /// one-rect `Shape` (Region needs no wrapping - its own `.rects` is
    /// already `List[Rect]`).
    ///
    /// `current_layout`/`remaining_depth` (E1) are only meaningful for a
    /// selected `PlacementId` - the Abstract-path caller passes `{}`/`0`,
    /// which it structurally never needs (a `PlacementId` can never
    /// appear in Abstract-view selection, since Placement hit-testing
    /// only ever runs in Layout view - see api.cpp's `le_mouse_up`).
    ///
    /// Key adds `Root::mutation_version()`/`Scene::current_abstract()`/
    /// `Scene::current_layout()` (mirroring `TransformToPixelsStage`'s
    /// own key) - without `current_abstract()`/`current_layout()`,
    /// editing a currently-selected shape's geometry (e.g. via TCL, no
    /// selection change) would keep serving a stale cached highlight;
    /// `current_layout()` specifically matters once this same class is
    /// also driven from Layout view (InstanceRenderer owns a second,
    /// independent instance - see its own class comment for why sharing
    /// one instance across two different views/callers isn't safe) -
    /// switching between two Layouts with an unchanged (empty) selection
    /// would otherwise leave selection_version() unbumped and could
    /// serve a stale picture from the other Layout.
    class BuildSelectionOverlayPictureStage
    {
    public:
        // `shapes` is unused (kept in the signature purely so every
        // existing caller - Renderer::build_selection_overlay_picture,
        // and the many render_test.cpp call sites that already pass it -
        // doesn't need to change): UPDATES.md item 21's piece-granular
        // selection resolves each selected piece straight from `root`
        // (Geometry::extract_piece against the real, raw stored
        // geometry) rather than from Pipeline's own generated geometry -
        // see this class's own header comment.
        const sk_sp<SkPicture> &run(const Scene &scene, const Root &root, const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, LayoutId current_layout = LayoutId{}, int remaining_depth = 0)
        {
            (void)shapes;
            const auto key = std::tuple{scene.current_abstract(), scene.current_layout(), scene.viewport_version(), scene.visibility_version(), scene.selection_version(), root.mutation_version()};
            return stage_.get(key, [&]
            {
                SkPictureRecorder recorder;
                SkCanvas *canvas = recorder.beginRecording(
                    SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));

                for (const auto &selected : scene.selection())
                {
                    std::visit([&](const auto &s)
                    {
                        using T = std::decay_t<decltype(s)>;
                        if constexpr (std::is_same_v<T, ShapePiece>)
                        {
                            if (const ShapeData *data = root.get_shape(s.shape_id))
                                draw_selected_piece_outline(*canvas, scene, Geometry::extract_piece(*data, s.piece_kind, s.piece_index));
                        }
                        else if constexpr (std::is_same_v<T, RowId>)
                        {
                            if (auto bbox = row_footprint_bbox(root, s))
                                draw_selected_piece_outline(*canvas, scene, Shape{.rects = {*bbox}});
                        }
                        else if constexpr (std::is_same_v<T, RegionId>)
                        {
                            if (const RegionData *region = root.get_region(s))
                                draw_selected_piece_outline(*canvas, scene, Shape{.rects = region->rects});
                        }
                        else if constexpr (std::is_same_v<T, PlacementId>)
                        {
                            if (current_layout.valid())
                                if (auto bbox = placement_world_bbox(root, s, remaining_depth))
                                    draw_selected_piece_outline(*canvas, scene, Shape{.rects = {*bbox}});
                        }
                    }, selected);
                }

                return recorder.finishRecordingAsPicture();
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<AbstractId, LayoutId, uint64_t, uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> stage_;
    };
}
