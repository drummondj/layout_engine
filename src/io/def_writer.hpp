#pragma once
#include <string>
#include <vector>
#include "../database/database.hpp"

namespace le
{
    /// @brief Writes a DEF file for a given LayoutId, using the vendored
    /// procedural writer API (defwWriter.hpp - defwInit(FILE*) then direct
    /// defw* calls), mirroring LEFWriter's own shape as closely as the two
    /// vendored writer APIs allow (fully static, no instance state, walks
    /// Root via the get_x_ids(parent_id) -> get_x(id) idiom).
    ///
    /// Migration Step 1 scope (see PROJECT_MIGRATION.md and this project's
    /// own plan history): DESIGN/VERSION/UNITS/DIEAREA/ROW/TRACKS/
    /// GCELLGRID/COMPONENTS/PINS/BLOCKAGES/VIAS/REGIONS/NETS/SPECIALNETS/
    /// NONDEFAULTRULES - the mirror image of DEFReader's own scope. Net
    /// *connectivity* is out of scope (Route only holds routed geometry,
    /// not which component pins it connects) - same deferral DEFReader
    /// itself already made. GROUPS/STYLES/SLOTS/FILLS/PINPROPERTIES/
    /// SCANCHAINS/CANPLACE/CANNOTOCCUPY/HISTORY are also out of scope, per
    /// the original migration plan.
    class DEFWriter
    {
    public:
        /// @brief Writes `path`, returning 0 on success (matches
        /// DEFReader::read_def's own convention) or a nonzero defw* error
        /// code (or 1 for a local failure, e.g. the file couldn't be
        /// opened, or an invalid `layout_id`) otherwise.
        int write_def(const std::string &path, const Root &root, LayoutId layout_id);

        // Messages produced by the most recent write_def() call - this
        // class's own synthesized error messages, not vendored-parser
        // output (the defw* writer API has no log-function hook, same gap
        // LEFWriter::messages() documents for lefw*). Cleared and
        // repopulated at the start of every write_def() call.
        const std::vector<std::string> &messages() const { return messages_; }

    private:
        // Unlike LEFWriter, none of these take a dbu_per_micron - DEF's
        // own defw* API (defwDieArea, defwRow, defwTracks, defwComponent,
        // defwViaRect, defwNetPathPoint, defwNonDefaultRuleLayer, ...)
        // takes every coordinate as a raw database-unit integer directly
        // (confirmed against defwWriter.cpp's own implementation, e.g.
        // printPoints()'s "%.11g" formatting of a whole-number double) -
        // DEF files themselves have no per-value micron form the way LEF
        // files do, only the single UNITS DISTANCE MICRONS scale factor
        // (written once, in write_def itself). A handful of defw*
        // point-list functions take double* rather than int* purely for
        // formatting convenience, not to signal microns - still raw dbu
        // values, just cast to double.
        static int write_die_area(const Root &root, LayoutId layout_id);
        static int write_rows(const Root &root, LayoutId layout_id);
        static int write_tracks(const Root &root, LayoutId layout_id);
        static int write_gcell_grids(const Root &root, LayoutId layout_id);
        static int write_placements(const Root &root, LayoutId layout_id);
        static int write_physical_ports(const Root &root, LayoutId layout_id);
        static int write_blockages(const Root &root, LayoutId layout_id);
        static int write_vias(const Root &root, LayoutId layout_id);
        static int write_regions(const Root &root, LayoutId layout_id);
        static int write_routes(const Root &root, LayoutId layout_id);
        static int write_non_default_rules(const Root &root, TechnologyId technology_id);
        // Shared by write_placements' STATUS and write_physical_ports'
        // STATUS (PhysicalPort and PhysicalPortSegment alike) - returns
        // nullptr for UNPLACED, matching every defw* status parameter's
        // own optional(NULL) convention for "not yet placed."
        static const char *placement_status_to_string(PlacementStatus status);
        // Shared by write_vias (top-level LayoutVia) and
        // write_non_default_rules' own inline VIA - writes per-layer
        // rect/polygon geometry, mirroring LEFWriter::write_via_layers'
        // own shape (minus the dbu_per_micron conversion - see above).
        static int write_via_layers(const std::vector<ViaLayerData> &layers);
        // Shared by write_routes (regular/special net paths) - walks one
        // Route's Shapes (each already grouped by layer, with one or more
        // Path segments per DEFReader::append_shapes_from_path's own
        // grouping) into a single ROUTED ... (each Shape/Path becoming
        // its own LAYER occurrence, DEF's own NEW-equivalent) block.
        // DEF's writer API has two entirely separate, same-shaped
        // function families for this - defwNetPath*/defwSpecialNetPath* -
        // not one shared family the way defwBlockage*/defwPin* are
        // reused across kinds elsewhere; is_special picks which.
        static int write_net_path(const std::vector<ShapeId> &shape_ids, const Root &root, bool is_special);

        std::vector<std::string> messages_;
    };
}
