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
