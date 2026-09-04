#pragma once

#include "../../core/placement_geometry.hpp"
#include "../../core/row_geometry.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"

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
    /// @brief One Shape (fully realized, in dbu) tagged with the ViewLayer
    /// it draws on - PIPELINE_REFACTOR.md's own ViewShape.
    struct ViewShape
    {
        Shape shape;
        ViewLayerId view_layer;
    };

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
    /// `location`/`orientation` and `bbox` serve two different downstream
    /// purposes and neither substitutes for the other: `bbox` (this
    /// placement's own resolved world-space footprint, in this Layout's
    /// own local dbu space - what placement_world_bbox, core/
    /// placement_geometry.hpp, computes) is what a Warm-tier viewport-
    /// culling stage tests against the viewport Rect - cheap AABB-vs-AABB,
    /// no per-shape work. `location`/`orientation` are what a *later*
    /// compose step needs to actually draw the child's own real content
    /// once a placement survives culling - `bbox` alone can't reconstruct
    /// that (orientation isn't recoverable from a bounding box, and even
    /// for a fixed orientation, `location` and `bbox.ll` only coincide
    /// when the referenced Abstract's own declared ORIGIN is (0, 0) -
    /// AbstractData.origin isn't applied yet, core/placement_geometry.hpp's
    /// own resolved_local_bbox comment, but once it is they can genuinely
    /// differ, so this doesn't collapse to one field even in principle).
    struct ViewPlacementData
    {
        HierarchyId id;
        Point location;
        Rect bbox;
        Orientation orientation = Orientation::N;
    };

    /// @brief One Abstract's or Layout's own resolved content -
    /// PIPELINE_REFACTOR.md's own ViewData. `shapes` is this node's own
    /// *direct* geometry only (an Abstract's Terminals/Obstructions/
    /// boundary; a Layout's own diearea/blockages/routes/physical ports/
    /// rows/tracks/gcell grids/regions) - a placed child's own shapes live
    /// under its own id in HierarchyResolverOutput::view_data, not
    /// duplicated here; composing a placement's own transform onto its
    /// child's shapes is a Warm-tier concern, not Cold's.
    struct ViewData
    {
        std::vector<ViewShape> shapes;
        std::vector<ViewPlacementData> placement_data;
    };

    /// @brief PIPELINE_REFACTOR.md's own HierarchyResolverOutput - every
    /// Abstract/Layout HierarchyResolverStage's traversal reached, keyed
    /// by its own id.
    struct HierarchyResolverOutput
    {
        std::unordered_map<HierarchyId, ViewData, HierarchyIdHash> view_data;
    };

    /// @brief HierarchyResolverStage's own InputData - LayerGenerationStage's
    /// own OutputHandle (tbb_core.hpp's MemoizingStage::OutputHandle), so
    /// ViewRenderPipeline (view_render_pipeline.hpp) can wire the two
    /// stages together with a real make_edge and no adapter node in
    /// between - both sides
    /// of that edge are exactly this type. The Root pointer this stage
    /// also needs travels via ColdPipelineOptions::root instead of being
    /// part of this InputData - it isn't part of LayerGenerationStage's
    /// own output, so it couldn't flow through that same edge.
    using ViewLayerSetHandle = std::shared_ptr<const ViewLayerSet>;

    /// @brief Cold-tier stage 2 (PIPELINE_REFACTOR.md): traverses
    /// Placement -> Design hierarchy from ColdPipelineOptions::top_level,
    /// consuming one unit of ColdPipelineOptions::hierarchy_depth per
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
    /// concerns with no equivalent field on ViewShape) and via-shape
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
    class HierarchyResolverStage : public MemoizingStage<ViewLayerSetHandle, HierarchyResolverOutput, ColdPipelineOptions>
    {
    public:
        explicit HierarchyResolverStage(oneapi::tbb::flow::graph &g, std::string label = "HierarchyResolver")
            : MemoizingStage(g, std::move(label)) {}

    protected:
        HierarchyResolverOutput compute(const ViewLayerSetHandle &view_layers_handle, const ColdPipelineOptions &options) override
        {
            HierarchyResolverOutput result;
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
                    ViewData data = collect_layout_content(root, view_layers, *layout_id);

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
                    // each up front). ViewShape has no SelectionRef/
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
                    placement_boundary_shape.texts.reserve(placements.size());

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

                        // Label position/size computed directly from
                        // `bbox` (a rect's own center, and
                        // min(width, height)) rather than calling
                        // Geometry::get_label_location/local_width_at on
                        // a throwaway single-rect Shape - see the
                        // preserved comment below for why.
                        const Point label_location{(bbox.ll.x + bbox.ur.x) / 2, (bbox.ll.y + bbox.ur.y) / 2};
                        const double label_size = static_cast<double>(std::min(bbox.ur.x - bbox.ll.x, bbox.ur.y - bbox.ll.y));
                        placement_boundary_shape.rects.push_back(bbox);
                        placement_boundary_shape.texts.push_back(Text{.label = placement->name, .location = label_location, .size = label_size});

                        if (item.remaining_depth <= 0)
                            continue; // depth exhausted - placeholder drawn above, nothing further resolved/visited

                        const int child_remaining_depth = target.kind == DesignTarget::Kind::Layout ? item.remaining_depth - 1 : 0;
                        data.placement_data.push_back(ViewPlacementData{
                            .id = child_id,
                            .location = *placement->location,
                            .bbox = bbox,
                            .orientation = orientation,
                        });
                        worklist.push_back(WorkItem{child_id, child_remaining_depth});
                    }

                    if (!placement_boundary_shape.rects.empty())
                    {
                        const ViewLayerId placement_boundary_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::PLACEMENT_BOUNDARY);
                        data.shapes.push_back(ViewShape{.shape = std::move(placement_boundary_shape), .view_layer = placement_boundary_view_layer});
                    }

                    result.view_data.emplace(item.id, std::move(data));
                }
                else
                {
                    const AbstractId abstract_id = std::get<AbstractId>(item.id);
                    result.view_data.emplace(item.id, collect_abstract_content(root, view_layers, abstract_id));
                }
            }

            return result;
        }

        bool options_did_change(const ColdPipelineOptions &last, const ColdPipelineOptions &current) const override
        {
            return last.root_mutation_version != current.root_mutation_version ||
                   last.top_level != current.top_level ||
                   last.hierarchy_depth != current.hierarchy_depth;
        }

    private:
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

        static ViewData collect_abstract_content(const Root &root, const ViewLayerSet &view_layers, AbstractId abstract_id)
        {
            ViewData data;
            const auto &terminals = root.get_abstract_terminals(abstract_id);
            const auto &obstructions = root.get_abstract_obstructions(abstract_id);
            data.shapes.reserve(terminals.size() + obstructions.size());

            for (TerminalId terminal_id : terminals)
            {
                // Accumulates just the geometry primitives (not whole
                // Shapes) per Layer, purely to place that Layer's own name
                // label once its combined bbox is known - mirrors
                // AbstractGeometryStage's own LabelAccumulator.
                struct LabelAccumulator
                {
                    Shape combined;
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

                        auto [it, inserted] = by_layer.try_emplace(shape.layer);
                        if (inserted)
                            it->second.first_shape_index = data.shapes.size();
                        Shape &combined = it->second.combined;
                        combined.rects.insert(combined.rects.end(), shape.rects.begin(), shape.rects.end());
                        combined.polygons.insert(combined.polygons.end(), shape.polygons.begin(), shape.polygons.end());
                        combined.paths.insert(combined.paths.end(), shape.paths.begin(), shape.paths.end());

                        data.shapes.push_back(ViewShape{.shape = std::move(shape), .view_layer = view_layer});
                    }
                }

                if (by_layer.empty())
                    continue;

                if (const TerminalData *terminal = root.get_terminal(terminal_id))
                {
                    for (const auto &[layer_id, acc] : by_layer)
                    {
                        const Point location = Geometry::get_label_location(acc.combined);
                        data.shapes[acc.first_shape_index].shape.texts.push_back(Text{
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
                    data.shapes.push_back(ViewShape{.shape = std::move(shape), .view_layer = view_layer});
                }
            }

            if (const Shape *boundary_shape = root.get_shape(root.get_abstract_boundary(abstract_id)))
                data.shapes.push_back(ViewShape{.shape = *boundary_shape, .view_layer = view_layers.boundary_view_layer()});

            return data;
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
        // compute() loop's own placement-boundary batching: ViewShape has
        // no SelectionRef/ShapeId of its own to preserve per-Row, unlike
        // the pre-restart RenderedShape) rather than one Shape/ViewShape per
        // Row, the pre-restart stage's own convention - Row count is far
        // below Placement's own (hundreds to low thousands, not hundreds
        // of thousands), so the absolute win is smaller, but it's the same
        // fix for the same reason.
        static void append_row_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<ViewShape> &shapes)
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

            shapes.push_back(ViewShape{.shape = std::move(shape), .view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::ROW)});
        }

        static void append_track_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<ViewShape> &shapes)
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
                    shapes.push_back(ViewShape{.shape = std::move(shape), .view_layer = view_layers.find(layer_id, purpose)});
                }
            }
        }

        static void append_gcell_grid_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<ViewShape> &shapes)
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
                shapes.push_back(ViewShape{.shape = std::move(lines), .view_layer = gcellgrid_view_layer});
        }

        static void append_region_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<ViewShape> &shapes)
        {
            const ViewLayerId region_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::REGION);
            for (RegionId region_id : root.get_layout_regions(layout_id))
            {
                const RegionData *region = root.get_region(region_id);
                if (!region || region->rects.empty())
                    continue;
                Shape shape;
                shape.rects = region->rects;
                shapes.push_back(ViewShape{.shape = std::move(shape), .view_layer = region_view_layer});
            }
        }

        static ViewData collect_layout_content(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id)
        {
            ViewData data;

            // Rough lower-bound reserve, cheap to compute up front (no
            // second pass over blockage_id/route_id/port_id's own shape
            // lists just to size this exactly - that would double the
            // real work below to save only some of data.shapes' own
            // growth-reallocation cost, not all of it). A Blockage/Route/
            // PhysicalPortSegment can carry more than one Shape, so this
            // undercounts the real total, but every reallocation this
            // avoids still saves moving every ViewShape gathered so far -
            // a real cost at real route counts (a Route is the single
            // largest DEF construct by count in every aes_scaling fixture).
            data.shapes.reserve(root.get_layout_blockages(layout_id).size() +
                                 root.get_layout_routes(layout_id).size() +
                                 root.get_layout_physical_ports(layout_id).size() +
                                 root.get_layout_rows(layout_id).size() +
                                 root.get_layout_tracks(layout_id).size() +
                                 root.get_layout_regions(layout_id).size() +
                                 3); // diearea + GCELLGRID + PLACEMENT_BOUNDARY, each at most one shape

            auto push_shape_id = [&](ShapeId shape_id, ViewLayerPurpose fallback_purpose)
            {
                const Shape *shape = root.get_shape(shape_id);
                if (!shape)
                    return;
                data.shapes.push_back(ViewShape{.shape = *shape, .view_layer = resolve_view_layer(view_layers, *shape, fallback_purpose)});
            };

            if (const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id)))
                data.shapes.push_back(ViewShape{.shape = *diearea, .view_layer = view_layers.boundary_view_layer()});

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

            append_row_shapes(root, layout_id, view_layers, data.shapes);
            append_track_shapes(root, layout_id, view_layers, data.shapes);
            append_gcell_grid_shapes(root, layout_id, view_layers, data.shapes);
            append_region_shapes(root, layout_id, view_layers, data.shapes);
            // PLACEMENT_BOUNDARY is added by the main compute() loop, not
            // here - it needs resolve_design_target's own per-placement
            // dispatch (Layout vs. Abstract, depth-dependent) and the
            // same resolved bbox that loop's own ViewPlacementData::bbox
            // uses, so it's computed once there rather than duplicated
            // into a second pass over this Layout's own placements.

            return data;
        }
    };
}
