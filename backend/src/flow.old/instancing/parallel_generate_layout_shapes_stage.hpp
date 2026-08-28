#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../core/row_geometry.hpp"
#include "../../core/shape_generation_stage.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../pipeline/stages/via_shapes.hpp"
#include "../../view_style/view_style.hpp"
#include <optional>
#include <taskflow/taskflow.hpp>
#include <tuple>
#include <vector>

namespace le::flow
{
    /// @brief A fresh, parallel-per-object-type port of the real
    /// GenerateLayoutShapesStage (src/pipeline/stages/) - read as
    /// reference, never edited (this session's own standing "don't touch
    /// pipeline/instancing/render" rule - see TASKFLOW_EXPERIMENT.md).
    /// The real stage walks 7 independent object-type sources (die area,
    /// blockages, routes, physical ports, rows, gcell grid, regions)
    /// serially into one shared vector; none of them read each other's
    /// output, so this class instead runs all 7 as dynamic Taskflow
    /// sub-tasks of the caller's own tf::Subflow, each writing into its
    /// own local vector<RenderedShape> (no shared mutable state, no lock
    /// needed), merged into one result once every task joins. TRACKS
    /// synthesis (the real stage's 8th source) is deliberately not
    /// generated at all right now - see TASKFLOW_EXPERIMENT.md: it turned
    /// out to be the actual dominant cost of the whole thing on a real
    /// design, and a user only looks at TRACK geometry zoomed in on a
    /// real routing question, not in this pipeline's bird's-eye view -
    /// real follow-up work (viewport-gated, or a cheaper synthesis),
    /// not attempted here. Reuses the real, genuinely-shared utility
    /// headers (row_geometry.hpp's row_footprint_bbox, via_shapes.hpp's
    /// append_via_shapes, Geometry::path_to_polygons) that
    /// GenerateLayoutShapesStage itself is built on - only the per-type
    /// loop bodies (private to that class, not reusable) are re-derived
    /// here. Same VersionedStage cache key shape as the real stage
    /// (`{LayoutId, ViewLayerSet::generation(), Root::mutation_version()}`)
    /// and the same ShapeGenerationStage interface, so it's a drop-in
    /// upstream for FilterByViewportAndSizeStage/FilterByLayerVisibilityStage
    /// downstream, same as the real stage would be.
    class ParallelGenerateLayoutShapesStage : public ShapeGenerationStage
    {
    public:
        const std::vector<RenderedShape> &run(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, tf::Subflow &sf)
        {
            return stage_.get(std::tuple{layout_id, view_layers.generation(), root.mutation_version()}, [&]
                               {
                std::vector<RenderedShape> diearea_shapes, blockage_shapes, route_shapes, port_shapes, row_shapes, gcellgrid_shapes, region_shapes;

                sf.emplace([&] { append_diearea_shape(root, layout_id, view_layers, diearea_shapes); }).name("gen_diearea");
                sf.emplace([&] { append_blockage_shapes(root, layout_id, view_layers, blockage_shapes); }).name("gen_blockages");
                sf.emplace([&] { append_route_shapes(root, layout_id, view_layers, route_shapes); }).name("gen_routes");
                sf.emplace([&] { append_port_shapes(root, layout_id, view_layers, port_shapes); }).name("gen_ports");
                sf.emplace([&] { append_row_shapes(root, layout_id, view_layers, row_shapes); }).name("gen_rows");
                sf.emplace([&] { append_gcell_grid_shapes(root, layout_id, view_layers, gcellgrid_shapes); }).name("gen_gcellgrid");
                sf.emplace([&] { append_region_shapes(root, layout_id, view_layers, region_shapes); }).name("gen_regions");
                sf.join();

                std::vector<RenderedShape> shapes;
                shapes.reserve(diearea_shapes.size() + blockage_shapes.size() + route_shapes.size() + port_shapes.size() + row_shapes.size() + gcellgrid_shapes.size() + region_shapes.size());
                for (std::vector<RenderedShape> *part : {&diearea_shapes, &blockage_shapes, &route_shapes, &port_shapes, &row_shapes, &gcellgrid_shapes, &region_shapes})
                    for (RenderedShape &rs : *part)
                        shapes.push_back(std::move(rs));
                return shapes; });
        }

