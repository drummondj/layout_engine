#pragma once

#include "../database/database.hpp"
#include "../view_style/view_style.hpp"

#include <cstdint>
#include <memory>
#include <variant>

namespace le
{
    /// @brief Options shared by every stage of ViewRenderPipeline (see
    /// PIPELINE_REFACTOR.md's own "Structure" section) - Cold, Warm, and
    /// (eventually) Hot alike. Every stage wired into the same
    /// oneapi::tbb::flow::graph must share this exact type (tbb_core.hpp's
    /// MemoizingStage is templated on one PipelineOptions type per graph),
    /// even though a given stage - e.g. LayerGenerationStage - only reads
    /// the subfield(s) it actually depends on, via its own
    /// options_did_change() override. Named for the whole pipeline, not
    /// "Cold", precisely because fields like `viewport` below only matter
    /// to Warm/Hot stages - a Cold-only name would be misleading the
    /// moment those stages join the same graph.
    struct ViewRenderOptions
    {
        /// @brief Non-owning pointer to the Root every Cold-tier stage
        /// reads from - shared context, not part of any one stage's own
        /// InputData (ViewRenderPipeline, view_render_pipeline.hpp, wires
        /// LayerGenerationStage's own OutputHandle directly into
        /// HierarchyResolverStage's InputData via a real make_edge; the
        /// Root pointer has to travel some other way, since it isn't part
        /// of that upstream output). Mirrors the pre-restart PipelineOptions'
        /// own PipelineContext pattern (backend/CLAUDE.md) for the same
        /// reason. Never null-checked by a stage before use - each
        /// degrades to an empty/default output instead (same convention
        /// as a null LeHandle in api.cpp).
        const Root *root = nullptr;

        /// @brief Root::mutation_version() at the time this options
        /// snapshot was taken. Every Cold-tier stage's own
        /// options_did_change() compares this field (directly, or via a
        /// narrower per-stage generation derived from it) since any
        /// database mutation can in principle affect what that stage
        /// produces.
        std::uint64_t root_mutation_version = 0;

        /// @brief Which AbstractId or LayoutId the caller wants shapes
        /// for - HierarchyResolver's own top-level starting point.
        std::variant<AbstractId, LayoutId> top_level;

        /// @brief How many further Placement -> Design levels a Layout
        /// view recurses into - HierarchyResolverStage's own doc comment
        /// has the exact semantics (0 shows only top_level's own direct
        /// content; each further unit lets one more Placement -> Layout
        /// hop actually resolve and get visited, falling back to a
        /// placement's own Abstract once a Layout hop is no longer
        /// possible - not a blanket "always show at least the Abstract"
        /// rule at any depth).
        int hierarchy_depth = 0;

        /// @brief Warm tier's own viewport, in dbu, in top_level's own
        /// coordinate space (ViewportCullStage's own doc comment) - a
        /// Placement's own bbox at any deeper level is only in that same
        /// space once composed through every ancestor placement's own
        /// transform on the way down, which is exactly what culling does.
        Rect viewport;

        /// @brief Warm tier's own pixels-per-dbu-unit scale, shared by
        /// every node's own rasterization (RasterizeStage) so composing
        /// them (ComposeStage) is a plain translate+rotate per placement,
        /// never a resample - see RasterizeStage's own doc comment.
        double scale = 1.0;

        /// @brief The current ViewLayerSet, so a Warm-tier drawing stage
        /// (RasterizeStage) can resolve a ViewShape's own view_layer id
        /// into real style (color/pattern) without a second graph input -
        /// mirrors `root` above (shared context, not part of any one
        /// stage's own InputData/OutputData) for the same reason: a real
        /// make_edge already carries LayerGenerationStage's own output
        /// into HierarchyResolverStage's InputData, but nothing wires it
        /// any further downstream, so a later stage that still needs the
        /// actual ViewLayerSet content (not just a recompute signal - the
        /// data_version chain already provides that on its own) has
        /// nowhere else to get it. Same type as LayerGenerationStage::
        /// OutputHandle/HierarchyResolverStage::ViewLayerSetHandle (both
        /// std::shared_ptr<const ViewLayerSet>) - spelled out directly
        /// here rather than named via either alias, to avoid a circular
        /// include (both those headers already include this one).
        /// Never null-checked before use, same "degrade to empty output"
        /// convention as `root`.
        std::shared_ptr<const ViewLayerSet> view_layers;

        /// @brief Whether RasterizeStage draws with antialiasing. Default
        /// false: RasterizeStage draws every individual rect/path/polygon
        /// with its own Skia draw call (no batching), and antialiasing
        /// each one is a real, measured cost at real geometry counts
        /// (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) - off by default so a
        /// caller opts into the slower, smoother path deliberately rather
        /// than paying for it unknowingly.
        bool antialiasing_enabled = false;
    };
}
