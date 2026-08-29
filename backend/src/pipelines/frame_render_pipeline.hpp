#pragma once
#include "design_render_pipeline.hpp"
#include "mouse_target_layer_pipeline.hpp"
#include "pipeline_options.hpp"
#include "selection_ghost_layer_pipeline.hpp"
#include "stages/compose_stage.hpp"
#include "stages/pixel_transform_stage.hpp"
#include "stages/tiny_pixel_transform_stage.hpp"
#include "tbb_core.hpp"
#include <map>
#include <vector>

namespace le
{
    /// @brief Top-level orchestrator (Phase 3, backend/ONETBB_INTEGRATION.md
    /// migration plan) combining DesignRenderPipeline, MouseTargetLayerPipeline,
    /// SelectionGhostLayerPipeline, and ComposeStage - the direct
    /// replacement for Renderer::render(), mirroring its own role and call
    /// order exactly (see that method's own doc comment). Deliberately
    /// plain sequential calls into the three layer pipelines followed by
    /// one manual ComposeStage submission, not one shared flow::graph
    /// spanning all three - matches the original Renderer::render()'s own
    /// shape (which is plain C++ calling five stage methods then compose,
    /// not one wired graph either) and the approved migration plan's own
    /// "final compose via plain code" decision.
    ///
    /// Deliberately does not link any shape pipeline (AbstractShapePipeline/
    /// LayoutShapePipeline) - mirrors `render` never linking `pipeline`
    /// today (see src/render/'s own CLAUDE.md bullet) - `run()` takes
    /// `shapes`/`tiny_shapes` as plain parameters, the same boundary types
    /// `core`/`RenderedShape`/`TinyShapeDot` already define, exactly like
    /// `Renderer::render()`'s own signature.
    class FrameRenderPipeline
    {
    public:
        FrameRenderPipeline()
            : compose_stage_(compose_graph_, "compose")
        {
            // ComposeStage has no upstream node within compose_graph_ to
            // wire via make_edge - its own three inputs (design/tiny
            // pipelines' rasterized frames, the mouse/selection/ruler
            // pipelines' own outputs) are computed by three *separate*
            // pipeline objects, each with their own separate graph, and
            // handed to this stage explicitly by run() below (see this
            // class's own doc comment for why that's the right shape, not
            // a design flaw to wire around).
            make_edge(compose_stage_.node(), compose_sink_);
        }

        FrameRenderPipeline(const FrameRenderPipeline &) = delete;
        FrameRenderPipeline &operator=(const FrameRenderPipeline &) = delete;

        /// @brief Runs the full render chain for a frame - mirrors
        /// Renderer::render()'s own role and call order: transform+build+
        /// rasterize the design and tiny-shapes frames, build the mouse
        /// overlay picture, transform+build+rasterize the selection and
        /// ruler frames, then compose all five into one final PixelBuffer.
        const PixelBuffer &run(AbstractId abstract_id, const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const std::map<ViewLayerId, std::vector<Point>> &tiny_shapes, const PipelineOptions &options)
        {
            ZoneScopedN("FrameRenderPipeline: run");
            const RasterizedFrame &design_frame = design_pipeline_.run(shapes, PixelTransformStage::data_version_for(abstract_id, options), options);
            const RasterizedFrame &tiny_frame = design_pipeline_.run_tiny_shapes(tiny_shapes, TinyPixelTransformStage::data_version_for(abstract_id, options), options);
            const sk_sp<SkPicture> &overlay_picture = mouse_pipeline_.run(options);
            const RasterizedFrame &selection_frame = selection_ghost_pipeline_.run(abstract_id, options);
            const RasterizedFrame &ruler_frame = selection_ghost_pipeline_.run_ruler(options);

            const uint64_t compose_version = ComposeStage::data_version_for(
                design_pipeline_.design_version(), design_pipeline_.tiny_shapes_version(),
                selection_ghost_pipeline_.selection_version(), selection_ghost_pipeline_.ruler_version(),
                mouse_pipeline_.version());

            return run_compose(ComposeInput{
                                   .design_frame = design_frame,
                                   .tiny_shapes_frame = tiny_frame,
                                   .selection_frame = selection_frame,
                                   .ruler_frame = ruler_frame,
                                   .overlay_picture = overlay_picture,
                               },
                               compose_version, options);
        }

        /// @brief Direct access to the two layer pipelines this class
        /// owns - lets HierarchyResolver's own render_layout_frame (Phase
        /// 4) drive their real RasterizePictureStage instances directly
        /// (via design_pipeline().run_design_rasterize(...), etc.) rather
        /// than duplicate them, mirroring Renderer's own
        /// design_rasterize_stage()/etc. accessors and their own doc
        /// comment's reasoning (same domain-tag caveat - see
        /// HierarchyResolver's own kLayoutVersionDomainTag).
        DesignRenderPipeline &design_pipeline() { return design_pipeline_; }
        SelectionGhostLayerPipeline &selection_ghost_pipeline() { return selection_ghost_pipeline_; }

        /// @brief Drives this class's own ComposeStage directly with a
        /// caller-supplied ComposeInput+version - same shape as
        /// DesignRenderPipeline's own run_design_rasterize (compose_stage_'s
        /// own node lives in this class's private compose_graph_, which
        /// only this class can correctly wait on).
        const PixelBuffer &run_compose(ComposeInput input, uint64_t compose_version, const PipelineOptions &options)
        {
            ZoneScopedN("FrameRenderPipeline: run_compose");
            compose_stage_.try_put({.data = std::move(input), .data_version = compose_version, .options = options});
            compose_graph_.wait_for_all();
            return compose_result_.data.buffer;
        }

    private:
        DesignRenderPipeline design_pipeline_;
        MouseTargetLayerPipeline mouse_pipeline_;
        SelectionGhostLayerPipeline selection_ghost_pipeline_;

        oneapi::tbb::flow::graph compose_graph_;
        ComposeStage compose_stage_;
        oneapi::tbb::flow::function_node<StageData<RasterizedFrame, PipelineOptions>> compose_sink_{compose_graph_, oneapi::tbb::flow::serial, [this](StageData<RasterizedFrame, PipelineOptions> in)
                                                                                                      { compose_result_ = std::move(in); }};
        StageData<RasterizedFrame, PipelineOptions> compose_result_{};
    };
}
