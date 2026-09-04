#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../database/database.hpp"
#include "../draw_helpers.hpp"
#include "../pixel_types.hpp"
#include "../../scene/scene.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "../version_utils.hpp"
#include <map>
#include <vector>

namespace le
{
    /// @brief oneTBB port of src/render/stages/transform_to_pixels_stage.hpp
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - transforms
    /// a filtered, ViewLayerId-grouped dbu-space shape map into pixel space
    /// (`pixel = (dbu - pan) * scale`), reusing draw_helpers.hpp's own
    /// transform_shapes_to_pixel_space unchanged. Root of DesignRenderPipeline's
    /// own design-frame chain.
    ///
    /// The original stage's key was `{AbstractId, Scene::viewport_version(),
    /// Scene::visibility_version(), Root::mutation_version()}`. Two ways to
    /// derive this stage's data_version, both valid: a caller that already
    /// has an AbstractShapePipeline's own shapes_version() may reuse it
    /// directly (it already composes the exact same triggers - AbstractId
    /// identity, epoch, viewport - see AbstractGeometryStage's own doc
    /// comment for why); data_version_for below is the fully independent
    /// derivation (matching the original key component-for-component)
    /// for a caller with no such upstream pipeline to hand, e.g.
    /// FrameRenderPipeline's own top-level orchestration, which
    /// deliberately doesn't link the shape pipelines (mirrors `render`
    /// never linking `pipeline` today).
    class PixelTransformStage : public MemoizingStage<std::map<ViewLayerId, std::vector<RenderedShape>>, std::map<ViewLayerId, std::vector<PixelShape>>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

        static uint64_t data_version_for(AbstractId abstract_id, const PipelineOptions &options)
        {
            return combine_versions(abstract_id.index, abstract_id.generation, options.viewport.viewport_version, options.viewport.visibility_version, options.epoch.root_mutation_version);
        }

    protected:
        std::map<ViewLayerId, std::vector<PixelShape>> compute(const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const PipelineOptions &options) override
        {
            const Scene &scene = *options.ctx.scene;
            return transform_shapes_to_pixel_space(shapes, scene.pan(), scene.scale());
        }
    };
}
