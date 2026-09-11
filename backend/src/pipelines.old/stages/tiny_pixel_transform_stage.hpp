#pragma once
#include "../../database/database.hpp"
#include "../pixel_types.hpp"
#include "../../scene/scene.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "../version_utils.hpp"
#include <map>
#include <vector>

namespace le
{
    /// @brief oneTBB port of src/render/stages/transform_tiny_shapes_to_pixels_stage.hpp
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - transforms
    /// the tiny-shape-dot map to pixel space, the same per-point transform
    /// PixelTransformStage applies for the single-point-per-shape case.
    /// Root of DesignRenderPipeline's own tiny-shapes-frame chain.
    ///
    /// Same key shape as PixelTransformStage - see that class's own doc
    /// comment.
    class TinyPixelTransformStage : public MemoizingStage<std::map<ViewLayerId, std::vector<Point>>, std::map<ViewLayerId, std::vector<PixelPoint>>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

        static uint64_t data_version_for(AbstractId abstract_id, const PipelineOptions &options)
        {
            return combine_versions(abstract_id.index, abstract_id.generation, options.viewport.viewport_version, options.viewport.visibility_version, options.epoch.root_mutation_version);
        }

    protected:
        std::map<ViewLayerId, std::vector<PixelPoint>> compute(const std::map<ViewLayerId, std::vector<Point>> &tiny_shapes, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
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
        }
    };
}
