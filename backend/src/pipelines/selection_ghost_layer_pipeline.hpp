#pragma once
#include "pipeline_options.hpp"
#include "stages/rasterize_picture_stage.hpp"
#include "stages/ruler_overlay_stage.hpp"
#include "stages/selection_overlay_stage.hpp"
#include "tbb_core.hpp"

namespace le
{
    /// @brief oneTBB flow::graph wiring of "selection and ghost layers"
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - the
    /// direct replacement for Renderer's own build_selection_overlay_picture
    /// and build_ruler_overlay_picture chains:
    ///
    ///   SelectionOverlayStage -> RasterizePictureStage   (run())
    ///   RulerOverlayStage -> RasterizePictureStage        (run_ruler())
    ///
    /// Note on naming: the live in-progress ruler segment ("ghost") is
    /// actually part of MouseTargetLayerPipeline's own MouseOverlayStage,
    /// not this pipeline - see that class's own doc comment. This
    /// pipeline's own "ghost" is the finished/active-but-not-live ruler
    /// geometry plus selection outlines; preserved as a real, deliberate
    /// split from the original code rather than reshuffled to match the
    /// migration outline's wording exactly (flagged in the approved
    /// migration plan, section 1.3c).
    class SelectionGhostLayerPipeline
    {
    public:
        SelectionGhostLayerPipeline()
            : selection_overlay_stage_(graph_, "selection_overlay"),
              selection_rasterize_stage_(graph_, "selection_rasterize"),
              ruler_overlay_stage_(graph_, "ruler_overlay"),
              ruler_rasterize_stage_(graph_, "ruler_rasterize"),
              selection_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<RasterizedFrame, PipelineOptions> in)
                              { selection_result_ = std::move(in); }),
              ruler_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<RasterizedFrame, PipelineOptions> in)
                          { ruler_result_ = std::move(in); })
        {
            using namespace oneapi::tbb::flow;
            make_edge(selection_overlay_stage_.node(), selection_rasterize_stage_.node());
            make_edge(selection_rasterize_stage_.node(), selection_sink_);

            make_edge(ruler_overlay_stage_.node(), ruler_rasterize_stage_.node());
            make_edge(ruler_rasterize_stage_.node(), ruler_sink_);
        }

        SelectionGhostLayerPipeline(const SelectionGhostLayerPipeline &) = delete;
        SelectionGhostLayerPipeline &operator=(const SelectionGhostLayerPipeline &) = delete;

        /// @brief `current_layout`/`remaining_depth` default to the
        /// Abstract-path convention (`{}`/`0`, matching
        /// SelectionOverlayStage's own original key shape) - FrameRenderPipeline's
        /// own Abstract-path usage doesn't pass them; HierarchyResolver's
        /// own Layout-path usage (Phase 4, render_layout_frame) does.
        const RasterizedFrame &run(AbstractId current_abstract, const PipelineOptions &options, LayoutId current_layout = LayoutId{}, int remaining_depth = 0)
        {
            ZoneScopedN("SelectionGhostLayerPipeline: run");
            const SelectionOverlayRequest request{.abstract_id = current_abstract, .current_layout = current_layout, .remaining_depth = remaining_depth};
            selection_overlay_stage_.try_put({.data = request, .data_version = SelectionOverlayStage::data_version_for(request, options), .options = options});
            graph_.wait_for_all();
            return selection_result_.data;
        }

        const RasterizedFrame &run_ruler(const PipelineOptions &options)
        {
            ZoneScopedN("SelectionGhostLayerPipeline: run_ruler");
            ruler_overlay_stage_.try_put({.data = 0, .data_version = 0, .options = options});
            graph_.wait_for_all();
            return ruler_result_.data;
        }

        uint64_t selection_version() const { return selection_result_.data_version; }
        uint64_t ruler_version() const { return ruler_result_.data_version; }

        /// @brief Drives this class's own selection/ruler
        /// RasterizePictureStage instances directly with a caller-supplied
        /// picture+version - see DesignRenderPipeline's own
        /// run_design_rasterize/run_tiny_shapes_rasterize for the full
        /// reasoning (same shape, same domain-tag caveat).
        const RasterizedFrame &run_selection_rasterize(const sk_sp<SkPicture> &picture, uint64_t picture_version, const PipelineOptions &options)
        {
            ZoneScopedN("SelectionGhostLayerPipeline: run_selection_rasterize");
            selection_rasterize_stage_.try_put({.data = picture, .data_version = picture_version, .options = options});
            graph_.wait_for_all();
            return selection_result_.data;
        }

        const RasterizedFrame &run_ruler_rasterize(const sk_sp<SkPicture> &picture, uint64_t picture_version, const PipelineOptions &options)
        {
            ZoneScopedN("SelectionGhostLayerPipeline: run_ruler_rasterize");
            ruler_rasterize_stage_.try_put({.data = picture, .data_version = picture_version, .options = options});
            graph_.wait_for_all();
            return ruler_result_.data;
        }

    private:
        oneapi::tbb::flow::graph graph_;
        SelectionOverlayStage selection_overlay_stage_;
        RasterizePictureStage selection_rasterize_stage_;
        RulerOverlayStage ruler_overlay_stage_;
        RasterizePictureStage ruler_rasterize_stage_;

        oneapi::tbb::flow::function_node<StageData<RasterizedFrame, PipelineOptions>> selection_sink_;
        oneapi::tbb::flow::function_node<StageData<RasterizedFrame, PipelineOptions>> ruler_sink_;

        StageData<RasterizedFrame, PipelineOptions> selection_result_{};
        StageData<RasterizedFrame, PipelineOptions> ruler_result_{};
    };
}