        uint64_t version() const override { return stage_.version(); }
        uint64_t call_count() const override { return stage_.call_count(); }

    private:
        // Mirrors the real stage's own push_shape_id lambda exactly - see
        // its own comment for the resolution rules.
        static void push_shape_id(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, ShapeId shape_id, ViewLayerPurpose fallback_purpose, SelectionRef origin, std::vector<RenderedShape> &out)
        {
            const Shape *shape = root.get_shape(shape_id);
            if (!shape)
                return;
            const ViewLayerId view_layer = shape->layer.valid()
                                                ? view_layers.find(shape->layer, fallback_purpose)
                                                : (shape->purpose ? view_layers.find(LayerId{}, to_view_layer_purpose(*shape->purpose)) : ViewLayerId{});
            out.push_back(RenderedShape{.shape = *shape, .view_layer = view_layer, .origin = origin, .shape_id = shape_id, .path_outlines = compute_path_outlines(*shape)});
            append_via_shapes(root, *shape, fallback_purpose, view_layers, layout_id, out);
        }

        static void append_diearea_shape(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &out)
        {
            if (const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id)))
                out.push_back(RenderedShape{.shape = *diearea, .view_layer = view_layers.boundary_view_layer(), .path_outlines = compute_path_outlines(*diearea)});
        }

        static void append_blockage_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &out)
        {
            for (BlockageId blockage_id : root.get_layout_blockages(layout_id))
                for (ShapeId shape_id : root.get_blockage_shapes(blockage_id))
                    push_shape_id(root, layout_id, view_layers, shape_id, ViewLayerPurpose::ROUTING_BLOCKAGE, SelectionRef{blockage_id}, out);
        }

        static void append_route_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &out)
        {
            for (RouteId route_id : root.get_layout_routes(layout_id))
                for (ShapeId shape_id : root.get_route_shapes(route_id))
                    push_shape_id(root, layout_id, view_layers, shape_id, ViewLayerPurpose::ROUTE, SelectionRef{route_id}, out);
        }

        static void append_port_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &out)
        {
            for (PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
                for (PhysicalPortSegmentId segment_id : root.get_physical_port_segments(port_id))
                    for (ShapeId shape_id : root.get_physical_port_segment_shapes(segment_id))
                        push_shape_id(root, layout_id, view_layers, shape_id, ViewLayerPurpose::TERMINAL, SelectionRef{port_id}, out);
        }

        // Mirrors the real stage's own append_row_shapes, reusing the
        // same shared row_footprint_bbox (src/core/row_geometry.hpp).
        static void append_row_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &out)
        {
            const ViewLayerId row_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::ROW);
            for (RowId row_id : root.get_layout_rows(layout_id))
            {
                const std::optional<Rect> bbox = row_footprint_bbox(root, row_id);
                if (!bbox)
                    continue;

                Shape shape;
                shape.rects.push_back(*bbox);
                out.push_back(RenderedShape{.shape = shape, .view_layer = row_view_layer, .origin = SelectionRef{row_id}, .path_outlines = compute_path_outlines(shape)});
            }
        }

        static void append_gcell_grid_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &out)
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
                out.push_back(RenderedShape{.shape = std::move(lines), .view_layer = gcellgrid_view_layer, .path_outlines = std::move(path_outlines)});
            }
        }

        static void append_region_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &out)
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
                out.push_back(RenderedShape{.shape = std::move(shape), .view_layer = region_view_layer, .origin = SelectionRef{region_id}, .path_outlines = std::move(path_outlines)});
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

        VersionedStage<std::tuple<LayoutId, uint64_t, uint64_t>, std::vector<RenderedShape>> stage_;
    };
}
