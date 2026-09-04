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
    struct ViewPlacementData
    {
        HierarchyId id;
        Point location;
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

    /// @brief HierarchyResolverStage's own InputData - a Root plus the
    /// ViewLayers LayerGenerationStage already resolved (its Output,
    /// PIPELINE_REFACTOR.md's own "LayerGenerationOutput"). Not yet wired
    /// to LayerGenerationStage via a real make_edge - that needs joining
    /// two different upstream OutputData types into one node's own
    /// InputData, deferred until the two stages are actually assembled
    /// into one flow::graph pipeline (see SynchronousStageRunner's own
    /// standalone-testing use in the meantime, same as LayerGenerationStage).
    struct HierarchyResolverInput
    {
        const Root *root = nullptr;
        const ViewLayerSet *view_layers = nullptr;
    };

    /// @brief Cold-tier stage 2 (PIPELINE_REFACTOR.md): traverses
    /// Placement -> Design hierarchy from ColdPipelineOptions::top_level,
    /// consuming one unit of ColdPipelineOptions::hierarchy_depth per
    /// Layout -> Layout hop, falling back to a placed instance's own
    /// Abstract once that budget is exhausted (or its reference_design
    /// simply has no Layout at all) - see resolve_design_target (core/
    /// placement_geometry.hpp), the single source of truth for that
    /// dispatch, also what Scene::hierarchy_depth()'s own documented
    /// semantics (backend/CLAUDE.md) are built on.
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
    /// in the database itself changed.
    class HierarchyResolverStage : public MemoizingStage<HierarchyResolverInput, HierarchyResolverOutput, ColdPipelineOptions>
    {
    public:
        explicit HierarchyResolverStage(oneapi::tbb::flow::graph &g, std::string label = "HierarchyResolver")
            : MemoizingStage(g, std::move(label)) {}

    protected:
        HierarchyResolverOutput compute(const HierarchyResolverInput &input, const ColdPipelineOptions &options) override
        {
            HierarchyResolverOutput result;
            if (input.root == nullptr)
                return result;

            const Root &root = *input.root;
            static const ViewLayerSet kEmptyViewLayers;
            const ViewLayerSet &view_layers = input.view_layers != nullptr ? *input.view_layers : kEmptyViewLayers;

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
                    ViewData data = collect_layout_content(root, view_layers, *layout_id, item.remaining_depth);

                    const auto &placements = root.get_layout_placements(*layout_id);
                    data.placement_data.reserve(placements.size()); // exact upper bound - not every placement resolves
                    for (PlacementId placement_id : placements)
                    {
                        const PlacementData *placement = root.get_placement(placement_id);
                        if (!placement || !placement->location || !placement->reference_design.valid())
                            continue;

                        const DesignTarget target = resolve_design_target(root, placement->reference_design, item.remaining_depth);
                        HierarchyId child_id;
                        int child_remaining_depth = 0;
                        if (target.kind == DesignTarget::Kind::Layout)
                        {
                            child_id = target.layout_id;
                            child_remaining_depth = item.remaining_depth - 1;
                        }
                        else if (target.kind == DesignTarget::Kind::Abstract)
                        {
                            child_id = target.abstract_id;
                        }
                        else
                        {
                            continue; // unresolved reference_design - nothing to place
                        }

                        data.placement_data.push_back(ViewPlacementData{
                            .id = child_id,
                            .location = *placement->location,
                            .orientation = placement->orientation.value_or(Orientation::N),
                        });
                        worklist.push_back(WorkItem{child_id, child_remaining_depth});
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
        // Batched into one shared Shape (same reasoning as
        // append_placement_boundary_shapes' own comment: ViewShape has no
        // SelectionRef/ShapeId of its own to preserve per-Row, unlike the
        // pre-restart RenderedShape) rather than one Shape/ViewShape per
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

        // One PLACEMENT_BOUNDARY-purpose rect per Placement in the Layout,
        // labeled with its own name (Text, same convention Terminal
        // labels use in collect_abstract_content) - the Placement's own
        // resolved footprint, in this Layout's own local dbu space. Uses
        // placement_world_bbox (core/placement_geometry.hpp) - the same
        // resolve_design_target-based dispatch compute()'s own placement
        // loop already applies for recursion, so a placement's own drawn
        // boundary always matches what it actually resolves to (Layout vs.
        // Abstract) at this remaining_depth, not just its raw declared
        // size. Skips a placement compute()'s own loop already skips too
        // (no location, no/unresolved reference_design) - same "nothing
        // to draw for an unplaced/dangling placement" convention.
        //
        // All rects/labels are batched into a single Shape (like
        // append_gcell_grid_shapes' own `lines` accumulator, not like
        // append_row_shapes' own one-Shape-per-Row) rather than one Shape
        // (and one ViewShape push_back) per placement - measured directly
        // against the real aes_scaling_3x3 fixture (372,096 placements):
        // the one-per-placement version spent ~126ms of its ~149ms total
        // on Shape/Text construction and the two heap allocations each
        // incurs (rects.push_back, texts.push_back), not on
        // placement_world_bbox or label geometry (~23ms combined) - a
        // real, measured cost, not a hypothetical one. Batching turns
        // O(placements) allocations for rects/texts into O(1) (one
        // reserve() each up front); ViewShape has no SelectionRef/ShapeId
        // of its own (unlike the pre-restart RenderedShape - see this
        // class's own top comment) so there's no independent per-
        // placement selection identity this would need to preserve,
        // unlike Row/Region's own one-per-item convention elsewhere in
        // this file.
        //
        // Label position/size is computed directly from `bbox` (a rect's
        // own center, and min(width, height) - exactly what
        // Geometry::get_label_location/local_width_at themselves compute
        // for a single-rect shape, confirmed against their own
        // implementation) rather than calling those generic functions on
        // a throwaway single-rect Shape: besides the avoidable allocation
        // that throwaway Shape's own rects vector would cost, calling
        // them on the real accumulating `shape` instead would rescan
        // every rect gathered *so far* on every single placement,
        // turning this loop quadratic in placement count.
        static void append_placement_boundary_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, int remaining_depth, std::vector<ViewShape> &shapes)
        {
            const auto &placements = root.get_layout_placements(layout_id);
            if (placements.empty())
                return;

            Shape shape;
            shape.rects.reserve(placements.size());
            shape.texts.reserve(placements.size());

            for (PlacementId placement_id : placements)
            {
                const std::optional<Rect> bbox = placement_world_bbox(root, placement_id, remaining_depth);
                if (!bbox)
                    continue;

                const PlacementData *placement = root.get_placement(placement_id);
                if (!placement)
                    continue;

                const Point label_location{(bbox->ll.x + bbox->ur.x) / 2, (bbox->ll.y + bbox->ur.y) / 2};
                const double label_size = static_cast<double>(std::min(bbox->ur.x - bbox->ll.x, bbox->ur.y - bbox->ll.y));

                shape.rects.push_back(*bbox);
                shape.texts.push_back(Text{.label = placement->name, .location = label_location, .size = label_size});
            }

            if (shape.rects.empty())
                return;

            const ViewLayerId placement_boundary_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::PLACEMENT_BOUNDARY);
            shapes.push_back(ViewShape{.shape = std::move(shape), .view_layer = placement_boundary_view_layer});
        }

        static ViewData collect_layout_content(const Root &root, const ViewLayerSet &view_layers, LayoutId layout_id, int remaining_depth)
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
            append_placement_boundary_shapes(root, layout_id, view_layers, remaining_depth, data.shapes);

            return data;
        }
    };
}
