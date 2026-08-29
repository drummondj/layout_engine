#pragma once
#include "pipeline_options.hpp"
#include "stages/layer_visibility_filter_stage.hpp"
#include "stages/layout_geometry_stage.hpp"
#include "stages/tiny_layer_visibility_filter_stage.hpp"
#include "stages/tiny_viewport_filter_stage.hpp"
#include "stages/viewport_filter_stage.hpp"
#include "tbb_core.hpp"
#include <map>
#include <vector>

namespace le
{
    /// @brief oneTBB flow::graph wiring of the Layout-path shape pipeline
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - the direct
    /// replacement for Pipeline's own Layout-path stage quintet
    /// (generated_layout_/viewport_filtered_layout_/layer_filtered_layout_/
    /// tiny_shapes_viewport_filtered_layout_/tiny_shapes_layer_filtered_layout_).
    /// Same shape as AbstractShapePipeline (see that class's own doc
    /// comment for the wiring diagram and non-copyable/non-movable note) -
    /// LayoutGeometryStage in place of AbstractGeometryStage, but its own
    /// separate instances of ViewportFilterStage/LayerVisibilityFilterStage/
    /// TinyViewportFilterStage/TinyLayerVisibilityFilterStage rather than
    /// sharing AbstractShapePipeline's (those 4 classes have nothing
    /// Abstract- or Layout-specific in their own logic, so the *class* is
    /// shared between the two pipelines per the user's own migration
    /// outline - "stages may be shared between pipelines" - but each
    /// pipeline still owns its own *instance*, since two different
    /// upstream geometry stages can't feed one shared node's single-slot
    /// cache without one evicting the other's result).
    ///
    /// Deliberately does not resolve placed instances (Layout.placements) -
    /// see LayoutGeometryStage's own doc comment; that's HierarchyResolver's
    /// job (Phase 4). HierarchyResolver won't reuse this class's own stage
    /// instances for that - it will own its own separate AbstractGeometryStage/
    /// LayoutGeometryStage members instead, mirroring InstanceRenderer's own
    /// generate_abstract_stage_/generate_layout_stage_ today (see that
    /// class's own doc comment for why: a shared node wired into this
    /// pipeline's graph would cascade into its own viewport/visibility
    /// filters - culled against the real Scene's current viewport, not
    /// appropriate for resolving a whole sub-hierarchy's content - and its
    /// single-slot cache could be evicted mid-call by a recursive call for
    /// a different LayoutId, the same load-bearing correctness constraint
    /// InstanceRenderer's own doc comment describes today).
    class LayoutShapePipeline
    {
    public:
        LayoutShapePipeline()
            : geometry_stage_(graph_, "layout_geometry"),
              viewport_filter_stage_(graph_, "layout_viewport_filter"),
              layer_visibility_filter_stage_(graph_, "layout_layer_visibility_filter"),
              tiny_viewport_filter_stage_(graph_, "layout_tiny_viewport_filter"),
              tiny_layer_visibility_filter_stage_(graph_, "layout_tiny_layer_visibility_filter"),
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

        LayoutShapePipeline(const LayoutShapePipeline &) = delete;
        LayoutShapePipeline &operator=(const LayoutShapePipeline &) = delete;

        const std::map<ViewLayerId, std::vector<RenderedShape>> &run(LayoutId layout_id, const PipelineOptions &options)
        {
            submit(layout_id, options);
            return shapes_result_.data;
        }

        const std::map<ViewLayerId, std::vector<Point>> &run_tiny_shapes(LayoutId layout_id, const PipelineOptions &options)
        {
            submit(layout_id, options);
            return tiny_shapes_result_.data;
        }

        uint64_t shapes_version() const { return shapes_result_.data_version; }
        uint64_t tiny_shapes_version() const { return tiny_shapes_result_.data_version; }

    private:
        void submit(LayoutId layout_id, const PipelineOptions &options)
        {
            ZoneScopedN("LayoutShapePipeline: submit");
            const uint64_t data_version = LayoutGeometryStage::data_version_for(layout_id, options);
            geometry_stage_.try_put({.data = layout_id, .data_version = data_version, .options = options});
            graph_.wait_for_all();
        }

        oneapi::tbb::flow::graph graph_;
        LayoutGeometryStage geometry_stage_;
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
