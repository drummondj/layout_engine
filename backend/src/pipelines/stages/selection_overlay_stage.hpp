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
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - records a
    /// white outline for every selected piece. compute()'s own body is
    /// unchanged from the original stage's run() lambda - see that file's
    /// own doc comment for the full per-SelectedObject-kind resolution.
    /// This Phase-3 port covers Abstract-view usage only (current_layout
    /// always invalid, remaining_depth always 0 - matches Renderer's own
    /// current scope, which is Abstract-path only; Layout-view usage is
    /// HierarchyResolver's own job, Phase 4, mirroring InstanceRenderer
    /// owning a second, independent instance of the original class today).
    ///
    /// InputData is AbstractId (`scene.current_abstract()`) - the original
    /// key's `{AbstractId, LayoutId, ...}` pair narrows to just AbstractId
    /// here since LayoutId is always invalid in this Phase-3 scope.
    /// data_version_for folds AbstractId + Root::mutation_version()
    /// together (the two "did the referenced content change" triggers);
    /// options_did_change compares viewport_version/visibility_version/
    /// selection_version, the remaining triggers from the original key.
    class SelectionOverlayStage : public MemoizingStage<AbstractId, sk_sp<SkPicture>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

        static uint64_t data_version_for(AbstractId abstract_id, const PipelineOptions &options)
        {
            return combine_versions(abstract_id.index, abstract_id.generation, options.epoch.root_mutation_version);
        }

    protected:
        sk_sp<SkPicture> compute(const AbstractId &, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            const Root &root = *options.ctx.root;
            constexpr LayoutId current_layout{};
            constexpr int remaining_depth = 0;

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
