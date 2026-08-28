#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include <map>
#include <vector>

namespace le
{
    /// @brief oneTBB port of src/pipeline/stages/tiny_shapes_by_layer_visibility_stage.hpp
    /// (Phase 2, backend/ONETBB_INTEGRATION.md migration plan) - groups
    /// TinyViewportFilterStage's output by ViewLayerId, dropping any whose
    /// ViewLayer the Scene has hidden - mirrors LayerVisibilityFilterStage
    /// exactly, but for TinyShapeDot (grouping by ViewLayerId drops the
    /// now-redundant view_layer field per dot).
    ///
    /// Wired downstream of TinyViewportFilterStage via make_edge; its own
    /// data_version arrives as that stage's own version(). options_did_change
    /// compares `viewport.visibility_version`, same as LayerVisibilityFilterStage.
    class TinyLayerVisibilityFilterStage : public MemoizingStage<std::vector<TinyShapeDot>, std::map<ViewLayerId, std::vector<Point>>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

    protected:
        std::map<ViewLayerId, std::vector<Point>> compute(const std::vector<TinyShapeDot> &tiny_shapes, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            const ViewLayerSet &view_layers = *options.ctx.view_layers;

            std::map<ViewLayerId, std::vector<Point>> grouped;
            for (const auto &dot : tiny_shapes)
                grouped[dot.view_layer].push_back(dot.location);

            for (auto it = grouped.begin(); it != grouped.end();)
            {
                const ViewLayerData *data = view_layers.get(it->first);
                if (data && !scene.is_view_layer_visible(data->layer_name, data->purpose))
                    it = grouped.erase(it);
                else
                    ++it;
            }

            return grouped;
        }

        bool options_did_change(const PipelineOptions &last, const PipelineOptions &current) const override
        {
            return last.viewport.visibility_version != current.viewport.visibility_version;
        }
    };
}
