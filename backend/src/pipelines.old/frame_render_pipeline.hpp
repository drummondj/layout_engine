#pragma once
#include "design_render_pipeline.hpp"
#include "mouse_target_layer_pipeline.hpp"
#include "pipeline_options.hpp"
#include "rasterize_compose_pipeline.hpp"
#include "selection_ghost_layer_pipeline.hpp"
#include "stages/pixel_transform_stage.hpp"
#include "stages/tiny_pixel_transform_stage.hpp"
#include <map>
#include <vector>

namespace le
{
    /// @brief Top-level orchestrator (Phase 3, backend/ONETBB_INTEGRATION.md
    /// migration plan) combining DesignRenderPipeline, MouseTargetLayerPipeline,
    /// SelectionGhostLayerPipeline, and RasterizeComposePipeline - the
    /// direct replacement for Renderer::render(), mirroring its own role
    /// and call order (see that method's own doc comment). Deliberately
    /// plain sequential calls into each pipeline's own run() followed by
    /// one RasterizeComposePipeline::run() call, not one shared
    /// flow::graph spanning all four - matches the original
    /// Renderer::render()'s own shape (plain C++ calling several stage
    /// methods then compose, not one wired graph) and the approved
    /// migration plan's own "final compose via plain code" decision. Each
    /// of the four owned pipelines is its own self-contained graph of
    /// stages; composing them here means feeding one's picture output
    /// into another's input, never reaching into another's internal
    /// nodes (see RasterizeComposePipeline's own doc comment) - the same
    /// pattern HierarchyResolver::render_layout_frame uses with its own
    /// separately-owned instances of these same pipeline types.
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
        FrameRenderPipeline() = default;

        FrameRenderPipeline(const FrameRenderPipeline &) = delete;
        FrameRenderPipeline &operator=(const FrameRenderPipeline &) = delete;

        /// @brief Runs the full render chain for a frame - mirrors
        /// Renderer::render()'s own role and call order: build the
        /// design/tiny-shapes content pictures, build the mouse overlay
        /// picture, build the selection and ruler pictures, then hand all
        /// five to RasterizeComposePipeline for the final rasterize+compose.
        const PixelBuffer &run(AbstractId abstract_id, const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const std::map<ViewLayerId, std::vector<Point>> &tiny_shapes, const PipelineOptions &options)
        {
            ZoneScopedN("FrameRenderPipeline: run");
            const sk_sp<SkPicture> &design_picture = design_pipeline_.run(shapes, PixelTransformStage::data_version_for(abstract_id, options), options);
            const uint64_t design_version = design_pipeline_.design_version();
            const sk_sp<SkPicture> &tiny_picture = design_pipeline_.run_tiny_shapes(tiny_shapes, TinyPixelTransformStage::data_version_for(abstract_id, options), options);
            const uint64_t tiny_shapes_version = design_pipeline_.tiny_shapes_version();
            const sk_sp<SkPicture> &overlay_picture = mouse_pipeline_.run(options);
            const sk_sp<SkPicture> &selection_picture = selection_ghost_pipeline_.run(abstract_id, options);
            const uint64_t selection_version = selection_ghost_pipeline_.selection_version();
            const sk_sp<SkPicture> &ruler_picture = selection_ghost_pipeline_.run_ruler(options);
            const uint64_t ruler_version = selection_ghost_pipeline_.ruler_version();

            return rasterize_compose_.run(RasterizeComposePipeline::Input{
                                               .design_picture = design_picture,
                                               .design_version = design_version,
                                               .tiny_shapes_picture = tiny_picture,
                                               .tiny_shapes_version = tiny_shapes_version,
                                               .selection_picture = selection_picture,
                                               .selection_version = selection_version,
                                               .ruler_picture = ruler_picture,
                                               .ruler_version = ruler_version,
                                               .overlay_picture = overlay_picture,
                                               .overlay_version = mouse_pipeline_.version(),
                                           },
                                           options);
        }

    private:
        DesignRenderPipeline design_pipeline_;
        MouseTargetLayerPipeline mouse_pipeline_;
        SelectionGhostLayerPipeline selection_ghost_pipeline_;
        RasterizeComposePipeline rasterize_compose_;
    };
}
