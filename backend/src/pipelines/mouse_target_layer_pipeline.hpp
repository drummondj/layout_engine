#pragma once
#include "pipeline_options.hpp"
#include "stages/mouse_overlay_stage.hpp"
#include "tbb_core.hpp"

namespace le
{
    /// @brief oneTBB flow::graph wiring of "the mouse target and movement
    /// layer" (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) -
    /// the direct replacement for Renderer's own build_overlay_picture.
    /// A single stage, MouseOverlayStage - stays un-rasterized (drawn
    /// directly by ComposeStage, not blitted from a cached raster), same
    /// as the original.
    class MouseTargetLayerPipeline
    {
    public:
        MouseTargetLayerPipeline()
            : mouse_overlay_stage_(graph_, "mouse_overlay"),
              sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<sk_sp<SkPicture>, PipelineOptions> in)
                    { result_ = std::move(in); })
        {
            make_edge(mouse_overlay_stage_.node(), sink_);
        }

        MouseTargetLayerPipeline(const MouseTargetLayerPipeline &) = delete;
        MouseTargetLayerPipeline &operator=(const MouseTargetLayerPipeline &) = delete;

        const sk_sp<SkPicture> &run(const PipelineOptions &options)
        {
            mouse_overlay_stage_.try_put({.data = 0, .data_version = 0, .options = options});
            graph_.wait_for_all();
            return result_.data;
        }

        uint64_t version() const { return result_.data_version; }

    private:
        oneapi::tbb::flow::graph graph_;
        MouseOverlayStage mouse_overlay_stage_;
        oneapi::tbb::flow::function_node<StageData<sk_sp<SkPicture>, PipelineOptions>> sink_;
        StageData<sk_sp<SkPicture>, PipelineOptions> result_{};
    };
}
