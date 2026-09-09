#pragma once

#include "pipeline_options.hpp"
#include "stages/compose_stage.hpp"
#include "stages/hierarchy_resolver_stage.hpp"
#include "stages/layer_generation_stage.hpp"
#include "stages/rasterize_blend2d_stage.hpp"
#include "stages/rasterize_stage.hpp"
#include "stages/viewport_cull_stage.hpp"
#include "tbb_core.hpp"

#include <string>
#include <utility>

namespace le
{
    /// @brief PIPELINE_REFACTOR.md's own ViewRenderPipeline - one shared
    /// oneapi::tbb::flow::graph wiring every tier's stages together with
    /// real make_edge connections, not the per-stage-private-graph
    /// SynchronousStageRunner pattern tests/benchmarks use to exercise one
    /// stage in isolation. Wires the full Cold+Warm chain: LayerGenerationStage
    /// -> HierarchyResolverStage -> ViewportCullStage -> RasterizeStage ->
    /// ComposeStage - a strict linear chain (every stage's own OutputHandle
    /// type matches the next stage's own InputData exactly), each with a
    /// sink so a caller can read that stage's own result back after
    /// wait_for_all() even on a call where a downstream stage alone needed
    /// to recompute. Hot tier stages get added to this same graph/class
    /// later - one pipeline for the whole thing, not a separate class per
    /// tier.
    ///
    /// One entry point into this one graph: run() submits at
    /// layer_generation_.node() and the existing make_edge chain carries
    /// that single message all the way through to compose_.node() in one
    /// try_put/wait_for_all - the whole graph was always wired this way
    /// end to end; what used to force two separate submissions
    /// (run_cold() then run_warm()) was RasterizeStage's own dependency on
    /// the ViewLayerSet LayerGenerationStage computes, which had nowhere
    /// to travel to but ViewRenderOptions::view_layers - a field every
    /// stage in one submission sees the exact same, caller-supplied copy
    /// of (StageData<T, Options>'s own contract), so a later stage could
    /// never see a value an earlier stage in that same submission had just
    /// computed. Fixed at the source instead: HierarchyResolverStage's own
    /// OutputData now echoes the ViewLayerSetHandle it received as input
    /// back out as one of its own fields (HierarchyResolverOutput::
    /// view_layers), and ViewportCullStage passes it through unchanged -
    /// so it now arrives at RasterizeStage as part of `data`, the same way
    /// every other stage's own real dependency does, and one submission
    /// through the whole chain is enough.
    ///
    /// The Root pointer every stage needs still travels via
    /// ViewRenderOptions::root, not any one stage's own InputData - run()
    /// sets it from its own `root` parameter, so a caller never has to set
    /// it independently.
    ///
    /// Templated on the Rasterize stage implementation (`RasterizeStageT`,
    /// default `RasterizeStage` - Skia) so a second, real backend
    /// (`RasterizeBlend2DStage` - rasterize_blend2d_stage.hpp, a side
    /// experiment benchmarked against Skia's own CPU rasterizer,
    /// PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) can be wired into this
    /// exact same graph shape at construction time via
    /// `ViewRenderPipelineBlend2D` (defined below) with zero code
    /// duplication - both stage types share the exact same
    /// `MemoizingStage<HierarchyResolverStage::OutputHandle, RasterizeOutput,
    /// ViewRenderOptions>` template shape, so `RasterizeStageT::OutputHandle`
    /// is the identical `RasterizeOutputHandle` (rasterize_output.hpp)
    /// either way and `ComposeStage` (already typed against
    /// `RasterizeOutputHandle` directly, not against either concrete
    /// stage) needs no changes at all. `ViewRenderPipeline` itself (the
    /// plain, non-template name every existing caller already uses) is
    /// just a type alias to `ViewRenderPipelineImpl<>` below - unchanged
    /// behavior for every pre-existing use.
    template <typename RasterizeStageT = RasterizeStage>
    class ViewRenderPipelineImpl
    {
    public:
        /// @brief The full chain's own combined output - every intermediate
        /// stage's own result, not just the final `frame`, so a caller
        /// (or a test) can inspect what actually survived culling/
        /// rasterization without re-deriving it independently.
        struct WarmOutput
        {
            LayerGenerationStage::OutputHandle view_layers;
            HierarchyResolverStage::OutputHandle hierarchy; // Cold's own unculled output
            HierarchyResolverStage::OutputHandle culled;    // ViewportCullStage's own output
            typename RasterizeStageT::OutputHandle rasterized;
            ComposeStage::OutputHandle frame;
        };

