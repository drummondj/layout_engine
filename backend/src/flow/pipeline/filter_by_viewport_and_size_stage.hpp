#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../core/shape_generation_stage.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../geometry/geometry.hpp"
#include "../../scene/scene.hpp"
#include <tuple>
#include <vector>

namespace le::flow
{
    /// @brief Fresh, standalone reimplementation of
    /// src/pipeline/stages/filter_by_viewport_and_size_stage.hpp's
    /// FilterByViewportAndSizeStage - same viewport/sub-pixel-size
    /// predicate, same `{scene.viewport_version(), upstream.version()}`
    /// cache key, composing via the same `le::ShapeGenerationStage` base
    /// every generate stage in this experiment derives from (so this one
    /// class serves `GenerateTerminalsOnLayerStage`/
    /// `GenerateObstructionsOnLayerStage`/`GenerateViasOnLayerStage`
    /// uniformly).
    ///
    /// Single-threaded, no internal fan-out (TASKFLOW_EXPERIMENT.md's
    /// per-layer redesign: parallelism now comes from having many small
    /// independent (generate, filter) task pairs, one per physical layer,
    /// not from chunking within a stage - each call here operates on one
    /// already-small per-layer shape list, so there's nothing worth
    /// chunking).
    class FilterByViewportAndSizeStage
    {
    public:
        const std::vector<RenderedShape> &run(const le::ShapeGenerationStage &upstream, const std::vector<RenderedShape> &shapes, const Scene &scene)
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
