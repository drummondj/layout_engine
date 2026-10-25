#include "def_writer.hpp"
#include "../lefdef/def/include/defwWriter.hpp"
#include "../geometry/geometry.hpp"
#include "../database/wire_helpers.hpp"
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <memory>
#include <cstdio>

// See lef_writer.cpp's own USE_LEFDEF_PARSER_NAMESPACE comment - same
// reasoning, defwWriter.hpp wraps every defw* declaration in namespace
// LefDefParser.
USE_LEFDEF_PARSER_NAMESPACE

namespace
{
    // DEF's own defw* API takes every coordinate as a raw database-unit
    // value directly (see def_writer.hpp's own class-level comment) - no
    // micron conversion anywhere, unlike LEFWriter. A few point-list
    // functions (defwNetPathPoint, defwViaPolygon, ...) take double*
    // rather than int* purely for formatting convenience; this just casts.
    double as_dbu(int64_t v) { return static_cast<double>(v); }

    // DEF's PINS DIRECTION grammar (defwPin/defwPinStr's own doc comment)
    // only lists INPUT/OUTPUT/INOUT/FEEDTHRU - no OUTPUT TRISTATE, unlike
    // LEF's PIN DIRECTION. Mirrors LEFWriter's own
    // signal_direction_to_string precedent (same "OUTPUT TRISTATE"
    // two-token spelling) since defwPin doesn't validate the string
    // against a fixed keyword set - it's just printed - so this is a
    // reasonable, low-risk choice rather than a hard requirement.
    const char *signal_direction_to_string(le::SignalDirection direction)
    {
        switch (direction)
        {
        case le::SignalDirection::INPUT:
            return "INPUT";
        case le::SignalDirection::OUTPUT:
            return "OUTPUT";
        case le::SignalDirection::INOUT:
            return "INOUT";
        case le::SignalDirection::OUTPUT_TRISTATE:
            return "OUTPUT TRISTATE";
        case le::SignalDirection::FEEDTHRU:
            return "FEEDTHRU";
        default:
            return nullptr; // NONE - DIRECTION is optional
        }
    }
}

namespace le
{
    const char *DEFWriter::placement_status_to_string(PlacementStatus status)
    {
        // le::to_string(PlacementStatus) (generated) already spells every
        // member exactly as the DEF STATUS keyword ("UNPLACED"/"PLACED"/
        // "FIXED"/"COVER"/"SOFTFIXED") - this switch just returns the same
        // spellings as literals (static storage, safe to hand back as a
        // bare const char*) instead of calling it, to avoid needing a
        // buffer whose lifetime/aliasing a caller would have to reason
        // about (two calls in the same expression - a real risk callers
        // shouldn't have to avoid by convention) - matches LEFWriter's own
        // orientation_to_string precedent.
        switch (status)
        {
        case PlacementStatus::UNPLACED:
            return "UNPLACED";
        case PlacementStatus::PLACED:
            return "PLACED";
        case PlacementStatus::FIXED:
            return "FIXED";
        case PlacementStatus::COVER:
            return "COVER";
        case PlacementStatus::SOFTFIXED:
            return "SOFTFIXED";
        }
        return "UNPLACED";
    }

    int DEFWriter::write_via_layers(const std::vector<ViaLayerData> &layers)
    {
        for (const ViaLayerData &layer : layers)
        {
            for (size_t i = 0; i < layer.rects.size(); i++)
            {
                const Rect &rect = layer.rects[i];
                const int mask = i < layer.rect_masks.size() ? layer.rect_masks[i] : 0;
                const int status = defwViaRect(layer.layer_name.c_str(),
                                                static_cast<int>(rect.ll.x), static_cast<int>(rect.ll.y),
                                                static_cast<int>(rect.ur.x), static_cast<int>(rect.ur.y), mask);
                if (status)
                    return status;
            }
            for (size_t i = 0; i < layer.polygons.size(); i++)
            {
                const Polygon &polygon = layer.polygons[i];
                const int mask = i < layer.polygon_masks.size() ? layer.polygon_masks[i] : 0;
                std::vector<double> xs, ys;
                xs.reserve(polygon.points.size());
                ys.reserve(polygon.points.size());
                for (const Point &point : polygon.points)
                {
                    xs.push_back(as_dbu(point.x));
                    ys.push_back(as_dbu(point.y));
                }
                const int status = defwViaPolygon(layer.layer_name.c_str(), static_cast<int>(xs.size()), xs.data(), ys.data(), mask);
                if (status)
                    return status;
            }
        }
        return 0;
    }

    int DEFWriter::write_die_area(const Root &root, LayoutId layout_id)
    {
        const ShapeId shape_id = root.get_layout_diearea(layout_id);
        const ShapeData *shape = root.get_shape(shape_id);
        if (!shape || shape->polygons.empty())
            return 0;

        const Polygon &polygon = shape->polygons.front();
        if (polygon.points.size() == 2)
        {
            const Point &p1 = polygon.points[0];
            const Point &p2 = polygon.points[1];
            return defwDieArea(static_cast<int>(p1.x), static_cast<int>(p1.y), static_cast<int>(p2.x), static_cast<int>(p2.y));
        }

        std::vector<int> xs, ys;
        xs.reserve(polygon.points.size());
        ys.reserve(polygon.points.size());
        for (const Point &point : polygon.points)
        {
            xs.push_back(static_cast<int>(point.x));
            ys.push_back(static_cast<int>(point.y));
        }
        return defwDieAreaList(static_cast<int>(xs.size()), xs.data(), ys.data());
    }

    int DEFWriter::write_rows(const Root &root, LayoutId layout_id)
    {
        for (RowId row_id : root.get_layout_rows(layout_id))
        {
            const RowData *row = root.get_row(row_id);
            if (!row)
                continue;

            const Point origin = row->origin.value_or(Point{});
            const int status = defwRowStr(row->name.c_str(), row->site_name.c_str(),
                                           static_cast<int>(origin.x), static_cast<int>(origin.y),
                                           le::to_string(row->orientation).c_str(),
                                           row->num_x.value_or(0), row->num_y.value_or(0),
                                           static_cast<int>(row->step_x.value_or(0)), static_cast<int>(row->step_y.value_or(0)));
            if (status)
                return status;
        }
        return 0;
    }

