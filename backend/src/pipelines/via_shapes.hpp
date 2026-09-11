#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../view_style/view_style.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace le
{
    /// @brief Resolves `shape.vias`/`shape.via_iterates` (a LEF PIN/OBS
    /// VIA, or a DEF routed path's own VIA/VIA-array placement) into new
    /// sibling Shapes appended directly into `shapes_by_layer`, keyed by
    /// each resolved ViaLayer's own physical Layer (`purpose` - whatever
    /// the referencing Shape's own purpose was: TERMINAL/OBSTRUCTION/
    /// ROUTE/ROUTING_BLOCKAGE - resolved against that Layer). Ported from
    /// pipelines.old/stages/via_shapes.hpp (git history) - see that file's
    /// own extensive comments for the full three-tier resolution algorithm
    /// (explicit LAYER/RECT geometry; a ViaRuleReference-driven array;
    /// a bare VIARULE ... GENERATE name fit to the enclosing path's own
    /// current width) and its documented approximations/gaps (PATTERN
    /// bitmaps ignored, one scalar width fit symmetrically to both axes
    /// for tier 3). The only real change from the old version: pushes a
    /// plain `Shape` straight into the caller's own ViewLayerShapes map
    /// (`shapes_by_layer[view_layer].push_back(...)`) instead of building
    /// a `RenderedShape` with its own SelectionRef/path_outlines - the new
    /// pipeline's collect_* functions already dropped that type (see
    /// hierarchy_resolver_stage.hpp's own ViewLayerShapes comment), and a
    /// via's own synthesized geometry has no independent selection
    /// identity in either design (the Shape that referenced it, already
    /// pushed as its own Shape by the caller, remains the selectable
    /// unit).
    ///
    /// via_name resolves against the Layout's own DEF VIAS (LayoutVia,
    /// tried first when layout_id is valid - a design-scoped definition
    /// can locally override a same-named library via) falling back to the
    /// Technology's own LEF VIA (Via) either way; pass LayoutId{} from the
    /// Abstract path (no Layout context there).
    ///
    /// Takes the raw `std::unordered_map<ViewLayerId, std::vector<Shape>>`
    /// rather than hierarchy_resolver_stage.hpp's own ViewLayerShapes
    /// alias for that exact type, to avoid this header depending on that
    /// one (which itself needs to call into this one) - a plain type
    /// alias costs nothing to bypass, unlike a real circular #include.
    inline void append_via_shapes(const Root &root, const Shape &shape, ViewLayerPurpose purpose, const ViewLayerSet &view_layers, LayoutId layout_id, std::unordered_map<ViewLayerId, std::vector<Shape>> &shapes_by_layer)
    {
        if (shape.vias.empty() && shape.via_iterates.empty())
            return;

        // Bounds num_x*num_y the same way expand_iterates does for
        // RectIterate/PathIterate/PolygonIterate - defense in depth, not
        // just trusting the database's own already-validated values. Also
        // used below to clamp a fitted GENERATE row/col count.
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

        // Shared core of tiers 2 and 3 - given an already-decided rows x
        // cols grid (however each tier arrived at it) plus cut/spacing/
        // enclosure/offset parameters, synthesizes the cut grid Shape plus
        // one bottom/top metal enclosure Shape each. `array_origin` is
        // tier 2's own ORIGIN (unset for tier 3 - GENERATE has no per-use
        // origin override concept); `bot_offset`/`top_offset` are tier 2's
        // own OFFSET (also unset for tier 3).
        auto synthesize_cut_array = [&](Point placement_origin, std::optional<Orientation> orientation,
                                         int rows, int cols, double cut_w, double cut_h, double space_x, double space_y,
                                         LayerId cut_layer_id,
                                         const std::string &bot_layer_name, std::optional<Point> bot_enclosure, std::optional<Point> bot_offset,
                                         const std::string &top_layer_name, std::optional<Point> top_enclosure, std::optional<Point> top_offset,
                                         std::optional<Point> array_origin)
        {
            const Geometry::InstanceTransform transform{
                .linear = Geometry::orientation_linear(orientation.value_or(Orientation::N)),
                .translation = placement_origin,
            };

            const double origin_x = array_origin ? static_cast<double>(array_origin->x) : 0.0;
            const double origin_y = array_origin ? static_cast<double>(array_origin->y) : 0.0;
            const double total_w = cols * cut_w + (cols - 1) * space_x;
            const double total_h = rows * cut_h + (rows - 1) * space_y;
            const double start_x = -total_w / 2.0 + origin_x;
            const double start_y = -total_h / 2.0 + origin_y;

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
            shapes_by_layer[view_layers.find(cut_layer_id, purpose)].push_back(std::move(cut_shape));

            auto append_enclosure = [&](const std::string &layer_name, const std::optional<Point> &enclosure, const std::optional<Point> &layer_offset)
            {
                if (!enclosure)
                    return;
                const LayerId layer_id = root.get_layer_by_name(layer_name);
                if (!layer_id.valid())
                    return;
                const double offset_x = layer_offset ? static_cast<double>(layer_offset->x) : 0.0;
                const double offset_y = layer_offset ? static_cast<double>(layer_offset->y) : 0.0;
                const Rect enclosure_rect{
                    .ll = Point{
                        .x = static_cast<int64_t>(std::llround(start_x - static_cast<double>(enclosure->x) + offset_x)),
                        .y = static_cast<int64_t>(std::llround(start_y - static_cast<double>(enclosure->y) + offset_y))},
                    .ur = Point{
                        .x = static_cast<int64_t>(std::llround(start_x + total_w + static_cast<double>(enclosure->x) + offset_x)),
                        .y = static_cast<int64_t>(std::llround(start_y + total_h + static_cast<double>(enclosure->y) + offset_y))},
                };
                Shape metal_shape;
                metal_shape.layer = layer_id;
                metal_shape.rects.push_back(Geometry::transform_bbox(transform, enclosure_rect));
                shapes_by_layer[view_layers.find(layer_id, purpose)].push_back(std::move(metal_shape));
            };
            append_enclosure(bot_layer_name, bot_enclosure, bot_offset);
            append_enclosure(top_layer_name, top_enclosure, top_offset);
        };

        // Tier 2 - see this file's own header comment. Takes the already-
        // resolved rule directly (append_one_via's own single
        // resolve_via_rule call, not a second lookup by name here).
        auto append_via_rule_array = [&](const ViaRuleReferenceData *rule, Point origin, std::optional<Orientation> orientation)
        {
            if (!rule || !rule->cut_size)
                return;

            const int rows = rule->num_cut_rows.value_or(1);
            const int cols = rule->num_cut_cols.value_or(1);
            if (rows <= 0 || cols <= 0 || rows > kMaxReasonableCount || cols > kMaxReasonableCount)
                return;

            const LayerId cut_layer_id = root.get_layer_by_name(rule->cut_layer_name);
            if (!cut_layer_id.valid())
                return;

            synthesize_cut_array(origin, orientation, rows, cols,
                                  static_cast<double>(rule->cut_size->x), static_cast<double>(rule->cut_size->y),
                                  rule->cut_spacing ? static_cast<double>(rule->cut_spacing->x) : 0.0,
                                  rule->cut_spacing ? static_cast<double>(rule->cut_spacing->y) : 0.0,
                                  cut_layer_id,
                                  rule->bot_layer_name, rule->bot_enclosure, rule->bot_offset,
                                  rule->top_layer_name, rule->top_enclosure, rule->top_offset,
                                  rule->origin);
        };

        // Tier 3 - see this file's own header comment for the fit
        // algorithm's known approximation (one scalar width, symmetric
        // both axes).
        auto append_generate_via_array = [&](const std::string &via_name, Point origin, std::optional<Orientation> orientation, std::optional<int64_t> width)
        {
            const ViaRuleId via_rule_id = root.get_via_rule_by_name(via_name);
            if (!via_rule_id.valid())
                return;
            const ViaRuleData *rule = root.get_via_rule(via_rule_id);
            if (!rule || !rule->is_generate)
                return;

            // Exactly 2 metal layers + 1 cut layer (the 3rd) for a
            // GENERATE rule - declaration order is bottom-then-top.
            const std::vector<ViaRuleLayerId> &layer_ids = root.get_via_rule_layers(via_rule_id);
            if (layer_ids.size() != 3)
                return;
            const ViaRuleLayerData *bot_layer = root.get_via_rule_layer(layer_ids[0]);
            const ViaRuleLayerData *top_layer = root.get_via_rule_layer(layer_ids[1]);
            const ViaRuleLayerData *cut_layer = root.get_via_rule_layer(layer_ids[2]);
            if (!bot_layer || !top_layer || !cut_layer || !cut_layer->rect)
                return;

            const LayerId cut_layer_id = root.get_layer_by_name(cut_layer->layer_name);
            if (!cut_layer_id.valid())
                return;

            const double cut_w = static_cast<double>(cut_layer->rect->ur.x - cut_layer->rect->ll.x);
            const double cut_h = static_cast<double>(cut_layer->rect->ur.y - cut_layer->rect->ll.y);
            if (cut_w <= 0.0 || cut_h <= 0.0)
                return;
            const double space_x = static_cast<double>(cut_layer->spacing_step_x.value_or(0));
            const double space_y = static_cast<double>(cut_layer->spacing_step_y.value_or(0));

            // Each metal layer's own margin requirement beyond the cut
            // array's own edge - ENCLOSURE overhang1/overhang2 if given
            // (direction-aware), else a single scalar OVERHANG applied to
            // both axes, else no margin at all.
            auto layer_margin = [](const ViaRuleLayerData *layer) -> std::pair<double, double>
            {
                if (layer->enclosure_overhang1 && layer->enclosure_overhang2)
                    return {static_cast<double>(*layer->enclosure_overhang1), static_cast<double>(*layer->enclosure_overhang2)};
                if (layer->overhang)
                    return {static_cast<double>(*layer->overhang), static_cast<double>(*layer->overhang)};
                return {0.0, 0.0};
            };
            const auto [bot_margin_x, bot_margin_y] = layer_margin(bot_layer);
            const auto [top_margin_x, top_margin_y] = layer_margin(top_layer);
            // The cut array's own size determines BOTH layers' enclosing
            // rim at once, so it must satisfy whichever layer needs MORE
            // margin on a given axis, not either one alone.
            const double margin_x = std::max(bot_margin_x, top_margin_x);
            const double margin_y = std::max(bot_margin_y, top_margin_y);

            // No routing-width context available (a LEF PORT/OBS VIA, or a
            // DEF routed path segment with a genuinely zero/unset width) -
            // fall back to a single cut, the same "nothing to size an
            // array from" meaning tier 2 already uses for a
            // ViaRuleReference with no ROWCOL clause.
            const double available = width.value_or(0) > 0 ? static_cast<double>(*width) : 0.0;
            auto fit_count = [&](double cut_size, double spacing, double margin) -> int
            {
                if (available <= 0.0)
                    return 1;
                const double count = std::floor((available - 2.0 * margin + spacing) / (cut_size + spacing));
                return static_cast<int>(std::clamp(count, 1.0, static_cast<double>(kMaxReasonableCount)));
            };
            const int cols = fit_count(cut_w, space_x, margin_x);
            const int rows = fit_count(cut_h, space_y, margin_y);

            synthesize_cut_array(origin, orientation, rows, cols, cut_w, cut_h, space_x, space_y, cut_layer_id,
                                  bot_layer->layer_name, Point{.x = static_cast<int64_t>(std::llround(bot_margin_x)), .y = static_cast<int64_t>(std::llround(bot_margin_y))}, std::nullopt,
                                  top_layer->layer_name, Point{.x = static_cast<int64_t>(std::llround(top_margin_x)), .y = static_cast<int64_t>(std::llround(top_margin_y))}, std::nullopt,
                                  std::nullopt);
        };

        auto append_one_via = [&](const std::string &via_name, Point origin, std::optional<Orientation> orientation, std::optional<int64_t> width)
        {
            const std::vector<ViaLayerId> &via_layer_ids = resolve_via_layers(via_name);
            if (via_layer_ids.empty())
            {
                if (const ViaRuleReferenceData *rule = resolve_via_rule(via_name))
                    append_via_rule_array(rule, origin, orientation);
                else
                    append_generate_via_array(via_name, origin, orientation, width);
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

                shapes_by_layer[view_layers.find(layer_id, purpose)].push_back(std::move(resolved));
            }
        };

        for (const ShapeVia &via : shape.vias)
            append_one_via(via.via_name, via.origin, via.orientation, via.width);

        for (const ShapeViaIterate &via : shape.via_iterates)
        {
            if (via.num_x <= 0 || via.num_y <= 0 || via.num_x > kMaxReasonableCount || via.num_y > kMaxReasonableCount)
                continue;
            for (int ix = 0; ix < via.num_x; ix++)
                for (int iy = 0; iy < via.num_y; iy++)
                    append_one_via(via.via_name, Point{.x = via.origin.x + ix * via.space_x, .y = via.origin.y + iy * via.space_y}, via.orientation, via.width);
        }
    }
}
