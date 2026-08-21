#pragma once
#include "filter_by_viewport_and_size_stage.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include <map>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Groups surviving shapes by ViewLayerId, dropping any whose
    /// ViewLayer the Scene has hidden - a shape whose ViewLayerId is
    /// invalid (its layer_name didn't resolve to a known Layer) is kept,
    /// grouped under that invalid id - there's no visibility toggle to
    /// check it against. Visibility is checked once per distinct
    /// ViewLayerId actually present, not once per shape.
    ///
    /// std::map (not unordered_map) is deliberate: ViewLayerId's natural
    /// ordering (its {index, generation} via the defaulted operator<=>)
    /// matches LEF-declared physical layer stacking order exactly -
    /// ViewLayerSet::build_for_technology creates each Layer's TERMINAL
    /// then OBSTRUCTION ViewLayer while iterating Root::get_technology_layers
    /// in LEF declaration order (bottom-up: M1, M2, ...), with BOUNDARY
    /// created last (highest index). So iterating this map in key order -
    /// which callers drawing it are expected to do - draws bottom-up with
    /// the boundary outline on top, with no separate sort/ordering step
    /// needed.
    ///
    /// Key: `{Scene::visibility_version(), upstream
    /// FilterByViewportAndSizeStage's version()}` - same composition
    /// reasoning as that stage's own doc comment.
    class FilterByLayerVisibilityStage
    {
    public:
        const std::map<ViewLayerId, std::vector<RenderedShape>> &run(FilterByViewportAndSizeStage &upstream, const std::vector<RenderedShape> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto key = std::tuple{scene.visibility_version(), upstream.version()};
            return stage_.get(key, [&]
            {
                std::map<ViewLayerId, std::vector<RenderedShape>> grouped;
                for (const auto &rs : shapes)
                    grouped[rs.view_layer].push_back(rs);

                for (auto it = grouped.begin(); it != grouped.end();)
                {
                    // Scene's visibility is keyed by (layer name, purpose) -
                    // not ViewLayerId directly (see its own comment) - so an
                    // unresolved ViewLayerId (no ViewLayerData behind it)
                    // has no visibility toggle to check, same as before.
                    const ViewLayerData *data = view_layers.get(it->first);
                    if (data && !scene.is_view_layer_visible(data->layer_name, data->purpose))
                        it = grouped.erase(it);
                    else
                        ++it;
                }

                return grouped;
            });
        }

        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t, uint64_t>, std::map<ViewLayerId, std::vector<RenderedShape>>> stage_;
    };
}