    int DEFWriter::write_tracks(const Root &root, LayoutId layout_id)
    {
        for (TrackId track_id : root.get_layout_tracks(layout_id))
        {
            const TrackData *track = root.get_track(track_id);
            if (!track)
                continue;

            // KNOWN VENDORED-WRITER GAP (see plans/LEFDEF_BUGS.md): DEF's own
            // TRACKS grammar has an optional LAYER clause (confirmed
            // against complete.5.8.def itself, e.g. "TRACKS Y 52 DO 857
            // STEP 104 MASK 1 ;" with no LAYER at all), but
            // defwTracks(...) unconditionally writes the literal token
            // "LAYER" regardless of num_layers (confirmed in
            // defwWriter.cpp - no num_layers==0 guard exists), producing
            // an empty, unparseable "... LAYER ;" for a Track with no
            // layers. No alternate API exists. Skipped entirely (not
            // written as broken output) - same "vendored writer literally
            // cannot produce valid output here" treatment
            // LEFWriter::write_via_rules gives its own non-GENERATE
            // VIARULE dead end, for the same reason (an unparseable
            // statement here would silently truncate everything written
            // after it too, not just lose this one Track).
            if (track->layer_names.empty())
                continue;

            std::vector<const char *> layer_ptrs;
            layer_ptrs.reserve(track->layer_names.size());
            for (const std::string &name : track->layer_names)
                layer_ptrs.push_back(name.c_str());

            const int status = defwTracks(track->is_x ? "X" : "Y",
                                           static_cast<int>(track->start), track->count, static_cast<int>(track->step),
                                           static_cast<int>(layer_ptrs.size()), layer_ptrs.data(),
                                           track->mask.value_or(0), track->same_mask ? 1 : 0);
            if (status)
                return status;
        }
        return 0;
    }

    int DEFWriter::write_gcell_grids(const Root &root, LayoutId layout_id)
    {
        for (GCellGridId grid_id : root.get_layout_gcell_grids(layout_id))
        {
            const GCellGridData *grid = root.get_g_cell_grid(grid_id);
            if (!grid)
                continue;

            const int status = defwGcellGrid(grid->is_x ? "X" : "Y", static_cast<int>(grid->start), grid->count, static_cast<int>(grid->step));
            if (status)
                return status;
        }
        return 0;
    }

    int DEFWriter::write_vias(const Root &root, LayoutId layout_id)
    {
        const auto &via_ids = root.get_layout_vias(layout_id);
        if (via_ids.empty())
            return 0;

        int status = defwStartVias(static_cast<int>(via_ids.size()));
        if (status)
            return status;

        for (LayoutViaId via_id : via_ids)
        {
            const LayoutViaData *via = root.get_layout_via(via_id);
            if (!via)
                continue;

            status = defwViaName(via->name.c_str());
            if (status)
                return status;

            // LayoutVia.foreign (populated only via the multi-parent
            // Foreign klass's own reuse) has no write site here - DEF's
            // own VIAS section grammar has no FOREIGN clause at all
            // (confirmed against defwWriter.hpp: no defwViaForeign* exists,
            // unlike LEF's VIA), and DEFReader itself never populates it
            // either - not a vendored-writer gap, just DEF format not
            // having this LEF-only construct.

            if (const ViaRuleReferenceData *vr = root.get_via_rule_reference(root.get_layout_via_via_rule(via_id)))
            {
                const Point cut_size = vr->cut_size.value_or(Point{});
                const Point cut_spacing = vr->cut_spacing.value_or(Point{});
                const Point bot_enclosure = vr->bot_enclosure.value_or(Point{});
                const Point top_enclosure = vr->top_enclosure.value_or(Point{});
                status = defwViaViarule(vr->via_rule_name.c_str(), as_dbu(cut_size.x), as_dbu(cut_size.y),
                                         vr->bot_layer_name.c_str(), vr->cut_layer_name.c_str(), vr->top_layer_name.c_str(),
                                         as_dbu(cut_spacing.x), as_dbu(cut_spacing.y),
                                         as_dbu(bot_enclosure.x), as_dbu(bot_enclosure.y),
                                         as_dbu(top_enclosure.x), as_dbu(top_enclosure.y));
                if (status)
                    return status;

                // ROWCOL/ORIGIN/OFFSET must be written, or a via array
                // collapses to a single cut on read-back (a
                // ViaRuleReference with no ROWCOL means exactly that - see
                // its own schema.py doc comment). defwViaViaruleRowCol/
                // Origin/Offset can each only be called once, immediately
                // after defwViaViarule (see their own header comments) -
                // same shape as LEFWriter::write_vias (lef_writer.cpp).
                if (vr->num_cut_rows.has_value() && vr->num_cut_cols.has_value())
                {
                    status = defwViaViaruleRowCol(*vr->num_cut_rows, *vr->num_cut_cols);
                    if (status)
                        return status;
                }
                if (vr->origin.has_value())
                {
                    status = defwViaViaruleOrigin(static_cast<int>(vr->origin->x), static_cast<int>(vr->origin->y));
                    if (status)
                        return status;
                }
                if (vr->bot_offset.has_value() || vr->top_offset.has_value())
                {
                    const Point bot_offset = vr->bot_offset.value_or(Point{});
                    const Point top_offset = vr->top_offset.value_or(Point{});
                    status = defwViaViaruleOffset(static_cast<int>(bot_offset.x), static_cast<int>(bot_offset.y),
                                                   static_cast<int>(top_offset.x), static_cast<int>(top_offset.y));
                    if (status)
                        return status;
                }
            }

            std::vector<ViaLayerData> layers;
            for (ViaLayerId layer_id : root.get_layout_via_layers(via_id))
                if (const ViaLayerData *data = root.get_via_layer(layer_id))
                    layers.push_back(*data);
            status = write_via_layers(layers);
            if (status)
                return status;

            status = defwOneViaEnd();
            if (status)
                return status;
        }

        return defwEndVias();
    }

