#pragma once

#include "../../core/placement_geometry.hpp"
#include "../../core/row_geometry.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../draw_helpers.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "../via_shapes.hpp"

#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace le
{
    /// @brief Which further Abstract or Layout a Placement resolves to -
    /// PIPELINE_REFACTOR.md's own id type (also HierarchyResolverOutput's
    /// own map key). See resolve_design_target (core/placement_geometry.hpp)
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

    /// @brief One placed instance of a Layout's own content -
    /// PIPELINE_REFACTOR.md's own ViewPlacementData. `id` is already
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
    ///     AbstractData.origin isn't applied yet (core/placement_geometry.hpp's
    ///     own resolved_local_bbox comment) - once it is, they can
    ///     genuinely differ.
    struct ViewPlacementData
    {
        HierarchyId id;
        Point location;
        Rect bbox;
        Geometry::InstanceTransform transform;
        Orientation orientation = Orientation::N;
    };

    /// @brief One node's own direct shapes, grouped by the ViewLayer they
    /// draw on - not a flat list, so a Warm-tier drawing stage
    /// (RasterizeStage) never needs to detect "same layer as the
    /// previous shape" while iterating: it walks ViewLayerSet::all() (in
    /// that ViewLayerSet's own bottom-to-top insertion/z-order, since a
    /// fresh ViewLayerSet's own ViewLayerId.index is assigned strictly in
    /// build_for_technology's own call order - see that method's own doc
    /// comment) and looks up each layer's own group directly - which
    /// also means constructing one SkPaint per *layer* instead of per
    /// *shape* (hoisted out of the per-shape loop, RasterizeStage's own
    /// comment) and, for a not-yet-built layer-visibility feature,
    /// skipping a hidden layer's whole group in one map lookup rather
    /// than checking every individual shape's own layer. `Id<Tag>`
    /// already has a std::hash specialization (ids.hpp) - no custom
    /// hasher needed here, unlike HierarchyIdHash above (a variant, not a
    /// plain Id).
    ///
    /// A Warm-tier stage (ViewportCullStage) builds a new ViewData per
    /// node with different `placement_data` but the exact same `shapes`,
    /// every single viewport-only call; measured directly
    /// (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) that copying a
    /// ~1,000,000-entry shapes structure by value on every such call,
    /// even though its content never actually changes call to call,
    /// dominated the Warm tier's own budget far more than the placement-
    /// culling work itself - the same "a shared_ptr copy is a refcount
    /// bump regardless of payload size" fix MemoizingStage's own
    /// OutputHandle already applies one level up, applied here one level
    /// down (hence a shared_ptr-wrapped handle, not the map itself).
    using ViewLayerShapes = std::unordered_map<ViewLayerId, std::vector<Shape>>;
    using ViewShapesHandle = std::shared_ptr<const ViewLayerShapes>;

    /// @brief Per-ViewLayer spatial index over `ViewLayerShapes`' own
    /// per-layer shape vectors, built once alongside `shapes` (see
    /// build_shape_index() below) so RasterizeStage can find which
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

    /// @brief Mirrors Scene::is_view_layer_visible exactly (that class's
    /// own doc comment): visible only if BOTH its own layer-name entry
    /// (if any) and its own purpose entry (if any) say so - an unset key
    /// in either map means visible, not hidden. Defined here (rather than
    /// in a Skia-specific drawing header) since it's a pure function of
    /// `le::`/std types with no rendering-backend dependency at all -
    /// every Rasterize backend's own draw_view_shapes-shaped function
    /// (rasterize_stage.hpp's Skia one, rasterize_blend2d_stage.hpp's
    /// Blend2D one) calls this same definition.
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

    /// @brief One Abstract's or Layout's own resolved content -
    /// PIPELINE_REFACTOR.md's own ViewData. `shapes` is this node's own
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
    struct ViewData
    {
        ViewShapesHandle shapes;
        ViewShapesIndexHandle shapes_index;
        std::vector<ViewPlacementData> placement_data;
    };

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

    /// @brief PIPELINE_REFACTOR.md's own HierarchyResolverOutput - every
    /// Abstract/Layout HierarchyResolverStage's traversal reached, keyed
    /// by its own id.
    struct HierarchyResolverOutput
    {
        std::unordered_map<HierarchyId, ViewData, HierarchyIdHash> view_data;

        /// @brief Echoes this stage's own InputData (ViewLayerSetHandle)
        /// back out, unchanged - the only way a stage further downstream
        /// in a real make_edge chain (ViewportCullStage, RasterizeStage/
        /// RasterizeBlend2DStage) can still reach the actual ViewLayerSet
        /// content: nothing else wires LayerGenerationStage's own output
        /// any further than this stage's own InputData. Used to carry
        /// `view_layers` from Cold tier through to RasterizeStage without
        /// going through ViewRenderOptions::view_layers (removed -
        /// ViewRenderPipelineImpl::run(), view_render_pipeline.hpp, no
        /// longer needs to patch it into `options` between two separate
        /// graph submissions once it travels as data like this instead).
        /// ViewportCullStage's own OutputData is this same struct type -
        /// its own compute() just copies this field through unchanged
        /// alongside its real (culled) view_data.
        ViewLayerSetHandle view_layers;
    };

    /// @brief Cold-tier stage 2 (PIPELINE_REFACTOR.md): traverses
    /// Placement -> Design hierarchy from ViewRenderOptions::top_level,
    /// consuming one unit of ViewRenderOptions::hierarchy_depth per
    /// Layout -> Layout hop. At remaining_depth == 0 a Layout's own
    /// placements are never *resolved* into anything at all (not even a
    /// fallback to their own Abstract) - placement_data stays empty and
    /// nothing is pushed onto the worklist - but each placement's own
    /// PLACEMENT_BOUNDARY placeholder rect+label is still drawn (see the
    /// main compute() loop's own comment), since that's real data about
    /// this Layout's own direct content, not about what a placement
    /// resolves to. This is a deliberate departure from
    /// resolve_design_target's own "fall back to the Abstract regardless
    /// of remaining depth" convention (core/placement_geometry.hpp) -
    /// still the right choice, and still used here for sizing a
    /// placement's own placeholder rect/ViewPlacementData::bbox, for
    /// every *other* caller (hit-testing, Scene::hierarchy_depth()'s own
    /// documented semantics, backend/CLAUDE.md) - this stage's own
    /// depth==0 case just isn't one of them: "traverses hierarchy ...
    /// until hierarchy_depth is 0" is read literally here, not as
    /// "one further Abstract-only hop past 0."
    ///
    /// Gathers every reached Abstract's/Layout's own *direct* shapes
    /// (Terminal/Obstruction/boundary for an Abstract; diearea/Blockage/
    /// Route/PhysicalPort/synthesized Row/Track/GCellGrid/Region/
    /// PlacementBoundary for a Layout - the last one a synthesized,
    /// name-labeled outline of each of the Layout's own Placements' own
    /// resolved footprint, not real LEF/DEF geometry), each resolved to
    /// its ViewLayerId - a simplified port of the pre-restart
    /// AbstractGeometryStage/LayoutGeometryStage compute()
    /// bodies (src/pipelines.old/stages/): RECT/PATH/POLYGON ITERATE
    /// expansion and Terminal name-label placement are carried over (both
    /// are real *data*, not a rendering-only concern), but SelectionRef/
    /// ShapeId/path_outlines (Hot-tier hit-testing/picture-caching
    /// concerns with no equivalent tracked here) and via-shape
    /// expansion (Shape.vias/via_iterates - src/pipelines.old/stages/
    /// via_shapes.hpp, ~380 lines of its own VIARULE/array-expansion logic)
    /// are deliberately deferred to a follow-up rather than ported
    /// speculatively in the same change - flag if real via-bearing fixture
    /// data makes that gap visible sooner than expected.
    ///
    /// Traversal is breadth-first, one worklist entry per discovered
    /// {id, remaining_depth}, deduplicating by `id` alone - not by
    /// {id, remaining_depth}, unlike the pre-restart per-node graph's own
    /// Layout key (HierarchyLayoutNodeStage's own doc comment): this
    /// stage's own ViewData never bakes in a recursively-composed picture
    /// the way that stage's own SkPicture output did, so a Layout's own
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
    class HierarchyResolverStage : public MemoizingStage<ViewLayerSetHandle, HierarchyResolverOutput, ViewRenderOptions>
    {
    public:
        explicit HierarchyResolverStage(oneapi::tbb::flow::graph &g, std::string label = "HierarchyResolver")
            : MemoizingStage(g, std::move(label)) {}

    protected:
        HierarchyResolverOutput compute(const ViewLayerSetHandle &view_layers_handle, const ViewRenderOptions &options) override
        {
            HierarchyResolverOutput result;
            result.view_layers = view_layers_handle;
            if (options.root == nullptr)
                return result;

            const Root &root = *options.root;
            static const ViewLayerSet kEmptyViewLayers;
            const ViewLayerSet &view_layers = view_layers_handle != nullptr ? *view_layers_handle : kEmptyViewLayers;

            struct WorkItem
            {
                HierarchyId id;
                int remaining_depth;
            };
            std::deque<WorkItem> worklist;
            worklist.push_back(WorkItem{options.top_level, options.hierarchy_depth});

            while (!worklist.empty())
            {
                const WorkItem item = worklist.front();
                worklist.pop_front();

                if (result.view_data.contains(item.id))
                    continue; // already resolved at an earlier (shallower) worklist entry

                if (const LayoutId *layout_id = std::get_if<LayoutId>(&item.id))
                {
                    ViewLayerShapes shapes_by_layer = collect_layout_content(root, view_layers, *layout_id);
                    ViewData data;

                    const auto &placements = root.get_layout_placements(*layout_id);
                    if (item.remaining_depth > 0)
                        data.placement_data.reserve(placements.size()); // exact upper bound - not every placement resolves

                    // One PLACEMENT_BOUNDARY rect+label per placement,
                    // batched into a single Shape - measured directly
                    // against aes_scaling_3x3 (372,096 placements): a
                    // one-Shape-per-placement version spent ~126ms of its
                    // ~149ms total on Shape/Text construction and the two
                    // heap allocations each incurs, not on bbox/label
                    // geometry (~23ms combined) - batching turns
                    // O(placements) allocations into O(1) (one reserve()
                    // each up front). A plain Shape has no SelectionRef/
                    // ShapeId of its own (unlike the pre-restart
                    // RenderedShape) so there's no independent per-
                    // placement selection identity this would need to
                    // preserve, unlike Row/Region's own one-per-item
                    // convention elsewhere in this file.
                    //
                    // Computed here, in the same loop as the resolve/
                    // recurse decision, rather than as a separate pass
                    // over collect_layout_content: both need
                    // resolve_design_target's own dispatch for this same
                    // placement, and both need the same resolved bbox
                    // (ViewPlacementData::bbox and this placeholder's own
                    // rect are now the exact same value, computed once,
                    // not twice) - a real, measured redundancy this
                    // consolidation removes, not just a tidiness pass.
                    Shape placement_boundary_shape;
                    placement_boundary_shape.rects.reserve(placements.size());

                    // A Placement's own name label is its own Shape, on
                    // its own dedicated PLACEMENT_NAME ViewLayer (view_style.hpp)
                    // - split out from PLACEMENT_BOUNDARY (BUGS_AND_ENHANCEMENTS.md
                    // E13) so a label's own color/visibility toggle is
                    // independent of the boundary outline it's drawn
                    // alongside, matching pipelines.old/draw_helpers.hpp's
                    // own draw_placement_labels. rects/texts stay index-
                    // parallel (rects[i] is texts[i]'s own reference box,
                    // for RasterizeStage's own width-fit truncation) - see
                    // draw_view_shapes' own comment.
                    Shape placement_name_shape;
                    placement_name_shape.rects.reserve(placements.size());
                    placement_name_shape.texts.reserve(placements.size());

                    for (PlacementId placement_id : placements)
                    {
                        const PlacementData *placement = root.get_placement(placement_id);
                        if (!placement || !placement->location || !placement->reference_design.valid())
                            continue;

                        // resolve_design_target is called unconditionally
                        // (regardless of remaining_depth) - the placeholder
                        // rect below always needs a resolved size, even at
                        // remaining_depth == 0 where nothing gets visited
                        // past this point (see this class's own top
                        // comment on why depth 0 still draws placeholders).
                        const DesignTarget target = resolve_design_target(root, placement->reference_design, item.remaining_depth);
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

                        // Label size/position ported from
                        // pipelines.old/draw_helpers.hpp's own
                        // draw_placement_labels: font size is a fraction
                        // of the placement's own on-screen *height*
                        // (kPlacementLabelHeightRatio, floored at
                        // kMinLabelPixelSize - applied at draw time,
                        // RasterizeStage's own text loop, once `scale` is
                        // known), anchored at the box's own bottom-left
                        // corner (RasterizeStage adds the fixed pixel
                        // padding at draw time too, in already-counter-
                        // scaled local space, so it stays a constant
                        // on-screen inset regardless of zoom - baking a
                        // dbu-space padding in here instead would grow/
                        // shrink with zoom, the wrong behavior). `size` is
                        // therefore a pure dbu quantity (bbox height x the
                        // ratio), matching Text.size's own schema
                        // convention ("local width of the shape geometry
                        // ... used to size the rendered text") rather than
                        // a literal pixel font size.
                        const double height_dbu = static_cast<double>(bbox.ur.y - bbox.ll.y);
                        placement_boundary_shape.rects.push_back(bbox);
                        placement_name_shape.rects.push_back(bbox);
                        placement_name_shape.texts.push_back(Text{.label = placement->name, .location = bbox.ll, .size = height_dbu * kPlacementLabelHeightRatio});

                        if (item.remaining_depth <= 0)
                            continue; // depth exhausted - placeholder drawn above, nothing further resolved/visited

                        const int child_remaining_depth = target.kind == DesignTarget::Kind::Layout ? item.remaining_depth - 1 : 0;
                        data.placement_data.push_back(ViewPlacementData{
                            .id = child_id,
                            .location = *placement->location,
                            .bbox = bbox,
                            .transform = transform,
                            .orientation = orientation,
                        });
                        worklist.push_back(WorkItem{child_id, child_remaining_depth});
                    }

                    if (!placement_boundary_shape.rects.empty())
                    {
                        const ViewLayerId placement_boundary_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::PLACEMENT_BOUNDARY);
                        shapes_by_layer[placement_boundary_view_layer].push_back(std::move(placement_boundary_shape));
                        shapes_by_layer[view_layers.placement_name_view_layer()].push_back(std::move(placement_name_shape));
                    }

                    data.shapes = std::make_shared<const ViewLayerShapes>(std::move(shapes_by_layer));
                    data.shapes_index = build_shape_index(*data.shapes);
                    result.view_data.emplace(item.id, std::move(data));
                }
                else
                {
                    const AbstractId abstract_id = std::get<AbstractId>(item.id);
                    ViewData data;
                    data.shapes = std::make_shared<const ViewLayerShapes>(collect_abstract_content(root, view_layers, abstract_id));
                    data.shapes_index = build_shape_index(*data.shapes);
                    result.view_data.emplace(item.id, std::move(data));
                }
            }

            return result;
        }

        bool options_did_change(const ViewRenderOptions &last, const ViewRenderOptions &current) const override
        {
            return last.root_mutation_version != current.root_mutation_version ||
                   last.top_level != current.top_level ||
                   last.hierarchy_depth != current.hierarchy_depth;
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
        // don't execute).
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

        // Expands RECT/PATH/POLYGON ITERATE (UPDATES.md 12 Phase 1's raw-
        // storage rework - see AbstractGeometryStage's own comment,
        // src/pipelines.old/) into concrete rects/paths/polygons on a copy
        // of `shape`. LEF-only in practice (DEF content never populates
        // these fields), so collect_layout_content doesn't call this -
        // matches the pre-restart stage split exactly.
        static Shape expand_iterates(Shape shape)
        {
            constexpr int kMaxReasonableCount = 1'000'000;

            for (const RectIterate &it : shape.rect_iterates)
            {
                if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                    continue;
                shape.rects.reserve(shape.rects.size() + static_cast<std::size_t>(it.num_x) * static_cast<std::size_t>(it.num_y));
                for (int ix = 0; ix < it.num_x; ix++)
                    for (int iy = 0; iy < it.num_y; iy++)
                        shape.rects.push_back(Rect{
                            .ll = Point{.x = it.rect.ll.x + ix * it.space_x, .y = it.rect.ll.y + iy * it.space_y},
                            .ur = Point{.x = it.rect.ur.x + ix * it.space_x, .y = it.rect.ur.y + iy * it.space_y},
                        });
            }
            shape.rect_iterates.clear();

            for (const PathIterate &it : shape.path_iterates)
            {
                if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                    continue;
                shape.paths.reserve(shape.paths.size() + static_cast<std::size_t>(it.num_x) * static_cast<std::size_t>(it.num_y));
                for (int ix = 0; ix < it.num_x; ix++)
                    for (int iy = 0; iy < it.num_y; iy++)
                    {
                        const Point offset{.x = ix * it.space_x, .y = iy * it.space_y};
                        shape.paths.push_back(Path{.width = it.path.width, .polygon = Geometry::transform(it.path.polygon, offset)});
                    }
            }
            shape.path_iterates.clear();

            for (const PolygonIterate &it : shape.polygon_iterates)
            {
                if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                    continue;
                shape.polygons.reserve(shape.polygons.size() + static_cast<std::size_t>(it.num_x) * static_cast<std::size_t>(it.num_y));
                for (int ix = 0; ix < it.num_x; ix++)
                    for (int iy = 0; iy < it.num_y; iy++)
                    {
                        const Point offset{.x = ix * it.space_x, .y = iy * it.space_y};
                        shape.polygons.push_back(Geometry::transform(it.polygon, offset));
                    }
            }
            shape.polygon_iterates.clear();

            return shape;
        }

        static ViewLayerPurpose to_view_layer_purpose(ShapePurpose purpose)
        {
            switch (purpose)
            {
            case ShapePurpose::PLACEMENT_BLOCKAGE:
                return ViewLayerPurpose::PLACEMENT_BLOCKAGE;
            case ShapePurpose::BOUNDARY:
            default:
                return ViewLayerPurpose::BOUNDARY;
            }
        }

        // A real physical Layer (Shape.layer valid) resolves via the given
        // purpose against that Layer's own row (e.g. a ROUTING Blockage's
        // Shape); one with no Layer instead carries its own Shape.purpose
        // (BOUNDARY/PLACEMENT_BLOCKAGE), resolved directly against that
        // purpose's own pseudo-row.
        static ViewLayerId resolve_view_layer(const ViewLayerSet &view_layers, const Shape &shape, ViewLayerPurpose fallback_purpose)
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
                // label once its combined bbox is known - mirrors
                // AbstractGeometryStage's own LabelAccumulator. Tracks
                // its own resolved view_layer too (not just first_shape_index)
                // since the label-attach loop below needs it to reach
                // back into shapes_by_layer, and it's a pure function of
                // shape.layer (this LabelAccumulator's own map key) plus
                // the fixed TERMINAL purpose - cheaper to remember than
                // to call resolve_view_layer a second time.
                struct LabelAccumulator
                {
                    Shape combined;
                    ViewLayerId view_layer;
                    std::size_t first_shape_index = 0;
                };
                std::unordered_map<LayerId, LabelAccumulator> by_layer;

                for (TerminalPortId port_id : root.get_terminal_ports(terminal_id))
                {
                    for (ShapeId shape_id : root.get_terminal_port_shapes(port_id))
                    {
                        const Shape *raw_shape = root.get_shape(shape_id);
                        if (!raw_shape)
                            continue;
                        Shape shape = expand_iterates(*raw_shape);
                        const ViewLayerId view_layer = resolve_view_layer(view_layers, shape, ViewLayerPurpose::TERMINAL);
                        std::vector<Shape> &layer_shapes = shapes_by_layer[view_layer];

                        auto [it, inserted] = by_layer.try_emplace(shape.layer);
                        if (inserted)
                        {
                            it->second.view_layer = view_layer;
                            it->second.first_shape_index = layer_shapes.size();
                        }
                        Shape &combined = it->second.combined;
                        combined.rects.insert(combined.rects.end(), shape.rects.begin(), shape.rects.end());
                        combined.polygons.insert(combined.polygons.end(), shape.polygons.begin(), shape.polygons.end());
                        combined.paths.insert(combined.paths.end(), shape.paths.begin(), shape.paths.end());

                        append_via_shapes(root, shape, ViewLayerPurpose::TERMINAL, view_layers, LayoutId{}, shapes_by_layer);
                        layer_shapes.push_back(std::move(shape));
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
                    const Shape *raw_shape = root.get_shape(shape_id);
                    if (!raw_shape)
                        continue;
                    Shape shape = expand_iterates(*raw_shape);
                    const ViewLayerId view_layer = resolve_view_layer(view_layers, shape, ViewLayerPurpose::OBSTRUCTION);
                    append_via_shapes(root, shape, ViewLayerPurpose::OBSTRUCTION, view_layers, LayoutId{}, shapes_by_layer);
                    shapes_by_layer[view_layer].push_back(std::move(shape));
                }
            }

            if (const Shape *boundary_shape = root.get_shape(root.get_abstract_boundary(abstract_id)))
                shapes_by_layer[view_layers.boundary_view_layer()].push_back(*boundary_shape);

            return shapes_by_layer;
        }

        static std::optional<Rect> layout_die_area_bbox(const Root &root, LayoutId layout_id)
        {
            const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id));
            if (!diearea)
                return std::nullopt;
            return Geometry::bbox(*diearea);
        }

        // Row/Track/GCellGrid have no stored Shape of their own (purely
        // parametric geometry - Migration Step 2's own plan) - synthesized
        // here exactly as LayoutGeometryStage's own append_*_shapes did.
        //
        // Batched into one shared Shape (same reasoning as the main
        // compute() loop's own placement-boundary batching: a plain Shape
        // has no SelectionRef/ShapeId of its own to preserve per-Row,
        // unlike the pre-restart RenderedShape) rather than one Shape per
        // Row, the pre-restart stage's own convention - Row count is far
        // below Placement's own (hundreds to low thousands, not hundreds
        // of thousands), so the absolute win is smaller, but it's the same
        // fix for the same reason.
        static void append_row_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, ViewLayerShapes &shapes_by_layer)
        {
            const auto &rows = root.get_layout_rows(layout_id);
            if (rows.empty())
                return;

            Shape shape;
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

                Shape lines;
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

                    // BUGS_AND_ENHANCEMENTS.md E2: a track resolves to
                    // TRACK_PREFERRED if its own line direction matches
                    // this Layer's own declared preferred routing
                    // direction, else TRACK_NON_PREFERRED.
                    const LayerData *layer = root.get_layer(layer_id);
                    const bool is_preferred = layer && ((track->is_x && layer->direction == RoutingDirection::V) ||
                                                         (!track->is_x && layer->direction == RoutingDirection::H));
                    const ViewLayerPurpose purpose = is_preferred ? ViewLayerPurpose::TRACK_PREFERRED : ViewLayerPurpose::TRACK_NON_PREFERRED;

                    Shape shape = lines;
                    shape.layer = layer_id;
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
            Shape lines;
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

        static void append_region_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, ViewLayerShapes &shapes_by_layer)
        {
            const ViewLayerId region_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::REGION);
            for (RegionId region_id : root.get_layout_regions(layout_id))
            {
                const RegionData *region = root.get_region(region_id);
                if (!region || region->rects.empty())
                    continue;
                Shape shape;
                shape.rects = region->rects;
                shapes_by_layer[region_view_layer].push_back(std::move(shape));
            }
        }

        // Returns just the shapes, grouped by ViewLayer - placement_data
        // is always filled in by the caller (compute()'s own Layout
        // branch), and the PLACEMENT_BOUNDARY shape below is appended to
        // this same structure by that caller too, before it wraps the
        // whole thing into ViewData::shapes' own ViewShapesHandle exactly
        // once (see that type's own comment) - collect_layout_content
        // itself can't do that wrap, since there's more to append after
        // it returns.
        static ViewLayerShapes collect_layout_content(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id)
        {
            ViewLayerShapes shapes_by_layer;

            auto push_shape_id = [&](ShapeId shape_id, ViewLayerPurpose fallback_purpose)
            {
                const Shape *shape = root.get_shape(shape_id);
                if (!shape)
                    return;
                append_via_shapes(root, *shape, fallback_purpose, view_layers, layout_id, shapes_by_layer);
                shapes_by_layer[resolve_view_layer(view_layers, *shape, fallback_purpose)].push_back(*shape);
            };

            if (const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id)))
                shapes_by_layer[view_layers.boundary_view_layer()].push_back(*diearea);

            for (BlockageId blockage_id : root.get_layout_blockages(layout_id))
                for (ShapeId shape_id : root.get_blockage_shapes(blockage_id))
                    push_shape_id(shape_id, ViewLayerPurpose::ROUTING_BLOCKAGE);

            for (RouteId route_id : root.get_layout_routes(layout_id))
                for (ShapeId shape_id : root.get_route_shapes(route_id))
                    push_shape_id(shape_id, ViewLayerPurpose::ROUTE);

            for (PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
                for (PhysicalPortSegmentId segment_id : root.get_physical_port_segments(port_id))
                    for (ShapeId shape_id : root.get_physical_port_segment_shapes(segment_id))
                        push_shape_id(shape_id, ViewLayerPurpose::TERMINAL);

            append_row_shapes(root, layout_id, view_layers, shapes_by_layer);
            append_track_shapes(root, layout_id, view_layers, shapes_by_layer);
            append_gcell_grid_shapes(root, layout_id, view_layers, shapes_by_layer);
            append_region_shapes(root, layout_id, view_layers, shapes_by_layer);
            // PLACEMENT_BOUNDARY is added by the main compute() loop, not
            // here - it needs resolve_design_target's own per-placement
            // dispatch (Layout vs. Abstract, depth-dependent) and the
            // same resolved bbox that loop's own ViewPlacementData::bbox
            // uses, so it's computed once there rather than duplicated
            // into a second pass over this Layout's own placements.

            return shapes_by_layer;
        }
    };
}
