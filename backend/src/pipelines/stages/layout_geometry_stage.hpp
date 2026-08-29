#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../core/row_geometry.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "via_shapes.hpp"
#include "../../view_style/view_style.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "../version_utils.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace le
{
    /// @brief oneTBB port of src/pipeline/stages/generate_layout_shapes_stage.hpp
    /// (Phase 3, backend/ONETBB_INTEGRATION.md migration plan) - collects
    /// every Shape from a Layout's own direct content (die area, blockages,
    /// routed net geometry, physical ports, synthesized rows/tracks/gcell
    /// grids/regions), each resolved to its ViewLayerId, in dbu-space.
    /// compute()'s own body is unchanged from the original stage's run()
    /// lambda plus its private helper statics - see that file's own doc
    /// comment for the full per-construct behavior; only the caching
    /// mechanism differs. Deliberately does not walk Layout.placements -
    /// see the original stage's own comment for why (placed-instance
    /// rendering is HierarchyResolver's own job, Phase 4).
    ///
    /// Same data_version derivation as AbstractGeometryStage - see that
    /// class's own doc comment for why MemoizingStage needs LayoutId's own
    /// {index, generation} folded in explicitly via data_version_for,
    /// unlike the original stage's `{LayoutId, ViewLayerSet::generation(),
    /// Root::mutation_version()}` VersionedStage key.
    class LayoutGeometryStage : public MemoizingStage<LayoutId, std::vector<RenderedShape>, PipelineOptions>
    {
    public:
        using MemoizingStage::MemoizingStage;

        static uint64_t data_version_for(LayoutId layout_id, const PipelineOptions &options)
        {
            return combine_versions(layout_id.index, layout_id.generation, options.epoch.view_layers_generation, options.epoch.root_mutation_version);
        }

    protected:
        std::vector<RenderedShape> compute(const LayoutId &layout_id, const PipelineOptions &options) override
        {
            const Root &root = *options.ctx.root;
            const ViewLayerSet &view_layers = *options.ctx.view_layers;

            std::vector<RenderedShape> shapes;

            // `origin` (E1, BUGS_AND_ENHANCEMENTS.md) makes the pushed
            // RenderedShape selectable via Pipeline::hit_test_point/
            // hit_test_rect, which already skip anything with no
            // origin - see SelectionRef's own comment for why
            // Blockage/Route/PhysicalPort all ride this same
            // mechanism (real backing Shape, just like Terminal/
            // Obstruction in the Abstract path).
            auto push_shape_id = [&](ShapeId shape_id, ViewLayerPurpose fallback_purpose, SelectionRef origin)
            {
                const Shape *shape = root.get_shape(shape_id);
                if (!shape)
                    return;
                // A real physical Layer (Shape.layer valid) resolves
                // via the given purpose against that Layer's own row -
                // e.g. a ROUTING Blockage's Shape. One with no Layer
                // instead carries its own Shape.purpose (BOUNDARY/
                // PLACEMENT_BLOCKAGE) - resolved directly against that
                // purpose's own pseudo-row, same convention
                // GenerateAbstractShapesStage's BOUNDARY handling
                // already established.
                const ViewLayerId view_layer = shape->layer.valid()
                                                    ? view_layers.find(shape->layer, fallback_purpose)
                                                    : (shape->purpose ? view_layers.find(LayerId{}, to_view_layer_purpose(*shape->purpose)) : ViewLayerId{});
                shapes.push_back(RenderedShape{.shape = *shape, .view_layer = view_layer, .origin = origin, .shape_id = shape_id, .path_outlines = compute_path_outlines(*shape)});
                append_via_shapes(root, *shape, fallback_purpose, view_layers, layout_id, shapes);
            };

            if (const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id)))
                shapes.push_back(RenderedShape{.shape = *diearea, .view_layer = view_layers.boundary_view_layer(), .path_outlines = compute_path_outlines(*diearea)});

            for (BlockageId blockage_id : root.get_layout_blockages(layout_id))
                for (ShapeId shape_id : root.get_blockage_shapes(blockage_id))
                    push_shape_id(shape_id, ViewLayerPurpose::ROUTING_BLOCKAGE, SelectionRef{blockage_id});

            for (RouteId route_id : root.get_layout_routes(layout_id))
                for (ShapeId shape_id : root.get_route_shapes(route_id))
                    push_shape_id(shape_id, ViewLayerPurpose::ROUTE, SelectionRef{route_id});

            for (PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
                for (PhysicalPortSegmentId segment_id : root.get_physical_port_segments(port_id))
                    for (ShapeId shape_id : root.get_physical_port_segment_shapes(segment_id))
                        push_shape_id(shape_id, ViewLayerPurpose::TERMINAL, SelectionRef{port_id});

            append_row_shapes(root, layout_id, view_layers, shapes);
            append_track_shapes(root, layout_id, view_layers, shapes);
            append_gcell_grid_shapes(root, layout_id, view_layers, shapes);
            append_region_shapes(root, layout_id, view_layers, shapes);

            return shapes;
        }

    private:
        static void append_row_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &shapes)
        {
            const ViewLayerId row_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::ROW);
            for (RowId row_id : root.get_layout_rows(layout_id))
            {
                const std::optional<Rect> bbox = row_footprint_bbox(root, row_id);
                if (!bbox)
                    continue;

                Shape shape;
                shape.rects.push_back(*bbox);
                shapes.push_back(RenderedShape{.shape = shape, .view_layer = row_view_layer, .origin = SelectionRef{row_id}, .path_outlines = compute_path_outlines(shape)});
            }
        }

        static void append_track_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &shapes)
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
                for (int i = 0; i < track->count; i++)
                {
                    const int64_t coord = track->start + static_cast<int64_t>(i) * track->step;
                    const Point p1 = track->is_x ? Point{.x = coord, .y = die_bbox->ll.y} : Point{.x = die_bbox->ll.x, .y = coord};
                    const Point p2 = track->is_x ? Point{.x = coord, .y = die_bbox->ur.y} : Point{.x = die_bbox->ur.x, .y = coord};
                    lines.paths.push_back(Path{.polygon = Polygon{.points = {p1, p2}}, .width = 0});
                }

                for (const std::string &layer_name : track->layer_names)
                {
                    const LayerId layer_id = root.get_layer_by_name(layer_name);
                    if (!layer_id.valid())
                        continue;
                    Shape shape = lines;
                    shape.layer = layer_id;
                    auto path_outlines = compute_path_outlines(shape);
                    shapes.push_back(RenderedShape{.shape = std::move(shape), .view_layer = view_layers.find(layer_id, ViewLayerPurpose::TRACK), .path_outlines = std::move(path_outlines)});
                }
            }
        }

        static void append_gcell_grid_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &shapes)
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

                for (int i = 0; i < grid->count; i++)
                {
                    const int64_t coord = grid->start + static_cast<int64_t>(i) * grid->step;
                    const Point p1 = grid->is_x ? Point{.x = coord, .y = die_bbox->ll.y} : Point{.x = die_bbox->ll.x, .y = coord};
                    const Point p2 = grid->is_x ? Point{.x = coord, .y = die_bbox->ur.y} : Point{.x = die_bbox->ur.x, .y = coord};
                    lines.paths.push_back(Path{.polygon = Polygon{.points = {p1, p2}}, .width = 0});
                }
            }
            if (!lines.paths.empty())
            {
                auto path_outlines = compute_path_outlines(lines);
                shapes.push_back(RenderedShape{.shape = std::move(lines), .view_layer = gcellgrid_view_layer, .path_outlines = std::move(path_outlines)});
            }
        }

        static void append_region_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &shapes)
        {
            const ViewLayerId region_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::REGION);
            for (RegionId region_id : root.get_layout_regions(layout_id))
            {
                const RegionData *region = root.get_region(region_id);
                if (!region || region->rects.empty())
                    continue;
                Shape shape;
                shape.rects = region->rects;
                auto path_outlines = compute_path_outlines(shape);
                shapes.push_back(RenderedShape{.shape = std::move(shape), .view_layer = region_view_layer, .origin = SelectionRef{region_id}, .path_outlines = std::move(path_outlines)});
            }
        }

        static std::optional<Rect> layout_die_area_bbox(const Root &root, LayoutId layout_id)
        {
            const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id));
            if (!diearea)
                return std::nullopt;
            return Geometry::bbox(*diearea);
        }

        static std::shared_ptr<const std::vector<std::vector<Polygon>>> compute_path_outlines(const Shape &shape)
        {
            std::vector<std::vector<Polygon>> outlines;
            outlines.reserve(shape.paths.size());
            for (const Path &path : shape.paths)
                outlines.push_back(Geometry::path_to_polygons(path));
            return std::make_shared<const std::vector<std::vector<Polygon>>>(std::move(outlines));
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
    };
}
