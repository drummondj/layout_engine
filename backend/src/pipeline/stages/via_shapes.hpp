#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include <optional>
#include <string>
#include <vector>

namespace le
{
    /// @brief Resolves `shape.vias`/`shape.via_iterates` (a LEF PIN/OBS
    /// VIA, or a DEF routed path's own VIA/VIA-array placement) into one
    /// RenderedShape per {via placement, resolved ViaLayer} pair, appended
    /// to `out` - each pushed shape reuses `purpose` (whatever the
    /// referencing Shape's own purpose was: TERMINAL/OBSTRUCTION/ROUTE/
    /// ROUTING_BLOCKAGE) resolved against that ViaLayer's OWN physical
    /// Layer, not shape.layer. A via's geometry spans several different
    /// Layers (unlike RectIterate/PathIterate/PolygonIterate, which stay
    /// within one Shape's own single Layer), so this can't expand into
    /// the referencing Shape's own rects - it synthesizes new sibling
    /// RenderedShapes instead, the same shape Track/GCellGrid/Row
    /// synthesis already takes (no ShapeId/origin of their own - not
    /// independently selectable; the Shape that referenced the via,
    /// already pushed as its own selectable RenderedShape by the caller,
    /// remains the selectable unit).
    ///
    /// via_name resolves against the Layout's own DEF VIAS (LayoutVia,
    /// tried first when layout_id is valid - a design-scoped definition
    /// can locally override a same-named library via) falling back to the
    /// Technology's own LEF VIA (Via) either way; pass LayoutId{} from the
    /// Abstract path (no Layout context there). A name that resolves to
    /// neither, or that resolves to a Via/LayoutVia with no explicit
    /// layers (e.g. a LEF 5.6 VIARULE-inside-VIA reference, or a pure
    /// VIARULE-GENERATE via - real cut-array generation from
    /// enclosure/spacing rules, a substantial separate feature, not
    /// attempted here), is skipped - no explicit geometry to draw, not an
    /// error. MASK data (ShapeVia.mask, ViaLayer.rect_masks/polygon_masks -
    /// a multi-patterning manufacturing detail) is ignored, same as
    /// everywhere else in this codebase that already reads mask data
    /// without rendering it specially.
    inline void append_via_shapes(const Root &root, const Shape &shape, ViewLayerPurpose purpose, const ViewLayerSet &view_layers, LayoutId layout_id, std::vector<RenderedShape> &out)
    {
        if (shape.vias.empty() && shape.via_iterates.empty())
            return;

        // Bounds num_x*num_y the same way generate_abstract_shapes_stage.hpp's
        // own expand_iterates does for RectIterate/PathIterate/PolygonIterate -
        // defense in depth, not just trusting the database's own already-
        // validated values.
        constexpr int kMaxReasonableCount = 1'000'000;

        auto resolve_via_layers = [&](const std::string &via_name) -> const std::vector<ViaLayerId> &
        {
            static const std::vector<ViaLayerId> empty;
            if (layout_id.valid())
            {
                const LayoutViaId layout_via_id = root.get_layout_via_by_name(layout_id, via_name);
                if (layout_via_id.valid())
                    return root.get_layout_via_layers(layout_via_id);
            }
            const ViaId via_id = root.get_via_by_name(via_name);
            if (via_id.valid())
                return root.get_via_layers(via_id);
            return empty;
        };

        auto append_one_via = [&](const std::string &via_name, Point origin, std::optional<Orientation> orientation)
        {
            const std::vector<ViaLayerId> &via_layer_ids = resolve_via_layers(via_name);
            if (via_layer_ids.empty())
                return;

            // ShapeVia.origin is already a direct placement point (unlike
            // Placement.location, which needs Geometry::instance_transform's
            // own bbox-corner derivation) - a via's own ViaLayer.rects/
            // .polygons are defined in the via's own local coordinate
            // space, so translation is just origin itself.
            const Geometry::InstanceTransform transform{
                .linear = Geometry::orientation_linear(orientation.value_or(Orientation::N)),
                .translation = origin,
            };

            for (ViaLayerId via_layer_id : via_layer_ids)
            {
                const ViaLayerData *via_layer = root.get_via_layer(via_layer_id);
                if (!via_layer)
                    continue;
                const LayerId layer_id = root.get_layer_by_name(via_layer->layer_name);
                if (!layer_id.valid())
                    continue;

                Shape resolved;
                resolved.layer = layer_id;

                resolved.rects.reserve(via_layer->rects.size());
                for (const Rect &rect : via_layer->rects)
                    resolved.rects.push_back(Geometry::transform_bbox(transform, rect));

                resolved.polygons.reserve(via_layer->polygons.size());
                for (const Polygon &polygon : via_layer->polygons)
                {
                    Polygon transformed;
                    transformed.points.reserve(polygon.points.size());
                    for (const Point &point : polygon.points)
                    {
                        const Point rotated = Geometry::apply_linear(transform.linear, point);
                        transformed.points.push_back(Point{.x = rotated.x + transform.translation.x, .y = rotated.y + transform.translation.y});
                    }
                    resolved.polygons.push_back(std::move(transformed));
                }

                out.push_back(RenderedShape{.shape = std::move(resolved), .view_layer = view_layers.find(layer_id, purpose)});
            }
        };

        for (const ShapeVia &via : shape.vias)
            append_one_via(via.via_name, via.origin, via.orientation);

        for (const ShapeViaIterate &via : shape.via_iterates)
        {
            if (via.num_x <= 0 || via.num_y <= 0 || via.num_x > kMaxReasonableCount || via.num_y > kMaxReasonableCount)
                continue;
            for (int ix = 0; ix < via.num_x; ix++)
                for (int iy = 0; iy < via.num_y; iy++)
                    append_one_via(via.via_name, Point{.x = via.origin.x + ix * via.space_x, .y = via.origin.y + iy * via.space_y}, via.orientation);
        }
    }
}
