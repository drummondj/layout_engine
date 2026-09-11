#pragma once
#include "pipeline_options.hpp"
#include "stages/build_design_picture_stage.hpp"
#include "stages/build_tiny_dots_picture_stage.hpp"
#include "stages/pixel_transform_stage.hpp"
#include "stages/tiny_pixel_transform_stage.hpp"
#include "tbb_core.hpp"
#include <map>
#include <vector>

namespace le
{
    /// @brief oneTBB flow::graph wiring of "shape data -> content picture"
    /// for the Abstract/Layout render view (Phase 3,
    /// backend/ONETBB_INTEGRATION.md migration plan) - the direct
    /// replacement for Renderer's own design-frame and tiny-shapes-frame
    /// build steps:
    ///
    ///   PixelTransformStage -> BuildDesignPictureStage      (run())
    ///   TinyPixelTransformStage -> BuildTinyDotsPictureStage (run_tiny_shapes())
    ///
    /// Two independent head nodes (not a fan-out from one) since they take
    /// genuinely different InputData (the filtered shape map vs. the
    /// tiny-dot map) - mirrors Renderer owning two separate stage chains
    /// today.
    ///
    /// Produces pictures only, not rasterized pixels - RasterizeComposePipeline
    /// owns the shared rasterize+compose tail (see that class's own doc
    /// comment for why the split lets both FrameRenderPipeline and
    /// HierarchyResolver each own their own private rasterize/compose
    /// instance instead of one reaching into this class's internal
    /// nodes). Non-copyable/non-movable, same reason as
    /// AbstractShapePipeline/LayoutShapePipeline.
    class DesignRenderPipeline
    {
    public:
        DesignRenderPipeline()
            : pixel_transform_stage_(graph_, "design_pixel_transform"),
              build_design_picture_stage_(graph_, "design_build_picture"),
              tiny_pixel_transform_stage_(graph_, "design_tiny_pixel_transform"),
              build_tiny_dots_picture_stage_(graph_, "design_build_tiny_dots"),
              design_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<sk_sp<SkPicture>, PipelineOptions> in)
                           { design_result_ = std::move(in); }),
              tiny_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<sk_sp<SkPicture>, PipelineOptions> in)
                         { tiny_result_ = std::move(in); })
        {
            using namespace oneapi::tbb::flow;
            make_edge(pixel_transform_stage_.node(), build_design_picture_stage_.node());
            make_edge(build_design_picture_stage_.node(), design_sink_);

            make_edge(tiny_pixel_transform_stage_.node(), build_tiny_dots_picture_stage_.node());
            make_edge(build_tiny_dots_picture_stage_.node(), tiny_sink_);
        }

        DesignRenderPipeline(const DesignRenderPipeline &) = delete;
        DesignRenderPipeline &operator=(const DesignRenderPipeline &) = delete;

        /// @brief Runs PixelTransformStage -> BuildDesignPictureStage for
        /// `shapes`. `shapes_data_version` is typically
        /// PixelTransformStage::data_version_for(abstract_id, options), or
        /// an AbstractShapePipeline's own shapes_version() if the caller
        /// already has one (see PixelTransformStage's own doc comment).
        const sk_sp<SkPicture> &run(const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, uint64_t shapes_data_version, const PipelineOptions &options)
        {
            ZoneScopedN("DesignRenderPipeline: run");
            pixel_transform_stage_.try_put({.data = shapes, .data_version = shapes_data_version, .options = options});
            graph_.wait_for_all();
            return design_result_.data;
        }

        const sk_sp<SkPicture> &run_tiny_shapes(const std::map<ViewLayerId, std::vector<Point>> &tiny_shapes, uint64_t tiny_shapes_data_version, const PipelineOptions &options)
        {
            ZoneScopedN("DesignRenderPipeline: run_tiny_shapes");
            tiny_pixel_transform_stage_.try_put({.data = tiny_shapes, .data_version = tiny_shapes_data_version, .options = options});
            graph_.wait_for_all();
            return tiny_result_.data;
        }

        uint64_t design_version() const { return design_result_.data_version; }
        uint64_t tiny_shapes_version() const { return tiny_result_.data_version; }

    private:
        oneapi::tbb::flow::graph graph_;
        PixelTransformStage pixel_transform_stage_;
        BuildDesignPictureStage build_design_picture_stage_;
        TinyPixelTransformStage tiny_pixel_transform_stage_;
        BuildTinyDotsPictureStage build_tiny_dots_picture_stage_;

        oneapi::tbb::flow::function_node<StageData<sk_sp<SkPicture>, PipelineOptions>> design_sink_;
        oneapi::tbb::flow::function_node<StageData<sk_sp<SkPicture>, PipelineOptions>> tiny_sink_;

        StageData<sk_sp<SkPicture>, PipelineOptions> design_result_{};
        StageData<sk_sp<SkPicture>, PipelineOptions> tiny_result_{};
    };
}
