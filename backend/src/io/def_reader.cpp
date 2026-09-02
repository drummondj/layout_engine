#include <fmt/format.h>
#include "def_reader.hpp"
#include "../geometry/geometry.hpp"
#include <cmath>
#include <cstring>
#include <utility>

namespace
{
    // See LEFReader's own g_pending_lef_messages/log_warning/log_error -
    // identical bridging need and reasoning, just for the DEF parser's own
    // no-userData log callbacks.
    thread_local std::vector<std::string> g_pending_def_messages;

    template <typename... Args>
    void log_error(fmt::format_string<Args...> fmt_str, Args &&...args)
    {
        std::string msg = fmt::format(fmt_str, std::forward<Args>(args)...);
        spdlog::error("{}", msg);
        g_pending_def_messages.push_back("ERROR: " + msg);
    }

    // See LEFReader's own log_warning - same reasoning, just for DEF.
    template <typename... Args>
    void log_warning(fmt::format_string<Args...> fmt_str, Args &&...args)
    {
        std::string msg = fmt::format(fmt_str, std::forward<Args>(args)...);
        spdlog::warn("{}", msg);
        g_pending_def_messages.push_back("WARNING: " + msg);
    }

    // Rescales one raw DEF database-unit value from this file's own UNITS
    // scale onto the shared Technology's actual scale - see
    // DEFReader::unit_scale_'s own comment. A no-op multiply (unit_scale ==
    // 1.0) whenever they already agree, the overwhelmingly common case.
    int64_t scale_dbu(int64_t raw, double unit_scale)
    {
        return unit_scale == 1.0 ? raw : static_cast<int64_t>(std::llround(static_cast<double>(raw) * unit_scale));
    }

    // defiComponent has no single placementStatus-to-enum accessor of its
    // own - just the boolean is*() family (isUnplaced/isPlaced/isFixed/
    // isCover/isSoftfixed) - so this maps those the same way this project
    // prefers named/safe accessors over trusting a raw placementStatus()
    // int code directly (same reasoning as using orientStr() over a raw
    // orient() int elsewhere in this file).
    le::PlacementStatus placement_status_from_component(defiComponent *component)
    {
        if (component->isFixed())
            return le::PlacementStatus::FIXED;
        if (component->isCover())
            return le::PlacementStatus::COVER;
        if (component->isSoftfixed())
            return le::PlacementStatus::SOFTFIXED;
        if (component->isPlaced())
            return le::PlacementStatus::PLACED;
        return le::PlacementStatus::UNPLACED;
    }

    // Shared by defiPin and defiPinPort (5.7+ multi-port pins, each
    // independently placed via its own setPortPlacement - see
    // PhysicalPortSegment's own schema.py comment) - neither has
    // isSoftfixed() (DEF PIN/PORT placement doesn't support SOFTFIXED,
    // only COMPONENTS does), otherwise the same reasoning as
    // placement_status_from_component above.
    template <typename PinLike>
    le::PlacementStatus placement_status_from_pin_like(PinLike *pin)
    {
        if (pin->isFixed())
            return le::PlacementStatus::FIXED;
        if (pin->isCover())
            return le::PlacementStatus::COVER;
        if (pin->isPlaced())
            return le::PlacementStatus::PLACED;
        return le::PlacementStatus::UNPLACED;
    }

    // Fills location/orientation from a defiPin/defiPinPort's own
    // placement, if any (isPlaced()/isFixed()/isCover() - see
    // placement_status_from_pin_like's own comment on why not
    // isUnplaced()/hasPlacement(), which don't distinguish "no placement
    // statement at all" from "explicitly UNPLACED" the same safe way).
    // Returns false (with an error already logged) only if a real
    // placement was present but its orientation string didn't parse;
    // true otherwise, including "no placement at all" (location/
    // orientation simply left unset).
    template <typename PinLike>
    bool fill_pin_like_placement(PinLike *pin, const char *pin_name, double unit_scale, std::optional<le::Point> &location, std::optional<le::Orientation> &orientation)
    {
        if (!pin->isPlaced() && !pin->isFixed() && !pin->isCover())
            return true;
        location = le::Point{.x = scale_dbu(pin->placementX(), unit_scale), .y = scale_dbu(pin->placementY(), unit_scale)};
        const std::optional<le::Orientation> parsed = le::orientation_from_string(pin->orientStr());
        if (!parsed)
        {
            log_error("PIN {} has an unrecognized orientation '{}' - ignored.", pin_name, pin->orientStr());
            return false;
        }
        orientation = parsed;
        return true;
    }

    // Shared by defiPin and defiPinPort (5.7+ multi-port pins) - both
    // expose the identical numLayer()/layer()/bounds() and numPolygons()/
    // polygonName()/getPolygon() accessor shape (no common base class,
    // hence the template rather than a shared interface type), and DEF's
    // own PIN geometry model is simpler than LEF's: each layer-rect is
    // already its own indexed (layer, rect) pair, and each polygon already
    // carries its own layer name, rather than LEF's single interleaved
    // "LAYER, then geometry" stream - so this only needs to group by
    // matching layer_name into one Shape per distinct layer, not walk an
    // itemType() stream like LEFReader::shapes_from_parser does.
    template <typename PinLike>
    std::vector<le::Shape> shapes_from_pin_like(le::Root &root, PinLike *pin, double unit_scale)
    {
        std::vector<le::Shape> shapes;
        // Returns nullptr (logging once) for a layer name that doesn't
        // resolve to a Technology Layer - the geometry on it is dropped,
        // same "log and skip" convention every other unresolvable
        // reference in this reader follows.
        auto find_or_create = [&](const std::string &layer_name) -> le::Shape *
        {
            for (le::Shape &shape : shapes)
                if (root.get_layer(shape.layer) && root.get_layer(shape.layer)->name == layer_name)
                    return &shape;
            const le::LayerId layer_id = root.get_layer_by_name(layer_name);
            if (!layer_id.valid())
            {
                log_error("PIN geometry on unknown LAYER '{}' - ignored.", layer_name);
                return nullptr;
            }
            shapes.push_back(le::Shape{.layer = layer_id});
            return &shapes.back();
        };

        for (int i = 0; i < pin->numLayer(); i++)
        {
            int xl = 0, yl = 0, xh = 0, yh = 0;
            pin->bounds(i, &xl, &yl, &xh, &yh);
            if (le::Shape *shape = find_or_create(pin->layer(i)))
                shape->rects.push_back(le::Rect{.ll = {.x = scale_dbu(xl, unit_scale), .y = scale_dbu(yl, unit_scale)}, .ur = {.x = scale_dbu(xh, unit_scale), .y = scale_dbu(yh, unit_scale)}});
        }
        for (int i = 0; i < pin->numPolygons(); i++)
        {
            le::Shape *shape = find_or_create(pin->polygonName(i));
            if (!shape)
                continue;
            const defiPoints points = pin->getPolygon(i);
            le::Polygon polygon;
            polygon.points.reserve(static_cast<size_t>(points.numPoints));
            for (int j = 0; j < points.numPoints; j++)
                polygon.points.push_back(le::Point{.x = scale_dbu(points.x[j], unit_scale), .y = scale_dbu(points.y[j], unit_scale)});
            shape->polygons.push_back(std::move(polygon));
        }
        return shapes;
    }

