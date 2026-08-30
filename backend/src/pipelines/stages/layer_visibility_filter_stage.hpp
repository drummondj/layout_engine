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
    /// @brief oneTBB port of src/pipeline/stages/filter_by_layer_visibility_stage.hpp
    /// (Phase 2, backend/ONETBB_INTEGRATION.md migration plan) - groups
    /// surviving shapes by ViewLayerId, dropping any whose ViewLayer the
    /// Scene has hidden. See the original stage's own doc comment for why
    /// std::map (not unordered_map) is deliberate (ViewLayerId's ordering
    /// matches LEF-declared layer stacking order).
    ///
    /// Wired downstream of ViewportFilterStage via make_edge - its own
    /// data_version arrives automatically as whatever ViewportFilterStage's
    /// node last emitted (that stage's own version(), bumped only on a
    /// real recompute), matching the original stage's key composing via
    /// `upstream FilterByViewportAndSizeStage's version()`. options_did_change
    /// compares `viewport.visibility_version`, the equivalent of the
    /// original key's `Scene::visibility_version()` component.
    class LayerVisibilityFilterStage : public MemoizingStage<std::vector<RenderedShape>, std::map<ViewLayerId, std::vector<RenderedShape>>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

    protected:
        std::map<ViewLayerId, std::vector<RenderedShape>> compute(const std::vector<RenderedShape> &shapes, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            const ViewLayerSet &view_layers = *options.ctx.view_layers;

            std::map<ViewLayerId, std::vector<RenderedShape>> grouped;
            for (const auto &rs : shapes)
                grouped[rs.view_layer].push_back(rs);

            for (auto it = grouped.begin(); it != grouped.end();)
            {
                // Scene's visibility is keyed by (layer name, purpose) -
                // not ViewLayerId directly - so an unresolved ViewLayerId
                // (no ViewLayerData behind it) has no visibility toggle to
                // check, same as before.
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
