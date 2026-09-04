#pragma once

#include "../database/database.hpp"

#include <cstdint>
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
    };
}