    // Same combined-mask convention as LEFReader::combine_via_mask
    // (lef_reader.cpp) - kept as its own copy since that one is scoped to
    // its own anonymous namespace, not shared/exported.
    int combine_via_mask(int top, int cut, int bottom)
    {
        return top * 100 + cut * 10 + bottom;
    }

    // Walks one defiPath's ordered element stream (LAYER/WIDTH/POINT/
    // FLUSHPOINT/VIA/VIAROTATION/VIAMASK/VIADATA, ...) into Shape entries
    // grouped by layer name (find_or_create, same idiom as
    // shapes_from_pin_like/the VIAS callback above) - appended into the
    // caller's own `shapes` accumulator, since a Net's several wires/
    // paths (ROUTED, NEW, ...) all contribute to the same flat set of
    // per-layer Shapes. VIAROTATION/VIAMASK are their own separate
    // stream elements *after* a VIA element (confirmed against
    // defiPath.cpp: getViaRotationStr()/getViaTopMask() etc. read
    // key_[*pointer_], returning nothing unless positioned exactly at an
    // 'O'/'C' element) - not attached to the VIA element itself, hence
    // the pending_via accumulator finalized whenever a *different*
    // element type is seen. Not yet handled: DEFIPATH_RECT (a
    // rectangular path segment, rare), DEFIPATH_TAPER/TAPERRULE/SHAPE/
    // STYLE (manufacturing/rendering-hint metadata with no schema field
    // for it yet) - skipped, not erroring.
    void append_shapes_from_path(le::Root &root, std::vector<le::Shape> &shapes, defiPath *path, double unit_scale)
    {
        // Returns nullptr (logging once) for a layer name that doesn't
        // resolve to a Technology Layer - every element of this path
        // segment is then dropped via the existing `current_shape`
        // null-guards below, same as before the first LAYER element is
        // ever seen.
        auto find_or_create = [&](const std::string &layer_name) -> le::Shape *
        {
            for (le::Shape &shape : shapes)
                if (root.get_layer(shape.layer) && root.get_layer(shape.layer)->name == layer_name)
                    return &shape;
            const le::LayerId layer_id = root.get_layer_by_name(layer_name);
            if (!layer_id.valid())
            {
                log_error("Routed path on unknown LAYER '{}' - ignored.", layer_name);
                return nullptr;
            }
            shapes.push_back(le::Shape{.layer = layer_id});
            return &shapes.back();
        };

        le::Shape *current_shape = nullptr;
        int64_t current_width = 0;
        std::vector<le::Point> current_points;
        le::Point last_point{};

        struct PendingVia
        {
            std::string via_name;
            le::Point origin;
            std::optional<le::Orientation> orientation;
            std::optional<int> mask;
            bool is_array = false;
            int num_x = 0, num_y = 0;
            int64_t space_x = 0, space_y = 0;
            // The enclosing path's own current_width at this via's own
            // point (BUGS_AND_ENHANCEMENTS.md B3 follow-up) - the routing-
            // width context via_shapes.hpp's own VIARULE GENERATE fit
            // algorithm needs when a via reference resolves only to a
            // top-level GENERATE rule, with no explicit CUTSIZE/ROWCOL
            // anywhere to fall back on. Always set here (current_width
            // defaults to the current LAYER's own declared LEF width even
            // with no DEFIPATH_WIDTH override - see the DEFIPATH_LAYER
            // case below), unlike ShapeVia.width's own is_optional=True
            // (which also covers the LEF PORT/OBS VIA case, with no
            // enclosing routed path at all).
            int64_t width = 0;
        };
        std::optional<PendingVia> pending_via;

        auto finalize_via = [&]()
        {
            if (pending_via && current_shape)
            {
                if (pending_via->is_array)
                {
                    current_shape->via_iterates.push_back(le::ShapeViaIterate{
                        .via_name = pending_via->via_name,
                        .origin = pending_via->origin,
                        .orientation = pending_via->orientation,
                        .num_x = pending_via->num_x,
                        .num_y = pending_via->num_y,
                        .space_x = pending_via->space_x,
                        .space_y = pending_via->space_y,
                        .mask = pending_via->mask,
                        .width = pending_via->width,
                    });
                }
                else
                {
                    current_shape->vias.push_back(le::ShapeVia{
                        .via_name = pending_via->via_name,
                        .origin = pending_via->origin,
                        .orientation = pending_via->orientation,
                        .mask = pending_via->mask,
                        .width = pending_via->width,
                    });
                }
            }
            pending_via.reset();
        };

        auto flush_path = [&]()
        {
            if (current_shape && current_points.size() >= 2)
                current_shape->paths.push_back(le::Path{.polygon = le::Polygon{.points = current_points}, .width = current_width});
            current_points.clear();
        };

        path->initTraverse();
        int type;
        while ((type = path->next()) != DEFIPATH_DONE)
        {
            if (type != DEFIPATH_VIAROTATION && type != DEFIPATH_VIAMASK && type != DEFIPATH_VIADATA)
                finalize_via();

            switch (type)
            {
            case DEFIPATH_LAYER:
                flush_path();
                current_shape = find_or_create(path->getLayer());
                // DEF's own WIDTH token is optional per LAYER occurrence -
                // when the writer never specifies one (the common case for
                // ordinary routing - most real DEF writers rely entirely on
                // this), default here to that Layer's own declared LEF
                // WIDTH rather than leaving every point at width 0 (which
                // renders as a hairline stroke, not the real trace width).
                // A DEFIPATH_WIDTH token encountered afterward (below)
                // still overrides this for the current layer occurrence -
                // reset again on the NEXT LAYER token regardless, so an
                // explicit override for one layer never silently bleeds
                // into a different layer later in the same path that
                // never asked for one.
                current_width = 0;
                if (current_shape)
                    if (const le::LayerData *layer = root.get_layer(current_shape->layer))
                        current_width = layer->width.value_or(0);
                break;
            case DEFIPATH_WIDTH:
                current_width = scale_dbu(path->getWidth(), unit_scale);
                break;
            case DEFIPATH_POINT:
            {
                int x = 0, y = 0;
                path->getPoint(&x, &y);
                last_point = le::Point{.x = scale_dbu(x, unit_scale), .y = scale_dbu(y, unit_scale)};
                current_points.push_back(last_point);
                break;
            }
            case DEFIPATH_FLUSHPOINT:
            {
                int x = 0, y = 0, ext = 0;
                path->getFlushPoint(&x, &y, &ext);
                last_point = le::Point{.x = scale_dbu(x, unit_scale), .y = scale_dbu(y, unit_scale)};
                current_points.push_back(last_point);
                break;
            }
            case DEFIPATH_VIA:
                pending_via = PendingVia{.via_name = path->getVia(), .origin = last_point, .width = current_width};
                break;
            case DEFIPATH_VIAROTATION:
                if (pending_via)
                    pending_via->orientation = le::orientation_from_string(path->getViaRotationStr());
                break;
            case DEFIPATH_VIAMASK:
                if (pending_via)
                {
                    const int mask = combine_via_mask(path->getViaTopMask(), path->getViaCutMask(), path->getViaBottomMask());
                    if (mask != 0)
                        pending_via->mask = mask;
                }
                break;
            case DEFIPATH_VIADATA:
                if (pending_via)
                {
                    int num_x = 0, num_y = 0, space_x = 0, space_y = 0;
                    path->getViaData(&num_x, &num_y, &space_x, &space_y);
                    pending_via->is_array = true;
                    pending_via->num_x = num_x;
                    pending_via->num_y = num_y;
                    pending_via->space_x = scale_dbu(space_x, unit_scale);
                    pending_via->space_y = scale_dbu(space_y, unit_scale);
                }
                break;
            default:
                break;
            }
        }
        flush_path();
        finalize_via();
    }
}

