#pragma once
#include "../draw_helpers.hpp"
#include "../pixel_types.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../scene/scene.hpp"
#include <map>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Transforms Pipeline's filtered, ViewLayerId-grouped dbu-space
    /// output into pixel space (Scene's `pixel = (dbu - pan) * scale`).
    /// Root of Renderer's own stage DAG - the `shapes` it transforms comes
    /// from `Pipeline`, outside this module (`render` doesn't link
    /// `pipeline` - see src/core/'s own doc comment), so there's no
    /// upstream Renderer stage to compose this key from.
    ///
    /// Key: `{AbstractId, Scene::viewport_version(), Scene::visibility_version(),
    /// Root::mutation_version()}` - matches Pipeline::filter_by_layer_visibility's
    /// own output-invalidation triggers exactly (pan/scale/viewport-size all
    /// bump viewport_version()).
    class TransformToPixelsStage
    {
    public:
        const std::map<ViewLayerId, std::vector<PixelShape>> &run(const Root &root, const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const Scene &scene)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version(), root.mutation_version()};
            return stage_.get(key, [&]
            {
                return transform_shapes_to_pixel_space(shapes, scene.pan(), scene.scale());
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<AbstractId, uint64_t, uint64_t, uint64_t>, std::map<ViewLayerId, std::vector<PixelShape>>> stage_;
    };
}
