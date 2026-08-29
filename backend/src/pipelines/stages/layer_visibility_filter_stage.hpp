#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include <map>
#include <unordered_map>
#include <vector>

namespace le
{
    /// @brief The actual per-ViewLayerId visibility filter/grouping, factored
    /// out as a plain function (not a MemoizingStage method) so a caller
    /// that has no use for caching - HierarchyResolver::record_local_picture
    /// (hierarchy_resolver.hpp) constructs one of these per recursive call
    /// and uses it exactly once - can run the filter directly, with no
    /// tbb::flow::graph involved at all. See that call site's own comment
    /// for why: a fresh single-shot flow::graph purely as a call adapter,
    /// repeated through a recursive descent, was the prime suspect for
    /// BUGS_AND_ENHANCEMENTS.md B2's TBB nested-graph race - this removes
    /// the graph from that path entirely rather than relying on it being
    /// safe. Groups surviving shapes by ViewLayerId, dropping any whose
    /// ViewLayer the Scene has hidden. std::map (not unordered_map) is
    /// deliberate - ViewLayerId's ordering matches LEF-declared layer
    /// stacking order, and callers rely on iterating this result in that
    /// order. Per-ViewLayerId visibility is decided once and cached in
    /// `visible_cache`, rather than grouping every shape first and then
    /// erasing back out whichever ViewLayerIds turned out hidden (this
    /// function's own earlier shape) - visibility never varies within one
    /// ViewLayerId's own shapes, so there's nothing to gain from building
    /// the group before knowing whether to keep it, and a hidden ViewLayer
    /// (BUGS_AND_ENHANCEMENTS.md E2 made TRACK_*/ROW/GCELLGRID hidden by
    /// default) can now be the common case on a real design, not the rare
    /// one the erase-based version used to assume.
    inline std::map<ViewLayerId, std::vector<RenderedShape>> filter_shapes_by_layer_visibility(const std::vector<RenderedShape> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
    {
        std::unordered_map<ViewLayerId, bool> visible_cache;
        std::map<ViewLayerId, std::vector<RenderedShape>> grouped;
        for (const auto &rs : shapes)
        {
            auto [it, inserted] = visible_cache.try_emplace(rs.view_layer, true);
            if (inserted)
            {
                // Scene's visibility is keyed by (layer name, purpose) -
                // not ViewLayerId directly - so an unresolved ViewLayerId
                // (no ViewLayerData behind it) has no visibility toggle to
                // check, same as before.
                const ViewLayerData *data = view_layers.get(rs.view_layer);
                it->second = !data || scene.is_view_layer_visible(data->layer_name, data->purpose);
            }
            if (it->second)
                grouped[rs.view_layer].push_back(rs);
        }

        return grouped;
    }

    /// @brief oneTBB port of src/pipeline/stages/filter_by_layer_visibility_stage.hpp
    /// (Phase 2, backend/ONETBB_INTEGRATION.md migration plan) - the
    /// MemoizingStage/flow::graph wrapper around
    /// filter_shapes_by_layer_visibility above, for pipeline call sites
    /// that actually benefit from caching (AbstractShapePipeline/
    /// LayoutShapePipeline's own persistent graphs).
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
            return filter_shapes_by_layer_visibility(shapes, *options.ctx.scene, *options.ctx.view_layers);
        }

        bool options_did_change(const PipelineOptions &last, const PipelineOptions &current) const override
        {
            return last.viewport.visibility_version != current.viewport.visibility_version;
        }
    };
}
