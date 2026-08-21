#pragma once
#include "../pixel_types.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../scene/scene.hpp"
#include <map>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Transforms Pipeline::tiny_shapes_by_layer_visibility's
    /// output to pixel space - the same per-point `to_pixel` transform
    /// TransformToPixelsStage already applies to every rect/polygon/path
    /// point, just for the single-point-per-shape case (UPDATES.md item 6).
    /// Root of its own small chain (mirrors TransformToPixelsStage - see
    /// its own doc comment for why there's no upstream Renderer stage to
    /// compose from).
    ///
    /// Key: same shape as TransformToPixelsStage's own.
    class TransformTinyShapesToPixelsStage
    {
    public:
        const std::map<ViewLayerId, std::vector<PixelPoint>> &run(const Root &root, const std::map<ViewLayerId, std::vector<Point>> &tiny_shapes, const Scene &scene)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version(), root.mutation_version()};
            return stage_.get(key, [&]
            {
                const Point pan = scene.pan();
                const double scale = scene.scale();
                auto to_pixel = [&](Point p)
                {
                    return PixelPoint{
                        .x = (static_cast<double>(p.x) - static_cast<double>(pan.x)) * scale,
                        .y = (static_cast<double>(p.y) - static_cast<double>(pan.y)) * scale,
                    };
                };

                std::map<ViewLayerId, std::vector<PixelPoint>> result;

                for (const auto &[view_layer, group] : tiny_shapes)
                {
                    std::vector<PixelPoint> pixel_group;
                    pixel_group.reserve(group.size());
                    for (const Point &p : group)
                        pixel_group.push_back(to_pixel(p));
                    result.emplace(view_layer, std::move(pixel_group));
                }

                return result;
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<AbstractId, uint64_t, uint64_t, uint64_t>, std::map<ViewLayerId, std::vector<PixelPoint>>> stage_;
    };
}
