#include <fmt/format.h>
#include "def_reader.hpp"
#include "../geometry/geometry.hpp"
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
    bool fill_pin_like_placement(PinLike *pin, const char *pin_name, std::optional<le::Point> &location, std::optional<le::Orientation> &orientation)
    {
        if (!pin->isPlaced() && !pin->isFixed() && !pin->isCover())
            return true;
        location = le::Point{.x = pin->placementX(), .y = pin->placementY()};
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
    std::vector<le::Shape> shapes_from_pin_like(PinLike *pin)
    {
        std::vector<le::Shape> shapes;
        auto find_or_create = [&](const std::string &layer_name) -> le::Shape &
        {
            for (le::Shape &shape : shapes)
                if (shape.layer_name == layer_name)
                    return shape;
            shapes.push_back(le::Shape{.layer_name = layer_name});
            return shapes.back();
        };

        for (int i = 0; i < pin->numLayer(); i++)
        {
            int xl = 0, yl = 0, xh = 0, yh = 0;
            pin->bounds(i, &xl, &yl, &xh, &yh);
            find_or_create(pin->layer(i)).rects.push_back(le::Rect{.ll = {.x = xl, .y = yl}, .ur = {.x = xh, .y = yh}});
        }
        for (int i = 0; i < pin->numPolygons(); i++)
        {
            const defiPoints points = pin->getPolygon(i);
            le::Polygon polygon;
            polygon.points.reserve(static_cast<size_t>(points.numPoints));
            for (int j = 0; j < points.numPoints; j++)
                polygon.points.push_back(le::Point{.x = points.x[j], .y = points.y[j]});
            find_or_create(pin->polygonName(i)).polygons.push_back(std::move(polygon));
        }
        return shapes;
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

    Polygon DEFReader::polygon_from_die_area(defiBox *box)
    {
        const defiPoints points = box->getPoint();
        if (points.numPoints >= 3)
        {
            Polygon polygon;
            polygon.points.reserve(static_cast<size_t>(points.numPoints));
            for (int i = 0; i < points.numPoints; i++)
                polygon.points.push_back(Point{.x = points.x[i], .y = points.y[i]});
            return polygon;
        }
        // Plain 2-corner shorthand (no explicit point list) - same
        // rect-to-polygon fallback LEFReader::post_process uses for its
        // own SIZE/ORIGIN fallback boundary.
        const Rect bbox{.ll = {.x = box->xl(), .y = box->yl()}, .ur = {.x = box->xh(), .y = box->yh()}};
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

    int DEFReader::defrUnitsCbkFn(defrCallbackType_e /*typ*/, double /*units*/, void * /*user_data*/)
    {
        // DEF coordinate/dimension values (ROW/TRACKS/GCELLGRID/DIEAREA/
        // COMPONENTS placement, etc.) are already expressed directly in
        // database units in the file itself (confirmed against this
        // project's own complete.5.8.def fixture: UNITS DISTANCE MICRONS
        // 1000, yet ROW/TRACKS coordinates are plain integers like 1000/
        // 8400, not 1.0/8.4) - unlike LEF, which is fully micron-based and
        // needs database_units_microns for every microns_to_dbu()
        // conversion. Nothing read so far in this DEFReader pass needs
        // this value at all; kept as a no-op callback (rather than left
        // unregistered) so a future construct that does need it (e.g. a
        // SPECIALNETS field expressed in microns, if one turns out to be)
        // has an obvious place to add real UNITS-vs-Technology validation,
        // mirroring LEFReader's own SecondReadWithDifferentUnitsIsIgnored
        // guard.
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
            .layer_name = "BOUNDARY",
            .polygons = {polygon_from_die_area(box)},
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
            .origin = Point{.x = static_cast<int64_t>(row->x()), .y = static_cast<int64_t>(row->y())},
            .orientation = *orientation,
        };
        if (row->hasDo())
        {
            data.num_x = static_cast<int>(row->xNum());
            data.num_y = static_cast<int>(row->yNum());
        }
        if (row->hasDoStep())
        {
            data.step_x = static_cast<int64_t>(row->xStep());
            data.step_y = static_cast<int64_t>(row->yStep());
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
            .start = static_cast<int64_t>(track->x()),
            .count = static_cast<int>(track->xNum()),
            .step = static_cast<int64_t>(track->xStep()),
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
            .start = static_cast<int64_t>(grid->x()),
            .count = grid->xNum(),
            .step = static_cast<int64_t>(grid->xStep()),
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

        PlacementData data{
            .layout = reader->layout_id_,
            .name = component->id(),
            // defiComponent's own naming is confusing: id() is the
            // instance name, name() is the referenced macro name (set
            // together by IdAndName() from the base "- compId macroName"
            // grammar - def.y's comp_id_and_name rule) - macroName() is a
            // separate, unrelated field only ever populated by a
            // "+ GENERATE genName [pattern]" attribute (setGenerate()),
            // never by the base parse. Confirmed empirically against
            // complete.5.8.def: macroName() was empty for every component,
            // GENERATE or not.
            .reference_name = component->name(),
            // Resolved eagerly if the referenced Design already exists
            // (e.g. its LEF was read earlier into this same Root) - left
            // invalid otherwise, same "linked later" convention as
            // Instance.reference_design (Schematic). No error either way:
            // a DEF COMPONENTS section routinely references macros from a
            // LEF file read as a separate step.
            .reference_design = reader->root_->get_design_by_name(component->name()),
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
            data.location = Point{.x = component->placementX(), .y = component->placementY()};
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
        if (!fill_pin_like_placement(pin, pin->pinName(), data.location, data.orientation))
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
                if (!fill_pin_like_placement(port, pin->pinName(), segment_data.location, segment_data.orientation))
                    return 0;
                const PhysicalPortSegmentId segment_id = reader->root_->create_physical_port_segment(std::move(segment_data));
                for (Shape &shape : shapes_from_pin_like(port))
                {
                    shape.physical_port_segment = segment_id;
                    reader->root_->create_shape(std::move(shape));
                }
            }
        }
        else
        {
            const PhysicalPortSegmentId segment_id = reader->root_->create_physical_port_segment(PhysicalPortSegmentData{.physical_port = physical_port_id});
            for (Shape &shape : shapes_from_pin_like(pin))
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
            data.spacing = blockage->minSpacing();
        if (blockage->hasDesignRuleWidth())
            data.design_rule_width = blockage->designRuleWidth();
        data.is_soft = static_cast<bool>(blockage->hasSoft());
        if (blockage->hasPartial())
            data.placement_max_density = blockage->placementMaxDensity();

        const BlockageId blockage_id = reader->root_->create_blockage(std::move(data));

        if (blockage->numRectangles() > 0 || blockage->numPolygons() > 0)
        {
            // A Blockage is scoped to at most one layer (or none, for a
            // PLACEMENT blockage), unlike a PIN's own per-rect layer
            // array - so unlike shapes_from_pin_like, this needs only one
            // Shape, not one grouped per distinct layer name.
            Shape shape{.layer_name = blockage->hasLayer() ? blockage->layerName() : "", .blockage = blockage_id};
            for (int i = 0; i < blockage->numRectangles(); i++)
                shape.rects.push_back(Rect{.ll = {.x = blockage->xl(i), .y = blockage->yl(i)}, .ur = {.x = blockage->xh(i), .y = blockage->yh(i)}});
            for (int i = 0; i < blockage->numPolygons(); i++)
            {
                const defiPoints points = blockage->getPolygon(i);
                Polygon polygon;
                polygon.points.reserve(static_cast<size_t>(points.numPoints));
                for (int j = 0; j < points.numPoints; j++)
                    polygon.points.push_back(Point{.x = points.x[j], .y = points.y[j]});
                shape.polygons.push_back(std::move(polygon));
            }
            reader->root_->create_shape(std::move(shape));
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
        defrSetLogFunction(&DEFReader::defrLogFn);
        defrSetWarningLogFunction(&DEFReader::defrLogFn);

        root_ = &root;
        library_name_ = library_name;
        library_id_ = LibraryId{};
        design_id_ = DesignId{};
        layout_id_ = LayoutId{};

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
