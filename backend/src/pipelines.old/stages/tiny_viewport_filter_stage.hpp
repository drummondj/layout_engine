#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../geometry/geometry.hpp"
#include "../../scene/scene.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include <vector>

namespace le
{
    /// @brief oneTBB port of src/pipeline/stages/tiny_shapes_by_viewport_stage.hpp
    /// (Phase 2, backend/ONETBB_INTEGRATION.md migration plan) - the
    /// population ViewportFilterStage's original (FilterByViewportAndSizeStage)
    /// *drops* for being under 1 pixel in both dimensions, one TinyShapeDot
    /// per such shape (its bbox center) instead of nothing. See the
    /// original stage's own doc comment for why this is a separate stage
    /// over the same upstream output rather than a second return value
    /// bolted onto ViewportFilterStage, and why its own viewport-overlap
    /// check must always match ViewportFilterStage's exactly.
    ///
    /// Wired directly off AbstractGeometryStage/LayoutGeometryStage's own
    /// node (fan-out - the same upstream shapes feed both this stage and
    /// ViewportFilterStage), so its data_version arrives as whatever that
    /// stage's node last emitted. options_did_change compares
    /// `viewport.viewport_version`, same as ViewportFilterStage.
    class TinyViewportFilterStage : public MemoizingStage<std::vector<RenderedShape>, std::vector<TinyShapeDot>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

    protected:
        std::vector<TinyShapeDot> compute(const std::vector<RenderedShape> &shapes, const PipelineOptions &options) override
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
        }

        bool options_did_change(const PipelineOptions &last, const PipelineOptions &current) const override
        {
            return last.viewport.viewport_version != current.viewport.viewport_version;
        }
    };
}
