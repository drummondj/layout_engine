#pragma once

#include "../../geometry/geometry.hpp"
#include "../draw_helpers.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "hierarchy_resolver_stage.hpp"

#include <boost/geometry/index/rtree.hpp>

#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace le
{
    namespace bgi = boost::geometry::index;

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
    ///   - `shapes`/`shapes_index` are carried through unchanged - a
    ///     shared_ptr copy (ViewShapesHandle/ViewShapesIndexHandle,
    ///     hierarchy_resolver_stage.hpp's own comments) is a refcount
    ///     bump regardless of how many shapes a node has, not a real
    ///     copy - this stage prunes *placements*, not individual shapes
    ///     within one node's own direct content; RasterizeBlend2DStage is
    ///     the one that queries `shapes_index` against its own per-node
    ///     render bbox to avoid walking every shape in a huge flat node.
    ///   - `placement_data` is filtered down to just the placements whose
    ///     own local (pre-ancestor-transform) bbox overlaps the viewport
    ///     once brought into this node's own local space - see the
    ///     spatial-index paragraph below for exactly how - AND whose own
    ///     bbox isn't sub-pixel at `options.scale` (`bbox_is_sub_pixel`,
    ///     draw_helpers.hpp - the same function/threshold RasterizeBlend2DStage
    ///     already applies per-shape). A placement's
    ///     own `bbox` is a dbu-space size, and dbu is a globally uniform
    ///     unit throughout the hierarchy (composing an ancestor chain
    ///     only ever translates/rotates, per Geometry::InstanceTransform -
    ///     never rescales), so testing it directly against `options.scale`
    ///     needs no transform at all, unlike the overlap test above.
    ///     Skipped (not visited, not pushed to the worklist, no
    ///     substitute mark) exactly like a sub-pixel Rect/Polygon already
    ///     is - real reported symptom this closes: at `hierarchy_depth >= 1`
    ///     and a zoomed-way-out (e.g. zoom-fit) viewport, a placement
    ///     whose own footprint is too small to matter used to still be
    ///     fully descended into and rendered at full per-shape detail
    ///     (only ITS OWN individual shapes were ever tested for being
    ///     sub-pixel, never the placement as a whole), so a design with
    ///     many placements each individually a few pixels wide - too big
    ///     for any single shape inside them to be sub-pixel, but too
    ///     small to be useful content - showed as visual noise the
    ///     top-level's own directly-owned geometry never showed, even
    ///     though it was culled the same way. Applying the identical
    ///     threshold one level up (to the placement itself, before ever
    ///     descending) closes that gap and also skips real, avoidable
    ///     work (an entire subtree's own shape iteration/rasterization),
    ///     not just a visual cleanup.
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
    ///
    /// Overlap testing is spatially indexed, not a linear scan (measured:
    /// a linear scan missed the Warm tier's own 500ms budget by ~3.5x at
    /// the 1M-component target scale, PIPELINE_REFACTOR_BENCHMARK_RESULTS.md's
    /// commit 6478286 entry). Two things make this fast rather than just
    /// "an rtree slapped on":
    ///   - The *viewport* is brought into each node's own local space
    ///     (Geometry::invert(accumulated_transform) applied once per
    ///     node, via Geometry::transform_bbox), not the other way around
    ///     - transforming one rect per node is far cheaper than
    ///     transforming every one of that node's own (up to ~1,000,000)
    ///     placement bboxes out to world space just to test them.
    ///   - Each node's own R-tree (Boost.Geometry Index, bulk-loaded over
    ///     that node's local, untransformed placement bboxes) is built at
    ///     most once per distinct Cold input and reused across every
    ///     later call that shares it - "zoom always re-computes" means a
    ///     fresh viewport every call, but Cold's own output (and
    ///     therefore what to index) only changes on a real database edit
    ///     or hierarchy_depth change. Indexed by the node's own id, keyed
    ///     off `input`'s own identity (a shared_ptr, held here to
    ///     guarantee no other allocation can reuse its address while this
    ///     cache still names it) - see spatial_index_for()'s own comment.
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
            result.view_layers = input->view_layers;

            if (input.get() != cached_input_.get())
            {
                cached_input_ = input;
                spatial_indices_.clear();
            }

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

                const ViewData &source_data = source_it->second;
                ViewData data;
                data.shapes = source_data.shapes;
                data.shapes_index = source_data.shapes_index;
                data.extent = source_data.extent;

                // One Rect transform per node, not one per placement -
                // see the class's own doc comment.
                const Rect local_viewport = Geometry::transform_bbox(Geometry::invert(item.accumulated_transform), options.viewport);

                const SpatialIndex &index = spatial_index_for(item.id, source_data);
                std::vector<IndexEntry> candidates;
                index.query(bgi::intersects(local_viewport), std::back_inserter(candidates));

                data.placement_data.reserve(candidates.size());
                for (const IndexEntry &entry : candidates)
                {
                    const ViewPlacementData &placement = source_data.placement_data[entry.second];
                    // See this class's own doc comment - a placement
                    // whose own bbox is sub-pixel at options.scale is
                    // skipped entirely, the same way a sub-pixel Rect/
                    // Polygon already is inside Rasterize.
                    if (bbox_is_sub_pixel(placement.extent.ur.x - placement.extent.ll.x, placement.extent.ur.y - placement.extent.ll.y, options.scale))
                        continue;
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
                   last.viewport.ur.y != current.viewport.ur.y ||
                   // Needed now that compute() also does a scale-dependent
                   // sub-pixel-placement test (this class's own doc
                   // comment) - a pure scale change with the viewport's
                   // own dbu-space bounds held fixed (unusual in practice,
                   // since a fixed on-screen pixel viewport normally
                   // implies the two change together, but not guaranteed
                   // by this struct itself) would otherwise return a
                   // stale, un-recomputed result.
                   last.scale != current.scale;
        }

        // pipeline_stage_benchmark cache-stat hooks (tbb_core.hpp) - same
        // OutputData shape as HierarchyResolverStage, so the same shared
        // helper applies (hierarchy_resolver_stage.hpp), but bytes must
        // use owned_bytes_excluding_shapes(), not owned_bytes_including_shapes():
        // this stage's own `shapes`/`shapes_index` fields are shared_ptr
        // copies of HierarchyResolverStage's own already-built data
        // (compute()'s own `data.shapes = source_data.shapes;`), not a
        // duplicate - reporting the shared shape bytes here too would
        // double-count them on top of HierarchyResolverStage's own
        // cache_bytes(). object_count() is unaffected - shape_count is
        // still meaningful as "how many geometries this cache reaches",
        // independent of who owns the memory.
        std::size_t estimate_output_object_count(const HierarchyResolverOutput &output) const override
        {
            return estimate_hierarchy_resolver_output_stats(output).object_count();
        }

        std::size_t estimate_output_bytes(const HierarchyResolverOutput &output) const override
        {
            return estimate_hierarchy_resolver_output_stats(output).owned_bytes_excluding_shapes();
        }

    private:
        // Rect (a node's own local placement bbox) paired with its own
        // index into that node's placement_data vector - the rtree's own
        // value type has to carry enough to recover the actual
        // ViewPlacementData a hit corresponds to.
        using IndexEntry = std::pair<Rect, std::size_t>;
        using SpatialIndex = bgi::rtree<IndexEntry, bgi::rstar<16>>;

        /// @brief This node's own spatial index over `data.placement_data`'s
        /// local (untransformed) bboxes - built once per distinct Cold
        /// input (see compute()'s own cached_input_ check) and reused
        /// across every later call that still shares it, rather than
        /// rebuilt on every viewport-only "zoom tick".
        const SpatialIndex &spatial_index_for(const HierarchyId &id, const ViewData &data)
        {
            const auto it = spatial_indices_.find(id);
            if (it != spatial_indices_.end())
                return it->second;

            std::vector<IndexEntry> entries;
            entries.reserve(data.placement_data.size());
            for (std::size_t i = 0; i < data.placement_data.size(); ++i)
                entries.emplace_back(data.placement_data[i].extent, i); // everything it draws, overhang included

            return spatial_indices_.emplace(id, SpatialIndex(entries)).first->second;
        }

        // Held (not just a raw pointer) so the underlying HierarchyResolverOutput
        // can't be freed - and its address reused by an unrelated allocation -
        // while spatial_indices_ still names it by identity.
        HierarchyResolverStage::OutputHandle cached_input_;
        std::unordered_map<HierarchyId, SpatialIndex, HierarchyIdHash> spatial_indices_;
    };
}
