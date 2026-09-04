#pragma once

#include "../../geometry/geometry.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "hierarchy_resolver_stage.hpp"

#include <deque>
#include <string>
#include <utility>

namespace le
{
    /// @brief Warm-tier stage 1 (PIPELINE_REFACTOR.md): prunes
    /// HierarchyResolverOutput down to what actually overlaps
    /// ViewRenderOptions::viewport, so downstream Rasterization/Compose
    /// never touch content that isn't visible. Output is the exact same
    /// shape as Cold's own HierarchyResolverOutput - a pruned copy, not a
    /// separate "visible instance list" type - so Compose can walk it the
    /// same way it would walk Cold's own unculled output.
    ///
    /// Walks the same hierarchy HierarchyResolverStage's own traversal
    /// discovered, top-down from options.top_level, composing a running
    /// Geometry::InstanceTransform as it descends (identity at top_level
    /// itself - its own placement_data bboxes are already in the
    /// viewport's own coordinate space; a nested placement's own bbox is
    /// only in that same space once transformed through every ancestor's
    /// own placement transform on the way down, via Geometry::compose -
    /// ViewPlacementData::bbox is only ever in its *immediate* parent's
    /// own local space, never pre-composed with anything above that). At
    /// each visited id:
    ///   - `shapes` are copied through unchanged - this stage prunes
    ///     *placements*, not individual shapes within one node's own
    ///     direct content (a finer-grained concern, deferred - Skia's own
    ///     clipping/quickReject covers the gap for now once Rasterization
    ///     exists).
    ///   - `placement_data` is filtered down to just the placements whose
    ///     own world-space bbox (this level's own accumulated transform
    ///     applied to ViewPlacementData::bbox via Geometry::transform_bbox)
    ///     overlaps the viewport (Geometry::rects_overlap).
    ///
    /// An id with no surviving placement anywhere never gets visited at
    /// all, and therefore never appears in the output - the same
    /// reachability principle HierarchyResolverStage itself already
    /// applies (dedup by id in the worklist below), just gated here by
    /// "did any parent's own surviving placement lead here" instead of
    /// "was this ever discoverable at all".
    ///
    /// The same id can be reached via more than one surviving placement
    /// (the same shared cell instanced at several visible positions, or a
    /// design placed by more than one parent) - this stage still only
    /// recurses into it once, using whichever surviving instance's own
    /// accumulated transform got there first (mirrors
    /// HierarchyResolverStage's own "shallowest-first, first discovered
    /// wins" dedup - see that class's own doc comment). One consequence:
    /// a child near that id's own edge that would be visible from a
    /// *different* surviving instance's own vantage point, but not this
    /// one, may be kept (or dropped) depending on which instance won -
    /// a deliberate simplification, not a correctness bug: Compose still
    /// draws every surviving instance of a shared id at its own correct
    /// position regardless, so this can only ever cost a little
    /// unnecessary off-screen work, never an incorrect on-screen result.
    class ViewportCullStage : public MemoizingStage<HierarchyResolverStage::OutputHandle, HierarchyResolverOutput, ViewRenderOptions>
    {
    public:
        explicit ViewportCullStage(oneapi::tbb::flow::graph &g, std::string label = "ViewportCull")
            : MemoizingStage(g, std::move(label)) {}

    protected:
        HierarchyResolverOutput compute(const HierarchyResolverStage::OutputHandle &input, const ViewRenderOptions &options) override
        {
            HierarchyResolverOutput result;
            if (input == nullptr)
                return result;

            struct WorkItem
            {
                HierarchyId id;
                Geometry::InstanceTransform accumulated_transform;
            };
            std::deque<WorkItem> worklist;
            worklist.push_back(WorkItem{options.top_level, Geometry::identity_transform()});

            while (!worklist.empty())
            {
                const WorkItem item = worklist.front();
                worklist.pop_front();

                if (result.view_data.contains(item.id))
                    continue; // already visited via an earlier surviving instance

                const auto source_it = input->view_data.find(item.id);
                if (source_it == input->view_data.end())
                    continue; // not present in Cold's own output - nothing to cull (degrade, don't crash)

                ViewData data;
                data.shapes = source_it->second.shapes;
                data.placement_data.reserve(source_it->second.placement_data.size()); // exact upper bound - not every placement survives culling

                for (const ViewPlacementData &placement : source_it->second.placement_data)
                {
                    const Rect world_bbox = Geometry::transform_bbox(item.accumulated_transform, placement.bbox);
                    if (!Geometry::rects_overlap(world_bbox, options.viewport))
                        continue; // culled - outside the viewport

                    data.placement_data.push_back(placement);
                    worklist.push_back(WorkItem{placement.id, Geometry::compose(item.accumulated_transform, placement.transform)});
                }

                result.view_data.emplace(item.id, std::move(data));
            }

            return result;
        }

        bool options_did_change(const ViewRenderOptions &last, const ViewRenderOptions &current) const override
        {
            return last.top_level != current.top_level ||
                   last.viewport.ll.x != current.viewport.ll.x ||
                   last.viewport.ll.y != current.viewport.ll.y ||
                   last.viewport.ur.x != current.viewport.ur.x ||
                   last.viewport.ur.y != current.viewport.ur.y;
        }
    };
}
