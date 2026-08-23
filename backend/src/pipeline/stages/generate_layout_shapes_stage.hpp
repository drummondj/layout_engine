#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../core/shape_generation_stage.hpp"
#include "../../core/versioned_stage.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Collects every Shape from a Layout's own direct content -
    /// its die area, blockages (both kinds), routed net geometry, physical
    /// (chip-boundary) ports, and synthesized rows/tracks/gcell grids/
    /// regions - each resolved to its ViewLayerId, in dbu-space. Mirrors
    /// GenerateAbstractShapesStage's own shape (see ShapeGenerationStage's
    /// own comment for why the two share a common base rather than
    /// downstream stages needing their own Layout-specific copies).
    ///
    /// Deliberately does **not** walk `Layout.placements` - placed-instance
    /// rendering (Placement -> Design's Abstract or nested Layout, cached
    /// per-instance as an SkPicture and drawn through a per-instance
    /// transform) is a separate mechanism entirely (Migration Step 3 Phase
    /// B) - materializing per-instance Shapes into this RenderedShape map
    /// would scale with instance count (up to 1,000,000x) and defeat the
    /// whole point of picture-level caching.
    ///
    /// `LayoutVia` (named via definitions) and any `ShapeVia` reference
    /// within Route/Blockage/PhysicalPort geometry are not drawn - same as
    /// GenerateAbstractShapesStage, which doesn't draw via-cut geometry
    /// for Abstracts either; not a new gap, an existing consistent one.
    ///
    /// Key: `{LayoutId, ViewLayerSet::generation(), Root::mutation_version()}` -
    /// same reasoning as GenerateAbstractShapesStage's own key (see its
    /// own comment).
    class GenerateLayoutShapesStage : public ShapeGenerationStage
    {
    public:
        const std::vector<RenderedShape> &run(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers)
        {
            return stage_.get(std::tuple{layout_id, view_layers.generation(), root.mutation_version()}, [&]
            {
                std::vector<RenderedShape> shapes;

                auto push_shape_id = [&](ShapeId shape_id, ViewLayerPurpose fallback_purpose)
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
                    shapes.push_back(RenderedShape{.shape = *shape, .view_layer = view_layer, .shape_id = shape_id, .path_outlines = compute_path_outlines(*shape)});
                };

                if (const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id)))
                    shapes.push_back(RenderedShape{.shape = *diearea, .view_layer = view_layers.boundary_view_layer(), .path_outlines = compute_path_outlines(*diearea)});

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

                append_row_shapes(root, layout_id, view_layers, shapes);
                append_track_shapes(root, layout_id, view_layers, shapes);
                append_gcell_grid_shapes(root, layout_id, view_layers, shapes);
                append_region_shapes(root, layout_id, view_layers, shapes);

                return shapes;
            });
        }

        uint64_t version() const override { return stage_.version(); }
        uint64_t call_count() const override { return stage_.call_count(); }

    private:
        // A Row has no stored Shape of its own (see Migration Step 2's own
        // plan for why - purely parametric geometry) - synthesizes one
        // bounding Rect spanning the whole row's own footprint (site size
        // tiled num_x/num_y times at step_x/step_y, falling back to the
        // site's own size as the step when unset - the common case where
        // sites sit edge-to-edge). Skips (logs nothing - an unresolved
        // Row.site_name is common enough in synthetic/partial data to not
        // warrant a log per row) a Row whose site_name doesn't resolve, or
        // whose Site has no stored size.
        static void append_row_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers, std::vector<RenderedShape> &shapes)
        {
            const ViewLayerId row_view_layer = view_layers.find(LayerId{}, ViewLayerPurpose::ROW);
            for (RowId row_id : root.get_layout_rows(layout_id))
            {
                const RowData *row = root.get_row(row_id);
                if (!row || !row->origin)
                    continue;
                const SiteId site_id = root.get_site_by_name(row->site_name);
                const SiteData *site = site_id.valid() ? root.get_site(site_id) : nullptr;
                if (!site || !site->size)
                    continue;

                const int num_x = row->num_x.value_or(1);
                const int num_y = row->num_y.value_or(1);
                const int64_t step_x = row->step_x.value_or(site->size->x);
                const int64_t step_y = row->step_y.value_or(site->size->y);
                const int64_t width = site->size->x + static_cast<int64_t>(num_x > 0 ? num_x - 1 : 0) * step_x;
                const int64_t height = site->size->y + static_cast<int64_t>(num_y > 0 ? num_y - 1 : 0) * step_y;

                Shape shape;
                shape.rects.push_back(Rect{.ll = *row->origin, .ur = Point{.x = row->origin->x + width, .y = row->origin->y + height}});
                shapes.push_back(RenderedShape{.shape = shape, .view_layer = row_view_layer, .path_outlines = compute_path_outlines(shape)});
            }
        }

        // Track/GCellGrid likewise have no stored Shape (Step 2's own
        // plan) - each entry synthesizes `count` parallel zero-width Path
        // lines (drawn as a thin centerline stroke regardless of width -
        // see draw_group's own path-drawing code), spanning the Layout's
        // own die area bbox in the perpendicular direction. A Track's own
        // `layer_names` (unresolved strings) each contribute the same line
        // set into that real Layer's own TRACK column - not a separate
        // pseudo-row (see Migration Step 2's own plan for why).
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

        // Region already stores real Rect geometry directly (no synthesis
        // needed) - just needs wrapping into a Shape for its own
        // REGION pseudo-row.
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
                shapes.push_back(RenderedShape{.shape = std::move(shape), .view_layer = region_view_layer, .path_outlines = std::move(path_outlines)});
            }
        }

        static std::optional<Rect> layout_die_area_bbox(const Root &root, LayoutId layout_id)
        {
            const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id));
            if (!diearea)
                return std::nullopt;
            return Geometry::bbox(*diearea);
        }

        // (*RenderedShape::path_outlines)[i] == Geometry::path_to_polygons(shape.paths[i])
        // for every i - see that field's own doc comment. Mirrors
        // GenerateAbstractShapesStage's own compute_path_outlines lambda
        // exactly, applied uniformly to every RenderedShape this stage
        // constructs (not just the ones with real Path geometry - Track/
        // GCellGrid always synthesize paths, a Route/Blockage/PhysicalPort
        // Shape sometimes does) rather than leaving it at its default-
        // constructed empty vector: RenderedShape::path_outlines is
        // otherwise a safely-dereferenceable but EMPTY vector by default,
        // and transform_shapes_to_pixel_space indexes it by shape.paths'
        // own index with no bounds check - a real crash (SIGSEGV, found on
        // a real DEF file with Tracks/routed Paths) this stage was
        // shipping before every one of these call sites computed its own
        // path_outlines to match.
        static std::shared_ptr<const std::vector<std::vector<Polygon>>> compute_path_outlines(const Shape &shape)
        {
            std::vector<std::vector<Polygon>> outlines;
            outlines.reserve(shape.paths.size());
            for (const Path &path : shape.paths)
                outlines.push_back(Geometry::path_to_polygons(path));
            return std::make_shared<const std::vector<std::vector<Polygon>>>(std::move(outlines));
        }

        // ShapePurpose (schema.py, DB-persisted) and ViewLayerPurpose
        // (view_style.hpp, rendering-only) are deliberately separate,
        // parallel enums (see ShapePurpose's own schema.py comment) - this
        // is the one place a Shape's own stored purpose needs translating
        // into its rendering-layer counterpart. Only BOUNDARY/
        // PLACEMENT_BLOCKAGE exist on ShapePurpose today, so this can't
        // fail in practice, but returns BOUNDARY (not an assert) for any
        // future ShapePurpose value added without a matching case here,
        // matching this codebase's own "degrade gracefully" convention.
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