        explicit ViewRenderPipelineImpl(std::string label = "ViewRenderPipeline")
            : layer_generation_(graph_, label + ".LayerGeneration"),
              hierarchy_resolver_(graph_, label + ".HierarchyResolver"),
              viewport_cull_(graph_, label + ".ViewportCull"),
              rasterize_(graph_, label + ".Rasterize"),
              compose_(graph_, label + ".Compose"),
              layer_generation_sink_(
                  graph_, oneapi::tbb::flow::serial,
                  [this](StageData<LayerGenerationStage::OutputHandle, ViewRenderOptions> in)
                  { layer_generation_result_ = std::move(in); }),
              hierarchy_resolver_sink_(
                  graph_, oneapi::tbb::flow::serial,
                  [this](StageData<HierarchyResolverStage::OutputHandle, ViewRenderOptions> in)
                  { hierarchy_resolver_result_ = std::move(in); }),
              viewport_cull_sink_(
                  graph_, oneapi::tbb::flow::serial,
                  [this](StageData<HierarchyResolverStage::OutputHandle, ViewRenderOptions> in)
                  { viewport_cull_result_ = std::move(in); }),
              rasterize_sink_(
                  graph_, oneapi::tbb::flow::serial,
                  [this](StageData<typename RasterizeStageT::OutputHandle, ViewRenderOptions> in)
                  { rasterize_result_ = std::move(in); }),
              compose_sink_(
                  graph_, oneapi::tbb::flow::serial,
                  [this](StageData<ComposeStage::OutputHandle, ViewRenderOptions> in)
                  { compose_result_ = std::move(in); })
        {
            make_edge(layer_generation_.node(), layer_generation_sink_);
            make_edge(layer_generation_.node(), hierarchy_resolver_.node());
            make_edge(hierarchy_resolver_.node(), hierarchy_resolver_sink_);
            make_edge(hierarchy_resolver_.node(), viewport_cull_.node());
            make_edge(viewport_cull_.node(), viewport_cull_sink_);
            make_edge(viewport_cull_.node(), rasterize_.node());
            make_edge(rasterize_.node(), rasterize_sink_);
            make_edge(rasterize_.node(), compose_.node());
            make_edge(compose_.node(), compose_sink_);
        }

        ViewRenderPipelineImpl(const ViewRenderPipelineImpl &) = delete;
        ViewRenderPipelineImpl &operator=(const ViewRenderPipelineImpl &) = delete;