    int DEFWriter::write_non_default_rules(const Root &root, TechnologyId technology_id)
    {
        const auto &rule_ids = root.get_technology_non_default_rules(technology_id);
        if (rule_ids.empty())
            return 0;

        // KNOWN VENDORED-WRITER GAP (see plans/LEFDEF_BUGS.md): unlike every
        // other DEF geometry statement (raw database-unit integers - see
        // def_writer.hpp's own class comment), NONDEFAULTRULES LAYER
        // WIDTH/DIAGWIDTH/SPACING/WIREEXT are written as real MICRON
        // decimal values (e.g. "WIDTH 10.1", confirmed against
        // complete.5.8.def itself - matches DEFReader::defrNonDefaultCbkFn's
        // own comment: "WIDTH/SPACING/WIREEXT/DIAGWIDTH are written in
        // real microns"). But defwNonDefaultRuleLayer's own width/
        // diagWidth/spacing/wireExt params are plain `int`, printed with
        // a bare "%d" (confirmed in defwWriter.cpp - no decimal point,
        // no unit-scale awareness at all) - there is no way to write a
        // fractional micron value through this function. Converting back
        // to a truncated micron int (dividing by dbu_per_micron) is the
        // closest available approximation - it loses sub-micron
        // precision (10.1 microns round-trips as a bare "10") but is at
        // least in the right unit, unlike passing the raw dbu value
        // through unconverted (which would print as "10100").
        const TechnologyData *technology = root.get_technology(technology_id);
        const double dbu_per_micron = (technology && technology->database_units_microns != 0) ? technology->database_units_microns : 1.0;
        auto to_micron_int = [&](const std::optional<int64_t> &v) -> int
        { return v ? static_cast<int>(static_cast<double>(*v) / dbu_per_micron) : 0; };

        int status = defwStartNonDefaultRules(static_cast<int>(rule_ids.size()));
        if (status)
            return status;

        for (NonDefaultRuleId rule_id : rule_ids)
        {
            const NonDefaultRuleData *rule = root.get_non_default_rule(rule_id);
            if (!rule)
                continue;

            status = defwNonDefaultRule(rule->name.c_str(), rule->hard_spacing ? 1 : 0);
            if (status)
                return status;

            for (NonDefaultRuleLayerId layer_id : root.get_non_default_rule_layers(rule_id))
            {
                const NonDefaultRuleLayerData *layer = root.get_non_default_rule_layer(layer_id);
                if (!layer)
                    continue;
                status = defwNonDefaultRuleLayer(layer->layer_name.c_str(),
                                                  to_micron_int(layer->width),
                                                  to_micron_int(layer->diag_width),
                                                  to_micron_int(layer->spacing),
                                                  to_micron_int(layer->wire_extension));
                if (status)
                    return status;
            }

            // Unlike LEF's own inline NONDEFAULTRULE VIA (a full embedded
            // via definition, NonDefaultRuleVia), DEF's own NONDEFAULTRULES
            // VIA is just a plain name reference (confirmed against
            // defwWriter.hpp: no defwNonDefaultRuleStartVia/EndVia exists,
            // unlike LEF's lefwNonDefaultRuleStartVia) - matches
            // DEFReader::defrNonDefaultCbkFn, which populates
            // use_via_names for this, not non_default_rule_vias (that
            // list is LEF-only).
            for (const std::string &via_name : rule->use_via_names)
            {
                status = defwNonDefaultRuleVia(via_name.c_str());
                if (status)
                    return status;
            }

            for (const std::string &via_rule_name : rule->use_via_rule_names)
            {
                status = defwNonDefaultRuleViaRule(via_rule_name.c_str());
                if (status)
                    return status;
            }

            for (const MinCutOverride &min_cut : rule->min_cuts)
            {
                status = defwNonDefaultRuleMinCuts(min_cut.cut_layer_name.c_str(), min_cut.num_cuts);
                if (status)
                    return status;
            }
        }

        return defwEndNonDefaultRules();
    }

    int DEFWriter::write_regions(const Root &root, LayoutId layout_id)
    {
        const auto &region_ids = root.get_layout_regions(layout_id);
        if (region_ids.empty())
            return 0;

        int status = defwStartRegions(static_cast<int>(region_ids.size()));
        if (status)
            return status;

        for (RegionId region_id : region_ids)
        {
            const RegionData *region = root.get_region(region_id);
            if (!region)
                continue;

            status = defwRegionName(region->name.c_str());
            if (status)
                return status;

            for (const Rect &rect : region->rects)
            {
                status = defwRegionPoints(static_cast<int>(rect.ll.x), static_cast<int>(rect.ll.y), static_cast<int>(rect.ur.x), static_cast<int>(rect.ur.y));
                if (status)
                    return status;
            }

            if (region->region_type)
            {
                status = defwRegionType(region->region_type->c_str());
                if (status)
                    return status;
            }
        }

        return defwEndRegions();
    }

    int DEFWriter::write_placements(const Root &root, LayoutId layout_id)
    {
        const auto &placement_ids = root.get_layout_placements(layout_id);
        if (placement_ids.empty())
            return 0;

        int status = defwStartComponents(static_cast<int>(placement_ids.size()));
        if (status)
            return status;

        for (PlacementId placement_id : placement_ids)
        {
            const PlacementData *placement = root.get_placement(placement_id);
            if (!placement)
                continue;

            const DesignData *reference = root.get_design(placement->reference_design);
            const std::string master = reference ? reference->name : std::string{};

            const bool is_placed = placement->location.has_value();
            const Point location = placement->location.value_or(Point{});
            const std::string orient = placement->orientation ? le::to_string(*placement->orientation) : std::string{};

            status = defwComponentStr(placement->name.c_str(), master.c_str(),
                                       0, nullptr, // netNames
                                       nullptr, nullptr, nullptr, // eeq, genName, genParameters
                                       placement->source ? placement->source->c_str() : nullptr,
                                       0, nullptr, nullptr, nullptr, nullptr, // foreigns
                                       is_placed ? placement_status_to_string(placement->placement_status) : nullptr,
                                       static_cast<int>(location.x), static_cast<int>(location.y),
                                       is_placed && !orient.empty() ? orient.c_str() : nullptr,
                                       placement->weight.value_or(0.0),
                                       nullptr, 0, 0, 0, 0); // region
            if (status)
                return status;
        }

        return defwEndComponents();
    }

