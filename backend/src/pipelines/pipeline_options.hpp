#pragma once

#include "../database/database.hpp"

#include <cstdint>
#include <variant>

namespace le
{
    /// @brief Options shared by every stage in the Cold tier of
    /// ViewRenderPipeline (see PIPELINE_REFACTOR.md's own "Cold" section) -
    /// converts a Root's raw content into per-Abstract/per-Layout dbu-space
    /// shapes plus the Technology's ViewLayers. Every stage wired into the
    /// same Cold flow::graph must share this exact type (tbb_core.hpp's
    /// MemoizingStage is templated on one PipelineOptions type per graph),
    /// even though a given stage - e.g. LayerGenerationStage - only reads
    /// the subfield(s) it actually depends on, via its own
    /// options_did_change() override.
    struct ColdPipelineOptions
    {
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
        /// view recurses into before falling back to a placed instance's
        /// own Abstract - see Scene::hierarchy_depth()'s own doc comment.
        int hierarchy_depth = 0;
    };
}
