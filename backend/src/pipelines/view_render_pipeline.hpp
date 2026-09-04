#pragma once

#include "pipeline_options.hpp"
#include "stages/compose_stage.hpp"
#include "stages/hierarchy_resolver_stage.hpp"
#include "stages/layer_generation_stage.hpp"
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
    /// Two independent entry points into this one graph, not one:
    /// run_cold() submits at layer_generation_.node() (LayerGeneration ->
    /// HierarchyResolver); run_warm() submits at viewport_cull_.node()
    /// directly (ViewportCull -> Rasterize -> Compose), after first
    /// calling run_cold() itself to ensure Cold's own output is fresh -
    /// see run_warm()'s own doc comment for why this can't be one single
    /// submission through the whole chain.
    ///
    /// The Root pointer every stage needs travels via
    /// ViewRenderOptions::root, not any one stage's own InputData -
    /// run_cold()/run_warm() set it from their own `root` parameter, so a
    /// caller never has to set it independently. ViewRenderOptions::
    /// view_layers (RasterizeStage's own dependency) is different: its
    /// correct value only exists once LayerGenerationStage has actually
    /// run, so run_warm() sets it from run_cold()'s own return value
    /// rather than expecting the caller to.
    class ViewRenderPipeline
    {
    public:
        /// @brief The Cold tier's own combined output (PIPELINE_REFACTOR.md:
        /// "Output: A vector of shapes per Abstract and Layout ... plus a
        /// vector of ViewLayers") - a HierarchyResolverOutput alone isn't
        /// enough for a consumer to actually render anything, since its own
        /// ViewShape.view_layer fields are ids that need resolving against
        /// this same ViewLayerSet for style/color/purpose.
        struct ColdOutput
        {
            LayerGenerationStage::OutputHandle view_layers;
            HierarchyResolverStage::OutputHandle hierarchy;
        };

        /// @brief The Warm tier's own combined output - every intermediate
        /// stage's own result, not just the final `frame`, so a caller
        /// (or a test) can inspect what actually survived culling/
        /// rasterization without re-deriving it independently.
        struct WarmOutput
        {
            LayerGenerationStage::OutputHandle view_layers;
            HierarchyResolverStage::OutputHandle hierarchy; // Cold's own unculled output
            HierarchyResolverStage::OutputHandle culled;    // ViewportCullStage's own output
            RasterizeStage::OutputHandle rasterized;
            ComposeStage::OutputHandle frame;
        };

        explicit ViewRenderPipeline(std::string label = "ViewRenderPipeline")
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
                  [this](StageData<RasterizeStage::OutputHandle, ViewRenderOptions> in)
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

        ViewRenderPipeline(const ViewRenderPipeline &) = delete;
        ViewRenderPipeline &operator=(const ViewRenderPipeline &) = delete;

        /// @brief Runs the Cold tier for `root` under `options` (`options.root`
        /// is overwritten with `root` here - a caller only has to set the
        /// fields that actually vary: root_mutation_version/top_level/
        /// hierarchy_depth). No data_version parameter, unlike
        /// SynchronousStageRunner::run() - neither stage's own recompute
        /// decision ever looks at one (both rely entirely on
        /// options_did_change(), see each stage's own doc comment), so
        /// there is nothing meaningful for a caller to thread through here;
        /// exposing one would only invite a caller to accidentally force
        /// recomputation by bumping it for an unrelated reason.
        ///
        /// Skips try_put/wait_for_all entirely when neither stage would
        /// recompute - see MemoizingStage::would_recompute()'s own doc
        /// comment (tbb_core.hpp) for why that's load-bearing, not just a
        /// nicety, even on a guaranteed cache hit.
        ColdOutput run_cold(const Root *root, ViewRenderOptions options)
        {
            options.root = root;

            const bool layer_generation_would_recompute = layer_generation_.would_recompute(0, options);
            const bool hierarchy_resolver_would_recompute =
                layer_generation_would_recompute || hierarchy_resolver_.would_recompute(layer_generation_.version(), options);

            if (layer_generation_would_recompute || hierarchy_resolver_would_recompute)
            {
                layer_generation_.try_put({.data = root, .data_version = 0, .options = options});
                graph_.wait_for_all();
            }

            return {layer_generation_result_.data, hierarchy_resolver_result_.data};
        }

        /// @brief Runs Cold (via run_cold(), reused directly rather than
        /// duplicated) followed by the Warm tier, for the given `options.
        /// viewport`/`options.scale`.
        ///
        /// Can't submit through the whole Cold+Warm chain in one
        /// try_put/wait_for_all the way run_cold() does for its own two
        /// stages: RasterizeStage reads ViewRenderOptions::view_layers,
        /// but the *correct* value for that field - LayerGenerationStage's
        /// own freshly computed output - doesn't exist until
        /// LayerGenerationStage has actually finished running, and every
        /// stage in one TBB flow::graph submission sees the exact same
        /// `options` copy the caller handed to try_put() up front, threaded
        /// through unchanged (StageData<T, Options>'s own contract) - there
        /// is no way for a later stage in that same submission to see a
        /// value an earlier stage in it just computed. So: run Cold to
        /// completion first (a real, separate try_put/wait_for_all round,
        /// gated by run_cold()'s own would_recompute check - a no-op call
        /// when Cold is already up to date), read `view_layers` back out
        /// of its own result, then submit a *second*, independent round
        /// directly at viewport_cull_.node() (this graph's own second entry
        /// point, alongside layer_generation_.node()) with `options.view_layers`
        /// now set correctly.
        WarmOutput run_warm(const Root *root, ViewRenderOptions options)
        {
            const ColdOutput cold = run_cold(root, options);
            options.root = root;
            options.view_layers = cold.view_layers;

            const bool viewport_cull_would_recompute = viewport_cull_.would_recompute(hierarchy_resolver_.version(), options);
            const bool rasterize_would_recompute =
                viewport_cull_would_recompute || rasterize_.would_recompute(viewport_cull_.version(), options);
            const bool compose_would_recompute =
                rasterize_would_recompute || compose_.would_recompute(rasterize_.version(), options);

            if (compose_would_recompute)
            {
                viewport_cull_.try_put({.data = cold.hierarchy, .data_version = hierarchy_resolver_.version(), .options = options});
                graph_.wait_for_all();
            }

            return WarmOutput{
                .view_layers = cold.view_layers,
                .hierarchy = cold.hierarchy,
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
        RasterizeStage rasterize_;
        ComposeStage compose_;
        oneapi::tbb::flow::function_node<StageData<LayerGenerationStage::OutputHandle, ViewRenderOptions>> layer_generation_sink_;
        oneapi::tbb::flow::function_node<StageData<HierarchyResolverStage::OutputHandle, ViewRenderOptions>> hierarchy_resolver_sink_;
        oneapi::tbb::flow::function_node<StageData<HierarchyResolverStage::OutputHandle, ViewRenderOptions>> viewport_cull_sink_;
        oneapi::tbb::flow::function_node<StageData<RasterizeStage::OutputHandle, ViewRenderOptions>> rasterize_sink_;
        oneapi::tbb::flow::function_node<StageData<ComposeStage::OutputHandle, ViewRenderOptions>> compose_sink_;
        StageData<LayerGenerationStage::OutputHandle, ViewRenderOptions> layer_generation_result_{};
        StageData<HierarchyResolverStage::OutputHandle, ViewRenderOptions> hierarchy_resolver_result_{};
        StageData<HierarchyResolverStage::OutputHandle, ViewRenderOptions> viewport_cull_result_{};
        StageData<RasterizeStage::OutputHandle, ViewRenderOptions> rasterize_result_{};
        StageData<ComposeStage::OutputHandle, ViewRenderOptions> compose_result_{};
    };
}
