#pragma once
#include <string>
#include <memory>
#include <expected>
#include <vector>
#include "../lefdef/lef/include/lefrReader.hpp"
#include "../database/database.hpp"
#include "spdlog/spdlog.h"

namespace le
{
    class LEFReader
    {
    public:
        int read_lef(std::string filename, Root &root, std::string library_name);

        // Messages produced by the most recent read_lef() call - parser
        // errors/warnings/info (already fully formatted and prefixed by
        // the vendored parser itself, e.g. "ERROR (LEFPARS-201): ...",
        // "WARNING (LEFPARS-2079): ...", "INFO (LEFPARS-nnn): ...", via
        // lefrSetLogFunction/lefrSetWarningLogFunction - see lefrLogFn),
        // plus this class's own synthesized fopen-failure/fallback
        // messages for the cases the parser callbacks never fire for.
        // Cleared and repopulated at the start of every read_lef() call -
        // reflects only the most recent call, not an accumulation across
        // calls (api.cpp's le_read_lef is what accumulates these into a
        // persistent, ever-growing handle-owned queue for the GUI).
        const std::vector<std::string> &messages() const { return messages_; }

        // Pure LEF-enum -> database-enum conversions. Public (and static, with
        // no dependency on parser/instance state) so they can be unit tested
        // directly rather than only incidentally through a full LEF parse.
        static Orientation orientation_from_parser(int v);
        static RoutingDirection routing_direction_from_parser(const char *name);
        static SignalDirection signal_direction_from_parser(const char *name);

        // x_count/y_count are LEF-file-controlled doubles (a RECT/PATH/
        // POLYGON ITERATE's iteration counts) - an extreme or malformed
        // value would make x_count * y_count overflow size_t's range, and
        // converting an out-of-range (or negative/non-finite) double to an
        // integer type is undefined behavior, not a safe wrap or
        // saturation (same hazard class le_zoom's own fix in api.cpp
        // addresses). Returns 0 (skip reserving, let push_back grow
        // naturally) rather than trusting the file's arithmetic for
        // anything outside a generous but bounded sane range - a real LEF
        // ITERATE count is a handful of repetitions, never anywhere close
        // to this limit. Public (and static, pure) for the same direct-
        // unit-testing reason as the *_from_parser conversions above.
        static size_t safe_iteration_count(double x_count, double y_count);

    private:
        // Callbacks
        static int lefrLayerCbkFn(lefrCallbackType_e typ, lefiLayer *lef_layer, void *user_data);
        static int lefrMacroBeginCbkFn(lefrCallbackType_e typ, const char *name, void *user_data);
        static int lefrUnitsCbkFn(lefrCallbackType_e typ, lefiUnits *lef_units, void *user_data);
        static int lefrMacroCbkFn(lefrCallbackType_e typ, lefiMacro *lef_macro, void *user_data);
        static int lefrPinCbkFn(lefrCallbackType_e typ, lefiPin *lef_pin, void *user_data);
        static int lefrObstructionCbkFn(lefrCallbackType_e typ, lefiObstruction *lef_obs, void *user_data);
        static int lefrBusBitCharsCbkFn(lefrCallbackType_e typ, const char *bus_bit_chars, void *user_data);
        static int lefrDividerCharCbkFn(lefrCallbackType_e typ, const char *divider_char, void *user_data);
        static int lefrViaCbkFn(lefrCallbackType_e typ, lefiVia *lef_via, void *user_data);
        static int lefrViaRuleCbkFn(lefrCallbackType_e typ, lefiViaRule *lef_via_rule, void *user_data);
        static int lefrSiteCbkFn(lefrCallbackType_e typ, lefiSite *lef_site, void *user_data);
        static int lefrNonDefaultCbkFn(lefrCallbackType_e typ, lefiNonDefault *lef_non_default, void *user_data);
        static int lefrDensityCbkFn(lefrCallbackType_e typ, lefiDensity *lef_density, void *user_data);
        static int lefrPropCbkFn(lefrCallbackType_e typ, lefiProp *lef_prop, void *user_data);
        static int lefrFixedMaskCbkFn(lefrCallbackType_e typ, int fixed_mask, void *user_data);
        static int lefrUseMinSpacingCbkFn(lefrCallbackType_e typ, lefiUseMinSpacing *lef_use_min_spacing, void *user_data);
        static int lefrClearanceMeasureCbkFn(lefrCallbackType_e typ, const char *clearance_measure, void *user_data);
        static int lefrManufacturingCbkFn(lefrCallbackType_e typ, double manufacturing_grid, void *user_data);
        static int lefrMaxStackViaCbkFn(lefrCallbackType_e typ, lefiMaxStackVia *lef_max_stack_via, void *user_data);
        static int lefrAntennaInputCbkFn(lefrCallbackType_e typ, double antenna_input_gate_area, void *user_data);
        static int lefrAntennaInoutCbkFn(lefrCallbackType_e typ, double antenna_inout_diff_area, void *user_data);
        static int lefrAntennaOutputCbkFn(lefrCallbackType_e typ, double antenna_output_diff_area, void *user_data);
        static int lefrVersionCbkFn(lefrCallbackType_e typ, double version, void *user_data);

