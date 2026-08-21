#pragma once
#include <string>
#include <vector>
#include "../database/database.hpp"

namespace le
{
    /// @brief Writes a LEF file for a given AbstractId, using the vendored
    /// procedural writer API (lefwWriter.h/.hpp - lefwInit(FILE*) then
    /// direct lefw* calls, matching the vendored sample driver
    /// src/lefdef/lef/lefwrite/lefwrite.cpp, not the alternate callback-
    /// registration lefwWriterCalls.h API).
    ///
    /// UPDATES.md item 12 Phase 1 scope: UNITS (database_units_microns
    /// only), LAYER (the basic scalar properties LEFReader reads - type,
    /// direction, width, pitch, offset, area, spacing, resistance,
    /// capacitance, height, thickness, wire_extension, shrinkage,
    /// cap_multiplier, edge_cap, antenna_area, antenna_length - see
    /// lef_reader.cpp's lefrLayerCbkFn for the exact set and why it stops
    /// there), and MACRO/PIN/PORT/OBS (class, foreigns, size, origin,
    /// symmetry, site, geometry including RECT/PATH/POLYGON ITERATE).
    /// Phase 2 scope: VIA (layers, resistance, foreign, the 5.6
    /// VIARULE-inside-VIA reference) and VIARULE (GENERATE and
    /// non-GENERATE). Phase 3 scope: LAYER spacing/cut-rule completeness
    /// (MINIMUMCUT/MINSTEP/SPACINGTABLE/multi-SPACING), SITE, and
    /// NONDEFAULTRULE. Phase 4 scope: PROPERTYDEFINITIONS and per-instance
    /// PROPERTY (readable everywhere; writable on LAYER/VIA/non-GENERATE
    /// VIARULE/MACRO/PIN - the vendored writer's generic property
    /// functions don't accept NONDEFAULTRULE/SITE/GENERATE-VIARULE write
    /// state, see write_via_rules/write_sites/write_non_default_rules's
    /// own comments). Phase 5 scope: LAYER ANTENNAMODEL (OXIDE1-4, both
    /// ROUTING and CUT layers) and PIN ANTENNA* fields (flat pre-5.5 plus
    /// 5.5 oxide-scoped). Phase 6 scope: the remaining LAYER scalar/table
    /// fields (MASK, two-value PITCH/OFFSET/DIAGPITCH, DIAGSPACING/WIDTH/
    /// MINEDGELENGTH, MAXWIDTH/MINWIDTH/MINSIZE/MINENCLOSEDAREA/
    /// PROTRUSIONWIDTH, ARRAYCUTS/ARRAYSPACING, SPACINGTABLE TWOWIDTHS,
    /// PREFERENCLOSURE, SPLITWIREWIDTH (read-only)/MINIMUMDENSITY/
    /// MAXIMUMDENSITY/DENSITYCHECKSTEP/DENSITYCHECKWINDOW/
    /// FILLACTIVESPACING, AC/DC CURRENTDENSITY, and DIRECTION DIAG45/
    /// DIAG135) plus SPACING's NOTCHLENGTH/ENDOFNOTCHWIDTH (read-only).
    /// RECT/POLYGON MASK color on VIA and the top-level ARRAY section are
    /// still out of scope (deferred to a later phase, see its own plan).
    class LEFWriter
    {
    public:
        /// @brief What to write about the Technology's layers, alongside
        /// (or instead of) `abstract_id`'s own MACRO content - mirrors
        /// UPDATES.md item 12 step 1's own wording ("an option to choose
        /// whether to include Technology layers or not, or just write out
        /// Technology layers").
        enum class LayerWriteMode
        {
            /// Don't write any LAYER statements - just `abstract_id`'s MACRO.
            None,
            /// Write every layer in the Root's (first) Technology, then
            /// `abstract_id`'s own MACRO.
            IncludeWithAbstract,
            /// Write only the Technology's layers - no MACRO at all
            /// (`abstract_id` is ignored in this mode).
            TechnologyOnly,
        };

        /// @brief Writes `path`, returning 0 on success (matches
        /// LEFReader::read_lef's own convention) or a nonzero lefw* error
        /// code (or 1 for a local failure, e.g. the file couldn't be
        /// opened) otherwise. An unknown/invalid `abstract_id` (with a
        /// LayerWriteMode that would write a MACRO) writes just the
        /// Technology layers (if requested) and no MACRO, rather than
        /// failing - Root's own lookups already degrade gracefully for
        /// that, matching this codebase's stated convention.
        int write_lef(const std::string &path, const Root &root, AbstractId abstract_id, LayerWriteMode mode);