    int DEFWriter::write_physical_ports(const Root &root, LayoutId layout_id)
    {
        const auto &port_ids = root.get_layout_physical_ports(layout_id);
        if (port_ids.empty())
            return 0;

        int status = defwStartPins(static_cast<int>(port_ids.size()));
        if (status)
            return status;

        for (PhysicalPortId port_id : port_ids)
        {
            const PhysicalPortData *port = root.get_physical_port(port_id);
            if (!port)
                continue;

            const bool is_placed = port->location.has_value();
            const Point location = port->location.value_or(Point{});
            const std::string orient = port->orientation ? le::to_string(*port->orientation) : std::string{};

            status = defwPinStr(port->name.c_str(), port->net_name ? port->net_name->c_str() : "",
                                 0, // special
                                 signal_direction_to_string(port->direction.value_or(SignalDirection::NONE)),
                                 port->use ? port->use->c_str() : nullptr,
                                 is_placed && port->placement_status ? placement_status_to_string(*port->placement_status) : nullptr,
                                 static_cast<int>(location.x), static_cast<int>(location.y),
                                 is_placed && !orient.empty() ? orient.c_str() : nullptr,
                                 nullptr, 0, 0, 0, 0); // plain (non-PORT) layer geometry - written per-segment below instead
            if (status)
                return status;

            const auto &segment_ids = root.get_physical_port_segments(port_id);
            for (PhysicalPortSegmentId segment_id : segment_ids)
            {
                const PhysicalPortSegmentData *segment = root.get_physical_port_segment(segment_id);
                if (!segment)
                    continue;

                // Only the real 5.7+ multi-PORT case writes a PORT
                // wrapper - a simple pre-5.7 pin gets one synthetic
                // segment with no placement_status of its own (see
                // PhysicalPortSegment's own schema comment), whose shapes
                // are written as plain top-level LAYER statements
                // instead, matching how DEFReader itself only sets
                // placement_status when a real PORT wrapper was present.
                const bool has_port_wrapper = segment->placement_status.has_value() || segment_ids.size() > 1;
                if (has_port_wrapper)
                {
                    status = defwPinPort();
                    if (status)
                        return status;
                }

                // LAYER/geometry before FIXED|COVER|PLACED, matching
                // complete.5.8.def's own real PORT ordering (e.g. PIN P0)
                // - DEF's own pin_port grammar is order-sensitive here,
                // unlike some other sections (the reverse order re-parses
                // with a syntax error at the next LAYER token).
                // Pin shapes are stored in design coordinates (DEFReader
                // converts on read); DEF wants them relative to the PORT's placement, or
                // the PIN's for a simple pin - the inverse of DEFReader's.
                const Geometry::InstanceTransform to_pin = Geometry::invert(
                    has_port_wrapper ? Geometry::pin_transform(segment->location, segment->orientation)
                                     : Geometry::pin_transform(port->location, port->orientation));
                for (ShapeId shape_id : root.get_physical_port_segment_shapes(segment_id))
                {
                    const ShapeData *stored = root.get_shape(shape_id);
                    if (!stored)
                        continue;
                    const ShapeData local = Geometry::transform(*stored, to_pin);
                    const ShapeData *shape = &local;
                    const LayerData *layer = root.get_layer(shape->layer);
                    const std::string layer_name = layer ? layer->name : std::string{};

                    for (size_t i = 0; i < shape->rects.size(); i++)
                    {
                        const Rect &rect = shape->rects[i];
                        const int mask = i < shape->rect_masks.size() ? shape->rect_masks[i] : 0;
                        status = has_port_wrapper
                                     ? defwPinPortLayer(layer_name.c_str(), static_cast<int>(shape->spacing.value_or(0)), static_cast<int>(shape->design_rule_width.value_or(0)),
                                                         static_cast<int>(rect.ll.x), static_cast<int>(rect.ll.y), static_cast<int>(rect.ur.x), static_cast<int>(rect.ur.y), mask)
                                     : defwPinLayer(layer_name.c_str(), static_cast<int>(shape->spacing.value_or(0)), static_cast<int>(shape->design_rule_width.value_or(0)),
                                                     static_cast<int>(rect.ll.x), static_cast<int>(rect.ll.y), static_cast<int>(rect.ur.x), static_cast<int>(rect.ur.y), mask);
                        if (status)
                            return status;
                    }

                    for (size_t i = 0; i < shape->polygons.size(); i++)
                    {
                        const Polygon &polygon = shape->polygons[i];
                        const int mask = i < shape->polygon_masks.size() ? shape->polygon_masks[i] : 0;
                        std::vector<double> xs, ys;
                        xs.reserve(polygon.points.size());
                        ys.reserve(polygon.points.size());
                        for (const Point &point : polygon.points)
                        {
                            xs.push_back(as_dbu(point.x));
                            ys.push_back(as_dbu(point.y));
                        }
                        status = has_port_wrapper
                                     ? defwPinPortPolygon(layer_name.c_str(), static_cast<int>(shape->spacing.value_or(0)), static_cast<int>(shape->design_rule_width.value_or(0)),
                                                           static_cast<int>(xs.size()), xs.data(), ys.data(), mask)
                                     : defwPinPolygon(layer_name.c_str(), static_cast<int>(shape->spacing.value_or(0)), static_cast<int>(shape->design_rule_width.value_or(0)),
                                                       static_cast<int>(xs.size()), xs.data(), ys.data(), mask);
                        if (status)
                            return status;
                    }

                    for (const ShapeVia &via : shape->vias)
                    {
                        status = has_port_wrapper
                                     ? defwPinPortVia(via.via_name.c_str(), static_cast<int>(via.origin.x), static_cast<int>(via.origin.y), via.mask.value_or(0))
                                     : defwPinVia(via.via_name.c_str(), static_cast<int>(via.origin.x), static_cast<int>(via.origin.y), via.mask.value_or(0));
                        if (status)
                            return status;
                    }
                }

                if (has_port_wrapper && segment->placement_status)
                {
                    const Point seg_location = segment->location.value_or(Point{});
                    const std::string seg_orient = segment->orientation ? le::to_string(*segment->orientation) : std::string{};
                    status = defwPinPortLocation(placement_status_to_string(*segment->placement_status),
                                                  static_cast<int>(seg_location.x), static_cast<int>(seg_location.y),
                                                  seg_orient.empty() ? nullptr : seg_orient.c_str());
                    if (status)
                        return status;
                }
            }
        }

        return defwEndPins();
    }

