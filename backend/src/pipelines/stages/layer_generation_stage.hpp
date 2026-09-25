#pragma once

#include "../../database/database.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"

#include <string>
#include <utility>

namespace le
{
    /// @brief Cold-tier stage 1 (PIPELINE_REFACTOR.md): builds the
    /// technology's ViewLayers - a TERMINAL/OBSTRUCTION/TRACK_PREFERRED/
    /// TRACK_NON_PREFERRED/ROUTING_BLOCKAGE/ROUTE ViewLayer per physical
    /// Layer plus the fixed ROW/BOUNDARY/PLACEMENT/GCELLGRID/
    /// PLACEMENT_BLOCKAGE/REGION ones - from a Root's own Technology data.
    /// A thin MemoizingStage wrapper around the existing, already-tested
    /// ViewLayerSet::build_for_technology(): this stage's only job is
    /// caching that (cheap, but not free - proportional to layer count,
    /// not design size) rebuild behind a version check, not reimplementing
    /// it.
    ///
    /// Input: a non-owning `const Root*` - never null-checked by the
    /// caller, this stage degrades to an empty ViewLayerSet instead (same
    /// "degrade gracefully rather than crash" convention as api.cpp's own
    /// null-handle checks).
    ///
    /// Recompute trigger: `ViewRenderOptions::root_mutation_version`
    /// alone (via options_did_change() below), not `data_version` - the
    /// input `Root*` itself never changes across calls within one handle's
    /// lifetime, so there is nothing meaningful to bump a data_version on;
    /// every database mutation already bumps root_mutation_version
    /// (Root::bump_mutation_version()), and a Technology/Layer change (the
    /// only thing this stage's own output actually depends on) is always
    /// itself a mutation. This recomputes on every mutation, not just a
    /// Technology/Layer one - the doc's own Cold tier has a 5s budget and
    /// this rebuild's cost tracks layer count (tens, not millions), so the
    /// extra recomputes are cheap; narrow this further only if a benchmark
    /// ever shows otherwise.
    class LayerGenerationStage : public MemoizingStage<const Root *, ViewLayerSet, ViewRenderOptions>
    {
    public:
        explicit LayerGenerationStage(oneapi::tbb::flow::graph &g, std::string label = "LayerGeneration")
            : MemoizingStage(g, std::move(label)) {}

    protected:
        ViewLayerSet compute(const Root *const &root, const ViewRenderOptions &options) override
        {
            if (root == nullptr)
                return ViewLayerSet{};

            const std::vector<TechnologyId> technology_ids = root->get_technology_ids();
            if (technology_ids.empty())
                return ViewLayerSet{};

            // A Root is expected to hold at most one Technology in this
            // MVP (see api.cpp's own le_read_lef, ViewLayerSet's own
            // caller) - front() is every existing call site's convention,
            // not a new assumption introduced here.
            return ViewLayerSet::build_for_technology(*root, technology_ids.front());
        }

        bool options_did_change(const ViewRenderOptions &last, const ViewRenderOptions &current) const override
        {
            return last.root_mutation_version != current.root_mutation_version;
        }

        // pipeline_stage_benchmark cache-stat hooks (tbb_core.hpp) - a
        // ViewLayerSet's own count is small (tens, not millions) and
        // fixed-size per ViewLayerData entry, so an exact object count
        // and an approximate (entry count x sizeof(ViewLayerData)) byte
        // estimate are both cheap and meaningful here, unlike a stage
        // whose OutputData holds real per-shape geometry.
        std::size_t estimate_output_object_count(const ViewLayerSet &output) const override
        {
            return output.all().size();
        }

        std::size_t estimate_output_bytes(const ViewLayerSet &output) const override
        {
            return output.all().size() * sizeof(ViewLayerData);
        }
    };
}