namespace le
{
    void DEFReader::defrLogFn(const char *msg)
    {
        if (!msg)
            return;
        std::string s(msg);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        if (!s.empty())
            g_pending_def_messages.push_back(std::move(s));
    }

    Polygon DEFReader::polygon_from_die_area(defiBox *box, double unit_scale)
    {
        const defiPoints points = box->getPoint();
        if (points.numPoints >= 3)
        {
            Polygon polygon;
            polygon.points.reserve(static_cast<size_t>(points.numPoints));
            for (int i = 0; i < points.numPoints; i++)
                polygon.points.push_back(Point{.x = scale_dbu(points.x[i], unit_scale), .y = scale_dbu(points.y[i], unit_scale)});
            return polygon;
        }
        // Plain 2-corner shorthand (no explicit point list) - same
        // rect-to-polygon fallback LEFReader::post_process uses for its
        // own SIZE/ORIGIN fallback boundary.
        const Rect bbox{.ll = {.x = scale_dbu(box->xl(), unit_scale), .y = scale_dbu(box->yl(), unit_scale)}, .ur = {.x = scale_dbu(box->xh(), unit_scale), .y = scale_dbu(box->yh(), unit_scale)}};
        return Geometry::rect_to_polygon(bbox);
    }

    int DEFReader::defrDesignCbkFn(defrCallbackType_e /*typ*/, const char *name, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);

        reader->library_id_ = reader->root_->get_library_by_name(reader->library_name_);
        if (!reader->library_id_.valid())
            reader->library_id_ = reader->root_->create_library(LibraryData{.name = reader->library_name_});

        reader->design_id_ = reader->root_->get_design_by_name(name);
        if (!reader->design_id_.valid())
            reader->design_id_ = reader->root_->create_design(DesignData{.library = reader->library_id_, .name = name});