        // Messages produced by the most recent write_lef() call - this
        // class's own synthesized error messages (e.g. "could not open
        // file"), not vendored-parser output (the lefw* writer API has no
        // log-function hook the way the lefr* reader API does). Cleared
        // and repopulated at the start of every write_lef() call.
        const std::vector<std::string> &messages() const { return messages_; }

    private:
        static int write_technology_layers(const Root &root, TechnologyId technology_id);
        static int write_units(const Root &root, TechnologyId technology_id);
        static int write_property_definitions(const Root &root, TechnologyId technology_id);
        // Shared by every property-carrying construct's own writer -
        // dispatches to lefwStringProperty/lefwRealProperty based on
        // is_number. Caller is responsible for only calling this while the
        // vendored writer is in a state its generic property functions
        // actually accept (see lef_writer.hpp's own class-level comment).
        // include_numeric defaults true; pass false for a ROUTING layer
        // specifically - lefwRealProperty/lefwIntProperty are missing
        // LEFW_LAYERROUTING(_START) from their own accepted-state list
        // (confirmed in lefwWriter.cpp; lefwStringProperty has it), so
        // numeric properties there are readable but not writable, unlike
        // every other property-carrying construct including CUT layers.
        static int write_properties(const std::vector<LefProperty> &properties, bool include_numeric = true);
        // Shared by every VIA-context FOREIGN write site (top-level VIA,
        // NONDEFAULTRULE-embedded VIA) - lefwViaForeignStr's own xl/yl/
        // orient are each independently optional(0.0)/optional("") (see
        // its own lefwWriter.hpp doc comment), matching Foreign.origin/
        // orient now being genuinely optional (UPDATES.md item 12 - LEF's
        // own FOREIGN grammar allows a bare "FOREIGN name ;" with no point,
        // and a point with no orientation) - passes 0.0/"" through exactly
        // when unset, real converted values otherwise.
        static int write_via_foreign(const ForeignData &foreign, double dbu_per_micron);
        // Shared by both the ROUTING and CUT branches of
        // write_technology_layers - lefwLayerAntennaModel/its per-field
        // siblings accept LEFW_LAYERROUTING(_START) AND LEFW_LAYER(_START)
        // alike (confirmed in lefwWriter.cpp), unlike write_properties'
        // ROUTING-layer numeric gap above.
        // is_cut selects whether the "SideArea"-family fields are written -
        // see write_layer_antenna_models's own comment in lef_writer.cpp
        // for the vendored-writer gap this works around.
        static int write_layer_antenna_models(const std::vector<AntennaModelData> &models, bool is_cut);
        // Shared helper for write_terminal's 4 flat (value, layer)
        // PinAntennaValue lists - dispatches to the matching
        // lefwMacroPinAntenna*(double, const char*) writer per field.
        static int write_pin_antenna_values(const std::vector<PinAntennaValue> &values, int (*writer)(double, const char *));
        // Shared by both AC and DC CURRENTDENSITY - the four writer
        // functions differ per family (lefwLayerAC*/lefwLayerDC*), passed
        // in directly; frequency is nullptr for DC (no such statement -
        // DCCURRENTDENSITY has no FREQUENCY, confirmed absent from
        // lefwWriter.hpp). See the .cpp definition for the one_entry-vs-
        // table dispatch and its own documented vendored-writer edge case
        // (a real value of exactly 0.0 would be misread as "open table
        // form").
        static int write_layer_current_density(
            const std::vector<LayerDensityEntryData> &entries,
            int (*current_density)(const char *, double),
            int (*frequency)(int, double *),
            int (*width)(int, double *),
            int (*cutarea)(int, double *),
            int (*table_entries)(int, double *),
            double dbu_per_micron);
        static int write_vias(const Root &root, TechnologyId technology_id);
        static int write_via_rules(const Root &root, TechnologyId technology_id);
        static int write_sites(const Root &root, TechnologyId technology_id);
        static int write_non_default_rules(const Root &root, TechnologyId technology_id);
        // Shared by write_vias (top-level VIA) and write_non_default_rules
        // (VIA embedded inline in a NONDEFAULTRULE) - writes just the
        // per-layer rect/polygon geometry, matching lefwNonDefaultRuleStartVia's
        // own doc comment that it's followed by the same lefwViaLayer*
        // calls as a top-level VIA.
        static int write_via_layers(const std::vector<ViaLayerData> &layers, double dbu_per_micron);
        static int write_macro(const Root &root, AbstractId abstract_id, double dbu_per_micron);
        static int write_terminal(const Root &root, TerminalId terminal_id, double dbu_per_micron);
        static int write_obstruction(const Root &root, ObstructionId obstruction_id, double dbu_per_micron);
        static int write_shape_geometry(const Shape &shape, double dbu_per_micron, bool is_pin_port);

        std::vector<std::string> messages_;
    };
}
