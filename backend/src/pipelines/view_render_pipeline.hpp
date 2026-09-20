#pragma once

#include "pipeline_options.hpp"
#include "stages/compose_stage.hpp"
#include "stages/hierarchy_resolver_stage.hpp"
#include "stages/layer_generation_stage.hpp"
#include "stages/rasterize_blend2d_stage.hpp"
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
    /// -> HierarchyResolverStage -> ViewportCullStage -> RasterizeBlend2DStage
    /// -> ComposeStage - a strict linear chain (every stage's own
    /// OutputHandle type matches the next stage's own InputData exactly),
    /// with a single sink on the terminal node (compose_sink_) so run()
    /// can read the final frame back after wait_for_all() - see
    /// WarmOutput's own doc comment for why the four intermediate stages
    /// don't get one of their own too. Hot tier stages get added to this
    /// same graph/class later - one pipeline for the whole thing, not a
    /// separate class per tier.
    ///
    /// One entry point into this one graph: run() submits at
    /// layer_generation_.node() and the existing make_edge chain carries
    /// that single message all the way through to compose_.node() in one
    /// try_put/wait_for_all - the whole graph was always wired this way
    /// end to end; what used to force two separate submissions
    /// (run_cold() then run_warm()) was RasterizeBlend2DStage's own
    /// dependency on the ViewLayerSet LayerGenerationStage computes,
    /// which had nowhere to travel to but ViewRenderOptions::view_layers -
    /// a field every stage in one submission sees the exact same,
    /// caller-supplied copy of (StageData<T, Options>'s own contract), so
    /// a later stage could never see a value an earlier stage in that
    /// same submission had just computed. Fixed at the source instead:
    /// HierarchyResolverStage's own OutputData now echoes the
    /// ViewLayerSetHandle it received as input back out as one of its own
    /// fields (HierarchyResolverOutput::view_layers), and ViewportCullStage
    /// passes it through unchanged - so it now arrives at
    /// RasterizeBlend2DStage as part of `data`, the same way every other
    /// stage's own real dependency does, and one submission through the
    /// whole chain is enough.
    ///
    /// The Root pointer every stage needs still travels via
    /// ViewRenderOptions::root, not any one stage's own InputData - run()
    /// sets it from its own `root` parameter, so a caller never has to set
    /// it independently.
    ///
    /// RasterizeBlend2DStage is the only Rasterize backend
    /// (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md - the earlier Skia-based
    /// RasterizeStage, benchmarked against it as a side experiment, and
    /// the ViewRenderPipelineImpl<RasterizeStageT> template this class
    /// used to be (parameterized to swap between the two) are both gone
    /// now that ComposeStage itself also composites BLImages natively,
    /// with no format-swappable seam left for a second backend to plug
    /// into anyway) - so this class is concrete, not a template.
    class ViewRenderPipeline
    {
    public:
        /// @brief The full chain's own observable output - just the final
        /// `frame` (RasterizedFrame, ComposeStage's own OutputHandle).
        /// Every intermediate stage's own result (view_layers/hierarchy/
        /// culled/rasterized) used to live here too, each captured off its
        /// own sink node - removed since nothing outside this class ever
        /// read them (api.cpp, every benchmark, and every still-relevant
        /// test all only ever needed `frame`); the sinks that captured them
        /// went with them (see the constructor's own comment below).
        struct WarmOutput
        {
            ComposeStage::OutputHandle frame;
        };

        /// @brief Only compose_sink_ remains, of what used to be five sink
        /// nodes (one per stage) - the other four existed purely to let
        /// run() read an intermediate stage's own result back into
        /// WarmOutput, and WarmOutput no longer carries those fields (this
        /// struct's own doc comment). Each removed sink's own make_edge
        /// was strictly additional fan-out off a node already wired into
        /// the main chain below it (e.g. layer_generation_.node() feeds
        /// both hierarchy_resolver_.node() and, previously,
        /// layer_generation_sink_) - removing it doesn't change what data
        /// reaches compose_.node(), only that nothing else also captures a
        /// copy of it along the way.
        explicit ViewRenderPipeline(std::string label = "ViewRenderPipeline")
            : layer_generation_(graph_, label + ".LayerGeneration"),
              hierarchy_resolver_(graph_, label + ".HierarchyResolver"),
              viewport_cull_(graph_, label + ".ViewportCull"),
              rasterize_(graph_, label + ".Rasterize"),
              compose_(graph_, label + ".Compose"),
              compose_sink_(
                  graph_, oneapi::tbb::flow::serial,
                  [this](StageData<ComposeStage::OutputHandle, ViewRenderOptions> in)
                  { compose_result_ = std::move(in); })
        {
            make_edge(layer_generation_.node(), hierarchy_resolver_.node());
            make_edge(hierarchy_resolver_.node(), viewport_cull_.node());
            make_edge(viewport_cull_.node(), rasterize_.node());
            make_edge(rasterize_.node(), compose_.node());
            make_edge(compose_.node(), compose_sink_);
        }

        ViewRenderPipeline(const ViewRenderPipeline &) = delete;
        ViewRenderPipeline &operator=(const ViewRenderPipeline &) = delete;

        /// @brief Runs the full Cold+Warm chain for `root` under `options`
        /// (`options.root` is overwritten with `root` here - a caller only
        /// has to set the fields that actually vary: root_mutation_version/
        /// top_level/hierarchy_depth/viewport/scale) in exactly one
        /// try_put/wait_for_all - see the class's own doc comment for how
        /// RasterizeBlend2DStage's own ViewLayerSet dependency, the thing
        /// that used to force two separate submissions here, now travels
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

            if (would_recompute(options))
            {
                layer_generation_.try_put({.data = root, .data_version = 0, .options = options});
                graph_.wait_for_all();
            }

            return WarmOutput{.frame = compose_result_.data};
        }

        /// @brief The same cascade run() itself uses to decide whether to
        /// submit the graph at all, exposed separately so a caller (
        /// api.cpp's own le_render_pixel_buffer) can know *before* calling
        /// run() whether it's about to do real work - used to bracket
        /// LeHandle::is_rendering_ around only the actual recompute,
        /// rather than the whole call (le_is_rendering's own doc comment,
        /// api.hpp). `options.root` does not need to be pre-set by the
        /// caller here the way run() requires of its own `root` parameter
        /// - every options_did_change() this cascades through compares
        /// fields other than `root` itself (root_mutation_version stands
        /// in for it). Pure/const - every would_recompute() call along
        /// the way only compares against each stage's own last_options_,
        /// it doesn't mutate anything, so calling this and then run()
        /// right after (which recomputes the identical cascade internally)
        /// is safe, just a small amount of cheap, duplicated comparison
        /// work - never a second real recompute.
        bool would_recompute(const ViewRenderOptions &options) const
        {
            const bool layer_generation_would_recompute = layer_generation_.would_recompute(0, options);
            const bool hierarchy_resolver_would_recompute =
                layer_generation_would_recompute || hierarchy_resolver_.would_recompute(layer_generation_.version(), options);
            const bool viewport_cull_would_recompute =
                hierarchy_resolver_would_recompute || viewport_cull_.would_recompute(hierarchy_resolver_.version(), options);
            const bool rasterize_would_recompute =
                viewport_cull_would_recompute || rasterize_.would_recompute(viewport_cull_.version(), options);
            return rasterize_would_recompute || compose_.would_recompute(rasterize_.version(), options);
        }

        // --- pipeline_stage_benchmark accessors (src/pipelines/benchmarks/) -
        // read-only access to each stage's own instrumentation
        // (tbb_core.hpp's last_call_recomputed()/last_compute_wall_ns()/
        // cache_object_count()/etc.) after a run() call, without exposing
        // any way to mutate a stage directly. ---
        const LayerGenerationStage &layer_generation_stage() const { return layer_generation_; }
        const HierarchyResolverStage &hierarchy_resolver_stage() const { return hierarchy_resolver_; }
        const ViewportCullStage &viewport_cull_stage() const { return viewport_cull_; }
        const RasterizeBlend2DStage &rasterize_stage() const { return rasterize_; }
        const ComposeStage &compose_stage() const { return compose_; }

    private:
        oneapi::tbb::flow::graph graph_;
        LayerGenerationStage layer_generation_;
        HierarchyResolverStage hierarchy_resolver_;
        ViewportCullStage viewport_cull_;
        RasterizeBlend2DStage rasterize_;
        ComposeStage compose_;
        oneapi::tbb::flow::function_node<StageData<ComposeStage::OutputHandle, ViewRenderOptions>> compose_sink_;
        StageData<ComposeStage::OutputHandle, ViewRenderOptions> compose_result_{};
    };
}
