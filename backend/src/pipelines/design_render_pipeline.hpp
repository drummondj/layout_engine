#pragma once
#include "pipeline_options.hpp"
#include "stages/build_design_picture_stage.hpp"
#include "stages/build_tiny_dots_picture_stage.hpp"
#include "stages/pixel_transform_stage.hpp"
#include "stages/rasterize_picture_stage.hpp"
#include "stages/tiny_pixel_transform_stage.hpp"
#include "tbb_core.hpp"
#include <map>
#include <vector>

namespace le
{
    /// @brief oneTBB flow::graph wiring of "the layout/abstract render
    /// view" (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - the
    /// direct replacement for Renderer's own design-frame and tiny-shapes-
    /// frame chains:
    ///
    ///   PixelTransformStage -> BuildDesignPictureStage -> RasterizePictureStage      (run())
    ///   TinyPixelTransformStage -> BuildTinyDotsPictureStage -> RasterizePictureStage (run_tiny_shapes())
    ///
    /// Two independent head nodes (not a fan-out from one) since they take
    /// genuinely different InputData (the filtered shape map vs. the
    /// tiny-dot map) - mirrors Renderer owning two separate stage chains
    /// today. Non-copyable/non-movable, same reason as AbstractShapePipeline/
    /// LayoutShapePipeline (see AbstractShapePipeline's own doc comment).
    class DesignRenderPipeline
    {
    public:
        DesignRenderPipeline()
            : pixel_transform_stage_(graph_, "design_pixel_transform"),
              build_design_picture_stage_(graph_, "design_build_picture"),
              design_rasterize_stage_(graph_, "design_rasterize"),
              tiny_pixel_transform_stage_(graph_, "design_tiny_pixel_transform"),
              build_tiny_dots_picture_stage_(graph_, "design_build_tiny_dots"),
              tiny_rasterize_stage_(graph_, "design_tiny_rasterize"),
              design_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<RasterizedFrame, PipelineOptions> in)
                           { design_result_ = std::move(in); }),
              tiny_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<RasterizedFrame, PipelineOptions> in)
                         { tiny_result_ = std::move(in); })
        {
            using namespace oneapi::tbb::flow;
            make_edge(pixel_transform_stage_.node(), build_design_picture_stage_.node());
            make_edge(build_design_picture_stage_.node(), design_rasterize_stage_.node());
            make_edge(design_rasterize_stage_.node(), design_sink_);

            make_edge(tiny_pixel_transform_stage_.node(), build_tiny_dots_picture_stage_.node());
            make_edge(build_tiny_dots_picture_stage_.node(), tiny_rasterize_stage_.node());
            make_edge(tiny_rasterize_stage_.node(), tiny_sink_);
        }

        DesignRenderPipeline(const DesignRenderPipeline &) = delete;
        DesignRenderPipeline &operator=(const DesignRenderPipeline &) = delete;

        /// @brief Runs PixelTransformStage -> BuildDesignPictureStage ->
        /// RasterizePictureStage for `shapes`. `shapes_data_version` is
        /// typically PixelTransformStage::data_version_for(abstract_id,
        /// options), or an AbstractShapePipeline's own shapes_version() if
        /// the caller already has one (see PixelTransformStage's own doc
        /// comment).
        const RasterizedFrame &run(const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, uint64_t shapes_data_version, const PipelineOptions &options)
        {
            pixel_transform_stage_.try_put({.data = shapes, .data_version = shapes_data_version, .options = options});
            graph_.wait_for_all();
            return design_result_.data;
        }

        const RasterizedFrame &run_tiny_shapes(const std::map<ViewLayerId, std::vector<Point>> &tiny_shapes, uint64_t tiny_shapes_data_version, const PipelineOptions &options)
        {
            tiny_pixel_transform_stage_.try_put({.data = tiny_shapes, .data_version = tiny_shapes_data_version, .options = options});
            graph_.wait_for_all();
            return tiny_result_.data;
        }

        uint64_t design_version() const { return design_result_.data_version; }
        uint64_t tiny_shapes_version() const { return tiny_result_.data_version; }

        /// @brief Drives this class's own design/tiny-shapes
        /// RasterizePictureStage instances directly with a caller-supplied
        /// picture+version, bypassing the normal PixelTransformStage/
        /// BuildDesignPictureStage upstream chain - lets a second caller
        /// with its own content (HierarchyResolver's own render_layout_frame,
        /// Phase 4) reuse the same real cached raster surfaces instead of
        /// duplicating them, mirroring Renderer's own
        /// design_rasterize_stage()/tiny_shapes_rasterize_stage()
        /// accessors and their own doc comment's reasoning - including the
        /// same domain-tag caveat (each RasterizePictureStage's own key is
        /// just `{data_version}`, no domain discriminator, so a second
        /// caller sharing these must keep its own version numbers disjoint
        /// from this class's own Abstract-path numbering - see
        /// HierarchyResolver's own kLayoutVersionDomainTag). Unlike a bare
        /// stage-reference accessor, these wrap the try_put+wait_for_all
        /// pairing themselves - `design_rasterize_stage_`'s own node lives
        /// in this class's private `graph_`, which only this class can
        /// correctly wait on.
        const RasterizedFrame &run_design_rasterize(const sk_sp<SkPicture> &picture, uint64_t picture_version, const PipelineOptions &options)
        {
            design_rasterize_stage_.try_put({.data = picture, .data_version = picture_version, .options = options});
            graph_.wait_for_all();
            return design_result_.data;
        }

        const RasterizedFrame &run_tiny_shapes_rasterize(const sk_sp<SkPicture> &picture, uint64_t picture_version, const PipelineOptions &options)
        {
            tiny_rasterize_stage_.try_put({.data = picture, .data_version = picture_version, .options = options});
            graph_.wait_for_all();
            return tiny_result_.data;
        }

    private:
        oneapi::tbb::flow::graph graph_;
        PixelTransformStage pixel_transform_stage_;
        BuildDesignPictureStage build_design_picture_stage_;
        RasterizePictureStage design_rasterize_stage_;
        TinyPixelTransformStage tiny_pixel_transform_stage_;
        BuildTinyDotsPictureStage build_tiny_dots_picture_stage_;
        RasterizePictureStage tiny_rasterize_stage_;

        oneapi::tbb::flow::function_node<StageData<RasterizedFrame, PipelineOptions>> design_sink_;
        oneapi::tbb::flow::function_node<StageData<RasterizedFrame, PipelineOptions>> tiny_sink_;

        StageData<RasterizedFrame, PipelineOptions> design_result_{};
        StageData<RasterizedFrame, PipelineOptions> tiny_result_{};
    };
}