    int DEFWriter::write_blockages(const Root &root, LayoutId layout_id)
    {
        const auto &blockage_ids = root.get_layout_blockages(layout_id);
        if (blockage_ids.empty())
            return 0;

        int status = defwStartBlockages(static_cast<int>(blockage_ids.size()));
        if (status)
            return status;

        for (BlockageId blockage_id : blockage_ids)
        {
            const BlockageData *blockage = root.get_blockage(blockage_id);
            if (!blockage)
                continue;

            const PlacementData *scoped_placement = blockage->placement.valid() ? root.get_placement(blockage->placement) : nullptr;

            if (blockage->kind == BlockageKind::ROUTING)
            {
                status = defwBlockagesLayer((blockage->layer_name ? *blockage->layer_name : std::string{}).c_str());
                if (status)
                    return status;
                if (scoped_placement)
                {
                    status = defwBlockagesLayerComponent(scoped_placement->name.c_str());
                    if (status)
                        return status;
                }
                if (blockage->spacing)
                {
                    status = defwBlockagesLayerSpacing(static_cast<int>(*blockage->spacing));
                    if (status)
                        return status;
                }
                else if (blockage->design_rule_width)
                {
                    status = defwBlockagesLayerDesignRuleWidth(static_cast<int>(*blockage->design_rule_width));
                    if (status)
                        return status;
                }
            }
            else
            {
                status = defwBlockagesPlacement();
                if (status)
                    return status;
                if (scoped_placement)
                {
                    status = defwBlockagesPlacementComponent(scoped_placement->name.c_str());
                    if (status)
                        return status;
                }
                if (blockage->is_soft)
                {
                    status = defwBlockagesPlacementSoft();
                    if (status)
                        return status;
                }
                else if (blockage->placement_max_density)
                {
                    status = defwBlockagesPlacementPartial(*blockage->placement_max_density);
                    if (status)
                        return status;
                }
            }

            for (ShapeId shape_id : root.get_blockage_shapes(blockage_id))
            {
                const ShapeData *shape = root.get_shape(shape_id);
                if (!shape)
                    continue;
                for (const Rect &rect : shape->rects)
                {
                    status = defwBlockagesRect(static_cast<int>(rect.ll.x), static_cast<int>(rect.ll.y), static_cast<int>(rect.ur.x), static_cast<int>(rect.ur.y));
                    if (status)
                        return status;
                }
                for (const Polygon &polygon : shape->polygons)
                {
                    std::vector<int> xs, ys;
                    xs.reserve(polygon.points.size());
                    ys.reserve(polygon.points.size());
                    for (const Point &point : polygon.points)
                    {
                        xs.push_back(static_cast<int>(point.x));
                        ys.push_back(static_cast<int>(point.y));
                    }
                    status = defwBlockagesPolygon(static_cast<int>(xs.size()), xs.data(), ys.data());
                    if (status)
                        return status;
                }
            }
        }

        return defwEndBlockages();
    }