        /// @brief Runs the full Cold+Warm chain for `root` under `options`
        /// (`options.root` is overwritten with `root` here - a caller only
        /// has to set the fields that actually vary: root_mutation_version/
        /// top_level/hierarchy_depth/viewport/scale) in exactly one
        /// try_put/wait_for_all - see the class's own doc comment for how
        /// RasterizeStage's own ViewLayerSet dependency, the thing that
        /// used to force two separate submissions here, now travels
        /// through `data` instead of `options`. No data_version parameter,
        /// unlike SynchronousStageRunner::run() - no stage's own recompute
        /// decision ever looks at one (all five rely entirely on
        /// options_did_change() plus the previous stage's own version(),
        /// see each stage's own doc comment), so there is nothing
        /// meaningful for a caller to thread through here; exposing one
        /// would only invite a caller to accidentally force recomputation
        /// by bumping it for an unrelated reason.
        ///
        /// Skips try_put/wait_for_all entirely when no stage in the whole
        /// chain would recompute - see MemoizingStage::would_recompute()'s
        /// own doc comment (tbb_core.hpp) for why that's load-bearing, not
        /// just a nicety, even on a guaranteed cache hit (300-600ms of
        /// pure TBB message-passing/scheduling overhead on a real
        /// ~478,000-shape Layout, measured before that method existed -
        /// paying that on every steady-state pan/zoom tick, when nothing
        /// changed at all, is exactly the cost this guard exists to avoid).
        /// Cascaded across all five stages in dependency order, same
        /// reasoning run_cold()/run_warm() each used on their own half of
        /// the chain before they were merged into this one method: an
        /// earlier stage's own future recompute isn't yet a real, bumped
        /// version() before it actually runs, so it has to be assumed to
        /// force every later stage's own data_version to change too rather
        /// than checked against a version number that doesn't exist yet.
        WarmOutput run(const Root *root, ViewRenderOptions options)
        {
            options.root = root;

            const bool layer_generation_would_recompute = layer_generation_.would_recompute(0, options);
            const bool hierarchy_resolver_would_recompute =
                layer_generation_would_recompute || hierarchy_resolver_.would_recompute(layer_generation_.version(), options);
            const bool viewport_cull_would_recompute =
                hierarchy_resolver_would_recompute || viewport_cull_.would_recompute(hierarchy_resolver_.version(), options);
            const bool rasterize_would_recompute =
                viewport_cull_would_recompute || rasterize_.would_recompute(viewport_cull_.version(), options);
            const bool compose_would_recompute =
                rasterize_would_recompute || compose_.would_recompute(rasterize_.version(), options);

            if (compose_would_recompute)
            {
                layer_generation_.try_put({.data = root, .data_version = 0, .options = options});
                graph_.wait_for_all();
            }

            return WarmOutput{
                .view_layers = layer_generation_result_.data,
                .hierarchy = hierarchy_resolver_result_.data,
                .culled = viewport_cull_result_.data,
                .rasterized = rasterize_result_.data,
                .frame = compose_result_.data,
            };
        }

    private:
        oneapi::tbb::flow::graph graph_;
        LayerGenerationStage layer_generation_;
        HierarchyResolverStage hierarchy_resolver_;
        ViewportCullStage viewport_cull_;
        RasterizeStageT rasterize_;
        ComposeStage compose_;
        oneapi::tbb::flow::function_node<StageData<LayerGenerationStage::OutputHandle, ViewRenderOptions>> layer_generation_sink_;
        oneapi::tbb::flow::function_node<StageData<HierarchyResolverStage::OutputHandle, ViewRenderOptions>> hierarchy_resolver_sink_;
        oneapi::tbb::flow::function_node<StageData<HierarchyResolverStage::OutputHandle, ViewRenderOptions>> viewport_cull_sink_;
        oneapi::tbb::flow::function_node<StageData<typename RasterizeStageT::OutputHandle, ViewRenderOptions>> rasterize_sink_;
        oneapi::tbb::flow::function_node<StageData<ComposeStage::OutputHandle, ViewRenderOptions>> compose_sink_;
        StageData<LayerGenerationStage::OutputHandle, ViewRenderOptions> layer_generation_result_{};
        StageData<HierarchyResolverStage::OutputHandle, ViewRenderOptions> hierarchy_resolver_result_{};
        StageData<HierarchyResolverStage::OutputHandle, ViewRenderOptions> viewport_cull_result_{};
        StageData<typename RasterizeStageT::OutputHandle, ViewRenderOptions> rasterize_result_{};
        StageData<ComposeStage::OutputHandle, ViewRenderOptions> compose_result_{};
    };

    /// @brief The plain, non-template name every existing caller uses -
    /// see ViewRenderPipelineImpl's own doc comment.
    using ViewRenderPipeline = ViewRenderPipelineImpl<>;

    /// @brief The Blend2D-backed sibling - see ViewRenderPipelineImpl's
    /// own doc comment and rasterize_blend2d_stage.hpp.
    using ViewRenderPipelineBlend2D = ViewRenderPipelineImpl<RasterizeBlend2DStage>;
}
