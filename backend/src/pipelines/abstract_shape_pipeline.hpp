#pragma once
#include "pipeline_options.hpp"
#include "stages/abstract_geometry_stage.hpp"
#include "stages/layer_visibility_filter_stage.hpp"
#include "stages/tiny_layer_visibility_filter_stage.hpp"
#include "stages/tiny_viewport_filter_stage.hpp"
#include "stages/viewport_filter_stage.hpp"
#include "tbb_core.hpp"
#include <map>
#include <vector>

namespace le
{
    /// @brief oneTBB flow::graph wiring of the Abstract-path shape
    /// pipeline (Phase 2, backend/ONETBB_INTEGRATION.md migration plan) -
    /// the direct replacement for Pipeline's own Abstract-path stage
    /// quintet (generated_abstract_/viewport_filtered_abstract_/
    /// layer_filtered_abstract_/tiny_shapes_viewport_filtered_abstract_/
    /// tiny_shapes_layer_filtered_abstract_, see src/pipeline/pipeline.hpp).
    /// Exposes the same run()/run_tiny_shapes() surface Pipeline has today
    /// (user's own migration outline item 1.1/1.2: "Separate pipelines for
    /// Abstract view shape generation").
    ///
    ///   AbstractGeometryStage -> ViewportFilterStage -> LayerVisibilityFilterStage        (run())
    ///                         -> TinyViewportFilterStage -> TinyLayerVisibilityFilterStage (run_tiny_shapes())
    ///
    /// AbstractGeometryStage fans out to both downstream chains (two
    /// make_edge calls off its one node - mirrors Pipeline's own
    /// tiny_shapes_by_viewport() calling generate_shapes() first). Owns one
    /// flow::graph and one instance of each of the five stages - construct
    /// one per Scene-equivalent lifetime and reuse it across repeated
    /// calls, same as Pipeline itself. Non-copyable/non-movable (each
    /// stage's own node captures `this`, so neither this class nor its
    /// stage members may move after construction - same constraint
    /// Pipeline/Renderer/InstanceRenderer already have via their own
    /// VersionedStage members, just made explicit here since
    /// MemoizingStage's copy ctor is deleted rather than merely
    /// discouraged).
    class AbstractShapePipeline
    {
    public:
        AbstractShapePipeline()
            : geometry_stage_(graph_, "abstract_geometry"),
              viewport_filter_stage_(graph_, "abstract_viewport_filter"),
              layer_visibility_filter_stage_(graph_, "abstract_layer_visibility_filter"),
              tiny_viewport_filter_stage_(graph_, "abstract_tiny_viewport_filter"),
              tiny_layer_visibility_filter_stage_(graph_, "abstract_tiny_layer_visibility_filter"),
              shapes_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<std::map<ViewLayerId, std::vector<RenderedShape>>, PipelineOptions> in)
                           { shapes_result_ = std::move(in); }),
              tiny_shapes_sink_(graph_, oneapi::tbb::flow::serial, [this](StageData<std::map<ViewLayerId, std::vector<Point>>, PipelineOptions> in)
                                { tiny_shapes_result_ = std::move(in); })
        {
            using namespace oneapi::tbb::flow;
            make_edge(geometry_stage_.node(), viewport_filter_stage_.node());
            make_edge(geometry_stage_.node(), tiny_viewport_filter_stage_.node());
            make_edge(viewport_filter_stage_.node(), layer_visibility_filter_stage_.node());
            make_edge(tiny_viewport_filter_stage_.node(), tiny_layer_visibility_filter_stage_.node());
            make_edge(layer_visibility_filter_stage_.node(), shapes_sink_);
            make_edge(tiny_layer_visibility_filter_stage_.node(), tiny_shapes_sink_);
        }

        AbstractShapePipeline(const AbstractShapePipeline &) = delete;
        AbstractShapePipeline &operator=(const AbstractShapePipeline &) = delete;

        /// @brief Runs the design-view chain (AbstractGeometryStage ->
        /// ViewportFilterStage -> LayerVisibilityFilterStage) for
        /// `abstract_id`, submitting synchronously (try_put + wait_for_all)
        /// and returning the final grouped-by-ViewLayerId result - mirrors
        /// Pipeline::run's own role.
        const std::map<ViewLayerId, std::vector<RenderedShape>> &run(AbstractId abstract_id, const PipelineOptions &options)
        {
            submit(abstract_id, options);
            return shapes_result_.data;
        }

        /// @brief Runs the sub-pixel-dot chain (AbstractGeometryStage ->
        /// TinyViewportFilterStage -> TinyLayerVisibilityFilterStage) for
        /// `abstract_id` - mirrors Pipeline::run_tiny_shapes's own role.
        const std::map<ViewLayerId, std::vector<Point>> &run_tiny_shapes(AbstractId abstract_id, const PipelineOptions &options)
        {
            submit(abstract_id, options);
            return tiny_shapes_result_.data;
        }

        // Exposed purely to make cache hits/misses observable in tests -
        // each MemoizingStage's own version() isn't public (unlike
        // core::VersionedStage's), so the last-received output's own
        // data_version (bumped only on a real recompute, see
        // MemoizingStage::execute) is the only externally observable
        // recompute signal.
        uint64_t shapes_version() const { return shapes_result_.data_version; }
        uint64_t tiny_shapes_version() const { return tiny_shapes_result_.data_version; }

    private:
        // Submits abstract_id to the head of both chains and blocks until
        // every downstream node has finished - both sinks below are
        // up to date by the time this returns, regardless of which run()/
        // run_tiny_shapes() overload the caller used (the input always
        // flows through the whole graph; only the fan-out edge taken next
        // differs by stage, not by which sink the caller reads from).
        void submit(AbstractId abstract_id, const PipelineOptions &options)
        {
            const uint64_t data_version = AbstractGeometryStage::data_version_for(abstract_id, options);
            geometry_stage_.try_put({.data = abstract_id, .data_version = data_version, .options = options});
            graph_.wait_for_all();
        }

        oneapi::tbb::flow::graph graph_;
        AbstractGeometryStage geometry_stage_;
        ViewportFilterStage viewport_filter_stage_;
        LayerVisibilityFilterStage layer_visibility_filter_stage_;
        TinyViewportFilterStage tiny_viewport_filter_stage_;
        TinyLayerVisibilityFilterStage tiny_layer_visibility_filter_stage_;

        oneapi::tbb::flow::function_node<StageData<std::map<ViewLayerId, std::vector<RenderedShape>>, PipelineOptions>> shapes_sink_;
        oneapi::tbb::flow::function_node<StageData<std::map<ViewLayerId, std::vector<Point>>, PipelineOptions>> tiny_shapes_sink_;

        StageData<std::map<ViewLayerId, std::vector<RenderedShape>>, PipelineOptions> shapes_result_{};
        StageData<std::map<ViewLayerId, std::vector<Point>>, PipelineOptions> tiny_shapes_result_{};
    };
}