    int DEFWriter::write_net_path(FILE *file, const Root &root, RouteId route_id, bool is_special)
    {
        // One wiring statement ("+ FIXED ... NEW ...") per routing status
        // and shielded net; within it each path and via of each Wire or
        // Shape is its own LAYER occurrence, DEF's NEW.
        // Unlike LEF's own LAYER-repetition-within-one-OBS, DEF's writer
        // state machine requires a fresh *PathStart("NEW") before every
        // occurrence after the first - *PathLayer only accepts state
        // DEFW_PATH_START, which only *PathStart sets.
        //
        // DEF's writer API has two entirely separate, same-shaped
        // function families for this - defwNetPath*/defwSpecialNetPath* -
        // not one shared family the way defwBlockage*/defwPin* are reused
        // across kinds elsewhere (defwSpecialNetPathLayer also has no
        // isTaper/ruleName params, unlike defwNetPathLayer).
        const RouteData *route = root.get_route(route_id);
        if (!route)
            return 0;

        // A Wire is written as the general Shape it converts to.
        std::vector<ShapeData> wire_shapes;
        for (WireId wire_id : root.get_route_wires(route_id))
            if (const WireData *wire = root.get_wire(wire_id))
                wire_shapes.push_back(wire_to_shape(root, *wire));
        std::vector<const ShapeData *> shapes;
        for (const ShapeData &shape : wire_shapes)
            shapes.push_back(&shape);
        for (ShapeId shape_id : root.get_route_shapes(route_id))
            if (const ShapeData *shape = root.get_shape(shape_id))
                shapes.push_back(shape);

        // The status keyword a statement starts with, and the net a SHIELD
        // statement shields (null otherwise). SHIELD is SPECIALNETS-only
        // and NOSHIELD NETS-only; either one out of place, or a SHIELD
        // whose net is unknown, is written as ROUTED.
        struct Wiring
        {
            std::string status;
            const std::string *shield = nullptr;

            bool operator==(const Wiring &other) const
            {
                return status == other.status && (shield == other.shield || (shield && other.shield && *shield == *other.shield));
            }
        };
        auto wiring_of = [&](const ShapeData &shape) -> Wiring
        {
            const RoutingStatus status = shape.routing_status.value_or(RoutingStatus::ROUTED);
            if (status == RoutingStatus::SHIELD)
            {
                if (is_special && shape.shield_index && *shape.shield_index < route->shield_nets.size())
                    return Wiring{.status = "SHIELD", .shield = &route->shield_nets[*shape.shield_index]};
                return Wiring{.status = "ROUTED"};
            }
            if (status == RoutingStatus::NOSHIELD && is_special)
                return Wiring{.status = "ROUTED"};
            return Wiring{.status = le::to_string(status)};
        };

        // A special-net path via: "name [orient]". DEF allows the
        // orientation (and a DO ... BY ... STEP array after it), but
        // defwSpecialNetPathVia has no orientation form, so it's printed
        // here, into the same FILE, between that call and
        // defwSpecialNetPathViaData.
        auto special_path_via = [&](const std::string &name, const std::optional<Orientation> &orientation) -> int
        {
            const int status = defwSpecialNetPathVia(name.c_str());
            if (status == 0 && orientation)
                std::fprintf(file, " %s", le::to_string(*orientation).c_str());
            return status;
        };

        std::vector<std::pair<Wiring, std::vector<const ShapeData *>>> statements;
        for (const ShapeData *shape : shapes)
        {
            const Wiring wiring = wiring_of(*shape);
            auto it = std::ranges::find_if(statements, [&](const auto &statement)
                                           { return statement.first == wiring; });
            if (it == statements.end())
                it = statements.insert(statements.end(), {wiring, {}});
            it->second.push_back(shape);
        }

        for (const auto &[wiring, statement_shapes] : statements)
        {
            bool started = false;
            int status = 0;
            // Starts the next LAYER occurrence: the statement itself, or a
            // NEW within it.
            auto start = [&](const std::string &layer_name, const ShapeData &shape) -> int
            {
                if (is_special)
                {
                    if ((status = defwSpecialNetPathStart(started ? "NEW" : wiring.status.c_str())))
                        return status;
                    if (!started && wiring.shield && (status = defwSpecialNetShieldNetName(wiring.shield->c_str())))
                        return status;
                }
                else if ((status = defwNetPathStart(started ? "NEW" : wiring.status.c_str())))
                    return status;
                started = true;
                return is_special ? defwSpecialNetPathLayer(layer_name.c_str()) : defwNetPathLayer(layer_name.c_str(), 0, nullptr);
            };
            // SPECIALNETS' WIDTH and SHAPE, after the LAYER. Regular NETS'
            // inline path WIDTH is DEF >= 6.0 syntax (this writer emits
            // 5.8), and regular wiring has no SHAPE.
            auto special_width_and_shape = [&](int64_t width, const ShapeData &shape) -> int
            {
                if (!is_special)
                    return 0;
                if ((status = defwSpecialNetPathWidth(static_cast<int>(width))))
                    return status;
                if (shape.route_shape)
                    return defwSpecialNetPathShape(le::to_string(*shape.route_shape).c_str());
                return 0;
            };

            for (const ShapeData *shape : statement_shapes)
            {
                const LayerData *layer = root.get_layer(shape->layer);
                const std::string layer_name = layer ? layer->name : std::string{};

                for (const Path &path : shape->paths)
                {
                    if ((status = start(layer_name, *shape)) || (status = special_width_and_shape(path.width, *shape)))
                        return status;

                    std::vector<double> xs, ys;
                    xs.reserve(path.polygon.points.size());
                    ys.reserve(path.polygon.points.size());
                    for (const Point &point : path.polygon.points)
                    {
                        xs.push_back(as_dbu(point.x));
                        ys.push_back(as_dbu(point.y));
                    }
                    status = is_special ? defwSpecialNetPathPoint(static_cast<int>(xs.size()), xs.data(), ys.data())
                                        : defwNetPathPoint(static_cast<int>(xs.size()), xs.data(), ys.data());
                    if (status)
                        return status;
                }

                // Every via/via_iterate gets its own isolated single-point
                // NEW segment (a degenerate "+ NEW layer (x y) via_name"
                // path - valid DEF: def.y's `path` rule requires only one
                // point before an optional via/point item list). Vias must
                // not share one segment: DEF treats every *consecutive*
                // point within one path/NEW segment as wire-connected, so
                // several unrelated vias (merged into this one Shape only
                // because they sit on the same layer) would gain phantom,
                // usually diagonal, wires between their origins.
                //
                // *PathStart resets the vendored writer's
                // defwLineItemCounter to 0 on every call, which starves its
                // periodic-newline heuristic; a power-strap SPECIALNET with
                // thousands of via taps would become one physical line long
                // enough to overflow the vendored reader's fixed-size line
                // buffer. So `defwNewLine()` (a raw fprintf, independent of
                // defwLineItemCounter) is called directly after every via.
                for (const ShapeVia &via : shape->vias)
                {
                    if ((status = start(layer_name, *shape)) || (status = special_width_and_shape(via.width.value_or(0), *shape)))
                        return status;

                    double x = as_dbu(via.origin.x);
                    double y = as_dbu(via.origin.y);
                    status = is_special ? defwSpecialNetPathPoint(1, &x, &y) : defwNetPathPoint(1, &x, &y);
                    if (status)
                        return status;

                    if (is_special)
                        status = special_path_via(via.via_name, via.orientation);
                    else
                        status = via.orientation
                                     ? defwNetPathViaWithOrientStr(via.via_name.c_str(), le::to_string(*via.orientation).c_str())
                                     : defwNetPathVia(via.via_name.c_str());
                    if (status)
                        return status;

                    defwNewLine();
                }

                // ShapeViaIterate (an arrayed VIA placement, "VIA DO n BY m
                // STEP x y", within a routed path). The vendored writer has
                // no regular-NETS form (defwSpecialNetPathViaData only), so
                // a regular net's via array is skipped. Same isolated
                // segment per entry as ShapeVia above, for the same reason.
                for (const ShapeViaIterate &via_iterate : shape->via_iterates)
                {
                    if (!is_special)
                        continue;
                    if ((status = start(layer_name, *shape)) ||
                        (status = special_width_and_shape(via_iterate.width.value_or(0), *shape)))
                        return status;

                    double x = as_dbu(via_iterate.origin.x);
                    double y = as_dbu(via_iterate.origin.y);
                    if ((status = defwSpecialNetPathPoint(1, &x, &y)))
                        return status;
                    if ((status = special_path_via(via_iterate.via_name, via_iterate.orientation)))
                        return status;
                    status = defwSpecialNetPathViaData(via_iterate.num_x, via_iterate.num_y,
                                                        static_cast<int>(as_dbu(via_iterate.space_x)), static_cast<int>(as_dbu(via_iterate.space_y)));
                    if (status)
                        return status;

                    defwNewLine();
                }
            }

            if (started && (status = is_special ? defwSpecialNetPathEnd() : defwNetPathEnd()))
                return status;
        }

        if (!is_special)
            return 0;

        // SPECIALNETS "+ RECT" and "+ POLYGON" statements (its "+ VIA"
        // statements are written as path vias, above).
        // defwSpecialNetRect/Polygon print only their own keyword, with no
        // call for a status, shield or SHAPE before it, so that prefix is
        // printed here, into the same FILE.
        for (const ShapeData *shape : shapes)
        {
            const LayerData *layer = root.get_layer(shape->layer);
            const std::string layer_name = layer ? layer->name : std::string{};
            const Wiring wiring = wiring_of(*shape);
            auto prefix = [&](int mask) -> int
            {
                if (shape->routing_status || wiring.shield)
                    std::fprintf(file, "\n      + %s%s%s", wiring.status.c_str(), wiring.shield ? " " : "",
                                 wiring.shield ? wiring.shield->c_str() : "");
                if (shape->route_shape)
                    std::fprintf(file, "\n      + SHAPE %s", le::to_string(*shape->route_shape).c_str());
                return mask != 0 ? defwSpecialNetMask(mask) : 0;
            };
            int status = 0;
            for (std::size_t i = 0; i < shape->rects.size(); ++i)
            {
                const Rect &rect = shape->rects[i];
                if ((status = prefix(i < shape->rect_masks.size() ? shape->rect_masks[i] : 0)) ||
                    (status = defwSpecialNetRect(layer_name.c_str(), static_cast<int>(as_dbu(rect.ll.x)), static_cast<int>(as_dbu(rect.ll.y)),
                                                 static_cast<int>(as_dbu(rect.ur.x)), static_cast<int>(as_dbu(rect.ur.y)))))
                    return status;
            }
            for (std::size_t i = 0; i < shape->polygons.size(); ++i)
            {
                std::vector<double> xs, ys;
                for (const Point &point : shape->polygons[i].points)
                {
                    xs.push_back(as_dbu(point.x));
                    ys.push_back(as_dbu(point.y));
                }
                if ((status = prefix(i < shape->polygon_masks.size() ? shape->polygon_masks[i] : 0)) ||
                    (status = defwSpecialNetPolygon(layer_name.c_str(), static_cast<int>(xs.size()), xs.data(), ys.data())))
                    return status;
            }
        }
        return 0;
    }

