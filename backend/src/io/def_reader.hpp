#pragma once
#include <string>
#include <vector>
#include <memory>
#include "../lefdef/def/include/defrReader.hpp"
#include "../database/database.hpp"
#include "spdlog/spdlog.h"

namespace le
{
    // First-pass DEFReader (migration Step 1): DESIGN/VERSION/UNITS/DIEAREA/
    // ROW/TRACKS/GCELLGRID/COMPONENTS/PINS/BLOCKAGES/VIAS/REGIONS/NETS/
    // SPECIALNETS/NONDEFAULTRULES - the full Step 1 reader scope (see
    // PROJECT_MIGRATION.md and this project's own plan history). Mirrors
    // LEFReader's own shape (callback-registration-then-defrRead driver,
    // instance-held parser-scratch state, thread_local message-bridging for
    // the no-userData log callbacks) as closely as the two vendored parsers'
    // own APIs allow - see that class's own comments for the reasoning
    // behind each convention repeated here.
    class DEFReader
    {
    public:
        int read_def(std::string filename, Root &root, std::string library_name);

        // See LEFReader::messages() - same contract, cleared and
        // repopulated at the start of every read_def() call.
        const std::vector<std::string> &messages() const { return messages_; }

    private:
        static int defrDesignCbkFn(defrCallbackType_e typ, const char *name, void *user_data);
        static int defrVersionCbkFn(defrCallbackType_e typ, double version, void *user_data);
        static int defrUnitsCbkFn(defrCallbackType_e typ, double units, void *user_data);
        static int defrDieAreaCbkFn(defrCallbackType_e typ, defiBox *box, void *user_data);
        static int defrRowCbkFn(defrCallbackType_e typ, defiRow *row, void *user_data);
        static int defrTrackCbkFn(defrCallbackType_e typ, defiTrack *track, void *user_data);
        static int defrGcellGridCbkFn(defrCallbackType_e typ, defiGcellGrid *grid, void *user_data);
        static int defrComponentCbkFn(defrCallbackType_e typ, defiComponent *component, void *user_data);
        static int defrPinCbkFn(defrCallbackType_e typ, defiPin *pin, void *user_data);
        static int defrBlockageCbkFn(defrCallbackType_e typ, defiBlockage *blockage, void *user_data);
        static int defrViaCbkFn(defrCallbackType_e typ, defiVia *via, void *user_data);
        static int defrRegionCbkFn(defrCallbackType_e typ, defiRegion *region, void *user_data);
        static int defrNetCbkFn(defrCallbackType_e typ, defiNet *net, void *user_data);
        static int defrSNetCbkFn(defrCallbackType_e typ, defiNet *net, void *user_data);
        static int defrNonDefaultCbkFn(defrCallbackType_e typ, defiNonDefault *rule, void *user_data);
        // Shared by both - a NET and a SPECIALNET arrive as the same
        // defiNet type (defrNetCbkFnType), only distinguished by which
        // callback fired.
        static int read_net(defiNet *net, void *user_data, bool is_special);

        // Registered for both defrSetLogFunction and
        // defrSetWarningLogFunction - see LEFReader::lefrLogFn's own
        // comment (this vendored parser's DEFI_LOG_FUNCTION/
        // DEFI_WARNING_LOG_FUNCTION carry no userData either).
        static void defrLogFn(const char *msg);

        // Builds a Polygon from a DIEAREA defiBox - its own >2-point form
        // (5.6+, defiBox::getPoint()) if present, else the plain 2-corner
        // xl/yl/xh/yh rect shorthand (via Geometry::rect_to_polygon, same
        // helper LEFReader's own post_process() fallback uses). unit_scale
        // (see unit_scale_'s own comment) is applied to every point.
        static Polygon polygon_from_die_area(defiBox *box, double unit_scale);

        Root *root_;
        std::string library_name_;
        LibraryId library_id_;
        DesignId design_id_;
        LayoutId layout_id_;
        // Only needed for NONDEFAULTRULES (NonDefaultRule.technology) -
        // reused/created the same "is_technology_empty() ? create :
        // reuse first" way LEFReader::read_lef does, since a DEF file is
        // routinely read after a LEF file already populated the shared
        // Technology.
        TechnologyId technology_id_;
        // Almost every DEF coordinate/dimension value is a raw database-unit
        // integer already expressed at *this file's own* UNITS DISTANCE
        // MICRONS scale (see defrUnitsCbkFn's own comment) - not
        // necessarily the shared Technology's scale, if this DEF disagrees
        // with an already-established Technology (e.g. from an earlier LEF
        // read). Set once by defrUnitsCbkFn (which always fires before any
        // geometry callback - DEF's own grammar puts UNITS ahead of DIEAREA/
        // ROW/TRACKS/... unconditionally) to
        // technology->database_units_microns / (this DEF's own units), so
        // every later raw value gets rescaled onto the Technology's actual
        // shared grid instead of being stored as if it were already there.
        // 1.0 (i.e. a no-op) whenever this DEF's own units already match -
        // the overwhelmingly common case.
        double unit_scale_ = 1.0;
        std::vector<std::string> messages_;
    };
}
