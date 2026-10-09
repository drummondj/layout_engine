#pragma once

#include "../../geometry/placement_geometry.hpp"
#include "../../geometry/row_geometry.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../view_style.hpp"
#include "../draw_helpers.hpp"
#include "../pipeline_options.hpp"
#include "../render_shape.hpp"
#include "../tbb_core.hpp"
#include "../port_markers.hpp"
#include "../via_shapes.hpp"

#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <cmath>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace le
{
    /// @brief Opt-in per-phase wall-clock timing of HierarchyResolverStage::
    /// compute(), for the resolver_profile dev tool (benchmarks/
    /// resolver_profile.cpp) - how a cold resolve splits across routes,
    /// placements, index building and so on. Set `g_resolver_phase_profile`
    /// to collect; left null (always, outside that tool) each phase costs
    /// one relaxed atomic load and no clock reads. Phases accumulate by
    /// name in first-seen order; compute() runs serially, never concurrently
    /// with itself.
    struct ResolverPhaseProfile
    {
        std::vector<std::pair<std::string, double>> ms;

        void add(const char *phase, double elapsed_ms)
        {
            for (auto &[name, total] : ms)
                if (name == phase)
                {
                    total += elapsed_ms;
                    return;
                }
            ms.emplace_back(phase, elapsed_ms);
        }
    };

    inline std::atomic<ResolverPhaseProfile *> g_resolver_phase_profile{nullptr};

    /// @brief Times its own scope into g_resolver_phase_profile, if set.
    class ResolverPhaseTimer
    {
    public:
        explicit ResolverPhaseTimer(const char *phase)
            : phase_(phase), profile_(g_resolver_phase_profile.load(std::memory_order_relaxed))
        {
            if (profile_)
                start_ = std::chrono::steady_clock::now();
        }
        ~ResolverPhaseTimer()
        {
            if (profile_)
                profile_->add(phase_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count());
        }
        ResolverPhaseTimer(const ResolverPhaseTimer &) = delete;
        ResolverPhaseTimer &operator=(const ResolverPhaseTimer &) = delete;

    private:
        const char *phase_;
        ResolverPhaseProfile *profile_;
        std::chrono::steady_clock::time_point start_;
    };

    /// @brief Which further Abstract or Layout a Placement resolves to
    /// (also HierarchyResolverOutput's own map key). See
    /// resolve_design_target (geometry/placement_geometry.hpp)
    /// for the single source of truth on Layout-vs-Abstract dispatch.
    using HierarchyId = std::variant<AbstractId, LayoutId>;

    /// @brief std::variant<AbstractId, LayoutId> has no std::hash of its
    /// own (unlike Id<Tag> itself, ids.hpp) - needed to key
    /// HierarchyResolverOutput::view_data.
    struct HierarchyIdHash
    {
        std::size_t operator()(const HierarchyId &id) const
        {
            std::size_t seed = id.index();
            std::visit(
                [&seed](const auto &value)
                {
                    seed ^= std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                },
                id);
            return seed;
        }
    };

    /// @brief One placed instance of a Layout's own content. `id` is already
    /// resolved (Layout vs. Abstract, per HierarchyId's own comment)
    /// rather than the raw PlacementData::reference_design, so a Warm-tier
    /// consumer never has to re-run resolve_design_target itself.
    ///
    /// `location`/`orientation`, `bbox`, and `transform` each serve a
    /// different downstream purpose - none substitutes for another:
    ///   - `bbox` (this placement's own resolved world-space footprint, in
    ///     this Layout's own local dbu space) is what a Warm-tier
    ///     viewport-culling stage tests against the viewport Rect - cheap
    ///     AABB-vs-AABB, no per-shape work.
    ///   - `transform` (Geometry::InstanceTransform - a linear component
    ///     plus a translation) is what culling composes with its own
    ///     running accumulated transform as it recurses into this
    ///     placement's own referenced id, so a *nested* placement's own
    ///     bbox (stored in its own immediate parent's local space) can be
    ///     tested against the same top-level viewport. Stored directly
    ///     (computed once here, in compute()) rather than recomputed
    ///     downstream from `location`/`orientation` - `bbox` is a one-way
    ///     AABB of the transformed corners, not invertible back into a
    ///     transform for a rotated/flipped orientation, and recomputing it
    ///     properly would mean a Warm-tier stage re-deriving the child's
    ///     own local bbox from Root again, defeating Cold's whole point of
    ///     producing a self-contained, Root-independent snapshot.
    ///   - `location`/`orientation` are the placement's own raw DEF-level
    ///     placement point and orientation - still meaningful in their own
    ///     right (e.g. a Hot-tier inspector showing a placement's nominal
    ///     origin) independent of the derived `bbox`/`transform`, and
    ///     `location`/`bbox.ll` only coincide today because
    ///     AbstractData.origin isn't applied yet (geometry/placement_geometry.hpp's
    ///     own resolved_local_bbox comment) - once it is, they can
    ///     genuinely differ.
    struct ViewPlacementData
    {
        HierarchyId id;
        Point location;
        Rect bbox;
        Geometry::InstanceTransform transform;
        Orientation orientation = Orientation::N;
        /// @brief Everything this placement draws, in the parent's space:
        /// `bbox` grown by the placed node's own `ViewData::extent` -
        /// content past the cell's boundary (an Abstract's pins or
        /// obstructions overhanging its SIZE, say) included. Set by
        /// HierarchyResolverStage::assign_extents.
        Rect extent;
    };

    /// @brief One node's own direct shapes, grouped by the ViewLayer they
    /// draw on - not a flat list, so a Warm-tier drawing stage
    /// (RasterizeBlend2DStage) never needs to detect "same layer as the
    /// previous shape" while iterating: it walks ViewLayerSet::all() (in
    /// that ViewLayerSet's own bottom-to-top insertion/z-order, since a
    /// fresh ViewLayerSet's own ViewLayerId.index is assigned strictly in
    /// build_for_technology's own call order - see that method's own doc
    /// comment) and looks up each layer's own group directly - which
    /// also means setting fill/stroke style once per *layer* instead of
    /// per *shape* (hoisted out of the per-shape loop,
    /// draw_view_shapes_blend2d's own comment) and, for a not-yet-built
    /// layer-visibility feature,
    /// skipping a hidden layer's whole group in one map lookup rather
    /// than checking every individual shape's own layer. `Id<Tag>`
    /// already has a std::hash specialization (ids.hpp) - no custom
    /// hasher needed here, unlike HierarchyIdHash above (a variant, not a
    /// plain Id).
    ///
    /// A Warm-tier stage (ViewportCullStage) builds a new ViewData per
    /// node with different `placement_data` but the exact same `shapes`,
    /// every single viewport-only call. Copying a ~1,000,000-entry shapes
    /// structure by value on every such call would dominate the Warm
    /// tier's budget, so it's a shared_ptr-wrapped handle (a refcount
    /// bump regardless of payload size, as with MemoizingStage's own
    /// OutputHandle).
    using ViewLayerShapes = std::unordered_map<ViewLayerId, std::vector<RenderShape>>;
    using ViewShapesHandle = std::shared_ptr<const ViewLayerShapes>;

    /// @brief Per-ViewLayer spatial index over `ViewLayerShapes`' own
    /// per-layer shape vectors, built once alongside `shapes` (see
    /// build_shape_index() below) so RasterizeBlend2DStage can find which
    /// shapes actually overlap the current viewport without walking
    /// every shape in a huge flat node on every pan/zoom tick - the
    /// per-*shape* culling gap ViewportCullStage's own doc comment
    /// explicitly names (that stage only culls placements/instances, one
    /// level up). Mirrors ViewportCullStage's own SpatialIndex/IndexEntry
    /// pattern exactly (a Boost.Geometry Index R-tree storing a bbox
    /// paired with an index into the *existing* shape vector, not a copy
    /// of the shape itself), just applied to shapes within a node instead
    /// of placements across nodes.
    using ShapeIndexEntry = std::pair<Rect, std::size_t>;
    using ShapeSpatialIndex = boost::geometry::index::rtree<ShapeIndexEntry, boost::geometry::index::rstar<16>>;
    using ViewLayerShapeIndex = std::unordered_map<ViewLayerId, ShapeSpatialIndex>;
    using ViewShapesIndexHandle = std::shared_ptr<const ViewLayerShapeIndex>;

    /// @brief Mirrors LeHandle::is_view_layer_visible exactly: visible
    /// only if BOTH its own layer-name entry
    /// (if any) and its own purpose entry (if any) say so - an unset key
    /// in either map means visible, not hidden. Defined here (rather than
    /// in a Blend2D-specific drawing header) since it's a pure function of
    /// `le::`/std types with no rendering-backend dependency at all -
    /// draw_view_shapes_blend2d (rasterize_blend2d_stage.hpp) calls this
    /// same definition.
    inline bool is_view_layer_visible(
        const std::unordered_map<std::string, bool> &layer_name_visible, const std::unordered_map<ViewLayerPurpose, bool> &purpose_visible,
        const std::string &layer_name, ViewLayerPurpose purpose)
    {
        const auto name_it = layer_name_visible.find(layer_name);
        if (name_it != layer_name_visible.end() && !name_it->second)
            return false;
        const auto purpose_it = purpose_visible.find(purpose);
        if (purpose_it != purpose_visible.end() && !purpose_it->second)
            return false;
        return true;
    }

    /// @brief One Abstract's or Layout's own resolved content. `shapes`
    /// is this node's own
    /// *direct* geometry only (an Abstract's Terminals/Obstructions/
    /// boundary; a Layout's own diearea/blockages/routes/physical ports/
    /// rows/tracks/gcell grids/regions) - a placed child's own shapes live
    /// under its own id in HierarchyResolverOutput::view_data, not
    /// duplicated here; composing a placement's own transform onto its
    /// child's shapes is a Warm-tier concern, not Cold's. `shapes_index`
    /// is built once from `shapes` right after it's constructed (see
    /// compute()'s own two call sites) - like `shapes` itself, a
    /// shared_ptr copy elsewhere (ViewportCullStage's own per-tick ViewData
    /// rebuild) is a refcount bump, not a rebuild.
    /// @brief One separately-cached slice of a node's direct shapes, with
    /// its own spatial index. Chunks are immutable once built and shared
    /// (by refcount) between successive outputs, so an edit rebuilds only
    /// the chunks it touched and every per-chunk downstream cache (e.g.
    /// RasterizeBlend2DStage's route-outline cache) survives it.
    /// @brief Which database object each of a chunk's RenderShapes came
    /// from - so click selection (api.cpp) can find the objects under the
    /// mouse by querying the render tree instead of scanning the design.
    /// `shapes[layer][i]` is the Shape behind `chunk.shapes->at(layer)[i]`
    /// (a via's owning Shape for its synthesized geometry; invalid for one
    /// with none, a port marker); `placements[i]` the Placement behind rect
    /// `i` of the chunk's batched PLACEMENT shape. Only chunks with
    /// selectable content carry sources: route tiles, PORTS, placement tiles.
    struct ChunkSources
    {
        std::unordered_map<ViewLayerId, std::vector<ShapeId>> shapes;
        std::vector<PlacementId> placements;
    };
    using ChunkSourcesHandle = std::shared_ptr<const ChunkSources>;

    struct ViewShapeChunk
    {
        ViewShapesHandle shapes;
        ViewShapesIndexHandle shapes_index;
        ChunkSourcesHandle sources; // null for a chunk with nothing selectable
    };

    /// @brief A Layout node's fixed chunks, first in ViewData::chunks -
    /// one per kind of small content, so an edit to one kind leaves the
    /// others untouched. After them come the Layout's route tiles, then
    /// its placement tiles (HierarchyResolverStage's LayoutTiling): routes
    /// and placements are nearly all of a real design, so they're split
    /// spatially and an edit rebuilds only the
    /// tiles it touched. An Abstract node has one chunk.
    /// After the core kinds come one chunk per renderable class
    /// (renderable_classes.hpp): chunk kCoreLayoutChunkCount + R::index.
    enum class LayoutChunk : std::uint8_t
    {
        DIEAREA_BLOCKAGES,
        PORTS,
        FREE_SHAPES,
        ROWS_TRACKS_GCELLS_REGIONS,
    };
    inline constexpr std::size_t kCoreLayoutChunkCount = 4;
    inline constexpr std::size_t kFixedLayoutChunkCount = kCoreLayoutChunkCount + renderable::kCount;

    /// @brief A renderable class's object id, whichever class (its R::Id's
    /// index and generation) - so one tiling type serves every class.
    struct RenderableObjectId
    {
        std::uint32_t index = 0;
        std::uint32_t generation = 0;
        bool operator==(const RenderableObjectId &) const = default;
    };
    struct RenderableObjectIdHash
    {
        std::size_t operator()(const RenderableObjectId &id) const noexcept { return (std::size_t{id.generation} << 32) ^ id.index; }
    };

    /// @brief The fixed chunk holding renderable class R's objects (empty
    /// for a tiled class, whose objects are in its own tiles).
    template <class R>
    constexpr LayoutChunk renderable_chunk() { return static_cast<LayoutChunk>(kCoreLayoutChunkCount + R::index); }

    /// @brief One placement tile's resolved child placements - immutable
    /// and shared between outputs like a chunk, so ViewportCullStage keys
    /// its per-tile index on it. `extent` is the union of the placements'
    /// extents (meaningless when empty); `children` the distinct nodes
    /// they place.
    struct ViewPlacementTile
    {
        std::vector<ViewPlacementData> placements;
        Rect extent;
        std::vector<HierarchyId> children;
    };
    using ViewPlacements = std::shared_ptr<const ViewPlacementTile>;

    /// @brief Which of one chunk's objects are hidden by the Placement.type/
    /// Route.use filters (ViewRenderOptions::hidden_objects) - set bits are
    /// hidden. `hidden_shapes[layer][i]` is parallel to
    /// `chunk.shapes->at(layer)` (a layer with nothing hidden is absent);
    /// `hidden_placements[i]` to `chunk.sources->placements` - rect and
    /// label `i` of the chunk's batched PLACEMENT shape. Built by
    /// ViewportCullStage, read by RasterizeBlend2DStage.
    struct ChunkVisibility
    {
        std::unordered_map<ViewLayerId, std::vector<bool>> hidden_shapes;
        std::vector<bool> hidden_placements;
    };
    using ChunkVisibilityHandle = std::shared_ptr<const ChunkVisibility>;

    struct ViewData
    {
        /// @brief The node's direct shapes: for a Layout its fixed chunks
        /// (LayoutChunk order), route tiles and placement tiles; for an
        /// Abstract one chunk.
        std::vector<ViewShapeChunk> chunks;
        /// @brief Resolved child placements, per placement tile (never
        /// null). ViewportCullStage's output holds at most one tile per
        /// node - the culled placements.
        std::vector<ViewPlacements> placement_tiles;
        /// @brief The node's declared bbox (diearea/boundary) grown to
        /// cover everything it draws - its own shapes and its placements'
        /// `extent`s, so nothing outside a cell's boundary is clipped.
        Rect extent;
        /// @brief Set only in ViewportCullStage's output: the part of the
        /// node any visible instance of it shows, in its local space (the
        /// viewport itself for the top level). RasterizeBlend2DStage sizes
        /// a nested node's image to this, or to `extent` when it's unset.
        std::optional<Rect> visible_region;
        /// @brief The hierarchy depth budget this node was resolved with
        /// (HierarchyResolverStage re-resolves its placements with it).
        int remaining_depth = 0;
        /// @brief Index in `chunks` of placement tile 0's chunk: placement
        /// tile `t`'s rects/labels and sources are `chunks[offset + t]`,
        /// whose `sources->placements[j]` is `placement_tiles[t]`'s
        /// placement `j` when the node has depth left.
        std::size_t placement_chunk_offset = 0;
        /// @brief Per-chunk hidden masks, parallel to `chunks` - set only
        /// in ViewportCullStage's output; empty, or a null entry, means
        /// nothing in that chunk is hidden.
        std::vector<ChunkVisibilityHandle> chunk_visibility;
    };

    /// @brief Calls `fn(const ViewPlacementData &)` for every placement of `data`.
    template <typename Fn>
    void for_each_placement(const ViewData &data, Fn &&fn)
    {
        for (const ViewPlacements &tile : data.placement_tiles)
            for (const ViewPlacementData &placement : tile->placements)
                fn(placement);
    }

    inline std::size_t placement_count(const ViewData &data)
    {
        std::size_t count = 0;
        for (const ViewPlacements &tile : data.placement_tiles)
            count += tile->placements.size();
        return count;
    }

    /// @brief Every shape `data` draws on `view_layer`, across its chunks.
    inline std::vector<const RenderShape *> view_data_shapes(const ViewData &data, ViewLayerId view_layer)
    {
        std::vector<const RenderShape *> shapes;
        for (const ViewShapeChunk &chunk : data.chunks)
            if (chunk.shapes)
                if (const auto it = chunk.shapes->find(view_layer); it != chunk.shapes->end())
                    for (const RenderShape &shape : it->second)
                        shapes.push_back(&shape);
        return shapes;
    }

    /// @brief HierarchyResolverStage's own InputData - LayerGenerationStage's
    /// own OutputHandle (tbb_core.hpp's MemoizingStage::OutputHandle), so
    /// ViewRenderPipeline (view_render_pipeline.hpp) can wire the two
    /// stages together with a real make_edge and no adapter node in
    /// between - both sides
    /// of that edge are exactly this type. The Root pointer this stage
    /// also needs travels via ViewRenderOptions::root instead of being
    /// part of this InputData - it isn't part of LayerGenerationStage's
    /// own output, so it couldn't flow through that same edge. Declared
    /// ahead of HierarchyResolverOutput below (not in the usual "InputData
    /// right before the stage that consumes it" spot a little further
    /// down) since that struct's own view_layers field needs this alias
    /// already in scope.
    using ViewLayerSetHandle = std::shared_ptr<const ViewLayerSet>;

    /// @brief HierarchyResolverStage's output - every
    /// Abstract/Layout its traversal reached, keyed
    /// by its own id.
    struct HierarchyResolverOutput
    {
        std::unordered_map<HierarchyId, ViewData, HierarchyIdHash> view_data;

        /// @brief Echoes this stage's own InputData (ViewLayerSetHandle)
        /// back out, unchanged - the only way a stage further downstream
        /// in a real make_edge chain (ViewportCullStage, RasterizeBlend2DStage)
        /// can still reach the actual ViewLayerSet content: nothing else
        /// wires LayerGenerationStage's own output any further than this
        /// stage's own InputData. Used to carry `view_layers` from Cold
        /// tier through to RasterizeBlend2DStage without going through
        /// ViewRenderOptions::view_layers (removed - ViewRenderPipeline::run(),
        /// view_render_pipeline.hpp, no longer needs to patch it into
        /// `options` between two separate graph submissions once it
        /// travels as data like this instead).
        /// ViewportCullStage's own OutputData is this same struct type -
        /// its own compute() just copies this field through unchanged
        /// alongside its real (culled) view_data.
        ViewLayerSetHandle view_layers;

        /// @brief What HierarchyResolverStage resolved this from - the Root,
        /// top_level and hierarchy_depth, and the Root change log's
        /// end_sequence() at the time - so a consumer outside the pipeline
        /// (click selection) can tell whether it matches the current view
        /// and which edits it doesn't include yet. Unset (null root) in
        /// ViewportCullStage's output.
        const Root *root = nullptr;
        HierarchyId top_level;
        int hierarchy_depth = 0;
        std::uint64_t log_end = 0;
    };

    /// @brief pipeline_stage_benchmark cache-stat helper (tbb_core.hpp's
    /// estimate_output_object_count/estimate_output_bytes hooks) - a
    /// rough per-RenderShape memory estimate: the struct itself plus every
    /// owned vector's own *capacity* (not size - capacity is what's
    /// actually allocated) times its element size, recursing one level
    /// into Polygon/Path's own point lists and Text's own label string.
    /// Approximate (doesn't count map/vector bucket overhead, small-
    /// string-optimization thresholds, etc.) but the right order of
    /// magnitude - real per-shape overhead is dominated by these vectors,
    /// not bookkeeping (RenderShape's own sizeof is just 4 empty-vector
    /// headers - see that struct's own doc comment, render_shape.hpp).
    inline std::size_t estimate_shape_bytes(const RenderShape &shape)
    {
        std::size_t bytes = sizeof(RenderShape);
        bytes += shape.rects.capacity() * sizeof(Rect);
        for (const Polygon &polygon : shape.polygons)
            bytes += polygon.points.capacity() * sizeof(Point);
        for (const Path &path : shape.paths)
            bytes += path.polygon.points.capacity() * sizeof(Point);
        for (const Text &text : shape.texts)
            bytes += sizeof(Text) + text.label.capacity();
        return bytes;
    }

    struct HierarchyResolverOutputStats
    {
        std::size_t shape_count = 0;
        std::size_t placement_count = 0;

        /// @brief Bytes of the actual Shape geometry (ViewData::shapes)
        /// reachable from this output - real, allocated memory, but only
        /// this output's own to *count* if it's also the output that
        /// *allocated* it. See owned_bytes()'s own comment: a stage that
        /// merely holds a shared_ptr alias to another stage's already-
        /// built shapes (ViewportCullStage) must not add this back into
        /// its own reported footprint, or the same bytes get counted
        /// twice across the two stages' reports.
        std::size_t shape_bytes = 0;

        /// @brief Bytes of every node's own placement_data vector plus
        /// per-node map/ViewData overhead - always a fresh allocation
        /// specific to *this* output (HierarchyResolverStage's own
        /// unfiltered list, or ViewportCullStage's own culled subset -
        /// never shared between the two), unlike shape_bytes above.
        std::size_t own_overhead_bytes = 0;

        /// @brief Total "objects" this output holds, per
        /// MemoizingStage::cache_object_count()'s own ask - shapes AND
        /// surviving placements both count. Distinguishing the two
        /// matters for ViewportCullStage specifically: at
        /// hierarchy_depth > 0, zooming in should shrink
        /// `placement_count` (fewer child placements survive spatial
        /// culling against a smaller viewport - see this stage's own
        /// compute()) even when `shape_count` doesn't change at all (a
        /// surviving node's own *direct* shapes are copied through
        /// unfiltered - shape-level culling isn't this stage's job,
        /// its own doc comment).
        std::size_t object_count() const { return shape_count + placement_count; }

        /// @brief The bytes HierarchyResolverStage's own cache is
        /// responsible for - it's the one stage that actually allocates
        /// the ViewLayerShapes/Shape content in the first place
        /// (compute()'s own `std::make_shared<const ViewLayerShapes>`
        /// calls), so shape_bytes is genuinely its own memory.
        std::size_t owned_bytes_including_shapes() const { return shape_bytes + own_overhead_bytes; }

        /// @brief The bytes ViewportCullStage's own cache is responsible
        /// for - excludes shape_bytes entirely: that stage's compute()
        /// does `data.shapes = source_data.shapes;`, a shared_ptr copy (a
        /// refcount bump) of the exact same ViewLayerShapes
        /// HierarchyResolverStage already built, not a duplicate (see
        /// ViewShapesHandle's own doc comment). Only `placement_data`
        /// (this stage's own freshly-built culled subset) is real,
        /// additional memory - reporting shape_bytes here too would
        /// double-count bytes already attributed to HierarchyResolverStage's
        /// own cache_bytes().
        std::size_t owned_bytes_excluding_shapes() const { return own_overhead_bytes; }
    };

    /// @brief Shared by HierarchyResolverStage and ViewportCullStage
    /// (identical OutputData shape, tbb_core.hpp's own cache-stat hooks) -
    /// sums real geometry across every resolved node's own `shapes` plus
    /// every node's own `placement_data` (surviving child placements).
    /// Always computes shape_bytes (needed either way to report
    /// shape_count without a second pass), but which of
    /// owned_bytes_including_shapes()/owned_bytes_excluding_shapes() a
    /// caller should actually report as "this stage's own cache_bytes()"
    /// depends on whether that stage allocated the shapes or merely
    /// references them - see those two methods' own comments.
    inline HierarchyResolverOutputStats estimate_hierarchy_resolver_output_stats(const HierarchyResolverOutput &output)
    {
        HierarchyResolverOutputStats stats;
        for (const auto &[id, data] : output.view_data)
        {
            stats.placement_count += placement_count(data);
            stats.own_overhead_bytes += sizeof(ViewData);
            for (const ViewPlacements &tile : data.placement_tiles)
                stats.own_overhead_bytes += tile->placements.capacity() * sizeof(ViewPlacementData);
            for (const ViewShapeChunk &chunk : data.chunks)
            {
                if (!chunk.shapes)
                    continue;
                for (const auto &[view_layer_id, shapes] : *chunk.shapes)
                {
                    stats.shape_count += shapes.size();
                    for (const RenderShape &shape : shapes)
                        stats.shape_bytes += estimate_shape_bytes(shape);
                }
            }
        }
        return stats;
    }

    /// @brief Cold-tier stage 2 (plans/PIPELINE_REFACTOR.md): traverses
    /// Placement -> Design hierarchy from ViewRenderOptions::top_level,
    /// consuming one unit of ViewRenderOptions::hierarchy_depth per
    /// Layout -> Layout hop. At remaining_depth == 0 a Layout's own
    /// placements are never *resolved* into anything at all (not even a
    /// fallback to their own Abstract) - placement_data stays empty and
    /// nothing is pushed onto the worklist - but each placement's own
    /// PLACEMENT placeholder rect+label is still drawn (see the
    /// main compute() loop's own comment), since that's real data about
    /// this Layout's own direct content, not about what a placement
    /// resolves to. This deliberately departs from
    /// resolve_design_target's "fall back to the Abstract regardless of
    /// remaining depth" convention (geometry/placement_geometry.hpp), which
    /// is still used here for sizing a placement's own placeholder
    /// rect/ViewPlacementData::bbox and by every other caller
    /// (hit-testing, LeHandle::hierarchy_depth()'s documented semantics):
    /// "traverses hierarchy ... until hierarchy_depth is 0" is read
    /// literally here, not as "one further Abstract-only hop past 0."
    ///
    /// Gathers every reached Abstract's/Layout's own *direct* shapes
    /// (Terminal/Obstruction/boundary for an Abstract; diearea/Blockage/
    /// Route/PhysicalPort/synthesized Row/Track/GCellGrid/Region/
    /// PlacementBoundary for a Layout - the last one a synthesized,
    /// name-labeled outline of each of the Layout's own Placements' own
    /// resolved footprint, not real LEF/DEF geometry), each resolved to
    /// its ViewLayerId. Also expands RECT/PATH/POLYGON ITERATE, places
    /// Terminal name labels, and expands vias (via_shapes.hpp). Shapes
    /// carry no ShapeId - hit-testing works against Root directly
    /// (geometry/placement_geometry.hpp).
    ///
    /// Traversal is breadth-first, one worklist entry per discovered
    /// {id, remaining_depth}, deduplicating by `id` alone - not by
    /// {id, remaining_depth}: ViewData never bakes in a recursively-
    /// composed picture, so a Layout's own
    /// *direct* shapes genuinely don't depend on remaining_depth - only
    /// which further id one of its own placements resolves to does, and
    /// BFS visits every id at its shallowest discovered depth first, which
    /// is also its most-generous one (an id placed at two different depths
    /// in a real hierarchy - unusual, but not impossible - resolves its
    /// own further placements using whichever depth got there first, never
    /// a stricter/shallower one found later). Revisit if a real fixture
    /// needs otherwise.
    ///
    /// Recompute trigger: root_mutation_version, top_level, and
    /// hierarchy_depth all matter here (unlike LayerGenerationStage, which
    /// only cares about the first) - a top_level/hierarchy_depth change
    /// re-walks the same Root from a different starting point or budget,
    /// producing a different HierarchyResolverOutput even though nothing
    /// in the database itself changed. When wired to LayerGenerationStage
    /// via a real make_edge (ViewRenderPipeline), a ViewLayerSet rebuild also
    /// forces a recompute here even if none of the three fields above
    /// changed - LayerGenerationStage's own bumped version() becomes this
    /// stage's own incoming data_version, and execute()'s should_recompute
    /// check ORs that against options_did_change() below.
    ///
    /// Incremental updates: a recompute after an edit (same Root,
    /// top_level and hierarchy_depth; a ViewLayerSet with the same ids -
    /// a rebuilt or recolored one qualifies) reads the Root change log
    /// since the previous compute() and rebuilds only the touched chunks -
    /// a Layout's routes and placements are split into spatial tiles
    /// (about kRoutesPerTile/kPlacementsPerTile each, LayoutTiling), so a
    /// route edit or placement move rebuilds its old and new tile, a fixed
    /// LayoutChunk edit that chunk, a cell edit that Abstract - sharing
    /// every other chunk and placement tile with the previous output. Anything
    /// it can't place precisely (technology/library/design edits, a
    /// created or deleted Abstract/Layout, a saturated or wrapped log)
    /// falls back to the full resolve. last_compute_was_incremental() says
    /// which ran. resolver_profile's edit.* rows measure it.
    class HierarchyResolverStage : public MemoizingStage<ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>
    {
    public:
        explicit HierarchyResolverStage(oneapi::tbb::flow::graph &g, std::string label = "HierarchyResolver")
            : MemoizingStage(g, std::move(label)) {}

    protected:
        HierarchyResolverOutput compute(const ViewLayerSetHandle &view_layers_handle, const ViewRenderOptions &options) override
        {
            last_compute_was_incremental_ = false;
            if (options.root == nullptr)
            {
                state_.reset();
                return HierarchyResolverOutput{.view_layers = view_layers_handle};
            }

            const Root &root = *options.root;
            static const ViewLayerSet kEmptyViewLayers;
            const ViewLayerSet &view_layers = view_layers_handle != nullptr ? *view_layers_handle : kEmptyViewLayers;
            // Captured before reading anything: entries logged after this
            // belong to the next compute().
            const std::uint64_t log_end = root.change_log().end_sequence();

            std::optional<HierarchyResolverOutput> result;
            if (can_update_incrementally(root, view_layers, options))
                result = update_incrementally(root, view_layers_handle, view_layers, options);
            last_compute_was_incremental_ = result.has_value();
            if (!result)
                result = resolve_everything(root, view_layers_handle, view_layers, options);

            state_ = ResolveState{.root = &root, .top_level = options.top_level, .hierarchy_depth = options.hierarchy_depth, .log_end = log_end};
            result->root = &root;
            result->top_level = options.top_level;
            result->hierarchy_depth = options.hierarchy_depth;
            result->log_end = log_end;
            return std::move(*result);
        }

    public:
        /// @brief Whether the last compute() updated the previous output
        /// from the Root change log rather than resolving everything.
        bool last_compute_was_incremental() const { return last_compute_was_incremental_; }

    protected:
        bool options_did_change(const ViewRenderOptions &last, const ViewRenderOptions &current) const override
        {
            return last.root_mutation_version != current.root_mutation_version ||
                   last.top_level != current.top_level ||
                   last.hierarchy_depth != current.hierarchy_depth;
        }

        // pipeline_stage_benchmark cache-stat hooks (tbb_core.hpp) - this
        // stage is the one that actually allocates the ViewLayerShapes/
        // Shape content in the first place (compute()'s own
        // std::make_shared<const ViewLayerShapes> calls), so
        // owned_bytes_including_shapes() is the right number here -
        // see HierarchyResolverOutputStats' own comment; ViewportCullStage's
        // own override (viewport_cull_stage.hpp) deliberately reports a
        // different, smaller number since it only references this data.
        std::size_t estimate_output_object_count(const HierarchyResolverOutput &output) const override
        {
            return estimate_hierarchy_resolver_output_stats(output).object_count();
        }

        std::size_t estimate_output_bytes(const HierarchyResolverOutput &output) const override
        {
            return estimate_hierarchy_resolver_output_stats(output).owned_bytes_including_shapes();
        }

    private:
        // Builds ViewData::shapes_index from an already-built ViewLayerShapes -
        // one Boost.Geometry Index R-tree per ViewLayerId, bulk-loaded
        // (constructing an rtree from a std::vector triggers Boost's own
        // packing algorithm, not incremental one-at-a-time insertion - the
        // same technique ViewportCullStage's own spatial_index_for uses),
        // storing each shape's own bbox paired with its index into that
        // layer's own vector rather than a copy of the Shape itself.
        // Geometry::bbox returns nullopt for a shape with no rects/
        // polygons/paths of its own (e.g. a via-only Shape before
        // append_via_shapes expands it into separate real-geometry
        // Shapes) - skipped here, exactly equivalent to today's
        // unindexed draw_view_shapes, which already draws nothing for
        // such a shape either way (its per-geometry-kind loops simply
        // don't execute). Geometry::bbox is a template (geometry.hpp) so
        // this resolves against RenderShape without any change here.
        // --- Full and incremental resolution -------------------------------
        //
        // A full resolve walks the hierarchy breadth-first from top_level
        // (one worklist entry per discovered {id, remaining_depth},
        // deduplicated by id - see the class comment) and builds every
        // node's chunks. An incremental one (after an edit) starts from the
        // previous output and rebuilds only the chunks and tiles the Root
        // change log says were touched; every other chunk and placement
        // tile is shared with the previous output, not copied or freed.

        struct ResolveState
        {
            const Root *root = nullptr;
            HierarchyId top_level;
            int hierarchy_depth = 0;
            std::uint64_t log_end = 0;
        };

        struct WorkItem
        {
            HierarchyId id;
            int remaining_depth;
        };

        // About this many routes/placements per tile: an edit rebuilds a
        // tile or two, so this bounds its cost; more tiles cost a little
        // per frame (one per-layer lookup each) and per resolve.
        static constexpr std::size_t kRoutesPerTile = 2000;
        static constexpr std::size_t kPlacementsPerTile = 2000;
        static constexpr std::size_t kRenderablesPerTile = 2000;
        static constexpr std::size_t kMaxTilesPerSide = 64;

        // An n x n grid over `bounds`; points outside land in edge tiles.
        struct TileGrid
        {
            Rect bounds;
            std::size_t n = 1;

            std::size_t count() const { return n * n; }

            std::size_t tile_of(Point p) const
            {
                if (n == 1)
                    return 0;
                auto cell = [&](int64_t v, int64_t lo, int64_t hi)
                {
                    const int64_t span = std::max<int64_t>(hi - lo, 1);
                    const int64_t c = (v - lo) * static_cast<int64_t>(n) / span;
                    return static_cast<std::size_t>(std::clamp<int64_t>(c, 0, static_cast<int64_t>(n) - 1));
                };
                return cell(p.y, bounds.ll.y, bounds.ur.y) * n + cell(p.x, bounds.ll.x, bounds.ur.x);
            }

            static TileGrid make(const Rect &bounds, std::size_t items, std::size_t per_tile)
            {
                const std::size_t tiles = std::max<std::size_t>(1, (items + per_tile - 1) / per_tile);
                const auto side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(tiles))));
                return TileGrid{.bounds = bounds, .n = std::clamp<std::size_t>(side, 1, kMaxTilesPerSide)};
            }
        };

        // Which tile each object of one kind is in, and each tile's
        // members (in assignment order).
        template <typename IdT>
        struct TileMembership
        {
            TileGrid grid;
            std::vector<std::vector<IdT>> members;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> by_index; // id.index -> {generation + 1, tile}; 0 = none

            std::optional<std::size_t> tile_of_id(IdT id) const
            {
                if (id.index >= by_index.size() || by_index[id.index].first != id.generation + 1)
                    return std::nullopt;
                return by_index[id.index].second;
            }

            void assign(IdT id, std::size_t tile)
            {
                members[tile].push_back(id);
                if (id.index >= by_index.size())
                    by_index.resize(static_cast<std::size_t>(id.index) + 1, {0, 0});
                by_index[id.index] = {id.generation + 1, static_cast<std::uint32_t>(tile)};
            }

            // Removes `id` from its tile, returning that tile.
            std::optional<std::size_t> remove(IdT id)
            {
                const std::optional<std::size_t> tile = tile_of_id(id);
                if (!tile)
                    return std::nullopt;
                std::erase(members[*tile], id);
                by_index[id.index] = {0, 0};
                return tile;
            }
        };

        // A Layout's tiles: route tiles, then placement tiles, then each
        // tiled renderable class's (an untiled class has a 0-tile grid).
        struct LayoutTiling
        {
            TileMembership<RouteId> routes;
            TileMembership<PlacementId> placements;
            std::array<TileMembership<RenderableObjectId>, renderable::kCount> renderables;

            std::size_t route_chunk(std::size_t tile) const { return kFixedLayoutChunkCount + tile; }
            std::size_t placement_chunk(std::size_t tile) const { return kFixedLayoutChunkCount + routes.grid.count() + tile; }
            std::size_t renderable_chunk(std::size_t r, std::size_t tile) const
            {
                std::size_t chunk = placement_chunk(0) + placements.grid.count();
                for (std::size_t i = 0; i < r; ++i)
                    chunk += renderables[i].grid.count();
                return chunk + tile;
            }
            std::size_t chunk_count() const { return renderable_chunk(renderable::kCount, 0); }
        };

        // A renderable object's tile anchor: the center of its first shape
        // with geometry, as for a route.
        template <class R>
        static std::optional<Point> renderable_anchor(const Root &root, RenderableObjectId id)
        {
            for (const ShapeId shape_id : R::shapes(root, typename R::Id{id.index, id.generation}))
                if (const ShapeData *shape = root.get_shape(shape_id))
                    if (const std::optional<Rect> box = Geometry::bbox(*shape))
                        return Point{.x = box->ll.x + (box->ur.x - box->ll.x) / 2, .y = box->ll.y + (box->ur.y - box->ll.y) / 2};
            return std::nullopt;
        }

        // A route's tile anchor: the center of its first shape with geometry.
        static std::optional<Point> route_anchor(const Root &root, RouteId route)
        {
            for (const ShapeId shape_id : root.get_route_shapes(route))
                if (const ShapeData *shape = root.get_shape(shape_id))
                    if (const std::optional<Rect> box = Geometry::bbox(*shape))
                        return Point{.x = box->ll.x + (box->ur.x - box->ll.x) / 2, .y = box->ll.y + (box->ur.y - box->ll.y) / 2};
            return std::nullopt;
        }

        static std::optional<Point> placement_anchor(const Root &root, PlacementId placement)
        {
            const PlacementData *data = root.get_placement(placement);
            return data ? data->location : std::nullopt;
        }

        // Placement tiles built this compute() and not yet published, per
        // node and tile - assign_extents fills their extents, then wraps them.
        using FreshPlacements = std::unordered_map<HierarchyId, std::unordered_map<std::size_t, std::vector<ViewPlacementData>>, HierarchyIdHash>;

        static ViewShapeChunk make_chunk(ViewLayerShapes shapes, const char *index_phase, std::optional<ChunkSources> sources = std::nullopt)
        {
            ViewShapeChunk chunk;
            chunk.shapes = std::make_shared<const ViewLayerShapes>(std::move(shapes));
            if (sources)
                chunk.sources = std::make_shared<const ChunkSources>(std::move(*sources));
            const ResolverPhaseTimer timer(index_phase);
            chunk.shapes_index = build_shape_index(*chunk.shapes);
            return chunk;
        }

        // One fixed LayoutChunk, with sources for PORTS (its port shapes are selectable).
        static ViewShapeChunk build_fixed_chunk(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, LayoutChunk chunk)
        {
            ChunkSources sources;
            ViewLayerShapes shapes = collect_layout_chunk(root, view_layers, layout_id, chunk, sources);
            // PORTS and the renderable classes' chunks hold selectable shapes.
            const bool selectable = chunk == LayoutChunk::PORTS || static_cast<std::size_t>(chunk) >= kCoreLayoutChunkCount;
            return make_chunk(std::move(shapes), "layout.shape_index", selectable ? std::optional<ChunkSources>(std::move(sources)) : std::nullopt);
        }

        // One route tile's shapes - its member routes still in `layout_id`.
        // Records a chunk's sources in push order into one flat list, then
        // distributes them into exactly-sized per-layer lists once the
        // chunk is complete - growing thousands of small per-layer lists
        // push by push measured 28% of a cold resolve at aes_scaling_8x8.
        struct SourceRecorder
        {
            std::vector<std::pair<ViewLayerId, ShapeId>> pushes;

            void record(ViewLayerId view_layer, ShapeId shape_id) { pushes.emplace_back(view_layer, shape_id); }

            void finish(ChunkSources &sources, const ViewLayerShapes &shapes_by_layer) const
            {
                for (const auto &[view_layer, shapes] : shapes_by_layer)
                    sources.shapes[view_layer].reserve(shapes.size());
                for (const auto &[view_layer, shape_id] : pushes)
                    sources.shapes[view_layer].push_back(shape_id);
            }
        };

        static ViewLayerShapes collect_route_tile(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, const std::vector<RouteId> &routes,
                                                  ChunkSources &sources)
        {
            SourceRecorder recorder;
            recorder.pushes.reserve(routes.size() * 4);
            const ResolverPhaseTimer timer("layout.routes");
            ViewLayerShapes shapes_by_layer;
            for (const RouteId route_id : routes)
            {
                const RouteData *route = root.get_route(route_id);
                if (!route || route->layout != layout_id)
                    continue;
                for (const ShapeId shape_id : root.get_route_shapes(route_id))
                {
                    const ShapeData *shape = root.get_shape(shape_id);
                    if (!shape)
                        continue;
                    append_via_shapes(root, *shape, ViewLayerPurpose::ROUTE, view_layers, layout_id, shapes_by_layer, [&](ViewLayerId view_layer)
                                      { recorder.record(view_layer, shape_id); });
                    const ViewLayerId view_layer = resolve_view_layer(view_layers, *shape, ViewLayerPurpose::ROUTE);
                    shapes_by_layer[view_layer].push_back(to_render_shape(*shape));
                    recorder.record(view_layer, shape_id);
                }
            }
            recorder.finish(sources, shapes_by_layer);
            return shapes_by_layer;
        }

        // One placement tile: a PLACEMENT rect + name label per placement,
        // batched into a single RenderShape (one-per-placement construction
        // measured ~126 of ~149ms on aes_scaling_3x3), plus - while depth
        // remains - the resolved ViewPlacementData and the child to visit.
        // resolve_design_target runs regardless of depth: the placeholder
        // rect needs the resolved size even at remaining_depth 0.
        static ViewLayerShapes collect_placement_tile(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, int remaining_depth,
                                                      const std::vector<PlacementId> &placements,
                                                      std::vector<ViewPlacementData> &placement_data, std::vector<WorkItem> &children,
                                                      std::vector<PlacementId> &rect_sources)
        {
            const ResolverPhaseTimer timer("layout.placements");
            if (remaining_depth > 0)
                placement_data.reserve(placements.size()); // upper bound - not every placement resolves

            RenderShape placement_shape;
            placement_shape.rects.reserve(placements.size());
            placement_shape.texts.reserve(placements.size());
            rect_sources.reserve(placements.size());

            for (const PlacementId placement_id : placements)
            {
                const PlacementData *placement = root.get_placement(placement_id);
                if (!placement || placement->layout != layout_id || !placement->location || !placement->reference_design.valid())
                    continue;

                const DesignTarget target = resolve_design_target(root, placement->reference_design, remaining_depth);
                HierarchyId child_id;
                Rect child_local_bbox;
                if (target.kind == DesignTarget::Kind::Layout)
                {
                    child_id = target.layout_id;
                    child_local_bbox = layout_declared_bbox(root, target.layout_id);
                }
                else if (target.kind == DesignTarget::Kind::Abstract)
                {
                    child_id = target.abstract_id;
                    child_local_bbox = abstract_declared_bbox(root, target.abstract_id);
                }
                else
                {
                    continue; // unresolved reference_design - nothing to place or draw
                }

                const Orientation orientation = placement->orientation.value_or(Orientation::N);
                const Geometry::InstanceTransform transform = Geometry::instance_transform(orientation, child_local_bbox, *placement->location);
                const Rect bbox = Geometry::transform_bbox(transform, child_local_bbox);

                // Label size is a fraction of the box height (a dbu
                // quantity, Text.size's convention), anchored at its
                // bottom-left; the pixel floor/padding apply at draw time.
                const double height_dbu = static_cast<double>(bbox.ur.y - bbox.ll.y);
                placement_shape.rects.push_back(bbox);
                placement_shape.texts.push_back(Text{.label = placement->name, .location = bbox.ll, .size = height_dbu * kPlacementLabelHeightRatio});
                rect_sources.push_back(placement_id);

                if (remaining_depth <= 0)
                    continue; // depth exhausted - placeholder only

                placement_data.push_back(ViewPlacementData{
                    .id = child_id,
                    .location = *placement->location,
                    .bbox = bbox,
                    .transform = transform,
                    .orientation = orientation,
                });
                children.push_back(WorkItem{child_id, target.kind == DesignTarget::Kind::Layout ? remaining_depth - 1 : 0});
            }

            ViewLayerShapes shapes;
            if (!placement_shape.rects.empty())
                shapes[view_layers.placement_view_layer()].push_back(std::move(placement_shape));
            return shapes;
        }

        // A renderable class's objects in one place - a tile's members, or a
        // whole untiled Layout's - still in `layout_id`: their shapes on the
        // class's row, or a per_layer class's on their layers' columns, each
        // recorded for selection. A labelled class's object gets one label per
        // view layer it draws on, placed on its geometry there and carried by
        // its first shape there, so it hides with that row.
        template <class R, class Objects>
        static void collect_renderable_objects(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, const Objects &objects,
                                               ViewLayerShapes &shapes_by_layer, ChunkSources &sources)
        {
            const ResolverPhaseTimer timer("layout.renderable");
            struct LabelAccumulator
            {
                ViewLayerId view_layer;
                std::size_t first_shape_index = 0;
                RenderShape combined;
            };
            std::vector<LabelAccumulator> labels; // one per view layer; an object rarely spans more than one or two
            for (const auto object : objects)
            {
                const typename R::Id id{object.index, object.generation};
                if (R::layout_of(root, id) != layout_id)
                    continue;
                labels.clear();
                for (const ShapeId shape_id : R::shapes(root, id))
                    if (const ShapeData *shape = root.get_shape(shape_id))
                    {
                        const ViewLayerId view_layer = view_layers.renderable_view_layer<R>(*shape);
                        std::vector<RenderShape> &layer_shapes = shapes_by_layer[view_layer];
                        if constexpr (R::has_label)
                        {
                            auto it = std::ranges::find(labels, view_layer, &LabelAccumulator::view_layer);
                            if (it == labels.end())
                                it = labels.insert(labels.end(), LabelAccumulator{.view_layer = view_layer, .first_shape_index = layer_shapes.size()});
                            it->combined.rects.insert(it->combined.rects.end(), shape->rects.begin(), shape->rects.end());
                            it->combined.polygons.insert(it->combined.polygons.end(), shape->polygons.begin(), shape->polygons.end());
                            it->combined.paths.insert(it->combined.paths.end(), shape->paths.begin(), shape->paths.end());
                        }
                        layer_shapes.push_back(to_render_shape(*shape));
                        sources.shapes[view_layer].push_back(shape_id);
                    }
                if constexpr (R::has_label)
                {
                    const std::string_view label = R::label(root, id);
                    if (label.empty())
                        continue;
                    for (const LabelAccumulator &acc : labels)
                    {
                        if (acc.combined.rects.empty() && acc.combined.polygons.empty() && acc.combined.paths.empty())
                            continue;
                        const Point location = Geometry::get_label_location(acc.combined);
                        shapes_by_layer[acc.view_layer][acc.first_shape_index].texts.push_back(Text{
                            .label = std::string(label),
                            .location = location,
                            .size = Geometry::local_width_at(acc.combined, location),
                        });
                    }
                }
            }
        }

        template <class R>
        static void rebuild_renderable_tile(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, const LayoutTiling &tiling,
                                            std::size_t tile, ViewData &data)
        {
            ChunkSources sources;
            ViewLayerShapes shapes;
            collect_renderable_objects<R>(root, view_layers, layout_id, tiling.renderables[R::index].members[tile], shapes, sources);
            data.chunks[tiling.renderable_chunk(R::index, tile)] = make_chunk(std::move(shapes), "layout.shape_index", std::move(sources));
        }

        static void rebuild_route_tile(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, const LayoutTiling &tiling,
                                       std::size_t tile, ViewData &data)
        {
            ChunkSources sources;
            ViewLayerShapes shapes = collect_route_tile(root, view_layers, layout_id, tiling.routes.members[tile], sources);
            data.chunks[tiling.route_chunk(tile)] = make_chunk(std::move(shapes), "layout.shape_index", std::move(sources));
        }

        static void rebuild_placement_tile(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, const LayoutTiling &tiling,
                                           std::size_t tile, ViewData &data, FreshPlacements &fresh, std::vector<WorkItem> &children)
        {
            std::vector<ViewPlacementData> &placement_data = fresh[HierarchyId{layout_id}][tile];
            placement_data.clear();
            ChunkSources sources;
            ViewLayerShapes shapes = collect_placement_tile(root, view_layers, layout_id, data.remaining_depth, tiling.placements.members[tile], placement_data,
                                                            children, sources.placements);
            data.chunks[tiling.placement_chunk(tile)] = make_chunk(std::move(shapes), "layout.shape_index", std::move(sources));
        }

        // Lays out `layout_id`'s route and placement tiles from scratch.
        static LayoutTiling make_tiling(const Root &root, LayoutId layout_id)
        {
            LayoutTiling tiling;
            const Rect die = layout_declared_bbox(root, layout_id);
            const bool die_valid = die.ur.x > die.ll.x && die.ur.y > die.ll.y;

            auto lay_out = [&](auto &membership, const auto &ids, auto anchor_of, std::size_t per_tile)
            {
                using IdT = typename std::decay_t<decltype(ids)>::value_type;
                std::vector<std::pair<IdT, std::optional<Point>>> anchored;
                anchored.reserve(ids.size());
                std::optional<Rect> bounds;
                if (die_valid)
                    bounds = die;
                for (const IdT id : ids)
                {
                    const std::optional<Point> anchor = anchor_of(id);
                    if (anchor && !die_valid)
                        bounds = bounds ? Rect{.ll = Point{std::min(bounds->ll.x, anchor->x), std::min(bounds->ll.y, anchor->y)},
                                               .ur = Point{std::max(bounds->ur.x, anchor->x), std::max(bounds->ur.y, anchor->y)}}
                                        : Rect{.ll = *anchor, .ur = *anchor};
                    anchored.emplace_back(id, anchor);
                }
                membership.grid = TileGrid::make(bounds.value_or(Rect{}), ids.size(), per_tile);
                membership.members.assign(membership.grid.count(), {});
                for (const auto &[id, anchor] : anchored)
                    membership.assign(id, anchor ? membership.grid.tile_of(*anchor) : 0);
            };
            lay_out(tiling.routes, root.get_layout_routes(layout_id), [&](RouteId id)
                    { return route_anchor(root, id); }, kRoutesPerTile);
            lay_out(tiling.placements, root.get_layout_placements(layout_id), [&](PlacementId id)
                    { return placement_anchor(root, id); }, kPlacementsPerTile);
            renderable::for_each([&]<class R>(R) {
                if constexpr (R::tiled)
                {
                    std::vector<RenderableObjectId> ids;
                    for (const typename R::Id id : R::in_layout(root, layout_id))
                        ids.push_back(RenderableObjectId{id.index, id.generation});
                    lay_out(tiling.renderables[R::index], ids, [&](RenderableObjectId id)
                            { return renderable_anchor<R>(root, id); }, kRenderablesPerTile);
                }
                else
                    tiling.renderables[R::index].grid.n = 0; // no tiles - its fixed chunk holds it
            });
            return tiling;
        }

        // Builds every chunk and tile of a new Layout node into `data`.
        void build_layout_node(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, ViewData &data, FreshPlacements &fresh,
                               std::vector<WorkItem> &children)
        {
            LayoutTiling &tiling = tilings_[layout_id] = make_tiling(root, layout_id);
            data.chunks.assign(tiling.chunk_count(), ViewShapeChunk{});
            data.placement_chunk_offset = tiling.placement_chunk(0);
            for (std::size_t c = 0; c < kFixedLayoutChunkCount; ++c)
                data.chunks[c] = build_fixed_chunk(root, view_layers, layout_id, static_cast<LayoutChunk>(c));
            for (std::size_t t = 0; t < tiling.routes.grid.count(); ++t)
                rebuild_route_tile(root, view_layers, layout_id, tiling, t, data);
            data.placement_tiles.assign(tiling.placements.grid.count(), empty_placement_tile());
            for (std::size_t t = 0; t < tiling.placements.grid.count(); ++t)
                rebuild_placement_tile(root, view_layers, layout_id, tiling, t, data, fresh, children);
            renderable::for_each([&]<class R>(R) {
                for (std::size_t t = 0; t < tiling.renderables[R::index].grid.count(); ++t)
                    rebuild_renderable_tile<R>(root, view_layers, layout_id, tiling, t, data);
            });
        }

        static const ViewPlacements &empty_placement_tile()
        {
            static const ViewPlacements empty = std::make_shared<const ViewPlacementTile>();
            return empty;
        }

        ViewData build_node(const Root &root, const ViewLayerSet &view_layers, const WorkItem &item, FreshPlacements &fresh, std::vector<WorkItem> &children)
        {
            ViewData data;
            data.remaining_depth = item.remaining_depth;
            if (const LayoutId *layout_id = std::get_if<LayoutId>(&item.id))
            {
                build_layout_node(root, view_layers, *layout_id, data, fresh, children);
            }
            else
            {
                const ResolverPhaseTimer timer("abstracts");
                data.chunks.push_back(make_chunk(collect_abstract_content(root, view_layers, std::get<AbstractId>(item.id)), "abstracts"));
            }
            return data;
        }

        // Breadth-first from `worklist`, adding every node not already in
        // `output` (shallowest depth first - see the class comment).
        void resolve_new_nodes(const Root &root, const ViewLayerSet &view_layers, std::deque<WorkItem> worklist, HierarchyResolverOutput &output,
                               FreshPlacements &fresh)
        {
            std::vector<WorkItem> children;
            while (!worklist.empty())
            {
                const WorkItem item = worklist.front();
                worklist.pop_front();
                if (output.view_data.contains(item.id))
                    continue;
                children.clear();
                output.view_data.emplace(item.id, build_node(root, view_layers, item, fresh, children));
                worklist.insert(worklist.end(), children.begin(), children.end());
            }
        }

        HierarchyResolverOutput resolve_everything(const Root &root, const ViewLayerSetHandle &view_layers_handle, const ViewLayerSet &view_layers,
                                                   const ViewRenderOptions &options)
        {
            tilings_.clear();
            HierarchyResolverOutput output{.view_layers = view_layers_handle};
            FreshPlacements fresh;
            resolve_new_nodes(root, view_layers, std::deque<WorkItem>{WorkItem{options.top_level, options.hierarchy_depth}}, output, fresh);
            const ResolverPhaseTimer timer("assign_extents");
            assign_extents(root, output, fresh);
            return output;
        }

        // Two ViewLayerSets that map every ViewLayerId to the same (layer,
        // purpose) - a rebuilt-but-unchanged set, or one differing only in
        // colors - leave every chunk's ViewLayerId keys valid.
        static bool same_view_layer_ids(const ViewLayerSet &a, const ViewLayerSet &b)
        {
            const std::vector<ViewLayerId> ids = a.all();
            if (ids != b.all())
                return false;
            for (const ViewLayerId id : ids)
            {
                const ViewLayerData *x = a.get(id);
                const ViewLayerData *y = b.get(id);
                if (!x || !y || x->layer != y->layer || x->purpose != y->purpose)
                    return false;
            }
            return true;
        }

        bool can_update_incrementally(const Root &root, const ViewLayerSet &view_layers, const ViewRenderOptions &options) const
        {
            const OutputHandle &previous = previous_result();
            return state_ && previous && previous->view_layers && state_->root == &root && state_->top_level == options.top_level &&
                   state_->hierarchy_depth == options.hierarchy_depth && root.change_log().covers(state_->log_end) &&
                   same_view_layer_ids(*previous->view_layers, view_layers);
        }

        // What the change log says an edit touched, per Layout.
        struct LayoutDirty
        {
            std::array<bool, kFixedLayoutChunkCount> fixed{};
            std::unordered_set<RouteId> routes;
            std::unordered_set<PlacementId> placements;
            // A tiled renderable class's touched objects, by R::index.
            std::array<std::unordered_set<RenderableObjectId, RenderableObjectIdHash>, renderable::kCount> renderables;
            bool all_routes = false;
            bool all_placements = false;
        };
        struct Dirty
        {
            std::unordered_map<LayoutId, LayoutDirty> layouts;
            std::unordered_set<AbstractId> abstracts;
            bool declared_bbox_changed = false; // an Abstract's or Layout's boundary - placement rects everywhere
            bool everything = false;
        };

        struct ChangeKey
        {
            ChangeKlass klass;
            std::uint32_t index;
            std::uint32_t generation;
            bool operator==(const ChangeKey &) const = default;
        };
        struct ChangeKeyHash
        {
            std::size_t operator()(const ChangeKey &k) const noexcept
            {
                return (static_cast<std::size_t>(k.klass) << 56) ^ (static_cast<std::size_t>(k.index) << 20) ^ k.generation;
            }
        };

        // Maps each change-log entry since the last compute() to what it
        // touched. Anything it can't place precisely marks everything -
        // the full resolve is always a correct fallback.
        static Dirty collect_dirty(const Root &root, std::uint64_t since)
        {
            Dirty dirty;
            // Owners of objects deleted in this batch - a live object's
            // owner is read from the Root, a deleted one's from here.
            std::unordered_map<ChangeKey, ChangeParent, ChangeKeyHash> deleted_parents;
            root.change_log().for_each_since(since, [&](const ChangeLogEntry &entry)
                                             {
                if (entry.op == ChangeOp::DELETE)
                    deleted_parents[ChangeKey{entry.klass, entry.index, entry.generation}] = entry.parent; });

            // Climbs from `parent` to the nearest ancestor of class `target`
            // (`parent` itself if it is one).
            auto ancestor = [&](ChangeParent parent, ChangeKlass target) -> std::optional<ChangeParent>
            {
                for (int hops = 0; hops < 8 && parent.klass != ChangeKlass::None; ++hops)
                {
                    if (parent.klass == target)
                        return parent;
                    ChangeParent next = root.change_parent_of(parent.klass, parent.index, parent.generation);
                    if (next.klass == ChangeKlass::None)
                        if (const auto it = deleted_parents.find(ChangeKey{parent.klass, parent.index, parent.generation}); it != deleted_parents.end())
                            next = it->second;
                    parent = next;
                }
                return std::nullopt;
            };
            auto self = [](const ChangeLogEntry &entry)
            { return ChangeParent{.klass = entry.klass, .index = entry.index, .generation = entry.generation}; };
            auto layout_dirty = [&](std::optional<ChangeParent> layout) -> LayoutDirty *
            {
                return layout ? &dirty.layouts[LayoutId{layout->index, layout->generation}] : nullptr;
            };
            auto mark_fixed = [&](std::optional<ChangeParent> layout, std::initializer_list<LayoutChunk> chunks)
            {
                if (LayoutDirty *d = layout_dirty(layout))
                    for (const LayoutChunk chunk : chunks)
                        d->fixed[static_cast<std::size_t>(chunk)] = true;
            };
            auto mark_route = [&](std::optional<ChangeParent> route, std::optional<ChangeParent> layout)
            {
                if (LayoutDirty *d = layout_dirty(layout); d && route)
                    d->routes.insert(RouteId{route->index, route->generation});
            };
            auto mark_abstract = [&](std::optional<ChangeParent> abstract)
            {
                if (abstract)
                    dirty.abstracts.insert(AbstractId{abstract->index, abstract->generation});
            };

            root.change_log().for_each_since(since, [&](const ChangeLogEntry &entry)
                                             {
                if (dirty.everything)
                    return;
                switch (entry.klass)
                {
                case ChangeKlass::Shape:
                {
                    if (entry.parent.klass == ChangeKlass::None)
                        return; // an orphan shape draws nowhere
                    const std::string_view field = Root::change_parent_field_name(ChangeKlass::Shape, entry.parent.slot);
                    const std::optional<ChangeParent> layout = ancestor(entry.parent, ChangeKlass::Layout);
                    if (field == "route")
                        mark_route(entry.parent, layout);
                    else if (field == "blockage")
                        mark_fixed(layout, {LayoutChunk::DIEAREA_BLOCKAGES});
                    else if (field == "physical_port_segment")
                        mark_fixed(layout, {LayoutChunk::PORTS});
                    else if (field == "in_layout")
                        mark_fixed(layout, {LayoutChunk::FREE_SHAPES});
                    else if (field == "layout") // the diearea: port markers face its sides
                    {
                        mark_fixed(layout, {LayoutChunk::DIEAREA_BLOCKAGES, LayoutChunk::PORTS});
                        dirty.declared_bbox_changed = true;
                    }
                    else if (field == "terminal_port" || field == "obstruction" || field == "in_abstract")
                        mark_abstract(ancestor(entry.parent, ChangeKlass::Abstract));
                    else if (field == "abstract") // the boundary
                    {
                        mark_abstract(ancestor(entry.parent, ChangeKlass::Abstract));
                        dirty.declared_bbox_changed = true;
                    }
                    else
                    {
                        bool renderable_owner = false;
                        renderable::for_each([&]<class R>(R) {
                            if (field != R::owner_option)
                                return;
                            renderable_owner = true;
                            if (!R::tiled)
                                mark_fixed(layout, {renderable_chunk<R>()});
                            else if (LayoutDirty *d = layout_dirty(layout))
                                d->renderables[R::index].insert(RenderableObjectId{entry.parent.index, entry.parent.generation});
                        });
                        if (!renderable_owner)
                            dirty.everything = true;
                    }
                    return;
                }
                case ChangeKlass::Route:
                    mark_route(self(entry), ancestor(entry.parent, ChangeKlass::Layout));
                    return;
                case ChangeKlass::Placement:
                    if (LayoutDirty *d = layout_dirty(ancestor(entry.parent, ChangeKlass::Layout)))
                        d->placements.insert(PlacementId{entry.index, entry.generation});
                    return;
                case ChangeKlass::Blockage:
                    mark_fixed(ancestor(entry.parent, ChangeKlass::Layout), {LayoutChunk::DIEAREA_BLOCKAGES});
                    return;
                case ChangeKlass::PhysicalPort:
                case ChangeKlass::PhysicalPortSegment:
                    mark_fixed(ancestor(entry.parent, ChangeKlass::Layout), {LayoutChunk::PORTS});
                    return;
                case ChangeKlass::Row:
                case ChangeKlass::Track:
                case ChangeKlass::GCellGrid:
                case ChangeKlass::Region:
                    mark_fixed(ancestor(entry.parent, ChangeKlass::Layout), {LayoutChunk::ROWS_TRACKS_GCELLS_REGIONS});
                    return;
                case ChangeKlass::Terminal:
                case ChangeKlass::TerminalPort:
                case ChangeKlass::Obstruction:
                    mark_abstract(ancestor(entry.parent, ChangeKlass::Abstract));
                    return;
                case ChangeKlass::Abstract:
                case ChangeKlass::Layout:
                    // Created or deleted, a design's view (what its
                    // placements resolve to) changes - resolve everything.
                    if (entry.op == ChangeOp::CREATE || entry.op == ChangeOp::DELETE)
                    {
                        dirty.everything = true;
                        return;
                    }
                    if (entry.klass == ChangeKlass::Abstract)
                        mark_abstract(self(entry));
                    else if (LayoutDirty *d = layout_dirty(self(entry)))
                    {
                        d->fixed.fill(true);
                        d->all_routes = true;
                        d->all_placements = true;
                    }
                    dirty.declared_bbox_changed = true; // Abstract.size, say
                    return;
                // Logical connectivity and property metadata: nothing drawn.
                case ChangeKlass::Net:
                case ChangeKlass::NetBus:
                case ChangeKlass::Instance:
                case ChangeKlass::Pin:
                case ChangeKlass::Port:
                case ChangeKlass::PortBus:
                case ChangeKlass::Schematic:
                case ChangeKlass::PropertyDefinition:
                    return;
                default: // technology, libraries, designs, vias, ...
                {
                    // A renderable class's object: its Layout's chunk for it.
                    bool renderable_object = false;
                    renderable::for_each([&]<class R>(R) {
                        if (entry.klass != R::klass)
                            return;
                        renderable_object = true;
                        const std::optional<ChangeParent> layout = ancestor(entry.parent, ChangeKlass::Layout);
                        if (!R::tiled)
                            mark_fixed(layout, {renderable_chunk<R>()});
                        else if (LayoutDirty *d = layout_dirty(layout))
                            d->renderables[R::index].insert(RenderableObjectId{entry.index, entry.generation});
                    });
                    if (!renderable_object)
                        dirty.everything = true;
                    return;
                }
                } });
            return dirty;
        }

        // Moves each of `changed` from its old tile to the one its anchor is
        // in now (none if it's gone or left the Layout), adding both tiles
        // to `tiles`.
        template <typename IdT, typename Changed, typename AnchorFn>
        static void retile(TileMembership<IdT> &membership, const Changed &changed, AnchorFn &&anchor_in_layout,
                           std::set<std::size_t> &tiles)
        {
            for (const IdT id : changed)
            {
                if (const std::optional<std::size_t> old_tile = membership.remove(id))
                    tiles.insert(*old_tile);
                if (const std::optional<std::optional<Point>> anchor = anchor_in_layout(id))
                {
                    const std::size_t tile = *anchor ? membership.grid.tile_of(**anchor) : 0;
                    membership.assign(id, tile);
                    tiles.insert(tile);
                }
            }
        }

        std::optional<HierarchyResolverOutput> update_incrementally(const Root &root, const ViewLayerSetHandle &view_layers_handle,
                                                                    const ViewLayerSet &view_layers, const ViewRenderOptions &options)
        {
            const ResolverPhaseTimer timer("incremental");
            Dirty dirty = collect_dirty(root, state_->log_end);
            if (dirty.everything)
                return std::nullopt;

            HierarchyResolverOutput output = *previous_result(); // chunks/tiles shared, not copied
            output.view_layers = view_layers_handle;

            // A boundary change moves placement rects wherever the cell is
            // placed - rebuild every Layout's placements.
            if (dirty.declared_bbox_changed)
                for (const auto &[id, data] : output.view_data)
                    if (const LayoutId *layout_id = std::get_if<LayoutId>(&id))
                        dirty.layouts[*layout_id].all_placements = true;

            FreshPlacements fresh;
            std::deque<WorkItem> new_children;
            bool placements_rebuilt = false;
            for (const auto &[layout_id, layout_dirty] : dirty.layouts)
            {
                const auto it = output.view_data.find(HierarchyId{layout_id});
                if (it == output.view_data.end())
                    continue; // not visible from top_level
                const auto tiling_it = tilings_.find(layout_id);
                if (tiling_it == tilings_.end())
                    return std::nullopt; // out of step - resolve everything
                LayoutTiling &tiling = tiling_it->second;
                ViewData &data = it->second;

                for (std::size_t c = 0; c < kFixedLayoutChunkCount; ++c)
                    if (layout_dirty.fixed[c])
                        data.chunks[c] = build_fixed_chunk(root, view_layers, layout_id, static_cast<LayoutChunk>(c));

                std::set<std::size_t> route_tiles;
                if (layout_dirty.all_routes)
                {
                    // Membership may have changed arbitrarily - lay routes
                    // out again on the same grid (chunk indices stay put).
                    TileMembership<RouteId> fresh_routes{.grid = tiling.routes.grid};
                    fresh_routes.members.assign(tiling.routes.grid.count(), {});
                    for (const RouteId route : root.get_layout_routes(layout_id))
                    {
                        const std::optional<Point> anchor = route_anchor(root, route);
                        fresh_routes.assign(route, anchor ? fresh_routes.grid.tile_of(*anchor) : 0);
                    }
                    tiling.routes = std::move(fresh_routes);
                    for (std::size_t t = 0; t < tiling.routes.grid.count(); ++t)
                        route_tiles.insert(t);
                }
                else
                    retile(tiling.routes, layout_dirty.routes, [&](RouteId id) -> std::optional<std::optional<Point>>
                           {
                        const RouteData *route = root.get_route(id);
                        if (!route || route->layout != layout_id)
                            return std::nullopt;
                        return route_anchor(root, id); }, route_tiles);
                for (const std::size_t t : route_tiles)
                    rebuild_route_tile(root, view_layers, layout_id, tiling, t, data);

                std::set<std::size_t> placement_tiles;
                if (layout_dirty.all_placements)
                {
                    TileMembership<PlacementId> fresh_placements{.grid = tiling.placements.grid};
                    fresh_placements.members.assign(tiling.placements.grid.count(), {});
                    for (const PlacementId placement : root.get_layout_placements(layout_id))
                    {
                        const std::optional<Point> anchor = placement_anchor(root, placement);
                        fresh_placements.assign(placement, anchor ? fresh_placements.grid.tile_of(*anchor) : 0);
                    }
                    tiling.placements = std::move(fresh_placements);
                    for (std::size_t t = 0; t < tiling.placements.grid.count(); ++t)
                        placement_tiles.insert(t);
                }
                else
                    retile(tiling.placements, layout_dirty.placements, [&](PlacementId id) -> std::optional<std::optional<Point>>
                           {
                        const PlacementData *placement = root.get_placement(id);
                        if (!placement || placement->layout != layout_id)
                            return std::nullopt;
                        return placement->location; }, placement_tiles);
                std::vector<WorkItem> children;
                for (const std::size_t t : placement_tiles)
                    rebuild_placement_tile(root, view_layers, layout_id, tiling, t, data, fresh, children);
                placements_rebuilt = placements_rebuilt || !placement_tiles.empty();

                renderable::for_each([&]<class R>(R) {
                    if constexpr (R::tiled)
                    {
                        std::set<std::size_t> tiles;
                        retile(tiling.renderables[R::index], layout_dirty.renderables[R::index], [&](RenderableObjectId id) -> std::optional<std::optional<Point>>
                               {
                            if (R::layout_of(root, typename R::Id{id.index, id.generation}) != layout_id)
                                return std::nullopt;
                            return renderable_anchor<R>(root, id); }, tiles);
                        for (const std::size_t t : tiles)
                            rebuild_renderable_tile<R>(root, view_layers, layout_id, tiling, t, data);
                    }
                });
                new_children.insert(new_children.end(), children.begin(), children.end());
            }
            for (const AbstractId abstract_id : dirty.abstracts)
            {
                const auto it = output.view_data.find(HierarchyId{abstract_id});
                if (it == output.view_data.end())
                    continue;
                const ResolverPhaseTimer abstract_timer("abstracts");
                it->second.chunks.assign(1, make_chunk(collect_abstract_content(root, view_layers, abstract_id), "abstracts"));
            }

            if (placements_rebuilt)
            {
                // Resolve any newly placed designs, then drop nodes no
                // placement reaches any more.
                resolve_new_nodes(root, view_layers, std::move(new_children), output, fresh);
                prune_unreachable(output, fresh, options.top_level);
            }

            const ResolverPhaseTimer extents_timer("assign_extents");
            assign_extents(root, output, fresh);
            return output;
        }

        void prune_unreachable(HierarchyResolverOutput &output, const FreshPlacements &fresh, const HierarchyId &top_level)
        {
            std::unordered_set<HierarchyId, HierarchyIdHash> reachable{top_level};
            std::deque<HierarchyId> queue{top_level};
            auto reach = [&](const HierarchyId &child)
            {
                if (reachable.insert(child).second)
                    queue.push_back(child);
            };
            while (!queue.empty())
            {
                const HierarchyId id = queue.front();
                queue.pop_front();
                const auto it = output.view_data.find(id);
                if (it == output.view_data.end())
                    continue;
                const auto fresh_it = fresh.find(id);
                for (std::size_t t = 0; t < it->second.placement_tiles.size(); ++t)
                {
                    if (fresh_it != fresh.end())
                        if (const auto tile_it = fresh_it->second.find(t); tile_it != fresh_it->second.end())
                        {
                            for (const ViewPlacementData &placement : tile_it->second)
                                reach(placement.id);
                            continue;
                        }
                    for (const HierarchyId &child : it->second.placement_tiles[t]->children)
                        reach(child);
                }
            }
            std::erase_if(output.view_data, [&](const auto &entry)
                          { return !reachable.contains(entry.first); });
            std::erase_if(tilings_, [&](const auto &entry)
                          { return !reachable.contains(HierarchyId{entry.first}); });
        }

        /// @brief Fills every node's `ViewData::extent` and every
        /// placement's `ViewPlacementData::extent`, children first (a
        /// placement's extent needs its placed node's). A node's own shapes
        /// contribute via their per-layer rtrees' cached bounds, not a walk
        /// over every shape. Placement tiles built this compute() (in
        /// `fresh`) are filled in and published; a published one is
        /// replaced only if one of the nodes it places changed extent.
        static void assign_extents(const Root &root, HierarchyResolverOutput &output, FreshPlacements &fresh)
        {
            auto grow = [](std::optional<Rect> &extent, const Rect &r)
            {
                if (!extent)
                {
                    extent = r;
                    return;
                }
                extent->ll.x = std::min(extent->ll.x, r.ll.x);
                extent->ll.y = std::min(extent->ll.y, r.ll.y);
                extent->ur.x = std::max(extent->ur.x, r.ur.x);
                extent->ur.y = std::max(extent->ur.y, r.ur.y);
            };
            auto same = [](const Rect &a, const Rect &b)
            { return a.ll.x == b.ll.x && a.ll.y == b.ll.y && a.ur.x == b.ur.x && a.ur.y == b.ur.y; };

            std::unordered_set<HierarchyId, HierarchyIdHash> done;
            std::unordered_set<HierarchyId, HierarchyIdHash> in_progress;
            std::unordered_set<HierarchyId, HierarchyIdHash> changed; // nodes whose extent differs from the previous output's
            auto visit = [&](auto &self, const HierarchyId &id) -> std::optional<Rect>
            {
                const auto it = output.view_data.find(id);
                if (it == output.view_data.end())
                    return std::nullopt;
                ViewData &data = it->second;
                if (done.contains(id))
                    return data.extent;
                if (!in_progress.insert(id).second)
                    return std::nullopt; // a placement cycle - leave the rest to the declared bboxes

                std::optional<Rect> extent;
                const Rect declared = std::holds_alternative<LayoutId>(id) ? layout_declared_bbox(root, std::get<LayoutId>(id))
                                                                           : abstract_declared_bbox(root, std::get<AbstractId>(id));
                if (declared.ur.x > declared.ll.x || declared.ur.y > declared.ll.y)
                    grow(extent, declared);
                for (const ViewShapeChunk &chunk : data.chunks)
                    if (chunk.shapes_index)
                        for (const auto &[view_layer, index] : *chunk.shapes_index)
                            if (!index.empty())
                            {
                                const auto bounds = index.bounds();
                                grow(extent, Rect{.ll = Point{bg::get<bg::min_corner, 0>(bounds), bg::get<bg::min_corner, 1>(bounds)},
                                                  .ur = Point{bg::get<bg::max_corner, 0>(bounds), bg::get<bg::max_corner, 1>(bounds)}});
                            }

                auto placement_extent = [&](const ViewPlacementData &placement)
                {
                    std::optional<Rect> placement_extent = placement.bbox;
                    if (const std::optional<Rect> child = self(self, placement.id))
                        grow(placement_extent, Geometry::transform_bbox(placement.transform, *child));
                    return *placement_extent;
                };
                // Fills `tile`'s placement extents, its extent and children.
                auto finish_tile = [&](ViewPlacementTile &tile)
                {
                    std::optional<Rect> tile_extent;
                    std::unordered_set<HierarchyId, HierarchyIdHash> children;
                    for (ViewPlacementData &placement : tile.placements)
                    {
                        placement.extent = placement_extent(placement);
                        grow(tile_extent, placement.extent);
                        children.insert(placement.id);
                    }
                    tile.extent = tile_extent.value_or(Rect{});
                    tile.children.assign(children.begin(), children.end());
                };

                const auto fresh_it = fresh.find(id);
                for (std::size_t t = 0; t < data.placement_tiles.size(); ++t)
                {
                    if (fresh_it != fresh.end())
                        if (const auto tile_it = fresh_it->second.find(t); tile_it != fresh_it->second.end())
                        {
                            ViewPlacementTile tile{.placements = std::move(tile_it->second)};
                            finish_tile(tile);
                            data.placement_tiles[t] = tile.placements.empty() ? empty_placement_tile() : std::make_shared<const ViewPlacementTile>(std::move(tile));
                            if (!data.placement_tiles[t]->placements.empty())
                                grow(extent, data.placement_tiles[t]->extent);
                            continue;
                        }
                    const ViewPlacements &tile = data.placement_tiles[t];
                    bool stale = false;
                    for (const HierarchyId &child : tile->children)
                    {
                        self(self, child);
                        stale = stale || changed.contains(child);
                    }
                    if (stale)
                    {
                        ViewPlacementTile updated = *tile;
                        finish_tile(updated);
                        data.placement_tiles[t] = std::make_shared<const ViewPlacementTile>(std::move(updated));
                    }
                    if (!data.placement_tiles[t]->placements.empty())
                        grow(extent, data.placement_tiles[t]->extent);
                }

                const Rect new_extent = extent.value_or(Rect{});
                if (!same(new_extent, data.extent))
                    changed.insert(id);
                data.extent = new_extent;
                in_progress.erase(id);
                done.insert(id);
                return data.extent;
            };
            for (const auto &[id, data] : output.view_data)
                visit(visit, id);
            fresh.clear();
        }

        static ViewShapesIndexHandle build_shape_index(const ViewLayerShapes &shapes_by_layer)
        {
            ViewLayerShapeIndex index_by_layer;
            for (const auto &[view_layer_id, shapes] : shapes_by_layer)
            {
                std::vector<ShapeIndexEntry> entries;
                entries.reserve(shapes.size());
                for (std::size_t i = 0; i < shapes.size(); ++i)
                    if (const std::optional<Rect> bbox = Geometry::bbox(shapes[i]))
                        entries.emplace_back(*bbox, i);
                index_by_layer.emplace(view_layer_id, ShapeSpatialIndex(entries));
            }
            return std::make_shared<const ViewLayerShapeIndex>(std::move(index_by_layer));
        }

        // Expands RECT/PATH/POLYGON ITERATE (stored raw by LEFReader) into
        // concrete rects/paths/polygons on a copy of `shape`. LEF-only in
        // practice (DEF content never populates these fields), so
        // collect_layout_content doesn't call this.
        static ShapeData expand_iterates(ShapeData shape)
        {
            return Geometry::expand_iterates(std::move(shape));
        }

        static ViewLayerPurpose to_view_layer_purpose(ShapePurpose purpose)
        {
            switch (purpose)
            {
            case ShapePurpose::PLACEMENT_BLOCKAGE:
                return ViewLayerPurpose::PLACEMENT_BLOCKAGE;
            case ShapePurpose::DEBUG:
                return ViewLayerPurpose::DEBUG;
            case ShapePurpose::BOUNDARY:
            default:
                return ViewLayerPurpose::BOUNDARY;
            }
        }

        // Free-standing shapes (Abstract/Layout.free_shapes): one on a real
        // Layer draws on that Layer's own CUSTOM_SHAPE column, a DEBUG one
        // on the DEBUG pseudo-row, and any other layer-less one isn't drawn
        // at all - resolve_view_layer alone would put a BOUNDARY/
        // PLACEMENT_BLOCKAGE-purpose free shape on those pseudo-rows.
        static void append_free_shapes(const Root &root, const ViewLayerSet &view_layers, const std::vector<ShapeId> &shape_ids,
                                       LayoutId layout_id, ViewLayerShapes &shapes_by_layer)
        {
            for (ShapeId shape_id : shape_ids)
            {
                const ShapeData *raw_shape = root.get_shape(shape_id);
                if (!raw_shape)
                    continue;
                ViewLayerId view_layer;
                if (raw_shape->layer.valid())
                    view_layer = view_layers.find(raw_shape->layer, ViewLayerPurpose::CUSTOM_SHAPE);
                else if (raw_shape->purpose == ShapePurpose::DEBUG)
                    view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::DEBUG);
                else
                    continue;
                if (!view_layer.valid())
                    continue;
                ShapeData shape = expand_iterates(*raw_shape);
                append_via_shapes(root, shape, ViewLayerPurpose::CUSTOM_SHAPE, view_layers, layout_id, shapes_by_layer);
                shapes_by_layer[view_layer].push_back(to_render_shape(std::move(shape)));
            }
        }

        // A real physical Layer (Shape.layer valid) resolves via the given
        // purpose against that Layer's own row (e.g. a ROUTING Blockage's
        // Shape); one with no Layer instead carries its own Shape.purpose
        // (BOUNDARY/PLACEMENT_BLOCKAGE), resolved directly against that
        // purpose's own pseudo-row.
        static ViewLayerId resolve_view_layer(const ViewLayerSet &view_layers, const ShapeData &shape, ViewLayerPurpose fallback_purpose)
        {
            if (shape.layer.valid())
                return view_layers.find(shape.layer, fallback_purpose);
            if (shape.purpose)
                return view_layers.find(LayerId{}, to_view_layer_purpose(*shape.purpose));
            return ViewLayerId{};
        }

        // Returns just the shapes, grouped by ViewLayer (an Abstract has
        // no placement_data of its own - LEF macros are leaves) - the
        // caller wraps this into ViewData::shapes' own ViewShapesHandle
        // once, after this function is done appending to it (see
        // ViewShapesHandle's own comment for why the wrap happens
        // exactly once, at the end, rather than as this function's own
        // return type).
        static ViewLayerShapes collect_abstract_content(const Root &root, const ViewLayerSet &view_layers, AbstractId abstract_id)
        {
            ViewLayerShapes shapes_by_layer;
            const auto &terminals = root.get_abstract_terminals(abstract_id);
            const auto &obstructions = root.get_abstract_obstructions(abstract_id);

            for (TerminalId terminal_id : terminals)
            {
                // Accumulates just the geometry primitives (not whole
                // Shapes) per Layer, purely to place that Layer's own name
                // label once its combined bbox is known. Tracks
                // its own resolved view_layer too (not just first_shape_index)
                // since the label-attach loop below needs it to reach
                // back into shapes_by_layer, and it's a pure function of
                // shape.layer (this LabelAccumulator's own map key) plus
                // the fixed TERMINAL purpose - cheaper to remember than
                // to call resolve_view_layer a second time.
                struct LabelAccumulator
                {
                    RenderShape combined;
                    ViewLayerId view_layer;
                    std::size_t first_shape_index = 0;
                };
                std::unordered_map<LayerId, LabelAccumulator> by_layer;

                for (TerminalPortId port_id : root.get_terminal_ports(terminal_id))
                {
                    for (ShapeId shape_id : root.get_terminal_port_shapes(port_id))
                    {
                        const ShapeData *raw_shape = root.get_shape(shape_id);
                        if (!raw_shape)
                            continue;
                        ShapeData shape = expand_iterates(*raw_shape);
                        const ViewLayerId view_layer = resolve_view_layer(view_layers, shape, ViewLayerPurpose::TERMINAL);
                        std::vector<RenderShape> &layer_shapes = shapes_by_layer[view_layer];

                        auto [it, inserted] = by_layer.try_emplace(shape.layer);
                        if (inserted)
                        {
                            it->second.view_layer = view_layer;
                            it->second.first_shape_index = layer_shapes.size();
                        }
                        RenderShape &combined = it->second.combined;
                        combined.rects.insert(combined.rects.end(), shape.rects.begin(), shape.rects.end());
                        combined.polygons.insert(combined.polygons.end(), shape.polygons.begin(), shape.polygons.end());
                        combined.paths.insert(combined.paths.end(), shape.paths.begin(), shape.paths.end());

                        // append_via_shapes reads shape.vias/.via_iterates -
                        // must run on the full Shape, before the shrink to
                        // RenderShape below (see render_shape.hpp's own
                        // to_render_shape comment on why the conversion has
                        // to be the last step).
                        append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes_by_layer);
                        layer_shapes.push_back(to_render_shape(std::move(shape)));
                    }
                }

                if (by_layer.empty())
                    continue;

                if (const TerminalData *terminal = root.get_terminal(terminal_id))
                {
                    for (const auto &[layer_id, acc] : by_layer)
                    {
                        const Point location = Geometry::get_label_location(acc.combined);
                        shapes_by_layer[acc.view_layer][acc.first_shape_index].texts.push_back(Text{
                            .label = terminal->name,
                            .location = location,
                            .size = Geometry::local_width_at(acc.combined, location),
                        });
                    }
                }
            }

            for (ObstructionId obstruction_id : obstructions)
            {
                for (ShapeId shape_id : root.get_obstruction_shapes(obstruction_id))
                {
                    const ShapeData *raw_shape = root.get_shape(shape_id);
                    if (!raw_shape)
                        continue;
                    ShapeData shape = expand_iterates(*raw_shape);
                    const ViewLayerId view_layer = resolve_view_layer(view_layers, shape, ViewLayerPurpose::OBSTRUCTION);
                    append_via_shapes(root, shape, ViewLayerPurpose::OBSTRUCTION, view_layers, LayoutId{}, shapes_by_layer);
                    shapes_by_layer[view_layer].push_back(to_render_shape(std::move(shape)));
                }
            }

            append_free_shapes(root, view_layers, root.get_abstract_free_shapes(abstract_id), LayoutId{}, shapes_by_layer);

            if (const ShapeData *boundary_shape = root.get_shape(root.get_abstract_boundary(abstract_id)))
                shapes_by_layer[view_layers.boundary_view_layer()].push_back(to_render_shape(*boundary_shape));

            return shapes_by_layer;
        }

        static std::optional<Rect> layout_die_area_bbox(const Root &root, LayoutId layout_id)
        {
            const ShapeData *diearea = root.get_shape(root.get_layout_diearea(layout_id));
            if (!diearea)
                return std::nullopt;
            return Geometry::bbox(*diearea);
        }

        // Row/Track/GCellGrid have no stored Shape of their own (purely
        // parametric geometry) - synthesized here.
        //
        // Batched into one shared Shape rather than one per Row (same
        // reasoning as the main compute() loop's own placement-boundary
        // batching: a plain RenderShape has no per-Row identity to
        // preserve).
        static void append_row_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, ViewLayerShapes &shapes_by_layer)
        {
            const auto &rows = root.get_layout_rows(layout_id);
            if (rows.empty())
                return;

            RenderShape shape;
            shape.rects.reserve(rows.size());
            for (RowId row_id : rows)
                if (const std::optional<Rect> bbox = row_footprint_bbox(root, row_id))
                    shape.rects.push_back(*bbox);

            if (shape.rects.empty())
                return;

            shapes_by_layer[view_layers.find(LayerId{}, ViewLayerPurpose::ROW)].push_back(std::move(shape));
        }

        static void append_track_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, ViewLayerShapes &shapes_by_layer)
        {
            const std::optional<Rect> die_bbox = layout_die_area_bbox(root, layout_id);
            if (!die_bbox)
                return;

            for (TrackId track_id : root.get_layout_tracks(layout_id))
            {
                const TrackData *track = root.get_track(track_id);
                if (!track || track->count <= 0)
                    continue;

                RenderShape lines;
                lines.paths.reserve(static_cast<std::size_t>(track->count)); // exact - every iteration below pushes exactly one
                for (int i = 0; i < track->count; i++)
                {
                    const int64_t coord = track->start + static_cast<int64_t>(i) * track->step;
                    const Point p1 = track->is_x ? Point{.x = coord, .y = die_bbox->ll.y} : Point{.x = die_bbox->ll.x, .y = coord};
                    const Point p2 = track->is_x ? Point{.x = coord, .y = die_bbox->ur.y} : Point{.x = die_bbox->ur.x, .y = coord};
                    lines.paths.push_back(Path{.width = 0, .polygon = Polygon{.points = {p1, p2}}});
                }

                for (const std::string &layer_name : track->layer_names)
                {
                    const LayerId layer_id = root.get_layer_by_name(layer_name);
                    if (!layer_id.valid())
                        continue;

                    // A track resolves to
                    // TRACK_PREFERRED if its own line direction matches
                    // this Layer's own declared preferred routing
                    // direction, else TRACK_NON_PREFERRED.
                    const LayerData *layer = root.get_layer(layer_id);
                    const bool is_preferred = layer && ((track->is_x && layer->direction == RoutingDirection::V) ||
                                                         (!track->is_x && layer->direction == RoutingDirection::H));
                    const ViewLayerPurpose purpose = is_preferred ? ViewLayerPurpose::TRACK_PREFERRED : ViewLayerPurpose::TRACK_NON_PREFERRED;

                    // Per-layer copy of the shared line geometry - .layer
                    // is deliberately not set here (RenderShape has no such
                    // field): the push below keys directly into
                    // shapes_by_layer by `layer_id`/`purpose`, so a
                    // per-shape layer field would never be read again
                    // anyway (see render_shape.hpp's own doc comment).
                    RenderShape shape = lines;
                    shapes_by_layer[view_layers.find(layer_id, purpose)].push_back(std::move(shape));
                }
            }
        }

        static void append_gcell_grid_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, ViewLayerShapes &shapes_by_layer)
        {
            const std::optional<Rect> die_bbox = layout_die_area_bbox(root, layout_id);
            if (!die_bbox)
                return;

            const ViewLayerId gcellgrid_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::GCELLGRID);
            RenderShape lines;
            for (GCellGridId grid_id : root.get_layout_gcell_grids(layout_id))
            {
                const GCellGridData *grid = root.get_g_cell_grid(grid_id);
                if (!grid || grid->count <= 0)
                    continue;

                lines.paths.reserve(lines.paths.size() + static_cast<std::size_t>(grid->count)); // exact - every iteration below pushes exactly one
                for (int i = 0; i < grid->count; i++)
                {
                    const int64_t coord = grid->start + static_cast<int64_t>(i) * grid->step;
                    const Point p1 = grid->is_x ? Point{.x = coord, .y = die_bbox->ll.y} : Point{.x = die_bbox->ll.x, .y = coord};
                    const Point p2 = grid->is_x ? Point{.x = coord, .y = die_bbox->ur.y} : Point{.x = die_bbox->ur.x, .y = coord};
                    lines.paths.push_back(Path{.width = 0, .polygon = Polygon{.points = {p1, p2}}});
                }
            }
            if (!lines.paths.empty())
                shapes_by_layer[gcellgrid_view_layer].push_back(std::move(lines));
        }

        // A Layout's PhysicalPorts (DEF PINS): their shapes on each
        // layer's TERMINAL column (stored in
        // design coordinates - DEFReader places them), the port's name as a
        // label per layer - placed like an Abstract terminal's (see
        // collect_abstract_content) - and a direction marker on
        // PORT_MARKER beside the outer edge of one piece: the port's
        // largest, which also carries its layer's label. Bounding every
        // piece instead would stretch a power grid's marker across the
        // block. One RenderShape per port, so the rasterizer can enlarge
        // each about its own anchor (port_marker_scale_factor).
        static void append_physical_port_shapes(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, ViewLayerShapes &shapes_by_layer,
                                                ChunkSources *sources = nullptr)
        {
            auto record = [&](ViewLayerId view_layer, ShapeId shape_id)
            {
                if (sources)
                    sources->shapes[view_layer].push_back(shape_id);
            };
            const std::optional<Rect> die = layout_die_area_bbox(root, layout_id);
            const ViewLayerId marker_view_layer = view_layers.port_marker_view_layer();

            for (PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
            {
                const PhysicalPortData *port = root.get_physical_port(port_id);
                if (!port)
                    continue;

                struct LabelAccumulator
                {
                    RenderShape combined;
                    ViewLayerId view_layer;
                    std::size_t first_shape_index = 0;
                };
                std::unordered_map<LayerId, LabelAccumulator> by_layer;
                RenderShape whole_port;

                for (PhysicalPortSegmentId segment_id : root.get_physical_port_segments(port_id))
                    for (ShapeId shape_id : root.get_physical_port_segment_shapes(segment_id))
                    {
                        const ShapeData *shape = root.get_shape(shape_id);
                        if (!shape)
                            continue;
                        append_via_shapes(root, *shape, ViewLayerPurpose::TERMINAL, view_layers, layout_id, shapes_by_layer, [&](ViewLayerId via_layer)
                                          { record(via_layer, shape_id); });
                        const ViewLayerId view_layer = resolve_view_layer(view_layers, *shape, ViewLayerPurpose::TERMINAL);
                        std::vector<RenderShape> &layer_shapes = shapes_by_layer[view_layer];

                        auto [it, inserted] = by_layer.try_emplace(shape->layer);
                        if (inserted)
                        {
                            it->second.view_layer = view_layer;
                            it->second.first_shape_index = layer_shapes.size();
                        }
                        for (RenderShape *acc : {&it->second.combined, &whole_port})
                        {
                            acc->rects.insert(acc->rects.end(), shape->rects.begin(), shape->rects.end());
                            acc->polygons.insert(acc->polygons.end(), shape->polygons.begin(), shape->polygons.end());
                            acc->paths.insert(acc->paths.end(), shape->paths.begin(), shape->paths.end());
                        }
                        layer_shapes.push_back(to_render_shape(*shape));
                        record(view_layer, shape_id);
                    }

                for (const auto &[layer_id, acc] : by_layer)
                {
                    if (acc.combined.rects.empty() && acc.combined.polygons.empty() && acc.combined.paths.empty())
                        continue;
                    const Point location = Geometry::get_label_location(acc.combined);
                    shapes_by_layer[acc.view_layer][acc.first_shape_index].texts.push_back(Text{
                        .label = port->name,
                        .location = location,
                        .size = Geometry::local_width_at(acc.combined, location),
                    });
                }

                if (die)
                    if (const std::optional<Rect> piece_bbox = Geometry::label_piece_bbox(whole_port))
                    {
                        RenderShape marker{.polygons = port_marker_polygons(*piece_bbox, *die, port->direction)};
                        if (!marker.polygons.empty())
                        {
                            shapes_by_layer[marker_view_layer].push_back(std::move(marker));
                            record(marker_view_layer, ShapeId{}); // not selectable
                        }
                    }
            }
        }

        static void append_region_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, ViewLayerShapes &shapes_by_layer)
        {
            const ViewLayerId region_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::REGION);
            for (RegionId region_id : root.get_layout_regions(layout_id))
            {
                const RegionData *region = root.get_region(region_id);
                if (!region || region->rects.empty())
                    continue;
                RenderShape shape;
                shape.rects = region->rects;
                shapes_by_layer[region_view_layer].push_back(std::move(shape));
            }
        }

        // Returns just the shapes, grouped by ViewLayer - placement_data
        // is always filled in by the caller (compute()'s own Layout
        // branch), and the PLACEMENT shape below is appended to
        // this same structure by that caller too, before it wraps the
        // whole thing into ViewData::shapes' own ViewShapesHandle exactly
        // once (see that type's own comment) - collect_layout_content
        // itself can't do that wrap, since there's more to append after
        // it returns.
        // One fixed LayoutChunk's shapes.
        static ViewLayerShapes collect_layout_chunk(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, LayoutChunk chunk,
                                                    ChunkSources &sources)
        {
            ViewLayerShapes shapes_by_layer;

            auto push_shape_id = [&](ShapeId shape_id, ViewLayerPurpose fallback_purpose)
            {
                const ShapeData *shape = root.get_shape(shape_id);
                if (!shape)
                    return;
                append_via_shapes(root, *shape, fallback_purpose, view_layers, layout_id, shapes_by_layer);
                shapes_by_layer[resolve_view_layer(view_layers, *shape, fallback_purpose)].push_back(to_render_shape(*shape));
            };

            if (chunk == LayoutChunk::DIEAREA_BLOCKAGES)
            {
                const ResolverPhaseTimer timer("layout.diearea_blockages");
                if (const ShapeData *diearea = root.get_shape(root.get_layout_diearea(layout_id)))
                    shapes_by_layer[view_layers.boundary_view_layer()].push_back(to_render_shape(*diearea));

                for (BlockageId blockage_id : root.get_layout_blockages(layout_id))
                    for (ShapeId shape_id : root.get_blockage_shapes(blockage_id))
                        push_shape_id(shape_id, ViewLayerPurpose::ROUTING_BLOCKAGE);
            }

            if (chunk == LayoutChunk::PORTS)
            {
                const ResolverPhaseTimer timer("layout.ports_free_shapes");
                append_physical_port_shapes(root, view_layers, layout_id, shapes_by_layer, &sources);
            }

            if (chunk == LayoutChunk::FREE_SHAPES)
            {
                const ResolverPhaseTimer timer("layout.ports_free_shapes");
                append_free_shapes(root, view_layers, root.get_layout_free_shapes(layout_id), layout_id, shapes_by_layer);
            }

            if (chunk == LayoutChunk::ROWS_TRACKS_GCELLS_REGIONS)
            {
                const ResolverPhaseTimer timer("layout.rows_tracks_gcells_regions");
                append_row_shapes(root, layout_id, view_layers, shapes_by_layer);
                append_track_shapes(root, layout_id, view_layers, shapes_by_layer);
                append_gcell_grid_shapes(root, layout_id, view_layers, shapes_by_layer);
                append_region_shapes(root, layout_id, view_layers, shapes_by_layer);
            }
            // Routes and placements are tiled: collect_route_tile,
            // collect_placement_tile.

            // A renderable class's objects: every Shape they own, on the
            // class's own row, each recorded for selection.
            renderable::for_each([&]<class R>(R) {
                if (!R::tiled && chunk == renderable_chunk<R>())
                    collect_renderable_objects<R>(root, view_layers, layout_id, R::in_layout(root, layout_id), shapes_by_layer, sources);
            });

            return shapes_by_layer;
        }

        std::optional<ResolveState> state_;
        // Route/placement tile membership of every Layout in the previous
        // output (kept in step with it).
        std::unordered_map<LayoutId, LayoutTiling> tilings_;
        bool last_compute_was_incremental_ = false;
    };
}