    int DEFWriter::write_routes(FILE *file, const Root &root, LayoutId layout_id)
    {
        const auto &route_ids = root.get_layout_routes(layout_id);

        std::vector<RouteId> net_ids, special_net_ids;
        for (RouteId route_id : route_ids)
        {
            const RouteData *route = root.get_route(route_id);
            if (!route)
                continue;
            (route->is_special ? special_net_ids : net_ids).push_back(route_id);
        }

        if (!special_net_ids.empty())
        {
            int status = defwStartSpecialNets(static_cast<int>(special_net_ids.size()));
            if (status)
                return status;
            for (RouteId route_id : special_net_ids)
            {
                const RouteData *route = root.get_route(route_id);
                status = defwSpecialNet(route->name.c_str());
                if (status)
                    return status;
                if (route->use)
                {
                    status = defwSpecialNetUse(route->use->c_str());
                    if (status)
                        return status;
                }
                if (route->width)
                {
                    // defwSpecialNetWidth needs a layer name, but
                    // Route.width is net-wide (see RouteData's own schema
                    // comment on the deferred per-layer form) - written
                    // against the net's first routed layer as the closest
                    // available approximation.
                    const auto &wire_ids = root.get_route_wires(route_id);
                    const auto &shape_ids = root.get_route_shapes(route_id);
                    const WireData *first_wire = wire_ids.empty() ? nullptr : root.get_wire(wire_ids.front());
                    const ShapeData *first_shape = shape_ids.empty() ? nullptr : root.get_shape(shape_ids.front());
                    const LayerData *layer = root.get_layer(first_wire ? first_wire->layer : first_shape ? first_shape->layer : LayerId{});
                    if (layer)
                    {
                        status = defwSpecialNetWidth(layer->name.c_str(), static_cast<int>(*route->width));
                        if (status)
                            return status;
                    }
                }
                if (route->voltage)
                {
                    status = defwSpecialNetVoltage(*route->voltage);
                    if (status)
                        return status;
                }

                status = write_net_path(file, root, route_id, true);
                if (status)
                    return status;

                status = defwSpecialNetEndOneNet();
                if (status)
                    return status;
            }
            status = defwEndSpecialNets();
            if (status)
                return status;
        }

        if (!net_ids.empty())
        {
            int status = defwStartNets(static_cast<int>(net_ids.size()));
            if (status)
                return status;
            for (RouteId route_id : net_ids)
            {
                const RouteData *route = root.get_route(route_id);
                status = defwNet(route->name.c_str());
                if (status)
                    return status;
                if (route->use)
                {
                    status = defwNetUse(route->use->c_str());
                    if (status)
                        return status;
                }

                status = write_net_path(file, root, route_id, false);
                if (status)
                    return status;

                status = defwNetEndOneNet();
                if (status)
                    return status;
            }
            status = defwEndNets();
            if (status)
                return status;
        }

        return 0;
    }

