#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../core/shape_generation_stage.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../geometry/geometry.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include <tuple>
#include <vector>

namespace le
{
    /// @brief The population FilterByViewportAndSizeStage *drops* for
    /// being under 1 pixel in both dimensions (UPDATES.md item 6) - one
    /// TinyShapeDot per such shape (its bbox center), instead of nothing,
    /// so the caller can render a single-pixel fallback rather than have
    /// the shape silently vanish when zoomed out. Deliberately a separate
    /// stage over the same upstream output, not a second return value
    /// bolted onto FilterByViewportAndSizeStage: keeps that stage's own
    /// signature (and everything downstream of it -
    /// hit_test_point/hit_test_rect, existing tests) untouched, at the
    /// cost of a second pass over the upstream's output - cheap relative
    /// to that stage itself (no Boost calls here, only bbox/overlap
    /// arithmetic, the same per-shape cost FilterByViewportAndSizeStage
    /// already pays - see BENCHMARKS.md for the measured cost). Mirrors
    /// FilterByViewportAndSizeStage's own viewport-overlap check exactly
    /// (same bbox, same viewport rect, same Geometry::rects_overlap call)
    /// so the two stages can never disagree about which shapes are "tiny"
    /// vs "normal" - only the size-threshold branch differs.
    ///
    /// Takes the already-computed `shapes` as an external parameter, same
    /// as FilterByViewportAndSizeStage (not calling the upstream stage's
    /// own run() internally, as an earlier Abstract-only version of this
    /// class once did) - required once Pipeline gained a second (Layout)
    /// path, since the upstream's run() takes a different id type
    /// (AbstractId vs LayoutId) per path; taking pre-computed shapes plus
    /// just the upstream's version() (via the shared ShapeGenerationStage
    /// base) lets this one class serve both paths unchanged, same
    /// reasoning as FilterByViewportAndSizeStage's own.
    class TinyShapesByViewportStage
    {
    public:
        const std::vector<TinyShapeDot> &run(const ShapeGenerationStage &upstream, const std::vector<RenderedShape> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
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

                std::vector<TinyShapeDot> result;

                for (const auto &s : shapes)
                {
                    auto bbox = Geometry::bbox(s.shape);
                    if (!bbox)
                        continue;

                    if (!Geometry::rects_overlap(*bbox, viewport))
                        continue;

                    const double width = static_cast<double>(bbox->ur.x - bbox->ll.x);
                    const double height = static_cast<double>(bbox->ur.y - bbox->ll.y);
                    if (!(width < min_visible_dbu && height < min_visible_dbu))
                        continue;

                    result.push_back(TinyShapeDot{
                        .location = Point{(bbox->ll.x + bbox->ur.x) / 2, (bbox->ll.y + bbox->ur.y) / 2},
                        .view_layer = s.view_layer,
                    });
                }

                return result;
            });
        }

        uint64_t version() const { return stage_.version(); }
        uint64_t call_count() const { return stage_.call_count(); }

    private:
        VersionedStage<std::tuple<uint64_t, uint64_t>, std::vector<TinyShapeDot>> stage_;
    };
}
