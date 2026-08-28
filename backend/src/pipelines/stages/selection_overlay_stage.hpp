#pragma once
#include "../../core/placement_geometry.hpp"
#include "../../core/row_geometry.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../render/draw_helpers.hpp"
#include "../../scene/scene.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "../version_utils.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <variant>

namespace le
{
    /// @brief oneTBB port of src/render/stages/build_selection_overlay_picture_stage.hpp
    /// (Phase 3/4, backend/ONETBB_INTEGRATION.md migration plan) - records a
    /// white outline for every selected piece. compute()'s own body is
    /// unchanged from the original stage's run() lambda - see that file's
    /// own doc comment for the full per-SelectedObject-kind resolution.
    /// Covers both Abstract- and Layout-view usage, matching the original
    /// class's own scope (its `current_layout`/`remaining_depth` optional
    /// params, `{}`/`0` for the Abstract path) - FrameRenderPipeline's own
    /// Abstract-path usage passes `current_layout = LayoutId{}`;
    /// HierarchyResolver's own Layout-path usage (Phase 4, render_layout_frame)
    /// passes the real LayoutId/remaining_depth, resolving a selected
    /// PlacementId's own world bbox via placement_world_bbox.
    ///
    /// InputData is SelectionOverlayRequest (AbstractId + LayoutId +
    /// remaining_depth) - the original key's `{AbstractId, LayoutId, ...}`
    /// pair, plus remaining_depth since a selected PlacementId's own bbox
    /// resolution depends on it. data_version_for folds all three
    /// identity fields + Root::mutation_version() together (the "did the
    /// referenced content change" triggers); options_did_change compares
    /// viewport_version/visibility_version/selection_version, the
    /// remaining triggers from the original key.
    struct SelectionOverlayRequest
    {
        AbstractId abstract_id;
        LayoutId current_layout;
        int remaining_depth = 0;
    };

    class SelectionOverlayStage : public MemoizingStage<SelectionOverlayRequest, sk_sp<SkPicture>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

        static uint64_t data_version_for(const SelectionOverlayRequest &request, const PipelineOptions &options)
        {
            return combine_versions(
                combine_versions(request.abstract_id.index, request.abstract_id.generation),
                combine_versions(request.current_layout.index, request.current_layout.generation),
                static_cast<uint64_t>(request.remaining_depth), options.epoch.root_mutation_version);
        }

    protected:
        sk_sp<SkPicture> compute(const SelectionOverlayRequest &request, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            const Root &root = *options.ctx.root;
            const LayoutId current_layout = request.current_layout;
            const int remaining_depth = request.remaining_depth;

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
                    } }, selected);
            }

            return recorder.finishRecordingAsPicture();
        }

        bool options_did_change(const PipelineOptions &last, const PipelineOptions &current) const override
        {
            return last.viewport.viewport_version != current.viewport.viewport_version ||
                   last.viewport.visibility_version != current.viewport.visibility_version ||
                   last.interaction.selection_version != current.interaction.selection_version;
        }
    };
}
