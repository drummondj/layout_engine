#pragma once
#include "tiny_shapes_by_viewport_stage.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include <map>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Groups TinyShapesByViewportStage's output by ViewLayerId,
    /// dropping any whose ViewLayer the Scene has hidden - mirrors
    /// FilterByLayerVisibilityStage exactly, but for TinyShapeDot (grouping
    /// by ViewLayerId drops the now-redundant view_layer field per dot,
    /// same convention PixelShape already uses - callers get it from the
    /// map key).
    ///
    /// Key: `{Scene::visibility_version(), upstream
    /// TinyShapesByViewportStage's version()}` - same composition
    /// reasoning as FilterByLayerVisibilityStage's own doc comment.
    class TinyShapesByLayerVisibilityStage
    {
    public:
        const std::map<ViewLayerId, std::vector<Point>> &run(TinyShapesByViewportStage &upstream, const std::vector<TinyShapeDot> &tiny_shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto key = std::tuple{scene.visibility_version(), upstream.version()};
            return stage_.get(key, [&]
            {
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
            });
        }

        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t, uint64_t>, std::map<ViewLayerId, std::vector<Point>>> stage_;
    };
}
