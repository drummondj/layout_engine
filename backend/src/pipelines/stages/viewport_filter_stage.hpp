#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../geometry/geometry.hpp"
#include "../../scene/scene.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include <vector>

namespace le
{
    /// @brief oneTBB port of src/pipeline/stages/filter_by_viewport_and_size_stage.hpp
    /// (Phase 1, backend/ONETBB_INTEGRATION.md migration plan) - drops
    /// shapes outside the Scene's viewport, and shapes whose bbox is under
    /// 1 pixel (at the Scene's scale) in BOTH dimensions - not just one, so
    /// a long thin wire survives even if its width alone is sub-pixel; only
    /// true "invisible dot" shapes are culled. Shapes with no rects/
    /// polygons/paths (e.g. text-only shapes - Geometry::bbox doesn't
    /// account for Shape::texts) have no bbox and are dropped for now.
    ///
    /// Same recompute triggers as the original stage's `{Scene::
    /// viewport_version(), upstream's version()}` key, split across
    /// MemoizingStage's own two independent triggers: the caller sets
    /// `data_version` to the upstream stage's own version() (the
    /// `upstream.version()` half), and options_did_change compares
    /// `options.viewport.viewport_version` (the `scene.viewport_version()`
    /// half) - recomputing whenever either one moves, exactly like the old
    /// std::tuple key did.
    class ViewportFilterStage : public MemoizingStage<std::vector<RenderedShape>, std::vector<RenderedShape>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

    protected:
        std::vector<RenderedShape> compute(const std::vector<RenderedShape> &shapes, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
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
        }

        bool options_did_change(const PipelineOptions &last, const PipelineOptions &current) const override
        {
            return last.viewport.viewport_version != current.viewport.viewport_version;
        }
    };
}
