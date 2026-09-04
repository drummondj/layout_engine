#pragma once
#include "../database/database.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include <cstdint>

namespace le
{
    /// @brief Stable-for-the-call, non-owning context every pipelines stage
    /// can reach - Root/ViewLayerSet/Scene all outlive one synchronous
    /// try_put()+wait_for_all() round trip (owned by the caller, e.g.
    /// api.cpp's LeHandle), so raw pointers are safe as long as that
    /// synchronous-per-call rule holds (see the migration plan's Threading
    /// model section - internal TBB parallelism must stay wholly inside one
    /// call, never span two API calls). Not itself part of any stage's
    /// options_did_change comparison - only the value structs below are.
    struct PipelineContext
    {
        const Root *root = nullptr;
        const ViewLayerSet *view_layers = nullptr;
        const Scene *scene = nullptr;
    };

    /// @brief Root/database-level recompute triggers - a stage that reads
    /// Root/ViewLayerSet content directly (not just already-resolved
    /// upstream output) compares this.
    struct FrameEpoch
    {
        uint64_t root_mutation_version = 0;
        uint64_t view_layers_generation = 0;

        bool operator==(const FrameEpoch &) const = default;
    };

    /// @brief Scene-viewport-level recompute triggers (pan/scale/viewport
    /// size, layer visibility) - the oneTBB/PipelineOptions equivalent of
    /// today's `scene.viewport_version()`/`scene.visibility_version()` key
    /// components.
    struct ViewportOptions
    {
        uint64_t viewport_version = 0;
        uint64_t visibility_version = 0;
        double scale = 0;

        bool operator==(const ViewportOptions &) const = default;
    };

    /// @brief Mouse/selection/ruler chrome recompute triggers.
    struct InteractionOptions
    {
        uint64_t mouse_version = 0;
        uint64_t selection_version = 0;
        uint64_t ruler_version = 0;

        bool operator==(const InteractionOptions &) const = default;
    };

    /// @brief The single PipelineOptions type shared by every stage/pipeline
    /// under src/pipelines (backend/ONETBB_INTEGRATION.md migration plan,
    /// item 3 of the user's own outline: "PipelineOptions is used to hold
    /// all data that can change how and if a pipeline stage is
    /// re-computed"). One shared type across the whole module is required
    /// anyway - every stage connected via make_edge in one flow::graph must
    /// share one PipelineOptions type, and the render pipelines consume the
    /// shape pipelines' own output. Each stage's own options_did_change
    /// override compares only the sub-fields it actually depends on (see
    /// each stage class's own doc comment) - not every stage cares about
    /// every field here.
    struct PipelineOptions
    {
        PipelineContext ctx;
        FrameEpoch epoch;
        ViewportOptions viewport;
        InteractionOptions interaction;
        double min_visible_instance_pixels = 100.0;
    };
}
