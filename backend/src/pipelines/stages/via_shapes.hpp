#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include <cmath>
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
    /// Abstract path (no Layout context there). A name that resolves to a
    /// Via/LayoutVia with no explicit layers falls back to its own
    /// ViaRuleReference (LEF 5.6 VIARULE-inside-VIA / DEF VIAS VIARULE) if
    /// it has one - BUGS_AND_ENHANCEMENTS.md B3: a via *array* (LEF/DEF
    /// ROWCOL, ViaRuleReferenceData::num_cut_rows/num_cut_cols) is
    /// synthesized into a real grid of cut rects plus one bottom/top
    /// metal enclosure rect sized to the whole array (not per-cut,
    /// matching how a real multi-cut via's own metal coverage looks); a
    /// ViaRuleReference with no ROWCOL clause at all synthesizes the
    /// same way with rows=cols=1 - a single cut, LEF's own meaning for
    /// that case, previously skipped entirely along with real arrays. No
    /// ORIGIN/OFFSET/PATTERN support (ViaRuleReference's own doc comment,
    /// schema.py, for why not) - the array is always centered on the via
    /// placement point with no cut-presence gaps. A name resolving to
    /// neither explicit layers nor a ViaRuleReference, or to a pure
    /// top-level VIARULE GENERATE (real cut-array generation from a
    /// *named rule's* enclosure/spacing ranges rather than one via's own
    /// explicit CUTSIZE/CUTSPACING/ENCLOSURE - a substantially different,
    /// still-unimplemented feature, not attempted here), is skipped - no
    /// explicit geometry to draw, not an error. MASK data (ShapeVia.mask,
    /// ViaLayer.rect_masks/polygon_masks - a multi-patterning
    /// manufacturing detail) is ignored, same as everywhere else in this
    /// codebase that already reads mask data without rendering it
    /// specially.
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

        // Same via_name resolution order as resolve_via_layers above -
        // only reached once that lookup already came back empty, so a
        // LayoutVia/Via that has *both* explicit layers and its own
        // ViaRuleReference (shouldn't normally happen - a real via is one
        // or the other) still prefers the explicit ones.
        auto resolve_via_rule = [&](const std::string &via_name) -> const ViaRuleReferenceData *
        {
            if (layout_id.valid())
            {
                const LayoutViaId layout_via_id = root.get_layout_via_by_name(layout_id, via_name);
                if (layout_via_id.valid())
                {
                    const ViaRuleReferenceId ref_id = root.get_layout_via_via_rule(layout_via_id);
                    return ref_id.valid() ? root.get_via_rule_reference(ref_id) : nullptr;
                }
            }
            const ViaId via_id = root.get_via_by_name(via_name);
            if (via_id.valid())
            {
                const ViaRuleReferenceId ref_id = root.get_via_via_rule(via_id);
                return ref_id.valid() ? root.get_via_rule_reference(ref_id) : nullptr;
            }
            return nullptr;
        };

        // Synthesizes a ViaRuleReference's own cuts - a rows x cols grid
        // (ROWCOL; unset rows/cols default to 1, LEF's own meaning for a
        // VIARULE-inside-VIA reference with no ROWCOL clause at all - a
        // single cut) centered on the via placement point, plus one
        // bottom/top metal enclosure rect sized to the whole array's own
        // bbox expanded by that side's own enclosure margin. See this
        // file's own header comment for what's deliberately not modeled
        // (ORIGIN/OFFSET/PATTERN, top-level VIARULE GENERATE).
        auto append_via_rule_array = [&](const std::string &via_name, Point origin, std::optional<Orientation> orientation)
        {
            const ViaRuleReferenceData *rule = resolve_via_rule(via_name);
            if (!rule || !rule->cut_size)
                return;

            const int rows = rule->num_cut_rows.value_or(1);
            const int cols = rule->num_cut_cols.value_or(1);
            if (rows <= 0 || cols <= 0 || rows > kMaxReasonableCount || cols > kMaxReasonableCount)
                return;

            const LayerId cut_layer_id = root.get_layer_by_name(rule->cut_layer_name);
            if (!cut_layer_id.valid())
                return;

            const Geometry::InstanceTransform transform{
                .linear = Geometry::orientation_linear(orientation.value_or(Orientation::N)),
                .translation = origin,
            };

            const double cut_w = static_cast<double>(rule->cut_size->x);
            const double cut_h = static_cast<double>(rule->cut_size->y);
            const double space_x = static_cast<double>(rule->cut_spacing ? rule->cut_spacing->x : 0);
            const double space_y = static_cast<double>(rule->cut_spacing ? rule->cut_spacing->y : 0);
            const double total_w = cols * cut_w + (cols - 1) * space_x;
            const double total_h = rows * cut_h + (rows - 1) * space_y;
            const double start_x = -total_w / 2.0;
            const double start_y = -total_h / 2.0;

            Shape cut_shape;
            cut_shape.layer = cut_layer_id;
            cut_shape.rects.reserve(static_cast<size_t>(rows) * static_cast<size_t>(cols));
            for (int row = 0; row < rows; row++)
            {
                for (int col = 0; col < cols; col++)
                {
                    const double x0 = start_x + col * (cut_w + space_x);
                    const double y0 = start_y + row * (cut_h + space_y);
                    const Rect cut_rect{
                        .ll = Point{.x = static_cast<int64_t>(std::llround(x0)), .y = static_cast<int64_t>(std::llround(y0))},
                        .ur = Point{.x = static_cast<int64_t>(std::llround(x0 + cut_w)), .y = static_cast<int64_t>(std::llround(y0 + cut_h))},
                    };
                    cut_shape.rects.push_back(Geometry::transform_bbox(transform, cut_rect));
                }
            }
            out.push_back(RenderedShape{.shape = std::move(cut_shape), .view_layer = view_layers.find(cut_layer_id, purpose)});

            auto append_enclosure = [&](const std::string &layer_name, const std::optional<Point> &enclosure)
            {
                if (!enclosure)
                    return;
                const LayerId layer_id = root.get_layer_by_name(layer_name);
                if (!layer_id.valid())
                    return;
                const Rect enclosure_rect{
                    .ll = Point{
                        .x = static_cast<int64_t>(std::llround(start_x - static_cast<double>(enclosure->x))),
                        .y = static_cast<int64_t>(std::llround(start_y - static_cast<double>(enclosure->y)))},
                    .ur = Point{
                        .x = static_cast<int64_t>(std::llround(start_x + total_w + static_cast<double>(enclosure->x))),
                        .y = static_cast<int64_t>(std::llround(start_y + total_h + static_cast<double>(enclosure->y)))},
                };
                Shape metal_shape;
                metal_shape.layer = layer_id;
                metal_shape.rects.push_back(Geometry::transform_bbox(transform, enclosure_rect));
                out.push_back(RenderedShape{.shape = std::move(metal_shape), .view_layer = view_layers.find(layer_id, purpose)});
            };
            append_enclosure(rule->bot_layer_name, rule->bot_enclosure);
            append_enclosure(rule->top_layer_name, rule->top_enclosure);
        };

        auto append_one_via = [&](const std::string &via_name, Point origin, std::optional<Orientation> orientation)
        {
            const std::vector<ViaLayerId> &via_layer_ids = resolve_via_layers(via_name);
            if (via_layer_ids.empty())
            {
                append_via_rule_array(via_name, origin, orientation);
                return;
            }

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
