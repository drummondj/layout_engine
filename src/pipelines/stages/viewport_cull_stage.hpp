#pragma once

#include "../../core/object_filters.hpp"
#include "../../geometry/geometry.hpp"
#include "../draw_helpers.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "hierarchy_resolver_stage.hpp"

#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace le
{
    namespace bgi = boost::geometry::index;

    /// @brief Warm-tier stage 1 (plans/PIPELINE_REFACTOR.md): prunes
    /// HierarchyResolverOutput down to what actually overlaps
    /// ViewRenderOptions::viewport, so downstream Rasterization/Compose
    /// never touch content that isn't visible. Output is the exact same
    /// shape as Cold's own HierarchyResolverOutput - a pruned copy, not a
    /// separate "visible instance list" type - so Compose can walk it the
    /// same way it would walk Cold's own unculled output.
    ///
    /// Walks the same hierarchy HierarchyResolverStage's own traversal
    /// discovered, top-down from options.top_level, carrying each node's
    /// visible region in its own local space (`ViewData::visible_region`):
    /// the viewport for top_level, and for a nested node the union, over
    /// every surviving placement of it, of the parent's region clipped to
    /// that placement's extent and brought into the node's space through
    /// the placement's inverse transform. At each visited id:
    ///   - `shapes`/`shapes_index` are carried through unchanged - a
    ///     shared_ptr copy (ViewShapesHandle/ViewShapesIndexHandle,
    ///     hierarchy_resolver_stage.hpp's own comments) is a refcount
    ///     bump regardless of how many shapes a node has, not a real
    ///     copy - this stage prunes *placements*, not individual shapes
    ///     within one node's own direct content; RasterizeBlend2DStage is
    ///     the one that queries `shapes_index` against its own per-node
    ///     render bbox to avoid walking every shape in a huge flat node.
    ///   - `placement_data` is filtered down to just the placements whose
    ///     own local bbox overlaps the node's visible region AND whose own
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
    ///     is. Testing only a placement's individual shapes isn't enough:
    ///     at `hierarchy_depth >= 1` on a zoomed-out viewport, many
    ///     placements a few pixels wide each (too big for any single shape
    ///     inside to be sub-pixel, too small to be useful content) would
    ///     render as visual noise. Culling the placement itself, before
    ///     descending, avoids that and skips the whole subtree's work.
    ///
    /// An id with no surviving placement anywhere never gets visited at
    /// all, and therefore never appears in the output - the same
    /// reachability principle HierarchyResolverStage itself already
    /// applies (dedup by id in the worklist below), just gated here by
    /// "did any parent's own surviving placement lead here" instead of
    /// "was this ever discoverable at all".
    ///
    /// The same id can be reached via more than one surviving placement
    /// (a shared cell instanced at several visible positions, or a design
    /// placed by more than one parent). Its region is the union of what
    /// each instance shows, and the node is culled again whenever that
    /// region grows, so its one shared image holds every child any
    /// instance shows. Rasterize sizes a nested node's image to its
    /// region, so a block much larger than the viewport is never
    /// rasterized whole.
    ///
    /// Overlap testing is spatially indexed, not a linear scan (a linear
    /// scan misses the Warm tier's 500ms budget by ~3.5x at the
    /// 1M-component target scale). Two things make this fast rather than just
    /// "an rtree slapped on":
    ///   - The visible region is carried in each node's own local space,
    ///     not the other way around - transforming one rect per surviving
    ///     placement is far cheaper than transforming every one of a
    ///     node's own (up to ~1,000,000) placement bboxes out to world
    ///     space just to test them.
    ///   - Each node's own R-tree (Boost.Geometry Index, bulk-loaded over
    ///     that node's local, untransformed placement bboxes) is built at
    ///     most once per distinct Cold input and reused across every
    ///     later call that shares it - "zoom always re-computes" means a
    ///     fresh viewport every call, but Cold's own output (and
    ///     therefore what to index) only changes on a real database edit
    ///     or hierarchy_depth change. Indexed by the node's own id, keyed
    ///     off that node's placement vector (a shared_ptr, held here to
    ///     guarantee no other allocation can reuse its address while this
    ///     cache still names it) - see spatial_index_for()'s own comment.
    ///
    /// Placement.type/Route.use filters (ViewRenderOptions::hidden_objects)
    /// are applied here, not by HierarchyResolverStage, so a toggle never
    /// re-resolves: positions don't change, only what's drawn. A hidden
    /// placement is skipped like an off-screen one (its subtree isn't
    /// visited), and each reached node's `chunk_visibility` marks the
    /// hidden route shapes and placement rects/labels of its chunks for
    /// RasterizeBlend2DStage. The masks come from the chunks'
    /// ChunkSources and are cached per chunk, so a pan reuses them and an
    /// edit only builds masks for the chunks the resolver rebuilt.
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

            // Drop indices whose placement tile is gone.
            std::erase_if(spatial_indices_, [](const auto &entry)
                          { return entry.second.tile.expired(); });
            update_filter(options);

            // Each reached node's visible region in its own local space: the
            // union, over every surviving instance, of the part of the node
            // that instance shows. A node is (re)culled whenever its region
            // grows, so a shared node reached at more than one position (or
            // depth) keeps every child any of its instances shows.
            std::unordered_map<HierarchyId, Rect, HierarchyIdHash> regions;
            std::unordered_set<HierarchyId, HierarchyIdHash> queued;
            std::deque<HierarchyId> worklist;
            regions.emplace(options.top_level, options.viewport);
            queued.insert(options.top_level);
            worklist.push_back(options.top_level);

            while (!worklist.empty())
            {
                const HierarchyId id = worklist.front();
                worklist.pop_front();
                queued.erase(id);

                const auto source_it = input->view_data.find(id);
                if (source_it == input->view_data.end())
                    continue; // not present in Cold's own output - nothing to cull (degrade, don't crash)

                const ViewData &source_data = source_it->second;
                const Rect region = regions.at(id);
                auto [data_it, first_visit] = result.view_data.try_emplace(id);
                ViewData &data = data_it->second;
                if (first_visit)
                {
                    data.chunks = source_data.chunks;
                    data.extent = source_data.extent;
                    data.placement_chunk_offset = source_data.placement_chunk_offset;
                    if (filter_.active)
                    {
                        data.chunk_visibility.reserve(data.chunks.size());
                        for (const ViewShapeChunk &chunk : data.chunks)
                            data.chunk_visibility.push_back(visibility_for(chunk, *options.root));
                    }
                }
                data.visible_region = region;
                data.placement_tiles.clear();

                ViewPlacementTile culled;
                std::vector<IndexEntry> candidates;
                for (std::size_t t = 0; t < source_data.placement_tiles.size(); ++t)
                {
                    const ViewPlacements &tile = source_data.placement_tiles[t];
                    if (tile->placements.empty() || !bg::intersects(tile->extent, region))
                        continue; // the whole tile is off-screen - don't even build its index
                    // The tile's chunk's placement mask is parallel to its placements.
                    const std::size_t chunk = source_data.placement_chunk_offset + t;
                    const ChunkVisibility *visibility = chunk < data.chunk_visibility.size() ? data.chunk_visibility[chunk].get() : nullptr;
                    const std::vector<bool> *hidden = visibility && !visibility->hidden_placements.empty() ? &visibility->hidden_placements : nullptr;
                    candidates.clear();
                    spatial_index_for(tile).query(bgi::intersects(region), std::back_inserter(candidates));
                    for (const IndexEntry &entry : candidates)
                    {
                        if (hidden && entry.second < hidden->size() && (*hidden)[entry.second])
                            continue; // its Placement.type is hidden - neither drawn nor descended into
                        const ViewPlacementData &placement = tile->placements[entry.second];
                        // See this class's own doc comment - a placement
                        // whose own bbox is sub-pixel at options.scale is
                        // skipped entirely, the same way a sub-pixel Rect/
                        // Polygon already is inside Rasterize.
                        if (bbox_is_sub_pixel(placement.extent.ur.x - placement.extent.ll.x, placement.extent.ur.y - placement.extent.ll.y, options.scale))
                            continue;
                        culled.placements.push_back(placement);

                        const Rect shown = Geometry::transform_bbox(Geometry::invert(placement.transform), intersection(region, placement.extent));
                        auto [region_it, first_instance] = regions.try_emplace(placement.id, shown);
                        if (!first_instance)
                        {
                            if (contains(region_it->second, shown))
                                continue;
                            region_it->second = united(region_it->second, shown);
                        }
                        if (queued.insert(placement.id).second)
                            worklist.push_back(placement.id);
                    }
                }

                if (!culled.placements.empty())
                    data.placement_tiles.push_back(std::make_shared<const ViewPlacementTile>(std::move(culled)));
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
                   last.scale != current.scale ||
                   last.hidden_objects != current.hidden_objects;
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
        static Rect intersection(const Rect &a, const Rect &b)
        {
            return Rect{.ll = Point{std::max(a.ll.x, b.ll.x), std::max(a.ll.y, b.ll.y)},
                        .ur = Point{std::min(a.ur.x, b.ur.x), std::min(a.ur.y, b.ur.y)}};
        }

        static Rect united(const Rect &a, const Rect &b)
        {
            return Rect{.ll = Point{std::min(a.ll.x, b.ll.x), std::min(a.ll.y, b.ll.y)},
                        .ur = Point{std::max(a.ur.x, b.ur.x), std::max(a.ur.y, b.ur.y)}};
        }

        static bool contains(const Rect &outer, const Rect &inner)
        {
            return outer.ll.x <= inner.ll.x && outer.ll.y <= inner.ll.y && inner.ur.x <= outer.ur.x && inner.ur.y <= outer.ur.y;
        }

        // Rect (a node's own local placement bbox) paired with its own
        // index into that node's placement_data vector - the rtree's own
        // value type has to carry enough to recover the actual
        // ViewPlacementData a hit corresponds to.
        using IndexEntry = std::pair<Rect, std::size_t>;
        using SpatialIndex = bgi::rtree<IndexEntry, bgi::rstar<16>>;

        /// @brief A placement tile's spatial index over its placements'
        /// local (untransformed) extents - built on first use and reused
        /// across every later call that still shares the tile (every
        /// viewport-only "zoom tick", and every edit that leaves the tile
        /// alone - HierarchyResolverStage shares unchanged tiles between
        /// outputs).
        const SpatialIndex &spatial_index_for(const ViewPlacements &tile)
        {
            CachedIndex &cached = spatial_indices_[tile.get()];
            if (cached.tile.lock() == tile)
                return cached.index;

            std::vector<IndexEntry> entries;
            entries.reserve(tile->placements.size());
            for (std::size_t i = 0; i < tile->placements.size(); ++i)
                entries.emplace_back(tile->placements[i].extent, i); // everything it draws, overhang included
            cached = CachedIndex{.tile = tile, .index = SpatialIndex(entries)};
            ++index_builds_;
            return cached.index;
        }

        // The Placement.type/Route.use filter, resolved against Root: the
        // Designs whose placements are hidden, and a hidden flag per Route
        // pool index. Rebuilt when the filter or Root changes (it's
        // per-Design and per-Route, not per-shape). The mask cache is
        // dropped when the filter or the hidden Designs change; a Route's
        // use changing needs nothing extra - the resolver rebuilds that
        // Route's tile, so its chunk (and mask) is new.
        void update_filter(const ViewRenderOptions &options)
        {
            std::erase_if(visibility_cache_, [](const auto &entry)
                          { return entry.second.source.expired(); });
            if (!options.root || options.hidden_objects.empty())
            {
                filter_ = FilterContext{};
                visibility_cache_.clear();
                return;
            }
            if (filter_.active && filter_.filter == options.hidden_objects && filter_.root == options.root &&
                filter_.root_mutation_version == options.root_mutation_version)
                return;

            const Root &root = *options.root;
            FilterContext next{.active = true, .filter = options.hidden_objects, .root = options.root,
                               .root_mutation_version = options.root_mutation_version,
                               .hidden_designs = designs_of_placement_types(root, options.hidden_objects.placement_types)};
            if (!options.hidden_objects.route_uses.empty())
                for (const RouteId route_id : root.get_route_ids())
                    if (const RouteData *route = root.get_route(route_id); route && options.hidden_objects.route_uses.contains(route_use(*route)))
                    {
                        if (next.hidden_routes.size() <= route_id.index)
                            next.hidden_routes.resize(route_id.index + 1);
                        next.hidden_routes[route_id.index] = true;
                    }
            if (!filter_.active || filter_.filter != next.filter || filter_.root != next.root || filter_.hidden_designs != next.hidden_designs)
                visibility_cache_.clear();
            filter_ = std::move(next);
        }

        // `chunk`'s hidden masks under the current filter_ - null when it
        // hides nothing (or has no sources: nothing in it is a Route's or
        // Placement's). Cached per chunk.
        ChunkVisibilityHandle visibility_for(const ViewShapeChunk &chunk, const Root &root)
        {
            if (!chunk.sources || !chunk.shapes)
                return nullptr;
            CachedVisibility &cached = visibility_cache_[chunk.shapes.get()];
            if (cached.source.lock() == chunk.shapes)
                return cached.visibility;

            ChunkVisibility visibility;
            if (!filter_.hidden_routes.empty())
                for (const auto &[layer, shape_ids] : chunk.sources->shapes)
                {
                    std::vector<bool> hidden(shape_ids.size());
                    bool any = false;
                    for (std::size_t i = 0; i < shape_ids.size(); ++i)
                        if (const ShapeData *shape = root.get_shape(shape_ids[i]);
                            shape && shape->route().valid() && shape->route().index < filter_.hidden_routes.size() && filter_.hidden_routes[shape->route().index])
                            hidden[i] = any = true;
                    if (any)
                        visibility.hidden_shapes.emplace(layer, std::move(hidden));
                }
            if (!filter_.hidden_designs.empty())
            {
                const std::vector<PlacementId> &placements = chunk.sources->placements;
                std::vector<bool> hidden(placements.size());
                bool any = false;
                for (std::size_t i = 0; i < placements.size(); ++i)
                    if (const PlacementData *placement = root.get_placement(placements[i]);
                        placement && filter_.hidden_designs.contains(placement->reference_design))
                        hidden[i] = any = true;
                if (any)
                    visibility.hidden_placements = std::move(hidden);
            }

            cached.source = chunk.shapes;
            cached.visibility = visibility.hidden_shapes.empty() && visibility.hidden_placements.empty()
                                    ? nullptr
                                    : std::make_shared<const ChunkVisibility>(std::move(visibility));
            ++visibility_builds_;
            return cached.visibility;
        }

    public:
        /// @brief How many placement-tile indices this stage has built (for
        /// tests: an edit reuses every untouched tile's index).
        std::size_t index_builds() const { return index_builds_; }

        /// @brief How many chunk visibility masks this stage has built (for
        /// tests: a pan or an edit reuses every untouched chunk's mask).
        std::size_t visibility_builds() const { return visibility_builds_; }

    private:
        std::size_t index_builds_ = 0;
        std::size_t visibility_builds_ = 0;

        struct FilterContext
        {
            bool active = false;
            ObjectFilterSets filter;
            const Root *root = nullptr;
            std::uint64_t root_mutation_version = 0;
            std::unordered_set<DesignId> hidden_designs;
            std::vector<bool> hidden_routes; // by RouteId::index
        };
        FilterContext filter_;

        // A chunk's mask; `source` is weak so a replaced chunk isn't kept
        // alive, and checked before use in case its address was reused.
        struct CachedVisibility
        {
            std::weak_ptr<const ViewLayerShapes> source;
            ChunkVisibilityHandle visibility;
        };
        std::unordered_map<const ViewLayerShapes *, CachedVisibility> visibility_cache_;

        // A tile's index; `tile` is weak so the cache never keeps a
        // replaced tile alive, and is checked before use in case a dead
        // tile's address was reused.
        struct CachedIndex
        {
            std::weak_ptr<const ViewPlacementTile> tile;
            SpatialIndex index;
        };
        std::unordered_map<const ViewPlacementTile *, CachedIndex> spatial_indices_;
    };
}