        if (reader->root_->get_design_layout(reader->design_id_).valid())
        {
            log_error("DEF DESIGN {} already has a Layout - re-reading a DEF file into the same design is not supported.", name);
            return 0;
        }
        reader->layout_id_ = reader->root_->create_layout(LayoutData{.design = reader->design_id_});
        return 0;
    }

    int DEFReader::defrVersionCbkFn(defrCallbackType_e /*typ*/, double /*version*/, void * /*user_data*/)
    {
        // Not yet validated against a minimum supported DEF version (no
        // known version-obsolescence gap like LEF's own >= 5.4 requirement
        // has surfaced for DEF yet) - revisit if LEFDEF_BUGS.md gains one.
        return 0;
    }

    int DEFReader::defrUnitsCbkFn(defrCallbackType_e /*typ*/, double units, void *user_data)
    {
        // Most DEF coordinate/dimension values (ROW/TRACKS/GCELLGRID/
        // DIEAREA/COMPONENTS placement, etc.) are already expressed
        // directly in database units in the file itself (confirmed
        // against this project's own complete.5.8.def fixture: UNITS
        // DISTANCE MICRONS 1000, yet ROW/TRACKS coordinates are plain
        // integers like 1000/8400, not 1.0/8.4). NONDEFAULTRULES LAYER
        // WIDTH/SPACING/WIREEXT/DIAGWIDTH are the one construct so far
        // that's the exception - written in real microns (e.g. "WIDTH
        // 10.1"), needing this same UNITS-derived factor LEF uses
        // everywhere - defiNonDefault's own "Val"-suffixed accessors
        // (layerWidthVal() etc.) turned out to just truncate the raw
        // micron double to an int, *not* convert using UNITS (found by
        // testing against real fixture values: 10.1 came back as 10, not
        // 10100) - real conversion needs the plain (non-Val) micron
        // accessor multiplied by this factor instead, same as LEF.
        auto reader = static_cast<DEFReader *>(user_data);
        TechnologyData *technology = reader->root_->get_technology(reader->technology_id_);
        if (!technology)
            return 0;
        if (technology->database_units_microns == 0)
        {
            technology->database_units_microns = units;
            reader->unit_scale_ = 1.0;
        }
        else if (technology->database_units_microns != units)
        {
            // The shared Technology's own scale (typically established by
            // an earlier LEF read) is never overwritten by a later DEF's
            // disagreeing UNITS - every other already-created Shape was
            // stored assuming that original scale. Instead, this DEF's own
            // raw database-unit values (see unit_scale_'s own comment) get
            // rescaled onto it via unit_scale_, so they land at the same
            // real-world position rather than being silently misread at
            // the wrong grid resolution (the previous behavior here -
            // logging and using the DEF's raw values unconverted - was a
            // real bug, not just a missed conversion).
            reader->unit_scale_ = technology->database_units_microns / units;
            if (units < technology->database_units_microns)
            {
                // DEF's own grid is coarser than the technology's - every
                // value it expresses scales up exactly (technology units
                // is always one of the fixed LEF/DEF UNITS values, though
                // not necessarily an exact multiple of this DEF's own, in
                // which case scaling can still round to the nearest
                // technology-grid position) - but the DEF's own original
                // authored precision can never exceed its own coarser
                // grid to begin with, so this scaling can't manufacture
                // detail the file never had.
                log_warning("DEF UNITS DISTANCE MICRONS {} is coarser than the current technology units {} - scaling DEF geometry up by {:.4g}x, but it cannot carry more precision than its own original {} units/micron.", units, technology->database_units_microns, reader->unit_scale_, units);
            }
            else
            {
                log_warning("DEF UNITS DISTANCE MICRONS {} does not equal current technology units {} - scaling DEF geometry by {:.4g}x to match.", units, technology->database_units_microns, reader->unit_scale_);
            }
        }
        else
        {
            reader->unit_scale_ = 1.0;
        }
        return 0;
    }

    int DEFReader::defrDieAreaCbkFn(defrCallbackType_e /*typ*/, defiBox *box, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);
        if (!reader->layout_id_.valid())
        {
            log_error("DIEAREA statement seen before DESIGN - ignored.");
            return 0;
        }
        reader->root_->create_shape(ShapeData{
            .layout = reader->layout_id_,
            .purpose = ShapePurpose::BOUNDARY,
            .polygons = {polygon_from_die_area(box, reader->unit_scale_)},
        });
        return 0;
    }

    int DEFReader::defrRowCbkFn(defrCallbackType_e /*typ*/, defiRow *row, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);
        if (!reader->layout_id_.valid())
        {
            log_error("ROW {} statement seen before DESIGN - ignored.", row->name());
            return 0;
        }

        const std::optional<Orientation> orientation = orientation_from_string(row->orientStr());
        if (!orientation)
        {
            log_error("ROW {} has an unrecognized orientation '{}' - ignored.", row->name(), row->orientStr());
            return 0;
        }

        RowData data{
            .layout = reader->layout_id_,
            .name = row->name(),
            .site_name = row->macro(),
            .origin = Point{.x = scale_dbu(row->x(), reader->unit_scale_), .y = scale_dbu(row->y(), reader->unit_scale_)},
            .orientation = *orientation,
        };
        if (row->hasDo())
        {
            data.num_x = static_cast<int>(row->xNum());
            data.num_y = static_cast<int>(row->yNum());
        }
        if (row->hasDoStep())
        {
            data.step_x = scale_dbu(row->xStep(), reader->unit_scale_);
            data.step_y = scale_dbu(row->yStep(), reader->unit_scale_);
        }
        reader->root_->create_row(std::move(data));
        return 0;
    }

    int DEFReader::defrTrackCbkFn(defrCallbackType_e /*typ*/, defiTrack *track, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);
        if (!reader->layout_id_.valid())
        {
            log_error("TRACKS statement seen before DESIGN - ignored.");
            return 0;
        }

        TrackData data{
            .layout = reader->layout_id_,
            .is_x = std::strcmp(track->macro(), "X") == 0,
            .start = scale_dbu(track->x(), reader->unit_scale_),
            .count = static_cast<int>(track->xNum()),
            .step = scale_dbu(track->xStep(), reader->unit_scale_),
            .same_mask = static_cast<bool>(track->sameMask()),
        };
        data.layer_names.reserve(static_cast<size_t>(track->numLayers()));
        for (int i = 0; i < track->numLayers(); i++)
            data.layer_names.push_back(track->layer(i));
        if (const int mask = track->firstTrackMask(); mask != 0)
            data.mask = mask;
        reader->root_->create_track(std::move(data));
        return 0;
    }

    int DEFReader::defrGcellGridCbkFn(defrCallbackType_e /*typ*/, defiGcellGrid *grid, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);
        if (!reader->layout_id_.valid())
        {
            log_error("GCELLGRID statement seen before DESIGN - ignored.");
            return 0;
        }

        reader->root_->create_g_cell_grid(GCellGridData{
            .layout = reader->layout_id_,
            .is_x = std::strcmp(grid->macro(), "X") == 0,
            .start = scale_dbu(grid->x(), reader->unit_scale_),
            .count = grid->xNum(),
            .step = scale_dbu(grid->xStep(), reader->unit_scale_),
        });
        return 0;
    }

    int DEFReader::defrComponentCbkFn(defrCallbackType_e /*typ*/, defiComponent *component, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);
        if (!reader->layout_id_.valid())
        {
            log_error("COMPONENT {} statement seen before DESIGN - ignored.", component->id());
            return 0;
        }

        // defiComponent's own naming is confusing: id() is the instance
        // name, name() is the referenced macro name (set together by
        // IdAndName() from the base "- compId macroName" grammar - def.y's
        // comp_id_and_name rule) - macroName() is a separate, unrelated
        // field only ever populated by a "+ GENERATE genName [pattern]"
        // attribute (setGenerate()), never by the base parse. Confirmed
        // empirically against complete.5.8.def: macroName() was empty for
        // every component, GENERATE or not.
        //
        // reference_design must resolve to an already-read Design (e.g.
        // its LEF read earlier into this same Root) - unlike the former
        // reference_name string this replaces, there's no fallback "store
        // the name, resolve later" path, so the real Technology/Library
        // LEF(s) must be read before the DEF that references them (the
        // normal real-world flow anyway - a router/placer always has the
        // cell library loaded first).
        const DesignId reference_design = reader->root_->get_design_by_name(component->name());
        if (!reference_design.valid())
        {
            log_error("COMPONENT {} references unknown macro/design '{}' - ignored.", component->id(), component->name());
            return 0;
        }

        PlacementData data{
            .layout = reader->layout_id_,
            .name = component->id(),
            .reference_design = reference_design,
            .placement_status = placement_status_from_component(component),
        };
        // isUnplaced() is true only for an explicit UNPLACED keyword - a
        // component with no placement statement at all (bare `- name
        // macro ;`, common before a placer has run) is neither placed nor
        // "unplaced" by this accessor's own definition, so only read
        // location/orientation when one of the genuinely-placed flags is
        // true (found the hard way: !isUnplaced() silently dropped every
        // no-placement-statement component below, since their empty
        // placementOrientStr() then failed orientation_from_string and hit
        // the early return before create_placement ever ran).
        if (component->isPlaced() || component->isFixed() || component->isCover() || component->isSoftfixed())
        {
            data.location = Point{.x = scale_dbu(component->placementX(), reader->unit_scale_), .y = scale_dbu(component->placementY(), reader->unit_scale_)};
            const std::optional<Orientation> orientation = orientation_from_string(component->placementOrientStr());
            if (!orientation)
            {
                log_error("COMPONENT {} has an unrecognized orientation '{}' - ignored.", component->id(), component->placementOrientStr());
                return 0;
            }
            data.orientation = *orientation;
        }
        if (component->hasWeight())
            data.weight = static_cast<double>(component->weight());
        if (component->hasSource())
            data.source = component->source();

        reader->root_->create_placement(std::move(data));
        return 0;
    }

    int DEFReader::defrPinCbkFn(defrCallbackType_e /*typ*/, defiPin *pin, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);
        if (!reader->layout_id_.valid())
        {
            log_error("PIN {} statement seen before DESIGN - ignored.", pin->pinName());
            return 0;
        }

        PhysicalPortData data{
            .layout = reader->layout_id_,
            .name = pin->pinName(),
        };
        if (pin->netName() && pin->netName()[0])
            data.net_name = pin->netName();
        if (pin->hasDirection())
        {
            const std::optional<SignalDirection> direction = signal_direction_from_string(pin->direction());
            if (!direction)
            {
                log_error("PIN {} has an unrecognized direction '{}' - ignored.", pin->pinName(), pin->direction());
                return 0;
            }
            data.direction = *direction;
        }
        if (pin->hasUse())
            data.use = pin->use();
        data.placement_status = placement_status_from_pin_like(pin);
        if (!fill_pin_like_placement(pin, pin->pinName(), reader->unit_scale_, data.location, data.orientation))
            return 0;

        const PhysicalPortId physical_port_id = reader->root_->create_physical_port(std::move(data));

        // 5.7+ multi-port pins (hasPort()) each get their own
        // PhysicalPortSegment, with its own independent placement (see
        // PhysicalPortSegment's own schema.py comment - each PORT's
        // LAYER/POLYGON coordinates are relative to that PORT's own
        // placement, not the parent PhysicalPort's); a pre-5.7 simple pin
        // has no PORT wrapper in the DEF syntax at all, so gets exactly
        // one synthetic segment (placement left unset - it lives on the
        // parent PhysicalPort instead) to hold its directly-attached
        // geometry - same reasoning as TerminalPort always existing under
        // a LEF Terminal, just without a syntactic counterpart to require
        // it here.
        if (pin->hasPort())
        {
            for (int i = 0; i < pin->numPorts(); i++)
            {
                defiPinPort *port = pin->pinPort(i);
                PhysicalPortSegmentData segment_data{.physical_port = physical_port_id};
                segment_data.placement_status = placement_status_from_pin_like(port);
                if (!fill_pin_like_placement(port, pin->pinName(), reader->unit_scale_, segment_data.location, segment_data.orientation))
                    return 0;
                const PhysicalPortSegmentId segment_id = reader->root_->create_physical_port_segment(std::move(segment_data));
                for (Shape &shape : shapes_from_pin_like(*reader->root_, port, reader->unit_scale_))
                {
                    shape.physical_port_segment = segment_id;
                    reader->root_->create_shape(std::move(shape));
                }
            }
        }
        else
        {
            const PhysicalPortSegmentId segment_id = reader->root_->create_physical_port_segment(PhysicalPortSegmentData{.physical_port = physical_port_id});
            for (Shape &shape : shapes_from_pin_like(*reader->root_, pin, reader->unit_scale_))
            {
                shape.physical_port_segment = segment_id;
                reader->root_->create_shape(std::move(shape));
            }
        }
        return 0;
    }

    int DEFReader::defrBlockageCbkFn(defrCallbackType_e /*typ*/, defiBlockage *blockage, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);
        if (!reader->layout_id_.valid())
        {
            log_error("BLOCKAGES statement seen before DESIGN - ignored.");
            return 0;
        }

        BlockageData data{
            .layout = reader->layout_id_,
            .kind = blockage->hasLayer() ? BlockageKind::ROUTING : BlockageKind::PLACEMENT,
        };
        if (blockage->hasLayer())
            data.layer_name = blockage->layerName();
        if (blockage->hasComponent())
        {
            // layerComponentName()/placementComponentName() are the same
            // underlying field regardless of blockage kind (confirmed
            // against defiBlockage.cpp: both grammar rules - LAYER's own
            // "+ COMPONENT" and PLACEMENT's own - call the same
            // setComponent()) - no Root-level get_placement_by_name
            // exists (Placement.name is unique_per_parent, same as
            // Terminal/Component/... elsewhere in this codebase), so this
            // is the same linear-scan-over-the-current-Layout pattern
            // api.cpp's own le_placement_by_name uses.
            const std::string component_name = blockage->layerComponentName();
            for (const PlacementId placement_id : reader->root_->get_layout_placements(reader->layout_id_))
            {
                const PlacementData *placement = reader->root_->get_placement(placement_id);
                if (placement && placement->name == component_name)
                {
                    data.placement = placement_id;
                    break;
                }
            }
        }
        if (blockage->hasSpacing())
            data.spacing = scale_dbu(blockage->minSpacing(), reader->unit_scale_);
        if (blockage->hasDesignRuleWidth())
            data.design_rule_width = scale_dbu(blockage->designRuleWidth(), reader->unit_scale_);
        data.is_soft = static_cast<bool>(blockage->hasSoft());
        if (blockage->hasPartial())
            data.placement_max_density = blockage->placementMaxDensity();

        const BlockageId blockage_id = reader->root_->create_blockage(std::move(data));

        if (blockage->numRectangles() > 0 || blockage->numPolygons() > 0)
        {
            // A Blockage is scoped to at most one layer (or none, for a
            // PLACEMENT blockage), unlike a PIN's own per-rect layer
            // array - so unlike shapes_from_pin_like, this needs only one
            // Shape, not one grouped per distinct layer name. A ROUTING
            // blockage's own Shape gets .layer, same as any other real
            // geometry (it really is scoped to a physical routing layer) -
            // an unresolvable layer name drops its geometry entirely
            // (log and skip, same convention as every other unresolvable
            // reference in this reader). A PLACEMENT blockage's own
            // region isn't tied to any routing layer at all (DEF's own
            // PLACEMENT blockage syntax has no LAYER clause), so it gets
            // .purpose = PLACEMENT_BLOCKAGE instead.
            Shape shape{.blockage = blockage_id};
            if (blockage->hasLayer())
            {
                shape.layer = reader->root_->get_layer_by_name(blockage->layerName());
                if (!shape.layer.valid())
                {
                    log_error("BLOCKAGES geometry on unknown LAYER '{}' - ignored.", blockage->layerName());
                    return 0;
                }
            }
            else
            {
                shape.purpose = ShapePurpose::PLACEMENT_BLOCKAGE;
            }
            for (int i = 0; i < blockage->numRectangles(); i++)
                shape.rects.push_back(Rect{.ll = {.x = scale_dbu(blockage->xl(i), reader->unit_scale_), .y = scale_dbu(blockage->yl(i), reader->unit_scale_)}, .ur = {.x = scale_dbu(blockage->xh(i), reader->unit_scale_), .y = scale_dbu(blockage->yh(i), reader->unit_scale_)}});
            for (int i = 0; i < blockage->numPolygons(); i++)
            {
                const defiPoints points = blockage->getPolygon(i);
                Polygon polygon;
                polygon.points.reserve(static_cast<size_t>(points.numPoints));
                for (int j = 0; j < points.numPoints; j++)
                    polygon.points.push_back(Point{.x = scale_dbu(points.x[j], reader->unit_scale_), .y = scale_dbu(points.y[j], reader->unit_scale_)});
                shape.polygons.push_back(std::move(polygon));
            }
            reader->root_->create_shape(std::move(shape));
        }
        return 0;
    }

    int DEFReader::defrViaCbkFn(defrCallbackType_e /*typ*/, defiVia *via, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);
        if (!reader->layout_id_.valid())
        {
            log_error("VIA {} statement seen before DESIGN - ignored.", via->name());
            return 0;
        }

        const LayoutViaId layout_via_id = reader->root_->create_layout_via(LayoutViaData{
            .layout = reader->layout_id_,
            .name = via->name(),
        });

        // Group rects/polygons by layer name, same idiom as
        // shapes_from_pin_like - a DEF-level VIA's own per-layer geometry
        // is otherwise structurally identical to a LEF VIA's (ViaLayer
        // already models both, see its own schema.py comment).
        std::vector<ViaLayerData> via_layers;
        auto find_or_create = [&](const std::string &layer_name) -> ViaLayerData &
        {
            for (ViaLayerData &via_layer : via_layers)
                if (via_layer.layer_name == layer_name)
                    return via_layer;
            via_layers.push_back(ViaLayerData{.layout_via = layout_via_id, .layer_name = layer_name});
            return via_layers.back();
        };

        for (int i = 0; i < via->numLayers(); i++)
        {
            char *layer_name = nullptr;
            int xl = 0, yl = 0, xh = 0, yh = 0;
            via->layer(i, &layer_name, &xl, &yl, &xh, &yh);
            find_or_create(layer_name).rects.push_back(Rect{.ll = {.x = scale_dbu(xl, reader->unit_scale_), .y = scale_dbu(yl, reader->unit_scale_)}, .ur = {.x = scale_dbu(xh, reader->unit_scale_), .y = scale_dbu(yh, reader->unit_scale_)}});
        }
        for (int i = 0; i < via->numPolygons(); i++)
        {
            const defiPoints points = via->getPolygon(i);
            Polygon polygon;
            polygon.points.reserve(static_cast<size_t>(points.numPoints));
            for (int j = 0; j < points.numPoints; j++)
                polygon.points.push_back(Point{.x = scale_dbu(points.x[j], reader->unit_scale_), .y = scale_dbu(points.y[j], reader->unit_scale_)});
            find_or_create(via->polygonName(i)).polygons.push_back(std::move(polygon));
        }
        for (ViaLayerData &via_layer : via_layers)
            reader->root_->create_via_layer(std::move(via_layer));

        if (via->hasViaRule())
        {
            char *via_rule_name = nullptr;
            char *bot_layer = nullptr;
            char *cut_layer = nullptr;
            char *top_layer = nullptr;
            int x_size = 0, y_size = 0, x_cut_spacing = 0, y_cut_spacing = 0;
            int x_bot_enc = 0, y_bot_enc = 0, x_top_enc = 0, y_top_enc = 0;
            via->viaRule(&via_rule_name, &x_size, &y_size, &bot_layer, &cut_layer, &top_layer,
                         &x_cut_spacing, &y_cut_spacing, &x_bot_enc, &y_bot_enc, &x_top_enc, &y_top_enc);
            ViaRuleReferenceData via_rule_data{
                .layout_via = layout_via_id,
                .via_rule_name = via_rule_name,
                .cut_size = Point{.x = scale_dbu(x_size, reader->unit_scale_), .y = scale_dbu(y_size, reader->unit_scale_)},
                .bot_layer_name = bot_layer,
                .cut_layer_name = cut_layer,
                .top_layer_name = top_layer,
                .cut_spacing = Point{.x = scale_dbu(x_cut_spacing, reader->unit_scale_), .y = scale_dbu(y_cut_spacing, reader->unit_scale_)},
                .bot_enclosure = Point{.x = scale_dbu(x_bot_enc, reader->unit_scale_), .y = scale_dbu(y_bot_enc, reader->unit_scale_)},
                .top_enclosure = Point{.x = scale_dbu(x_top_enc, reader->unit_scale_), .y = scale_dbu(y_top_enc, reader->unit_scale_)},
            };
            // ROWCOL (BUGS_AND_ENHANCEMENTS.md B3) - DEF VIAS VIARULE's
            // own mirror of LEF's ROWCOL clause, same "a real via array"
            // meaning - see lef_reader.cpp's own matching comment.
            if (via->hasRowCol())
            {
                int num_cut_rows = 0;
                int num_cut_cols = 0;
                via->rowCol(&num_cut_rows, &num_cut_cols);
                via_rule_data.num_cut_rows = num_cut_rows;
                via_rule_data.num_cut_cols = num_cut_cols;
            }
            // ORIGIN/OFFSET/PATTERN (B3 follow-up) - DEF VIAS VIARULE's
            // own mirror of LEF's ORIGIN/OFFSET/PATTERN clauses, same
            // meaning - see lef_reader.cpp's own matching comment.
            if (via->hasOrigin())
            {
                int x_origin = 0, y_origin = 0;
                via->origin(&x_origin, &y_origin);
                via_rule_data.origin = Point{.x = scale_dbu(x_origin, reader->unit_scale_), .y = scale_dbu(y_origin, reader->unit_scale_)};
            }
            if (via->hasOffset())
            {
                int x_bot_offset = 0, y_bot_offset = 0, x_top_offset = 0, y_top_offset = 0;
                via->offset(&x_bot_offset, &y_bot_offset, &x_top_offset, &y_top_offset);
                via_rule_data.bot_offset = Point{.x = scale_dbu(x_bot_offset, reader->unit_scale_), .y = scale_dbu(y_bot_offset, reader->unit_scale_)};
                via_rule_data.top_offset = Point{.x = scale_dbu(x_top_offset, reader->unit_scale_), .y = scale_dbu(y_top_offset, reader->unit_scale_)};
            }
            if (via->hasCutPattern())
                log_warning("VIA {} has a PATTERN clause on its VIARULE (cut presence bitmap {}) - not supported, rendering every grid cell as a real cut instead of respecting the pattern's own gaps.", via->name(), via->cutPattern());
            reader->root_->create_via_rule_reference(std::move(via_rule_data));
        }
        return 0;
    }

    int DEFReader::defrRegionCbkFn(defrCallbackType_e /*typ*/, defiRegion *region, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);
        if (!reader->layout_id_.valid())
        {
            log_error("REGION {} statement seen before DESIGN - ignored.", region->name());
            return 0;
        }

        RegionData data{
            .layout = reader->layout_id_,
            .name = region->name(),
        };
        if (region->hasType())
            data.region_type = region->type();
        data.rects.reserve(static_cast<size_t>(region->numRectangles()));
        for (int i = 0; i < region->numRectangles(); i++)
            data.rects.push_back(Rect{.ll = {.x = scale_dbu(region->xl(i), reader->unit_scale_), .y = scale_dbu(region->yl(i), reader->unit_scale_)}, .ur = {.x = scale_dbu(region->xh(i), reader->unit_scale_), .y = scale_dbu(region->yh(i), reader->unit_scale_)}});

        reader->root_->create_region(std::move(data));
        return 0;
    }

    int DEFReader::read_net(defiNet *net, void *user_data, bool is_special)
    {
        auto reader = static_cast<DEFReader *>(user_data);
        if (!reader->layout_id_.valid())
        {
            log_error("NET {} statement seen before DESIGN - ignored.", net->name());
            return 0;
        }

        RouteData data{
            .layout = reader->layout_id_,
            .name = net->name(),
            .is_special = is_special,
        };
        if (net->hasUse())
            data.use = net->use();
        if (is_special && net->hasVoltage())
            data.voltage = net->voltage();
        // Per-layer WIDTH overrides (DEF SPECIALNETS WIDTH layerName
        // dist, can appear more than once per net) aren't modeled -
        // Route.width would need to become a per-layer child klass (same
        // shape as NonDefaultRuleLayer) to represent correctly; deferred,
        // not a geometry-correctness concern the way PhysicalPortSegment's
        // own placement was - each routed path's actual width already
        // comes through per-Path via append_shapes_from_path below.

        const RouteId route_id = reader->root_->create_route(std::move(data));

        std::vector<Shape> shapes;
        for (int i = 0; i < net->numWires(); i++)
        {
            defiWire *wire = net->wire(i);
            for (int j = 0; j < wire->numPaths(); j++)
                append_shapes_from_path(*reader->root_, shapes, wire->path(j), reader->unit_scale_);
        }
        for (Shape &shape : shapes)
        {
            shape.route = route_id;
            reader->root_->create_shape(std::move(shape));
        }
        return 0;
    }

    int DEFReader::defrNetCbkFn(defrCallbackType_e /*typ*/, defiNet *net, void *user_data)
    {
        return read_net(net, user_data, false);
    }

    int DEFReader::defrSNetCbkFn(defrCallbackType_e /*typ*/, defiNet *net, void *user_data)
    {
        return read_net(net, user_data, true);
    }

    int DEFReader::defrNonDefaultCbkFn(defrCallbackType_e /*typ*/, defiNonDefault *rule, void *user_data)
    {
        auto reader = static_cast<DEFReader *>(user_data);

        if (reader->root_->get_non_default_rule_by_name(rule->name()).valid())
        {
            log_error("NONDEFAULTRULE {} already exists (e.g. from an earlier LEF read) - ignored.", rule->name());
            return 0;
        }

        NonDefaultRuleData data{
            .technology = reader->technology_id_,
            .name = rule->name(),
            .hard_spacing = static_cast<bool>(rule->hasHardspacing()),
        };

        // LAYER WIDTH/SPACING/WIREEXT/DIAGWIDTH are written in real
        // microns (e.g. "WIDTH 10.1") - layerWidthVal() et al (the
        // "Val"-suffixed accessors) just truncate that double to an int,
        // they don't convert using UNITS (found empirically: 10.1 came
        // back as 10, not 10100 at UNITS DISTANCE MICRONS 1000) - real
        // conversion needs the plain micron accessor multiplied by the
        // shared Technology's own database_units_microns, same as every
        // LEF microns_to_dbu() conversion.
        const TechnologyData *technology = reader->root_->get_technology(reader->technology_id_);
        const double dbu_per_micron = (technology && technology->database_units_microns != 0) ? technology->database_units_microns : 1.0;
        auto microns_to_dbu = [dbu_per_micron](double microns) -> int64_t
        {
            return static_cast<int64_t>(std::llround(microns * dbu_per_micron));
        };

        // Unlike LEF's own inline NONDEFAULTRULE VIA (a full embedded via
        // definition, NonDefaultRuleVia), DEF's own addVia()/addViaRule()
        // are plain name references - the same shape as
        // use_via_names/use_via_rule_names (LEF 5.6 USEVIA/USEVIARULE),
        // so no NonDefaultRuleVia pending-then-attach step is needed here
        // at all.
        data.use_via_names.reserve(static_cast<size_t>(rule->numVias()));
        for (int i = 0; i < rule->numVias(); i++)
            data.use_via_names.push_back(rule->viaName(i));
        data.use_via_rule_names.reserve(static_cast<size_t>(rule->numViaRules()));
        for (int i = 0; i < rule->numViaRules(); i++)
            data.use_via_rule_names.push_back(rule->viaRuleName(i));
        data.min_cuts.reserve(static_cast<size_t>(rule->numMinCuts()));
        for (int i = 0; i < rule->numMinCuts(); i++)
            data.min_cuts.push_back(MinCutOverride{.cut_layer_name = rule->cutLayerName(i), .num_cuts = rule->numCuts(i)});

        std::vector<NonDefaultRuleLayerData> pending_layers;
        pending_layers.reserve(static_cast<size_t>(rule->numLayers()));
        for (int i = 0; i < rule->numLayers(); i++)
        {
            // WIDTH has no has*() guard in defiNonDefault's own API -
            // DEF's NONDEFAULTRULES LAYER grammar requires it
            // unconditionally, unlike SPACING/WIREEXT/DIAGWIDTH.
            NonDefaultRuleLayerData layer{.layer_name = rule->layerName(i), .width = microns_to_dbu(rule->layerWidth(i))};
            if (rule->hasLayerSpacing(i))
                layer.spacing = microns_to_dbu(rule->layerSpacing(i));
            if (rule->hasLayerWireExt(i))
                layer.wire_extension = microns_to_dbu(rule->layerWireExt(i));
            if (rule->hasLayerDiagWidth(i))
                layer.diag_width = microns_to_dbu(rule->layerDiagWidth(i));
            pending_layers.push_back(std::move(layer));
        }

        const NonDefaultRuleId rule_id = reader->root_->create_non_default_rule(std::move(data));
        for (NonDefaultRuleLayerData &layer : pending_layers)
        {
            layer.non_default_rule = rule_id;
            reader->root_->create_non_default_rule_layer(std::move(layer));
        }
        return 0;
    }

    int DEFReader::read_def(std::string filename, Root &root, std::string library_name)
    {
        defrInit();
        messages_.clear();
        g_pending_def_messages.clear();

        defrSetDesignCbk(defrDesignCbkFn);
        defrSetVersionCbk(defrVersionCbkFn);
        defrSetUnitsCbk(defrUnitsCbkFn);
        defrSetDieAreaCbk(defrDieAreaCbkFn);
        defrSetRowCbk(defrRowCbkFn);
        defrSetTrackCbk(defrTrackCbkFn);
        defrSetGcellGridCbk(defrGcellGridCbkFn);
        defrSetComponentCbk(defrComponentCbkFn);
        defrSetPinCbk(defrPinCbkFn);
        defrSetBlockageCbk(defrBlockageCbkFn);
        defrSetViaCbk(defrViaCbkFn);
        defrSetRegionCbk(defrRegionCbkFn);
        defrSetNetCbk(defrNetCbkFn);
        defrSetSNetCbk(defrSNetCbkFn);
        defrSetNonDefaultCbk(defrNonDefaultCbkFn);
        defrSetLogFunction(&DEFReader::defrLogFn);
        defrSetWarningLogFunction(&DEFReader::defrLogFn);

        root_ = &root;
        library_name_ = library_name;
        library_id_ = LibraryId{};
        design_id_ = DesignId{};
        layout_id_ = LayoutId{};
        technology_id_ = root.is_technology_empty() ? root.create_technology(TechnologyData{}) : root.get_technology_ids().front();
        // Reset per read_def() call, same as the ids above - re-derived by
        // defrUnitsCbkFn (which always fires before any geometry callback)
        // once this file's own UNITS statement is seen; 1.0 is the correct
        // default even before that if the technology is still fresh
        // (database_units_microns == 0), or if this DEF happens to omit
        // UNITS entirely (values then pass through unscaled, same as
        // before this fix - not making anything worse for that case).
        unit_scale_ = 1.0;

        std::unique_ptr<FILE, int (*)(FILE *)> file(fopen(filename.c_str(), "r"), &fclose);
        if (!file)
        {
            log_error("Could not open DEF file {}.", filename);
            messages_ = std::move(g_pending_def_messages);
            return 1;
        }

        const int result = defrRead(file.get(), filename.c_str(), (void *)this, 1);
        messages_ = std::move(g_pending_def_messages);
        if (result != 0)
        {
            if (messages_.empty())
                messages_.push_back(fmt::format("ERROR: Could not parse DEF file {}.", filename));
            spdlog::error("Could not parse DEF file {}.", filename);
            return 2;
        }

        return 0;
    }
}
