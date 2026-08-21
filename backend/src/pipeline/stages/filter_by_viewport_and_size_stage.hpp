#pragma once
#include "generate_shapes_stage.hpp"
#include "../../core/rendered_shape.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../geometry/geometry.hpp"
#include "../../scene/scene.hpp"
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Drops shapes outside the Scene's viewport, and shapes whose
    /// bbox is under 1 pixel (at the Scene's scale) in BOTH dimensions -
    /// not just one, so a long thin wire survives even if its width alone
    /// is sub-pixel; only true "invisible dot" shapes are culled. Shapes
    /// with no rects/polygons/paths (e.g. text-only shapes -
    /// Geometry::bbox doesn't account for Shape::texts) have no bbox and
    /// are dropped for now; text rendering isn't subject to this kind of
    /// culling anyway and needs its own handling later.
    ///
    /// Key: `{Scene::viewport_version(), upstream GenerateShapesStage's
    /// version()}` - composing via the upstream stage's own version (see
    /// VersionedStage's own doc comment) instead of manually re-deriving
    /// everything GenerateShapesStage's key depends on
    /// (ViewLayerSet::generation(), Root::mutation_version()); this stage
    /// never reads Root/ViewLayerSet directly (`shapes` is already fully
    /// resolved), so composing is strictly more correct, not just
    /// shorter - the old hand-written key had to be manually kept in sync
    /// with GenerateShapesStage's own every time *that* stage's triggers
    /// changed, which is exactly the class of bug TCL_EXPLORATION.md
    /// describes.
    class FilterByViewportAndSizeStage
    {
    public:
        const std::vector<RenderedShape> &run(GenerateShapesStage &upstream, const std::vector<RenderedShape> &shapes, const Scene &scene)
        {
            const auto key = std::tuple{scene.viewport_version(), upstream.version()};
            return stage_.get(key, [&]
            {
                const double scale = scene.scale();
                const double min_visible_dbu = 1.0 / scale;

                const Point viewport_ll = scene.pan();
                const Rect viewport{
                    .ll = viewport_ll,
                    .ur = Point{
                        viewport_ll.x + static_cast<int64_t>(scene.viewport_width_px() / scale),
                        viewport_ll.y + static_cast<int64_t>(scene.viewport_height_px() / scale),
                    },
                };

                std::vector<RenderedShape> result;
                result.reserve(shapes.size());

                for (const auto &s : shapes)
                {
                    auto bbox = Geometry::bbox(s.shape);
                    if (!bbox)
                        continue;

                    if (!Geometry::rects_overlap(*bbox, viewport))
                        continue;

                    const double width = static_cast<double>(bbox->ur.x - bbox->ll.x);
                    const double height = static_cast<double>(bbox->ur.y - bbox->ll.y);
                    if (width < min_visible_dbu && height < min_visible_dbu)
                        continue;

                    result.push_back(s);
                }

                return result;
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t, uint64_t>, std::vector<RenderedShape>> stage_;
    };
}