    int DEFWriter::write_def(const std::string &path, const Root &root, LayoutId layout_id)
    {
        spdlog::info(
            "write_def: writing '{}' - {} component(s), {} pin(s), {} route(s)...", path,
            root.get_layout_placements(layout_id).size(), root.get_layout_physical_ports(layout_id).size(),
            root.get_layout_routes(layout_id).size());

        messages_.clear();
        auto log_error = [this](std::string msg)
        {
            spdlog::error("{}", msg);
            messages_.push_back("ERROR: " + std::move(msg));
        };

        const LayoutData *layout = root.get_layout(layout_id);
        if (!layout)
        {
            log_error("Invalid LayoutId.");
            return 1;
        }
        const DesignData *design = root.get_design(layout->design);
        if (!design)
        {
            log_error("Layout's Design not found.");
            return 1;
        }

        std::unique_ptr<FILE, int (*)(FILE *)> file(fopen(path.c_str(), "w"), &fclose);
        if (!file)
        {
            log_error(fmt::format("Could not open {} for writing.", path));
            return 1;
        }

        const auto technology_ids = root.get_technology_ids();
        const TechnologyId technology_id = technology_ids.empty() ? TechnologyId{} : technology_ids.front();
        const TechnologyData *technology = technology_id.valid() ? root.get_technology(technology_id) : nullptr;

        // KNOWN VENDORED-WRITER GAP (see plans/LEFDEF_BUGS.md): defwInit's own
        // vers1/vers2 parameter writes "VERSION x.y ;" to the file
        // directly, but never updates the writer's internal defVersionNum
        // (confirmed against defwWriter.cpp - only defwVersion() itself
        // does), which several later calls gate on (e.g. defwTracks'
        // MASK/SAMEMASK, 5.8-only, checks `defVersionNum < 5.8`). The
        // "proper" fix, calling defwVersion() to set it, requires
        // defwState == DEFW_INIT, but defwInit() itself always leaves
        // defwState == DEFW_DESIGN - the two can never be sequenced
        // together. defwInitCbk() (init-only, no callbacks actually
        // registered - we still drive every section via the direct defw*
        // API below, not defwWrite()'s own callback loop) is the only
        // entry point that leaves defwState == DEFW_INIT, so it's used
        // here purely for that side effect, followed by the same
        // individual defw* calls defwInit would otherwise have combined.
        // NAMESCASESENSITIVE has no write site at all once defVersionNum
        // is correctly >= 5.6 - defwCaseSensitive() itself returns
        // DEFW_OBSOLETE past that version (the vendored writer's own
        // understanding is that DEF 5.6+ dropped this statement) - not
        // written here even though real 5.8 files (e.g. complete.5.8.def)
        // still carry it; defdiff's own normalization doesn't appear to
        // require it back (see the roundtrip fidelity test).
        int status = defwInitCbk(file.get());
        if (status)
        {
            log_error(fmt::format("defwInitCbk failed with status {}.", status));
            return status;
        }

        status = defwVersion(5, 8);
        if (status)
        {
            log_error(fmt::format("defwVersion failed with status {}.", status));
            return status;
        }

        // DIVIDERCHAR/BUSBITCHARS reuse the same shared Technology fields
        // LEF already writes (see LEFWriter::write_lef) - same DEF/LEF
        // convention.
        status = defwDividerChar(technology && technology->divider_char ? technology->divider_char->c_str() : "/");
        if (status)
        {
            log_error(fmt::format("defwDividerChar failed with status {}.", status));
            return status;
        }

        status = defwBusBitChars(technology && technology->bus_bit_chars ? technology->bus_bit_chars->c_str() : "[]");
        if (status)
        {
            log_error(fmt::format("defwBusBitChars failed with status {}.", status));
            return status;
        }

        status = defwDesignName(design->name.c_str());
        if (status)
        {
            log_error(fmt::format("defwDesignName failed with status {}.", status));
            return status;
        }

        if (technology)
        {
            status = defwUnits(static_cast<int>(technology->database_units_microns));
            if (status)
            {
                log_error(fmt::format("defwUnits failed with status {}.", status));
                return status;
            }
        }

        status = write_die_area(root, layout_id);
        if (status)
        {
            log_error(fmt::format("Writing DIEAREA failed with status {}.", status));
            return status;
        }

        status = write_rows(root, layout_id);
        if (status)
        {
            log_error(fmt::format("Writing ROWs failed with status {}.", status));
            return status;
        }

        status = write_tracks(root, layout_id);
        if (status)
        {
            log_error(fmt::format("Writing TRACKS failed with status {}.", status));
            return status;
        }

        status = write_gcell_grids(root, layout_id);
        if (status)
        {
            log_error(fmt::format("Writing GCELLGRID failed with status {}.", status));
            return status;
        }

        status = write_vias(root, layout_id);
        if (status)
        {
            log_error(fmt::format("Writing VIAS failed with status {}.", status));
            return status;
        }

        if (technology_id.valid())
        {
            status = write_non_default_rules(root, technology_id);
            if (status)
            {
                log_error(fmt::format("Writing NONDEFAULTRULES failed with status {}.", status));
                return status;
            }
        }

        status = write_regions(root, layout_id);
        if (status)
        {
            log_error(fmt::format("Writing REGIONS failed with status {}.", status));
            return status;
        }

        status = write_placements(root, layout_id);
        if (status)
        {
            log_error(fmt::format("Writing COMPONENTS failed with status {}.", status));
            return status;
        }

        status = write_physical_ports(root, layout_id);
        if (status)
        {
            log_error(fmt::format("Writing PINS failed with status {}.", status));
            return status;
        }

        status = write_blockages(root, layout_id);
        if (status)
        {
            log_error(fmt::format("Writing BLOCKAGES failed with status {}.", status));
            return status;
        }

        status = write_routes(file.get(), root, layout_id);
        if (status)
        {
            log_error(fmt::format("Writing NETS/SPECIALNETS failed with status {}.", status));
            return status;
        }

        status = defwEnd();
        if (status)
        {
            log_error(fmt::format("defwEnd failed with status {}.", status));
            return status;
        }

        spdlog::info(
            "write_def: completed '{}' - {} row(s), {} track(s), {} gcell grid(s), {} component(s), {} pin(s), {} blockage(s), {} route(s), {} region(s), {} via(s)",
            path, root.get_layout_rows(layout_id).size(), root.get_layout_tracks(layout_id).size(),
            root.get_layout_gcell_grids(layout_id).size(), root.get_layout_placements(layout_id).size(),
            root.get_layout_physical_ports(layout_id).size(), root.get_layout_blockages(layout_id).size(),
            root.get_layout_routes(layout_id).size(), root.get_layout_regions(layout_id).size(),
            root.get_layout_vias(layout_id).size());

        return 0;
    }
}
