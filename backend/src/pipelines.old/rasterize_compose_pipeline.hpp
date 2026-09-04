#pragma once
#include "pipeline_options.hpp"
#include "pixel_types.hpp"
#include "stages/compose_stage.hpp"
#include "stages/rasterize_picture_stage.hpp"
#include "stages/tiled_rasterize_picture_stage.hpp"
#include "tbb_core.hpp"
#include "include/core/SkPicture.h"

namespace le
{
    /// @brief Self-contained "rasterize the four content pictures, then
    /// composite them with the mouse-overlay picture into one final
    /// PixelBuffer" pipeline - the shared tail every displayable frame
    /// needs (design/tiny-shapes/selection/ruler -> RasterizePictureStage,
    /// then ComposeStage). Factored out of FrameRenderPipeline so both the
    /// Abstract-view path (FrameRenderPipeline::run) and the Layout-view
    /// path (HierarchyResolver::render_layout_frame) can each own their
    /// own private instance, rather than one reaching into the other's
    /// internal stage nodes across a pipeline boundary - a pipeline is a
    /// self-contained graph of stages, and composing two pipelines means
    /// feeding one's output into another's input, not exposing one's
    /// internal nodes for a second caller to try_put into directly (the
    /// old DesignRenderPipeline::run_design_rasterize/
    /// SelectionGhostLayerPipeline::run_selection_rasterize bypass this
    /// replaces). Each caller owning a private instance also means no
    /// shared cache slot between the two view domains, so the
    /// kLayoutVersionDomainTag bit-tagging the old shared-instance design
    /// needed to keep their version numbers from colliding is gone too -
    /// two separate MemoizingStage instances can't collide with each
    /// other's version() counters in the first place.
    ///
    /// The four rasterize stages are independent (no make_edge between
    /// them - same "fixed, compile-time-known arity, no FanInCollectStage
    /// needed" reasoning as ComposeStage's own doc comment), so run()
    /// dispatches all four via try_put before a single wait_for_all() -
    /// they settle concurrently within this pipeline's own graph_ rather
    /// than one-at-a-time. design_rasterize_ is a TiledRasterizePictureStage
    /// (parallel row-band tiling within itself, on top of that) rather
    /// than a plain RasterizePictureStage like the other three - see its
    /// own doc comment for why the design slot specifically earns that
    /// (it's the content-heavy one Tracy identified as the bottleneck).
    class RasterizeComposePipeline
    {
    public:
        struct Input
        {
            sk_sp<SkPicture> design_picture;
            uint64_t design_version = 0;
            sk_sp<SkPicture> tiny_shapes_picture;
            uint64_t tiny_shapes_version = 0;
            sk_sp<SkPicture> selection_picture;
            uint64_t selection_version = 0;
            sk_sp<SkPicture> ruler_picture;
            uint64_t ruler_version = 0;
            sk_sp<SkPicture> overlay_picture;
            uint64_t overlay_version = 0;
        };

        RasterizeComposePipeline()
            : design_rasterize_(graph_, "rasterize_compose_design"),
              tiny_rasterize_(graph_, "rasterize_compose_tiny"),
              selection_rasterize_(graph_, "rasterize_compose_selection"),
              ruler_rasterize_(graph_, "rasterize_compose_ruler"),
              compose_stage_(graph_, "rasterize_compose_compose"),
              design_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<RasterizedFrame, PipelineOptions> in)
                            { design_result_ = std::move(in); }),
              tiny_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<RasterizedFrame, PipelineOptions> in)
                         { tiny_result_ = std::move(in); }),
              selection_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<RasterizedFrame, PipelineOptions> in)
                              { selection_result_ = std::move(in); }),
              ruler_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<RasterizedFrame, PipelineOptions> in)
                          { ruler_result_ = std::move(in); }),
              compose_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<RasterizedFrame, PipelineOptions> in)
                            { compose_result_ = std::move(in); })
        {
            using namespace oneapi::tbb::flow;
            make_edge(design_rasterize_.node(), design_sink_);
            make_edge(tiny_rasterize_.node(), tiny_sink_);
            make_edge(selection_rasterize_.node(), selection_sink_);
            make_edge(ruler_rasterize_.node(), ruler_sink_);
            make_edge(compose_stage_.node(), compose_sink_);
        }

        RasterizeComposePipeline(const RasterizeComposePipeline &) = delete;
        RasterizeComposePipeline &operator=(const RasterizeComposePipeline &) = delete;

        const PixelBuffer &run(const Input &in, const PipelineOptions &options)
        {
            ZoneScopedN("RasterizeComposePipeline: run");
            design_rasterize_.try_put({.data = in.design_picture, .data_version = in.design_version, .options = options});
            tiny_rasterize_.try_put({.data = in.tiny_shapes_picture, .data_version = in.tiny_shapes_version, .options = options});
            selection_rasterize_.try_put({.data = in.selection_picture, .data_version = in.selection_version, .options = options});
            ruler_rasterize_.try_put({.data = in.ruler_picture, .data_version = in.ruler_version, .options = options});
            graph_.wait_for_all();

            const uint64_t compose_version = ComposeStage::data_version_for(
                design_result_.data_version, tiny_result_.data_version,
                selection_result_.data_version, ruler_result_.data_version, in.overlay_version);

            compose_stage_.try_put({.data = ComposeInput{
                                         .design_frame = design_result_.data,
                                         .tiny_shapes_frame = tiny_result_.data,
                                         .selection_frame = selection_result_.data,
                                         .ruler_frame = ruler_result_.data,
                                         .overlay_picture = in.overlay_picture,
                                     },
                                     .data_version = compose_version,
                                     .options = options});
            graph_.wait_for_all();
            return compose_result_.data.buffer;
        }

    private:
        oneapi::tbb::flow::graph graph_;
        TiledRasterizePictureStage design_rasterize_;
        RasterizePictureStage tiny_rasterize_;
        RasterizePictureStage selection_rasterize_;
        RasterizePictureStage ruler_rasterize_;
        ComposeStage compose_stage_;

        oneapi::tbb::flow::function_node<StageData<RasterizedFrame, PipelineOptions>> design_sink_;
        oneapi::tbb::flow::function_node<StageData<RasterizedFrame, PipelineOptions>> tiny_sink_;
        oneapi::tbb::flow::function_node<StageData<RasterizedFrame, PipelineOptions>> selection_sink_;
        oneapi::tbb::flow::function_node<StageData<RasterizedFrame, PipelineOptions>> ruler_sink_;
        oneapi::tbb::flow::function_node<StageData<RasterizedFrame, PipelineOptions>> compose_sink_;

        StageData<RasterizedFrame, PipelineOptions> design_result_{};
        StageData<RasterizedFrame, PipelineOptions> tiny_result_{};
        StageData<RasterizedFrame, PipelineOptions> selection_result_{};
        StageData<RasterizedFrame, PipelineOptions> ruler_result_{};
        StageData<RasterizedFrame, PipelineOptions> compose_result_{};
    };
}