        // Registered for *both* lefrSetLogFunction (errors) and
        // lefrSetWarningLogFunction (warnings and info both route through
        // this one registration point - see lefWarning/lefInfo in the
        // vendored src/lefdef/lef/lef/lef_keywords.cpp) - one function
        // suffices since the distinguishing "ERROR ("/"WARNING ("/
        // "INFO (" prefix is already baked into the message text by the
        // parser itself. Unlike the lefrSet*Cbk callbacks above, this
        // signature (LEFI_LOG_FUNCTION/LEFI_WARNING_LOG_FUNCTION, see
        // lefrReader.hpp) carries no userData - see read_lef's own
        // comment for how messages still reach this specific instance.
        static void lefrLogFn(const char *msg);

        // Helper functions
        int64_t microns_to_dbu(const double microns);
        // Area (LEF AREA/ANTENNAAREA) scales as the *square* of the linear
        // dbu-per-micron factor, unlike every other microns_to_dbu()
        // consumer - a plain microns_to_dbu(area_in_microns_squared) would
        // be wrong by a factor of database_units_microns.
        int64_t microns_squared_to_dbu(const double microns_squared);
        static Polygon polygon_from_parser(LEFReader *reader, int count, double *x, double *y);
        static Rect rect_from_parser(LEFReader *reader, double xl, double yl, double xh, double yh);
        static std::vector<Shape> shapes_from_parser(LEFReader *reader, lefiGeometries *geometries);
        // Shared by lefrViaCbkFn (top-level VIA) and lefrNonDefaultCbkFn
        // (VIA embedded inline in a NONDEFAULTRULE) - lefiVia's per-layer
        // rect/polygon shape is identical in both contexts.
        static std::vector<ViaLayerData> via_layers_from_parser(LEFReader *reader, lefiVia *lef_via);
        static void post_process(LEFReader *reader);

        // Private variables
        Root *root_;
        TechnologyId technology_id_;
        TechnologyData *technology_;
        std::string library_name_;
        LibraryId library_id_;
        AbstractData abstract_data_;
        AbstractId abstract_id_;
        std::vector<std::string> messages_;

        // Set by microns_to_dbu/microns_squared_to_dbu the first time
        // either is called while technology_->database_units_microns is
        // still its unset-sentinel 0 (see lefrUnitsCbkFn's own "0 means
        // unset" convention) - i.e. this file's geometry needed a
        // DATABASE MICRONS declaration that was never read, from this
        // file or an earlier one sharing the same Root/Technology.
        // Checked once at the end of read_lef (not aborted mid-parse from
        // inside a callback) so this doesn't depend on whether returning
        // nonzero from a callback actually stops the vendored parser -
        // simpler to reason about and test.
        bool used_dbu_before_units_declared_ = false;

        // Set by lefrVersionCbkFn from the file's own VERSION statement,
        // 0.0 if never read (a missing VERSION statement is itself a
        // separate lefrRead() parse error, not something this needs to
        // handle). Checked once at the end of read_lef, same
        // not-aborted-mid-parse convention as
        // used_dbu_before_units_declared_ above - this project only
        // supports LEF >= 5.4 (see UPDATES.md item 12 - a deliberate
        // decision not to carry read/write support for the pre-5.4
        // PIN electrical-characteristics fields lef.y itself obsoletes
        // at >= 5.4 anyway).
        double version_ = 0.0;
    };
}
