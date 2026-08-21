#include "lef_writer.hpp"
#include "../lefdef/lef/include/lefwWriter.hpp"
#include <fmt/format.h>
#include <memory>
#include <cstdio>
#include <cctype>

// The vendored library's C++ writer header (lefwWriter.hpp) wraps every
// lefw* declaration in `namespace LefDefParser` - this codebase's own
// lef_reader.cpp resolves the equivalent reader-side names the same way
// (see its own include of lefrReader.hpp). Using the plain C header
// (lefwWriter.h) instead would pull in lefiTypedefs.h's unconditional
// `#define bool int`, which corrupts any C++ header included afterward -
// confirmed the hard way: it broke fmt/format.h's own template code.
USE_LEFDEF_PARSER_NAMESPACE

namespace
{
    // Inverse of LEFReader::microns_to_dbu (see lef_reader.cpp) - dbu was
    // itself std::llround(microns * dbu_per_micron), so this can't recover
    // the exact original decimal in every case (round-trip through a
    // rounded integer), but matches to within the same dbu-grid precision
    // every other coordinate conversion in this codebase already accepts.
    double to_microns(int64_t dbu, double dbu_per_micron)
    {
        return dbu_per_micron > 0.0 ? static_cast<double>(dbu) / dbu_per_micron : 0.0;
    }

    // Inverse of LEFReader::microns_squared_to_dbu - area (LEF AREA/
    // ANTENNAAREA) scales as the *square* of dbu_per_micron, unlike every
    // other to_microns() call site here.
    double to_microns_squared(int64_t dbu_squared, double dbu_per_micron)
    {
        return dbu_per_micron > 0.0 ? static_cast<double>(dbu_squared) / (dbu_per_micron * dbu_per_micron) : 0.0;
    }

    // lefiProp::propType()'s own setPropType() calls (see lef.y's
    // PROPERTYDEFINITIONS grammar) store the owner keyword lowercase
    // ("layer", "via", ...), but lefwIntPropDef/RealPropDef/StringPropDef
    // validate objType against the uppercase LEF keywords ("LAYER", "VIA",
    // ...) and reject anything else with LEFW_BAD_DATA.
    std::string to_upper(std::string s)
    {
        for (char &c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    const char *routing_direction_to_string(le::RoutingDirection direction)
    {
        // NONE falls back to HORIZONTAL - lefwLayerRouting's direction
        // argument is required (LEF's own ROUTING statement always has
        // one), unlike LEFReader's own read side where DIRECTION is
        // optional per-layer (see lefrLayerCbkFn's own has*() guard).
        switch (direction)
        {
        case le::RoutingDirection::V:
            return "VERTICAL";
        case le::RoutingDirection::DIAG45:
            return "DIAG45";
        case le::RoutingDirection::DIAG135:
            return "DIAG135";
        default:
            return "HORIZONTAL";
        }
    }

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
            return nullptr; // NONE - DIRECTION is optional, matches read side
        }
    }

    const char *orientation_to_string(le::Orientation orient)
    {
        // The enum's own member names already are the LEF orient strings
        // (N/S/E/W/FN/FS/FE/FW) - no lookup table needed, unlike the
        // int-code form LEFReader::orientation_from_parser has to decode.
        switch (orient)
        {
        case le::Orientation::N:
            return "N";
        case le::Orientation::S:
            return "S";
        case le::Orientation::E:
            return "E";
        case le::Orientation::W:
            return "W";
        case le::Orientation::FN:
            return "FN";
        case le::Orientation::FS:
            return "FS";
        case le::Orientation::FE:
            return "FE";
        case le::Orientation::FW:
            return "FW";
        default:
            return "N";
        }
    }
}

namespace le
{
    int LEFWriter::write_property_definitions(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const auto &def_ids = root.get_technology_property_definitions(technology_id);
        if (def_ids.empty())
            return 0;

        int status = lefwStartPropDef();
        if (status)
            return status;

        for (const PropertyDefinitionId def_id : def_ids)
        {
            const PropertyDefinitionData *def_ptr = root.get_property_definition(def_id);
            if (!def_ptr)
                continue;
            const PropertyDefinitionData &def = *def_ptr;
            const double left = def.range_min.value_or(0.0);
            const double right = def.range_max.value_or(0.0);
            const std::string owner_type = to_upper(def.owner_type);
            // 'I'nteger/'R'eal/'S'tring/'Q'uoted-string - Q is written the
            // same way as S (lefwStringPropDef has no separate "quoted"
            // form; the writer always quotes string values regardless -
            // see lefwStringProperty's own "%s \"%s\"" format).
            if (def.data_type == "I")
                status = lefwIntPropDef(owner_type.c_str(), def.name.c_str(), left, right, static_cast<int>(def.default_number.value_or(0.0)));
            else if (def.data_type == "R")
                status = lefwRealPropDef(owner_type.c_str(), def.name.c_str(), left, right, def.default_number.value_or(0.0));
            else
                status = lefwStringPropDef(owner_type.c_str(), def.name.c_str(), left, right, def.default_string ? def.default_string->c_str() : nullptr);
            if (status)
                return status;
        }

        return lefwEndPropDef();
    }

    int LEFWriter::write_properties(const std::vector<LefProperty> &properties, bool include_numeric)
    {
        for (const LefProperty &property : properties)
        {
            if (property.is_number && !include_numeric)
                continue;
            const int status = property.is_number
                                    ? lefwRealProperty(property.name.c_str(), property.number_value)
                                    : lefwStringProperty(property.name.c_str(), property.string_value.c_str());
            if (status)
                return status;
        }
        return 0;
    }

    namespace
    {
        int write_antenna_pwl(int (*writer)(int, double *, double *), const std::vector<AntennaPWLEntry> &entries)
        {
            std::vector<double> diffusions;
            std::vector<double> ratios;
            diffusions.reserve(entries.size());
            ratios.reserve(entries.size());
            for (const AntennaPWLEntry &entry : entries)
            {
                diffusions.push_back(entry.diffusion);
                ratios.push_back(entry.ratio);
            }
            return writer(static_cast<int>(diffusions.size()), diffusions.data(), ratios.data());
        }
    }

    int LEFWriter::write_layer_antenna_models(const std::vector<AntennaModelData> &models, bool is_cut)
    {
        for (const AntennaModelData &model : models)
        {
            int status = lefwLayerAntennaModel(model.oxide.c_str());
            if (status)
                return status;

            if (model.area_ratio)
            {
                status = lefwLayerAntennaAreaRatio(*model.area_ratio);
                if (status)
                    return status;
            }
            if (model.cum_area_ratio)
            {
                status = lefwLayerAntennaCumAreaRatio(*model.cum_area_ratio);
                if (status)
                    return status;
            }
            if (model.area_factor)
            {
                status = lefwLayerAntennaAreaFactor(*model.area_factor, model.area_factor_diffuse_only ? "DIFFUSEONLY" : nullptr);
                if (status)
                    return status;
            }

            // KNOWN VENDORED-LIBRARY GAP: every "SideArea"-family writer
            // (lefwLayerAntennaSideAreaRatio/DiffSideAreaRatio(Pwl)/
            // CumSideAreaRatio/CumDiffSideAreaRatio(Pwl)/SideAreaFactor)
            // checks `!lefwIsRouting` and rejects CUT layers outright
            // (LEFW_BAD_DATA) - unlike AntennaModel/AreaRatio/DiffAreaRatio/
            // CumAreaRatio/AreaFactor above, which all accept
            // `!lefwIsRouting && !lefwIsCut` (confirmed by reading
            // lefwWriter.cpp's own state checks function by function). Some
            // of these functions' own doc comments in lefwWriter.hpp even
            // claim "valid... if the layer type is either ROUTING or CUT",
            // contradicting their actual .cpp implementation. lefiAntennaModel
            // has no such restriction on the read side - a CUT layer's
            // ANTENNASIDEAREARATIO etc. reads fine (lefrLayerCbkFn) - so
            // these fields are readable but not re-writable on a CUT layer
            // with this vendored writer version. Not fixed here (vendored
            // code, per CLAUDE.md's "never edit src/lefdef" rule).
            if (!is_cut)
            {
                if (model.side_area_ratio)
                {
                    status = lefwLayerAntennaSideAreaRatio(*model.side_area_ratio);
                    if (status)
                        return status;
                }
                if (model.cum_side_area_ratio)
                {
                    status = lefwLayerAntennaCumSideAreaRatio(*model.cum_side_area_ratio);
                    if (status)
                        return status;
                }
                if (model.side_area_factor)
                {
                    status = lefwLayerAntennaSideAreaFactor(*model.side_area_factor, model.side_area_factor_diffuse_only ? "DIFFUSEONLY" : nullptr);
                    if (status)
                        return status;
                }
            }

            // Each of these four is EITHER the scalar OR the pwl list,
            // never both (matching lefrLayerCbkFn's own reading of
            // lefiAntennaModel's mutually-exclusive has* flags).
            if (!model.diff_area_ratio_pwl.empty())
                status = write_antenna_pwl(lefwLayerAntennaDiffAreaRatioPwl, model.diff_area_ratio_pwl);
            else if (model.diff_area_ratio)
                status = lefwLayerAntennaDiffAreaRatio(*model.diff_area_ratio);
            else
                status = 0;
            if (status)
                return status;

            if (!model.cum_diff_area_ratio_pwl.empty())
                status = write_antenna_pwl(lefwLayerAntennaCumDiffAreaRatioPwl, model.cum_diff_area_ratio_pwl);
            else if (model.cum_diff_area_ratio)
                status = lefwLayerAntennaCumDiffAreaRatio(*model.cum_diff_area_ratio);
            else
                status = 0;
            if (status)
                return status;

            if (!is_cut)
            {
                if (!model.diff_side_area_ratio_pwl.empty())
                    status = write_antenna_pwl(lefwLayerAntennaDiffSideAreaRatioPwl, model.diff_side_area_ratio_pwl);
                else if (model.diff_side_area_ratio)
                    status = lefwLayerAntennaDiffSideAreaRatio(*model.diff_side_area_ratio);
                else
                    status = 0;
                if (status)
                    return status;

                if (!model.cum_diff_side_area_ratio_pwl.empty())
                    status = write_antenna_pwl(lefwLayerAntennaCumDiffSideAreaRatioPwl, model.cum_diff_side_area_ratio_pwl);
                else if (model.cum_diff_side_area_ratio)
                    status = lefwLayerAntennaCumDiffSideAreaRatio(*model.cum_diff_side_area_ratio);
                else
                    status = 0;
                if (status)
                    return status;
            }
        }
        return 0;
    }

    int LEFWriter::write_pin_antenna_values(const std::vector<PinAntennaValue> &values, int (*writer)(double, const char *))
    {
        for (const PinAntennaValue &entry : values)
        {
            const int status = writer(entry.value, entry.layer_name ? entry.layer_name->c_str() : nullptr);
            if (status)
                return status;
        }
        return 0;
    }

    int LEFWriter::write_layer_current_density(
        const std::vector<LayerDensityEntryData> &entries,
        int (*current_density)(const char *, double),
        int (*frequency)(int, double *),
        int (*width)(int, double *),
        int (*cutarea)(int, double *),
        int (*table_entries)(int, double *),
        double dbu_per_micron)
    {
        for (const LayerDensityEntryData &entry : entries)
        {
            // KNOWN VENDORED-WRITER EDGE CASE: lefwLayerACCurrentDensity/
            // DCCurrentDensity dispatch on `if (value)` - a real one_entry
            // value of exactly 0.0 would be misread as "open table form"
            // with no closing TableEntries call, producing an invalid
            // file. Not a concern for any value seen in complete.5.8.lef;
            // not worked around here (vendored code).
            if (entry.one_entry)
            {
                int status = current_density(entry.type.c_str(), *entry.one_entry);
                if (status)
                    return status;
                continue;
            }

            int status = current_density(entry.type.c_str(), 0.0);
            if (status)
                return status;

            if (frequency && !entry.frequency.empty())
            {
                std::vector<double> frequency_hz = entry.frequency;
                status = frequency(static_cast<int>(frequency_hz.size()), frequency_hz.data());
                if (status)
                    return status;
            }
            if (!entry.width.empty())
            {
                std::vector<double> width_um;
                width_um.reserve(entry.width.size());
                for (int64_t w : entry.width)
                    width_um.push_back(to_microns(w, dbu_per_micron));
                status = width(static_cast<int>(width_um.size()), width_um.data());
                if (status)
                    return status;
            }
            if (!entry.cutarea.empty())
            {
                std::vector<double> cutarea_um2;
                cutarea_um2.reserve(entry.cutarea.size());
                for (int64_t c : entry.cutarea)
                    cutarea_um2.push_back(to_microns_squared(c, dbu_per_micron));
                status = cutarea(static_cast<int>(cutarea_um2.size()), cutarea_um2.data());
                if (status)
                    return status;
            }

            // Required to close the table form.
            std::vector<double> table_values = entry.table_entries;
            status = table_entries(static_cast<int>(table_values.size()), table_values.data());
            if (status)
                return status;
        }
        return 0;
    }

    int LEFWriter::write_units(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology || technology->database_units_microns <= 0.0)
            return 0;

        int status = lefwStartUnits();
        if (status)
            return status;

        // lefwUnits skips writing a sub-statement for any falsy (0.0)
        // argument (confirmed in lefwWriter.cpp) - value_or(0.0) is
        // therefore exactly "omit this one if never read", not a lossy
        // sentinel, for every field here except (in principle) a real
        // value of exactly 0.0, which none of these units ever legitimately
        // are. TIME isn't modeled (no LEFReader field reads it - out of
        // this phase's scope, same as when this function only wrote
        // DATABASE MICRONS).
        status = lefwUnits(0,
                            technology->capacitance_units_pf.value_or(0.0),
                            technology->resistance_units_ohms.value_or(0.0),
                            technology->power_units_mw.value_or(0.0),
                            technology->current_units_ma.value_or(0.0),
                            technology->voltage_units_v.value_or(0.0),
                            technology->database_units_microns);
        if (status)
            return status;

        if (technology->frequency_units_mhz)
        {
            status = lefwUnitsFrequency(*technology->frequency_units_mhz);
            if (status)
                return status;
        }

        return lefwEndUnits();
    }

    int LEFWriter::write_technology_layers(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const double dbu_per_micron = technology->database_units_microns;

        for (LayerId layer_id : root.get_technology_layers(technology_id))
        {
            const LayerData *layer = root.get_layer(layer_id);
            if (!layer)
                continue;

            // Moved to pooled classes (Phase 2) - materialized into plain
            // local vectors once here (both the ROUTING and CUT branches
            // below read the same layer, so this avoids re-materializing
            // per branch), rather than reading layer->field directly.
            std::vector<SpacingRuleData> spacing_rules;
            for (const SpacingRuleId id : root.get_layer_spacing_rules(layer_id))
                if (const SpacingRuleData *data = root.get_spacing_rule(id))
                    spacing_rules.push_back(*data);
            std::vector<MinimumCutData> minimum_cuts;
            for (const MinimumCutId id : root.get_layer_minimum_cuts(layer_id))
                if (const MinimumCutData *data = root.get_minimum_cut(id))
                    minimum_cuts.push_back(*data);
            std::vector<MinStepData> min_steps;
            for (const MinStepId id : root.get_layer_min_steps(layer_id))
                if (const MinStepData *data = root.get_min_step(id))
                    min_steps.push_back(*data);
            std::vector<InfluenceSpacingEntryData> spacing_table_influence;
            for (const InfluenceSpacingEntryId id : root.get_layer_spacing_table_influence(layer_id))
                if (const InfluenceSpacingEntryData *data = root.get_influence_spacing_entry(id))
                    spacing_table_influence.push_back(*data);
            std::vector<TwoWidthsSpacingEntryData> spacing_table_two_widths;
            for (const TwoWidthsSpacingEntryId id : root.get_layer_spacing_table_two_widths(layer_id))
                if (const TwoWidthsSpacingEntryData *data = root.get_two_widths_spacing_entry(id))
                    spacing_table_two_widths.push_back(*data);
            std::vector<PreferEnclosureEntryData> prefer_enclosures;
            for (const PreferEnclosureEntryId id : root.get_layer_prefer_enclosures(layer_id))
                if (const PreferEnclosureEntryData *data = root.get_prefer_enclosure_entry(id))
                    prefer_enclosures.push_back(*data);
            std::vector<EnclosureEntryData> enclosures;
            for (const EnclosureEntryId id : root.get_layer_enclosures(layer_id))
                if (const EnclosureEntryData *data = root.get_enclosure_entry(id))
                    enclosures.push_back(*data);
            std::vector<LayerDensityEntryData> ac_current_density;
            for (const LayerDensityEntryId id : root.get_layer_ac_current_density(layer_id))
                if (const LayerDensityEntryData *data = root.get_layer_density_entry(id))
                    ac_current_density.push_back(*data);
            std::vector<LayerDensityEntryData> dc_current_density;
            for (const LayerDensityEntryId id : root.get_layer_dc_current_density(layer_id))
                if (const LayerDensityEntryData *data = root.get_layer_density_entry(id))
                    dc_current_density.push_back(*data);
            std::vector<AntennaModelData> antenna_models;
            for (const AntennaModelId id : root.get_layer_antenna_models(layer_id))
                if (const AntennaModelData *data = root.get_antenna_model(id))
                    antenna_models.push_back(*data);
            const ArraySpacingId array_spacing_id = root.get_layer_array_spacing(layer_id);
            const ArraySpacingData *array_spacing = root.get_array_spacing(array_spacing_id);

            const bool is_routing = layer->type == "ROUTING";
            int status;

            if (is_routing)
            {
                status = lefwStartLayerRouting(layer->name.c_str());
                if (status)
                    return status;

                // lefwLayerMask only accepts LEFW_LAYERROUTING_START (not
                // LEFW_LAYERROUTING) - must be written before the
                // lefwLayerRouting() call below transitions state away
                // from _START (confirmed in lefwWriter.cpp; matches the
                // fixture's own MASK-right-after-TYPE ordering).
                if (layer->default_mask)
                {
                    status = lefwLayerMask(*layer->default_mask);
                    if (status)
                        return status;
                }

                status = lefwLayerRouting(routing_direction_to_string(layer->direction), layer->width ? to_microns(*layer->width, dbu_per_micron) : 0.0);
                if (status)
                    return status;

                // pitch_xy/offset_xy (two-value form) are mutually
                // exclusive with pitch/offset (single-value form).
                //
                // KNOWN VENDORED-WRITER GAP: pitch_xy/offset_xy/diag_pitch/
                // diag_pitch_xy/diag_spacing/diag_width all require
                // lefwIsRouting (confirmed in lefwWriter.cpp) - unwritable
                // on a CUT layer even though the reader can populate them
                // there (e.g. complete.5.8.lef's LAYER CUT01, TYPE CUT,
                // uses DIAGPITCH/two-value PITCH/two-value OFFSET). Still
                // fully read; just never re-written for a CUT layer.
                if (layer->pitch_xy)
                {
                    status = lefwLayerRoutingPitchXYDistance(to_microns(layer->pitch_xy->x, dbu_per_micron), to_microns(layer->pitch_xy->y, dbu_per_micron));
                    if (status)
                        return status;
                }
                else if (layer->pitch)
                {
                    status = lefwLayerRoutingPitch(to_microns(*layer->pitch, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->offset_xy)
                {
                    status = lefwLayerRoutingOffsetXYDistance(to_microns(layer->offset_xy->x, dbu_per_micron), to_microns(layer->offset_xy->y, dbu_per_micron));
                    if (status)
                        return status;
                }
                else if (layer->offset)
                {
                    status = lefwLayerRoutingOffset(to_microns(*layer->offset, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->area)
                {
                    status = lefwLayerRoutingArea(to_microns_squared(*layer->area, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->diag_pitch_xy)
                {
                    status = lefwLayerRoutingDiagPitchXYDistance(to_microns(layer->diag_pitch_xy->x, dbu_per_micron), to_microns(layer->diag_pitch_xy->y, dbu_per_micron));
                    if (status)
                        return status;
                }
                else if (layer->diag_pitch)
                {
                    status = lefwLayerRoutingDiagPitch(to_microns(*layer->diag_pitch, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->diag_spacing)
                {
                    status = lefwLayerRoutingDiagSpacing(to_microns(*layer->diag_spacing, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->diag_width)
                {
                    status = lefwLayerRoutingDiagWidth(to_microns(*layer->diag_width, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->diag_min_edge_length)
                {
                    status = lefwLayerRoutingDiagMinEdgeLength(to_microns(*layer->diag_min_edge_length, dbu_per_micron));
                    if (status)
                        return status;
                }
                for (const SpacingRuleData &rule : spacing_rules)
                {
                    status = lefwLayerRoutingSpacing(to_microns(rule.distance, dbu_per_micron));
                    if (status)
                        return status;

                    // rule.center_to_center is deliberately never written
                    // here: lefwWriter.hpp's own comments list
                    // lefwLayerSpacingCenterToCenter (the ROUTING-layer
                    // CENTERTOCENTER writer) as "obsoleted in 5.7" with no
                    // replacement - only lefwLayerCutSpacingCenterToCenter
                    // (CUT layers, used below) still exists. Still fully
                    // read (lefrLayerCbkFn), just unwritable for ROUTING
                    // via this vendored writer version.

                    // At most one of RANGE/LENGTHTHRESHOLD/SAMENET follows a
                    // ROUTING SPACING statement (lefwWriter.hpp's own "either
                    // this routine ... or ..." comments on each).
                    // "SPACING d LENGTHTHRESHOLD len RANGE min max ;" has its
                    // own dedicated trailing-range fields in the vendored
                    // reader (lefiLayer::hasSpacingLengthThresholdRange()/
                    // spacingLengthThresholdRangeMin()/Max()) - a genuinely
                    // separate pair from hasSpacingRange()/spacingRangeMin()/
                    // Max() used by the plain "SPACING d RANGE min max ...;"
                    // construct below, even though lefwLayerRoutingSpacingLengthThreshold
                    // also happens to print its own min/max as "RANGE %g %g"
                    // text - confirmed the hard way (a length_threshold_range_min/
                    // max of exactly 0/0.1 silently wrote no RANGE clause at
                    // all when this branch was still reading range_min/
                    // range_max instead of its own dedicated fields).
                    if (rule.length_threshold)
                    {
                        status = lefwLayerRoutingSpacingLengthThreshold(to_microns(*rule.length_threshold, dbu_per_micron),
                                                                         rule.length_threshold_range_min ? to_microns(*rule.length_threshold_range_min, dbu_per_micron) : 0.0,
                                                                         rule.length_threshold_range_max ? to_microns(*rule.length_threshold_range_max, dbu_per_micron) : 0.0);
                        if (status)
                            return status;
                    }
                    else if (rule.range_min && rule.range_max)
                    {
                        status = lefwLayerRoutingSpacingRange(to_microns(*rule.range_min, dbu_per_micron), to_microns(*rule.range_max, dbu_per_micron));
                        if (status)
                            return status;

                        if (rule.range_use_length_threshold)
                        {
                            status = lefwLayerRoutingSpacingRangeUseLengthThreshold();
                            if (status)
                                return status;
                        }
                        else if (rule.range_influence)
                        {
                            status = lefwLayerRoutingSpacingRangeInfluence(to_microns(*rule.range_influence, dbu_per_micron),
                                                                            rule.range_influence_range_min ? to_microns(*rule.range_influence_range_min, dbu_per_micron) : 0.0,
                                                                            rule.range_influence_range_max ? to_microns(*rule.range_influence_range_max, dbu_per_micron) : 0.0);
                            if (status)
                                return status;
                        }
                        else if (rule.range_range_min && rule.range_range_max)
                        {
                            status = lefwLayerRoutingSpacingRangeRange(to_microns(*rule.range_range_min, dbu_per_micron), to_microns(*rule.range_range_max, dbu_per_micron));
                            if (status)
                                return status;
                        }
                    }
                    else if (rule.same_net)
                    {
                        status = lefwLayerRoutingSpacingSameNet(rule.same_net_pg_only ? 1 : 0);
                        if (status)
                            return status;
                    }

                    // KNOWN VENDORED-LIBRARY BUG: lefwLayerRoutingSpacingEndOfLine
                    // unconditionally flushes (";\n") whatever SPACING
                    // statement is still open *before* writing "ENDOFLINE
                    // ...", producing an orphaned top-level "ENDOFLINE ..."
                    // statement - but lef.y's own grammar only ever
                    // accepts K_ENDOFLINE nested inside a SPACING
                    // statement's own layer_spacing_cut_routing option (one
                    // grammar occurrence, confirmed by grep), so the
                    // written file is unparseable on re-read. Not fixed
                    // (vendored code). ENDOFLINE/PARALLELEDGE/TWOEDGES are
                    // still fully read (see lefrLayerCbkFn) - just never
                    // re-written.

                    // SAME BUG, same root cause: lefwLayerRoutingSpacingNotchLength/
                    // SpacingEndOfNotchWidth also flush the open SPACING
                    // statement and emit NOTCHLENGTH/ENDOFNOTCHWIDTH as a
                    // separate top-level statement, but lef.y only accepts
                    // them nested inside SPACING's own option grammar - not
                    // called here either. rule.notch_length/end_of_notch_*
                    // are read-only.
                }

                for (const MinimumCutData &cut : minimum_cuts)
                {
                    if (cut.within)
                    {
                        status = lefwLayerRoutingMinimumcutWithin(cut.cuts, to_microns(cut.width, dbu_per_micron), to_microns(*cut.within, dbu_per_micron));
                        if (status)
                            return status;
                    }
                    else
                    {
                        status = lefwLayerRoutingMinimumcut(cut.cuts, to_microns(cut.width, dbu_per_micron));
                        if (status)
                            return status;
                    }

                    if (cut.connection)
                    {
                        status = lefwLayerRoutingMinimumcutConnections(cut.connection->c_str());
                        if (status)
                            return status;
                    }
                    if (cut.length && cut.distance)
                    {
                        status = lefwLayerRoutingMinimumcutLengthWithin(to_microns(*cut.length, dbu_per_micron), to_microns(*cut.distance, dbu_per_micron));
                        if (status)
                            return status;
                    }
                }

                for (const MinStepData &step : min_steps)
                {
                    if (step.max_edges)
                    {
                        status = lefwLayerRoutingMinstepMaxEdges(to_microns(step.distance, dbu_per_micron), *step.max_edges);
                        if (status)
                            return status;
                    }
                    else if (step.min_step_type || step.lengthsum)
                    {
                        status = lefwLayerRoutingMinstepWithOptions(to_microns(step.distance, dbu_per_micron), step.min_step_type ? step.min_step_type->c_str() : nullptr, step.lengthsum ? to_microns(*step.lengthsum, dbu_per_micron) : 0.0);
                        if (status)
                            return status;
                    }
                    else
                    {
                        status = lefwLayerRoutingMinstep(to_microns(step.distance, dbu_per_micron));
                        if (status)
                            return status;
                    }
                }

                if (layer->spacing_table_parallel_run_length)
                {
                    const ParallelRunLengthSpacingTable &table = *layer->spacing_table_parallel_run_length;
                    std::vector<double> lengths_um;
                    lengths_um.reserve(table.lengths.size());
                    for (int64_t length : table.lengths)
                        lengths_um.push_back(to_microns(length, dbu_per_micron));

                    status = lefwLayerRoutingStartSpacingtableParallel(static_cast<int>(lengths_um.size()), lengths_um.data());
                    if (status)
                        return status;

                    const size_t num_lengths = table.lengths.size();
                    for (size_t w = 0; w < table.widths.size(); w++)
                    {
                        std::vector<double> row_um;
                        row_um.reserve(num_lengths);
                        for (size_t l = 0; l < num_lengths; l++)
                            row_um.push_back(to_microns(table.spacings[w * num_lengths + l], dbu_per_micron));

                        status = lefwLayerRoutingSpacingtableParallelWidth(to_microns(table.widths[w], dbu_per_micron), static_cast<int>(row_um.size()), row_um.data());
                        if (status)
                            return status;
                    }

                    // lefwLayerRoutineEndSpacingtable (sic - the vendored
                    // header really does spell it "Routine", not "Routing")
                    // is the only call that resets lefwState from
                    // LEFW_LAYERROUTINGWIDTH (where the last
                    // SpacingtableParallelWidth call above leaves it) back
                    // to LEFW_LAYERROUTING - every other layer-routing
                    // writer function, including lefwEndLayerRouting
                    // itself, rejects LEFW_LAYERROUTINGWIDTH outright, so
                    // skipping this call would silently break every
                    // statement written after a SPACINGTABLE (found via
                    // lefwWriter.cpp's own state-constant checks, then
                    // confirmed against the vendored sample driver
                    // src/lefdef/lef/lefwrite/lefwrite.cpp's own usage,
                    // which is the only place this misspelled function name
                    // turns up).
                    status = lefwLayerRoutineEndSpacingtable();
                    if (status)
                        return status;
                }

                if (!spacing_table_influence.empty())
                {
                    status = lefwLayerRoutingStartSpacingtableInfluence();
                    if (status)
                        return status;

                    for (const InfluenceSpacingEntryData &entry : spacing_table_influence)
                    {
                        status = lefwLayerRoutingSpacingInfluenceWidth(to_microns(entry.width, dbu_per_micron), to_microns(entry.distance, dbu_per_micron), to_microns(entry.spacing, dbu_per_micron));
                        if (status)
                            return status;
                    }

                    status = lefwLayerRoutineEndSpacingtable();
                    if (status)
                        return status;
                }

                if (!spacing_table_two_widths.empty())
                {
                    // lefwLayerRoutingStartSpacingtableTwoWidths is
                    // ROUTING-only (LEFW_LAYERROUTING_START/LEFW_LAYERROUTING
                    // - confirmed in lefwWriter.cpp) despite TWOWIDTHS
                    // appearing on layers named "cutNN" in complete.5.8.lef
                    // (e.g. cut25) - those are actually TYPE ROUTING, not
                    // CUT, per the fixture's own TYPE statement.
                    status = lefwLayerRoutingStartSpacingtableTwoWidths();
                    if (status)
                        return status;
                    // KNOWN VENDORED-WRITER EDGE CASE:
                    // lefwLayerRoutingSpacingtableTwoWidthsWidth checks
                    // `if (runLength)` to decide whether to write "PRL ..."
                    // at all - a real PRL of exactly 0.0 (present in
                    // complete.5.8.lef's own "WIDTH 0.25 PRL 0.0 ...") is
                    // indistinguishable from "no PRL" and gets silently
                    // dropped. Not worked around here (vendored code).
                    for (const TwoWidthsSpacingEntryData &entry : spacing_table_two_widths)
                    {
                        std::vector<double> spacings_um;
                        spacings_um.reserve(entry.spacings.size());
                        for (int64_t spacing : entry.spacings)
                            spacings_um.push_back(to_microns(spacing, dbu_per_micron));
                        status = lefwLayerRoutingSpacingtableTwoWidthsWidth(to_microns(entry.width, dbu_per_micron), entry.prl ? to_microns(*entry.prl, dbu_per_micron) : 0.0, static_cast<int>(spacings_um.size()), spacings_um.data());
                        if (status)
                            return status;
                    }
                    status = lefwLayerRoutineEndSpacingtable();
                    if (status)
                        return status;
                }

                if (layer->wire_extension)
                {
                    status = lefwLayerRoutingWireExtension(to_microns(*layer->wire_extension, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->resistance)
                {
                    status = lefwLayerRoutingResistance(fmt::format("{}", *layer->resistance).c_str());
                    if (status)
                        return status;
                }
                if (layer->capacitance)
                {
                    status = lefwLayerRoutingCapacitance(fmt::format("{}", *layer->capacitance).c_str());
                    if (status)
                        return status;
                }
                if (layer->height)
                {
                    status = lefwLayerRoutingHeight(to_microns(*layer->height, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->thickness)
                {
                    status = lefwLayerRoutingThickness(to_microns(*layer->thickness, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->shrinkage)
                {
                    status = lefwLayerRoutingShrinkage(to_microns(*layer->shrinkage, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->cap_multiplier)
                {
                    status = lefwLayerRoutingCapMultiplier(*layer->cap_multiplier);
                    if (status)
                        return status;
                }
                if (layer->edge_cap)
                {
                    status = lefwLayerRoutingEdgeCap(*layer->edge_cap);
                    if (status)
                        return status;
                }
                if (layer->antenna_length)
                {
                    status = lefwLayerRoutingAntennaLength(to_microns(*layer->antenna_length, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->max_width)
                {
                    status = lefwLayerRoutingMaxwidth(to_microns(*layer->max_width, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->min_width)
                {
                    status = lefwLayerRoutingMinwidth(to_microns(*layer->min_width, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (!layer->min_sizes.empty())
                {
                    std::vector<double> min_widths_um;
                    std::vector<double> min_lengths_um;
                    min_widths_um.reserve(layer->min_sizes.size());
                    min_lengths_um.reserve(layer->min_sizes.size());
                    for (const MinSizeEntry &entry : layer->min_sizes)
                    {
                        min_widths_um.push_back(to_microns(entry.width, dbu_per_micron));
                        min_lengths_um.push_back(to_microns(entry.length, dbu_per_micron));
                    }
                    status = lefwLayerRoutingMinsize(static_cast<int>(min_widths_um.size()), min_widths_um.data(), min_lengths_um.data());
                    if (status)
                        return status;
                }
                if (!layer->min_enclosed_areas.empty())
                {
                    std::vector<double> areas_um2;
                    std::vector<double> widths_um;
                    areas_um2.reserve(layer->min_enclosed_areas.size());
                    widths_um.reserve(layer->min_enclosed_areas.size());
                    for (const MinEnclosedAreaEntry &entry : layer->min_enclosed_areas)
                    {
                        areas_um2.push_back(to_microns_squared(entry.area, dbu_per_micron));
                        widths_um.push_back(entry.width ? to_microns(*entry.width, dbu_per_micron) : 0.0);
                    }
                    status = lefwLayerRoutingMinenclosedarea(static_cast<int>(areas_um2.size()), areas_um2.data(), widths_um.data());
                    if (status)
                        return status;
                }
                if (layer->protrusion_width1)
                {
                    status = lefwLayerRoutingProtrusion(to_microns(*layer->protrusion_width1, dbu_per_micron), to_microns(*layer->protrusion_length, dbu_per_micron), to_microns(*layer->protrusion_width2, dbu_per_micron));
                    if (status)
                        return status;
                }
                // layer->split_wire_width is deliberately never written -
                // no lefwLayer*SplitWireWidth* function exists anywhere in
                // the vendored writer (grepped lefwWriter.hpp/.cpp - only
                // an internal LEFW_SPLITWIREWIDTH state-name constant).
                // Still fully read (lefrLayerCbkFn) - just unwritable.
                if (layer->minimum_density)
                {
                    status = lefwMinimumDensity(*layer->minimum_density);
                    if (status)
                        return status;
                }
                if (layer->maximum_density)
                {
                    status = lefwMaximumDensity(*layer->maximum_density);
                    if (status)
                        return status;
                }
                if (layer->density_check_step)
                {
                    status = lefwDensityCheckStep(to_microns(*layer->density_check_step, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->density_check_window)
                {
                    status = lefwDensityCheckWindow(to_microns(layer->density_check_window->length, dbu_per_micron), to_microns(layer->density_check_window->width, dbu_per_micron));
                    if (status)
                        return status;
                }
                if (layer->fill_active_spacing)
                {
                    status = lefwFillActiveSpacing(to_microns(*layer->fill_active_spacing, dbu_per_micron));
                    if (status)
                        return status;
                }

                status = write_layer_current_density(ac_current_density, lefwLayerACCurrentDensity, lefwLayerACFrequency, lefwLayerACWidth, lefwLayerACCutarea, lefwLayerACTableEntries, dbu_per_micron);
                if (status)
                    return status;
                status = write_layer_current_density(dc_current_density, lefwLayerDCCurrentDensity, nullptr, lefwLayerDCWidth, lefwLayerDCCutarea, lefwLayerDCTableEntries, dbu_per_micron);
                if (status)
                    return status;

                status = write_layer_antenna_models(antenna_models, /*is_cut=*/false);
                if (status)
                    return status;

                status = write_properties(layer->properties, /*include_numeric=*/false);
                if (status)
                    return status;

                status = lefwEndLayerRouting(layer->name.c_str());
                if (status)
                    return status;
            }
            else
            {
                status = lefwStartLayer(layer->name.c_str(), layer->type.c_str());
                if (status)
                    return status;

                if (layer->default_mask)
                {
                    status = lefwLayerMask(*layer->default_mask);
                    if (status)
                        return status;
                }

                if (layer->width)
                {
                    status = lefwLayerWidth(to_microns(*layer->width, dbu_per_micron));
                    if (status)
                        return status;
                }
                for (const SpacingRuleData &rule : spacing_rules)
                {
                    status = lefwLayerCutSpacing(to_microns(rule.distance, dbu_per_micron));
                    if (status)
                        return status;

                    if (rule.center_to_center)
                    {
                        status = lefwLayerCutSpacingCenterToCenter();
                        if (status)
                            return status;
                    }
                    if (rule.same_net)
                    {
                        status = lefwLayerCutSpacingSameNet();
                        if (status)
                            return status;
                    }

                    // At most one of LAYER/ADJACENTCUTS/PARALLELOVERLAP/AREA
                    // follows a CUT SPACING statement (lefwWriter.hpp's own
                    // "either this routine ... or ..." comments on each).
                    if (rule.second_layer_name)
                    {
                        status = lefwLayerCutSpacingLayer(rule.second_layer_name->c_str(), rule.second_layer_stack ? 1 : 0);
                        if (status)
                            return status;
                    }
                    else if (rule.adjacent_cuts)
                    {
                        status = lefwLayerCutSpacingAdjacent(*rule.adjacent_cuts, rule.adjacent_within ? to_microns(*rule.adjacent_within, dbu_per_micron) : 0.0, rule.adjacent_except_same_pg_net ? 1 : 0);
                        if (status)
                            return status;
                    }
                    else if (rule.parallel_overlap)
                    {
                        status = lefwLayerCutSpacingParallel();
                        if (status)
                            return status;
                    }
                    else if (rule.area)
                    {
                        status = lefwLayerCutSpacingArea(to_microns(*rule.area, dbu_per_micron));
                        if (status)
                            return status;
                    }

                    status = lefwLayerCutSpacingEnd();
                    if (status)
                        return status;
                }

                if (!layer->spacing_table_orthogonal.empty())
                {
                    std::vector<double> cut_withins_um;
                    std::vector<double> ortho_spacings_um;
                    cut_withins_um.reserve(layer->spacing_table_orthogonal.size());
                    ortho_spacings_um.reserve(layer->spacing_table_orthogonal.size());
                    for (const OrthogonalSpacingEntry &entry : layer->spacing_table_orthogonal)
                    {
                        cut_withins_um.push_back(to_microns(entry.cut_within, dbu_per_micron));
                        ortho_spacings_um.push_back(to_microns(entry.ortho_spacing, dbu_per_micron));
                    }
                    status = lefwLayerCutSpacingTableOrtho(static_cast<int>(cut_withins_um.size()), cut_withins_um.data(), ortho_spacings_um.data());
                    if (status)
                        return status;
                }

                // KNOWN VENDORED-WRITER GAP: lefwLayerArraySpacing requires
                // lefwIsCut (confirmed in lefwWriter.cpp) - unwritable on a
                // layer whose TYPE is ROUTING even though the reader can
                // populate it there (e.g. complete.5.8.lef's LAYER cut24,
                // TYPE ROUTING, uses ARRAYSPACING). Still fully read; just
                // never re-written for a ROUTING-typed layer.
                if (!layer->array_cuts.empty() || array_spacing)
                {
                    std::vector<int> cuts;
                    std::vector<double> spacings_um;
                    cuts.reserve(layer->array_cuts.size());
                    spacings_um.reserve(layer->array_cuts.size());
                    for (const ArrayCutsEntry &entry : layer->array_cuts)
                    {
                        cuts.push_back(entry.cuts);
                        spacings_um.push_back(to_microns(entry.spacing, dbu_per_micron));
                    }
                    const bool long_array = array_spacing && array_spacing->long_array;
                    const double via_width_um = (array_spacing && array_spacing->via_width) ? to_microns(*array_spacing->via_width, dbu_per_micron) : 0.0;
                    const double cut_spacing_um = array_spacing ? to_microns(array_spacing->cut_spacing, dbu_per_micron) : 0.0;
                    status = lefwLayerArraySpacing(long_array ? 1 : 0, via_width_um, cut_spacing_um, static_cast<int>(cuts.size()), cuts.data(), spacings_um.data());
                    if (status)
                        return status;
                }

                for (const PreferEnclosureEntryData &entry : prefer_enclosures)
                {
                    status = lefwLayerPreferEnclosure(entry.location.value_or("").c_str(), to_microns(entry.overhang1, dbu_per_micron), to_microns(entry.overhang2, dbu_per_micron), entry.min_width ? to_microns(*entry.min_width, dbu_per_micron) : 0.0);
                    if (status)
                        return status;
                }

                // width/except_extra_cut and min_length are mutually
                // exclusive per the vendored writer's own three ENCLOSURE
                // variants (see lefrLayerCbkFn's own matching read side).
                for (const EnclosureEntryData &entry : enclosures)
                {
                    if (entry.width)
                        status = lefwLayerEnclosureWidth(entry.location.value_or("").c_str(), to_microns(entry.overhang1, dbu_per_micron), to_microns(entry.overhang2, dbu_per_micron), to_microns(*entry.width, dbu_per_micron), entry.except_extra_cut ? to_microns(*entry.except_extra_cut, dbu_per_micron) : 0.0);
                    else if (entry.min_length)
                        status = lefwLayerEnclosureLength(entry.location.value_or("").c_str(), to_microns(entry.overhang1, dbu_per_micron), to_microns(entry.overhang2, dbu_per_micron), to_microns(*entry.min_length, dbu_per_micron));
                    else
                        status = lefwLayerEnclosure(entry.location.value_or("").c_str(), to_microns(entry.overhang1, dbu_per_micron), to_microns(entry.overhang2, dbu_per_micron), 0.0);
                    if (status)
                        return status;
                }

                // KNOWN VENDORED-LIBRARY BUG: lefwLayerResistancePerCut
                // literally writes the keyword "RESISTANCEPERCUT", but
                // lef.y has no such token anywhere - the real CUT-layer
                // grammar rule is plain "RESISTANCE <value> ;" (same
                // keyword as ROUTING's own, just a different lefiLayer
                // accessor pair - see lefrLayerCbkFn's own comment). A file
                // written with this call fails to re-parse entirely
                // (confirmed: LEFPARS-1 "encountered an error ... on token
                // RESISTANCEPERCUT"). Not called here - layer->resistance
                // is read-only for CUT layers with this vendored writer
                // version (still fully written for ROUTING layers above).

                status = write_layer_current_density(ac_current_density, lefwLayerACCurrentDensity, lefwLayerACFrequency, lefwLayerACWidth, lefwLayerACCutarea, lefwLayerACTableEntries, dbu_per_micron);
                if (status)
                    return status;
                status = write_layer_current_density(dc_current_density, lefwLayerDCCurrentDensity, nullptr, lefwLayerDCWidth, lefwLayerDCCutarea, lefwLayerDCTableEntries, dbu_per_micron);
                if (status)
                    return status;

                status = write_layer_antenna_models(antenna_models, /*is_cut=*/true);
                if (status)
                    return status;

                status = write_properties(layer->properties);
                if (status)
                    return status;

                status = lefwEndLayer(layer->name.c_str());
                if (status)
                    return status;
            }
        }

        return 0;
    }

    int LEFWriter::write_via_layers(const std::vector<ViaLayerData> &layers, double dbu_per_micron)
    {
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        for (const ViaLayerData &layer : layers)
        {
            int status = lefwViaLayer(layer.layer_name.c_str());
            if (status)
                return status;

            // rect_masks is a parallel array (see Shape.rect_masks's own
            // schema comment for the same convention) - shorter than
            // rects, or empty, if no rect in this layer ever set a mask.
            for (size_t i = 0; i < layer.rects.size(); i++)
            {
                const Rect &rect = layer.rects[i];
                const int mask = i < layer.rect_masks.size() ? layer.rect_masks[i] : 0;
                status = lefwViaLayerRect(to_um(rect.ll.x), to_um(rect.ll.y), to_um(rect.ur.x), to_um(rect.ur.y), mask);
                if (status)
                    return status;
            }

            // KNOWN VENDORED-LIBRARY BUG (lefwWriter.cpp's own
            // lefwViaLayerPolygon, not our code, so not something to
            // hand-edit per CLAUDE.md's "never edit src/lefdef/" rule):
            // its non-encrypted branch prints the first point as
            // "%.11g %.11g" (no trailing separator) and every later
            // point as "%.11g %.11g " (no LEADING separator either),
            // so point 0's y and point 1's x land back-to-back with
            // zero characters between them - e.g. y0=-1, x1=-0.2
            // writes as the single unparseable token "-1-0.2" (found
            // via the lef_roundtrip_diff dev tool against
            // complete.5.8.lef's myVia23, which has real via-layer
            // POLYGON geometry - it made lefdiff choke partway through
            // and silently truncate the rest of that dump, hiding every
            // real diff after it - NONDEFAULTRULE/SITE were never
            // actually missing, lefdiff just never got that far). Every
            // OTHER polygon writer in this file (lefwMacroPinPortLayerPolygon/
            // lefwMacroObsLayerPolygon, called from write_shape_geometry)
            // does NOT have this bug - only the VIA-specific one does,
            // and there's no bug-free alternate API for it (unlike
            // RECT, which lefwViaLayerRect still writes correctly
            // above). Not called here - ViaLayer.polygons is read-only
            // for VIA geometry with this vendored writer version, same
            // "skip the call, mark read-only" treatment as this file's
            // other unparseable-output bugs (see LEFDEF_BUGS.md).
            // ViaLayer.polygon_masks is read (see via_layers_from_parser)
            // for the same reason rects/rect_masks both are - the database
            // shouldn't lose information the reader can hand it just
            // because the writer can't use it yet - but has nowhere to go
            // here until lefwViaLayerPolygon itself is usable.
        }

        return 0;
    }

    int LEFWriter::write_via_foreign(const ForeignData &foreign, double dbu_per_micron)
    {
        // lefwViaForeignStr treats xl==0.0 && yl==0.0 as "no point" (unless
        // orient is also set, in which case it assumes the point really is
        // (0,0) and writes both) and an empty orient string as "no
        // orientation" - see its own lefwWriter.cpp implementation. That
        // convention already matches exactly what "unset" should mean here,
        // so passing origin/orient's value_or(...) defaults straight
        // through (rather than special-casing has_value() ourselves) is
        // correct, not just convenient.
        const double xl = foreign.origin ? to_microns(foreign.origin->x, dbu_per_micron) : 0.0;
        const double yl = foreign.origin ? to_microns(foreign.origin->y, dbu_per_micron) : 0.0;
        const std::string orient = foreign.orient ? orientation_to_string(*foreign.orient) : "";
        return lefwViaForeignStr(foreign.name.c_str(), xl, yl, orient.c_str());
    }

    int LEFWriter::write_vias(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const double dbu_per_micron = technology->database_units_microns;
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        for (ViaId via_id : root.get_technology_vias(technology_id))
        {
            const ViaData *via = root.get_via(via_id);
            if (!via)
                continue;

            int status = lefwStartVia(via->name.c_str(), via->is_default ? "DEFAULT" : nullptr);
            if (status)
                return status;

            // foreign/via_rule/layers moved to pooled classes (Phase 2) -
            // materialized from the pool here rather than read as plain
            // fields on via.
            const ForeignId foreign_id = root.get_via_foreign(via_id);
            const ForeignData *foreign = root.get_foreign(foreign_id);
            if (foreign)
            {
                // lefwViaForeignStr's own xl/yl/orient are each independently
                // optional(0)/optional("") - see write_via_foreign's own
                // comment (shared by every FOREIGN write site) for why 0.0/""
                // is passed through untouched rather than mapped to some
                // other sentinel.
                status = write_via_foreign(*foreign, dbu_per_micron);
                if (status)
                    return status;
            }

            // Mutually exclusive per lefwViaResistance/lefwViaViarule's
            // own "either...or" contract - the reader only ever sets one
            // or the other (see lefrViaCbkFn's own comment).
            if (via->resistance)
            {
                status = lefwViaResistance(*via->resistance);
                if (status)
                    return status;
            }
            else
            {
                const ViaRuleReferenceId via_rule_ref_id = root.get_via_via_rule(via_id);
                if (const ViaRuleReferenceData *vr_ptr = root.get_via_rule_reference(via_rule_ref_id))
                {
                    const ViaRuleReferenceData &vr = *vr_ptr;
                    // value_or(Point{}), not vr.cut_size->x etc. - the
                    // reader always populates all four together, but
                    // update_via_rule_reference (generated) can now set
                    // them independently, so a partially-set object is a
                    // real possibility here, not just a reader artifact.
                    const Point cut_size = vr.cut_size.value_or(Point{});
                    const Point cut_spacing = vr.cut_spacing.value_or(Point{});
                    const Point bot_enclosure = vr.bot_enclosure.value_or(Point{});
                    const Point top_enclosure = vr.top_enclosure.value_or(Point{});
                    status = lefwViaViarule(vr.via_rule_name.c_str(), to_um(cut_size.x), to_um(cut_size.y),
                                             vr.bot_layer_name.c_str(), vr.cut_layer_name.c_str(), vr.top_layer_name.c_str(),
                                             to_um(cut_spacing.x), to_um(cut_spacing.y),
                                             to_um(bot_enclosure.x), to_um(bot_enclosure.y),
                                             to_um(top_enclosure.x), to_um(top_enclosure.y));
                    if (status)
                        return status;
                }
            }

            status = write_properties(via->properties);
            if (status)
                return status;

            std::vector<ViaLayerData> layers;
            for (const ViaLayerId id : root.get_via_layers(via_id))
                if (const ViaLayerData *data = root.get_via_layer(id))
                    layers.push_back(*data);
            status = write_via_layers(layers, dbu_per_micron);
            if (status)
                return status;

            status = lefwEndVia(via->name.c_str());
            if (status)
                return status;
        }

        return 0;
    }

    int LEFWriter::write_via_rules(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const double dbu_per_micron = technology->database_units_microns;
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };
        auto to_um_opt = [&](const std::optional<int64_t> &v)
        { return v ? to_um(*v) : 0.0; };

        for (ViaRuleId via_rule_id : root.get_technology_via_rules(technology_id))
        {
            const ViaRuleData *via_rule = root.get_via_rule(via_rule_id);
            if (!via_rule)
                continue;

            std::vector<ViaRuleLayerData> layers;
            for (const ViaRuleLayerId id : root.get_via_rule_layers(via_rule_id))
                if (const ViaRuleLayerData *data = root.get_via_rule_layer(id))
                    layers.push_back(*data);

            int status;
            if (via_rule->is_generate)
            {
                status = lefwStartViaRuleGen(via_rule->name.c_str());
                if (status)
                    return status;

                if (via_rule->is_default)
                {
                    status = lefwViaRuleGenDefault();
                    if (status)
                        return status;
                }

                // The cut layer (RECT + SPACING x BY y, GENERATE only) isn't
                // always via_rule->layers[2] - lefiViaRuleLayer's own file
                // order is preserved as-is by the reader (see
                // lefrViaRuleCbkFn above), and complete.5.8.lef's own
                // VIAGEN3T lists its cut layer (v3) second, not third
                // (metal/cut/metal, not metal/metal/cut). Identify it by
                // which layer actually has a rect instead of assuming a
                // fixed index - the two "regular" (ENCLOSURE-bearing) layers
                // are whichever ones don't.
                size_t regular_written = 0;
                for (const ViaRuleLayerData &layer : layers)
                {
                    if (layer.rect)
                        continue;
                    if (regular_written >= 2)
                        continue;
                    regular_written++;
                    // lefwViaRuleGenLayer's DIRECTION/OVERHANG/METALOVERHANG
                    // args are rejected with LEFW_OBSOLETE for any version
                    // >= 5.6 (see lefwWriter.cpp's shared lefwViaRulePrtLayer
                    // helper) - write_lef always writes VERSION 5.8, so
                    // those three must never be passed; ENCLOSURE is the
                    // only way to write overhang data at this version,
                    // matching the reader's own translation (see
                    // lefrViaRuleCbkFn / K_OVERHANG in lef.y).
                    if (layer.enclosure_overhang1 && layer.enclosure_overhang2)
                        status = lefwViaRuleGenLayerEnclosure(layer.layer_name.c_str(), to_um(*layer.enclosure_overhang1), to_um(*layer.enclosure_overhang2), to_um_opt(layer.width_min), to_um_opt(layer.width_max));
                    else
                        status = lefwViaRuleGenLayer(layer.layer_name.c_str(), nullptr, to_um_opt(layer.width_min), to_um_opt(layer.width_max), 0.0, 0.0);
                    if (status)
                        return status;
                }

                for (const ViaRuleLayerData &cut_layer : layers)
                {
                    if (!cut_layer.rect)
                        continue;
                    status = lefwViaRuleGenLayer3(cut_layer.layer_name.c_str(),
                                                   to_um(cut_layer.rect->ll.x), to_um(cut_layer.rect->ll.y),
                                                   to_um(cut_layer.rect->ur.x), to_um(cut_layer.rect->ur.y),
                                                   to_um_opt(cut_layer.spacing_step_x), to_um_opt(cut_layer.spacing_step_y),
                                                   cut_layer.resistance.value_or(0.0));
                    if (status)
                        return status;
                    break;
                }

                status = lefwEndViaRuleGen(via_rule->name.c_str());
                if (status)
                    return status;
            }
            else
            {
                // KNOWN VENDORED-LIBRARY DEAD END (see LEFDEF_BUGS.md): a
                // non-GENERATE VIARULE's LAYER requires exactly 2 LAYER
                // sub-statements (lef.y's own grammar - an empty VIARULE is
                // a fatal LEFPARS-1), each requiring a DIRECTION construct
                // - but lefwViaRuleLayer (shared lefwViaRulePrtLayer helper)
                // hard-rejects direction/overhang/metalOverhang with
                // LEFW_OBSOLETE at this writer's fixed VERSION 5.8. There
                // is no version this writer can pick that satisfies both
                // sides, and no alternate API. Writing the LAYER
                // sub-statements without DIRECTION anyway (the previous
                // behavior) doesn't just lose this VIARULE's own data - a
                // real LEFPARS-1705 re-parsing it also silently truncates
                // lefdiff's own comparison for everything written after it
                // in the same file (confirmed: NONDEFAULTRULE/SITE, which
                // round-trip correctly on their own, disappeared from the
                // diff purely because of this). So the whole non-GENERATE
                // VIARULE is skipped here, not just DIRECTION - same
                // "vendored writer literally cannot produce valid output,
                // skip and mark read-only" treatment as this file's other
                // dead ends, chosen deliberately over "write something
                // broken" because broken here doesn't stay contained to
                // just this construct.
            }
        }

        return 0;
    }

    int LEFWriter::write_sites(const Root &root, TechnologyId technology_id)
    {
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const double dbu_per_micron = technology->database_units_microns;
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        for (SiteId site_id : root.get_technology_sites(technology_id))
        {
            const SiteData *site = root.get_site(site_id);
            if (!site)
                continue;

            // lefwSite takes one space-joined symmetry string, same
            // convention as write_macro's own SYMMETRY handling.
            // value_or(Symmetry{}) - unset means "no symmetry flags set",
            // the same default an all-false Symmetry already represents.
            const Symmetry site_symmetry = site->symmetry.value_or(Symmetry{});
            std::string symmetry;
            if (site_symmetry.x)
                symmetry += "X ";
            if (site_symmetry.y)
                symmetry += "Y ";
            if (site_symmetry.r90)
                symmetry += "R90 ";
            if (!symmetry.empty())
                symmetry.pop_back(); // trailing space

            int status = lefwSite(site->name.c_str(), site->site_class ? site->site_class->c_str() : nullptr,
                                   symmetry.empty() ? nullptr : symmetry.c_str(),
                                   site->size ? to_um(site->size->x) : 0.0, site->size ? to_um(site->size->y) : 0.0);
            if (status)
                return status;

            for (const RowPatternEntry &entry : site->row_pattern)
            {
                status = lefwSiteRowPatternStr(entry.site_name.c_str(), orientation_to_string(entry.orient));
                if (status)
                    return status;
            }

            // site->properties is deliberately never written: the vendored
            // writer's lefwStringProperty/lefwRealProperty/lefwIntProperty
            // only accept lefwState values LEFW_VIA/LAYER/VIARULE/MACRO(_START)/
            // VIA_START/VIARULE_START/LAYER_START/BEGINEXT/VIAVIARULE/
            // LAYERROUTING(_START) (confirmed in lefwWriter.cpp) - LEFW_SITE
            // is not among them, so SITE properties are readable but not
            // writable via this API. Same gap for NONDEFAULTRULE, see
            // write_non_default_rules below.
            status = lefwEndSite(site->name.c_str());
            if (status)
                return status;
        }

        return 0;
    }

    int LEFWriter::write_non_default_rules(const Root &root, TechnologyId technology_id)
    {
        // rule->properties is deliberately never written here either -
        // LEFW_NONDEFAULTRULE(_START) isn't among the states the vendored
        // writer's generic property functions accept (see write_sites's
        // own comment for the full accepted-state list).
        const TechnologyData *technology = root.get_technology(technology_id);
        if (!technology)
            return 0;
        const double dbu_per_micron = technology->database_units_microns;
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        for (NonDefaultRuleId rule_id : root.get_technology_non_default_rules(technology_id))
        {
            const NonDefaultRuleData *rule = root.get_non_default_rule(rule_id);
            if (!rule)
                continue;

            int status = lefwStartNonDefaultRule(rule->name.c_str());
            if (status)
                return status;

            // HARDSPACING must come first - lef.y's own grammar has
            // nd_hardspacing appear before nd_rules (LAYER/VIA/...) in the
            // NONDEFAULTRULE production, not just anywhere in the block.
            if (rule->hard_spacing)
            {
                status = lefwNonDefaultRuleHardspacing();
                if (status)
                    return status;
            }

            std::vector<NonDefaultRuleLayerData> layers;
            for (const NonDefaultRuleLayerId id : root.get_non_default_rule_layers(rule_id))
                if (const NonDefaultRuleLayerData *data = root.get_non_default_rule_layer(id))
                    layers.push_back(*data);

            for (const NonDefaultRuleLayerData &layer : layers)
            {
                // diag_width has no writer parameter in this vendored
                // version's lefwNonDefaultRuleLayer (confirmed against
                // lefwWriter.hpp - width/minSpacing/wireExtension/
                // resistance/capacitance/edgeCap only) - read-only, same
                // "vendored writer gap" pattern as this phase's other
                // documented cases.
                status = lefwNonDefaultRuleLayer(layer.layer_name.c_str(),
                                                  layer.width ? to_um(*layer.width) : 0.0,
                                                  layer.spacing ? to_um(*layer.spacing) : 0.0,
                                                  layer.wire_extension ? to_um(*layer.wire_extension) : 0.0,
                                                  layer.resistance.value_or(0.0),
                                                  layer.capacitance.value_or(0.0),
                                                  layer.edge_cap.value_or(0.0));
                if (status)
                    return status;
            }

            for (const NonDefaultRuleViaId via_id : root.get_non_default_rule_vias(rule_id))
            {
                const NonDefaultRuleViaData *via = root.get_non_default_rule_via(via_id);
                if (!via)
                    continue;

                status = lefwNonDefaultRuleStartVia(via->name.c_str(), via->is_default ? "DEFAULT" : nullptr);
                if (status)
                    return status;

                if (const ForeignData *foreign = root.get_foreign(root.get_non_default_rule_via_foreign(via_id)))
                {
                    status = write_via_foreign(*foreign, dbu_per_micron);
                    if (status)
                        return status;
                }
                if (via->resistance)
                {
                    status = lefwViaResistance(*via->resistance);
                    if (status)
                        return status;
                }

                // lefwNonDefaultRuleStartVia sets lefwState =
                // LEFW_VIA_START, the same state a top-level VIA uses -
                // write_properties' generic property functions already
                // accept that state (see its own comment), so this works
                // without any of the SITE/NONDEFAULTRULE-itself gaps.
                status = write_properties(via->properties);
                if (status)
                    return status;

                std::vector<ViaLayerData> via_layers;
                for (const ViaLayerId layer_id : root.get_non_default_rule_via_layers(via_id))
                    if (const ViaLayerData *layer_data = root.get_via_layer(layer_id))
                        via_layers.push_back(*layer_data);

                status = write_via_layers(via_layers, dbu_per_micron);
                if (status)
                    return status;

                status = lefwNonDefaultRuleEndVia(via->name.c_str());
                if (status)
                    return status;
            }

            for (const std::string &via_name : rule->use_via_names)
            {
                status = lefwNonDefaultRuleUseVia(via_name.c_str());
                if (status)
                    return status;
            }

            for (const std::string &via_rule_name : rule->use_via_rule_names)
            {
                status = lefwNonDefaultRuleUseViaRule(via_rule_name.c_str());
                if (status)
                    return status;
            }

            for (const MinCutOverride &min_cut : rule->min_cuts)
            {
                status = lefwNonDefaultRuleMinCuts(min_cut.cut_layer_name.c_str(), min_cut.num_cuts);
                if (status)
                    return status;
            }

            status = lefwEndNonDefaultRule(rule->name.c_str());
            if (status)
                return status;
        }

        return 0;
    }

    int LEFWriter::write_shape_geometry(const Shape &shape, double dbu_per_micron, bool is_pin_port)
    {
        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        // LAYER/DESIGNRULEWIDTH/EXCEPTPGNET are mutually exclusive per
        // lef.y's own grammar - one combined statement opens this LAYER
        // occurrence, matching lefwMacroObsLayer's own doc comment
        // ("Either this routine, lefwMacroObsDesignRuleWidth, ... or
        // lefwMacroExceptPGNet must be called").
        int status;
        if (shape.except_pg_net && !is_pin_port)
        {
            // KNOWN VENDORED-WRITER GAP: lefwMacroExceptPGNet only
            // accepts !lefwIsMacroObs (confirmed in lefwWriter.cpp) - it
            // cannot be called from a PIN PORT context at all, even
            // though lef.y's own layer_exceptpgnet grammar rule is shared
            // by both PORT and OBS geometry. It also guards on an
            // internal lefwSpacingVal flag reset only once per OBS
            // section (not per LAYER) - once any LAYER-with-SPACING has
            // been written anywhere earlier in this OBS section,
            // EXCEPTPGNET is permanently blocked for the rest of it.
            // Restricted to OBS here (is_pin_port already excluded above)
            // and callers should avoid mixing SPACING-layers before an
            // EXCEPTPGNET-layer in the same OBS, matching how
            // complete.5.8.lef's own OBS blocks never mix the two.
            status = lefwMacroExceptPGNet(shape.layer_name.c_str());
        }
        else if (shape.design_rule_width)
        {
            // KNOWN VENDORED-WRITER GAP (see LEFDEF_BUGS.md): even though
            // Shape.design_rule_width now correctly distinguishes "unset"
            // from "explicitly 0" (UPDATES.md item 12 - 0 is a real,
            // meaningful DESIGNRULEWIDTH, not a sentinel - complete.5.8.lef
            // has one: "LAYER a1sig DESIGNRULEWIDTH 0"), the vendored
            // lefwMacroPinPortDesignRuleWidth/lefwMacroObsDesignRuleWidth
            // both gate on `if (width)`, so a genuine 0 still can't reach
            // the file - not something to work around here (out of scope:
            // would require patching vendored source).
            status = is_pin_port
                         ? lefwMacroPinPortDesignRuleWidth(shape.layer_name.c_str(), to_um(*shape.design_rule_width))
                         : lefwMacroObsDesignRuleWidth(shape.layer_name.c_str(), to_um(*shape.design_rule_width));
        }
        else
        {
            // Same vendored `if (spacing)` gap as DESIGNRULEWIDTH above
            // applies here too - an explicit SPACING 0 can't be written
            // either, only "no SPACING clause at all" (spacing unset, the
            // value_or(0) case) and any nonzero value.
            const int64_t spacing_dbu = shape.spacing.value_or(0);
            status = is_pin_port ? lefwMacroPinPortLayer(shape.layer_name.c_str(), to_um(spacing_dbu)) : lefwMacroObsLayer(shape.layer_name.c_str(), to_um(spacing_dbu));
        }
        if (status)
            return status;

        // LEF's WIDTH statement applies to subsequent PATHs within this
        // LAYER context until it next changes - re-derived here from each
        // Path's own stored width (Shape stores width per-Path, not as
        // separate parse-time state - see schema.py's Path Klass) rather
        // than tracked separately.
        bool has_current_width = false;
        uint64_t current_width = 0;

        // rect_masks/polygon_masks/path_masks are parallel arrays (see
        // shapes_from_parser's own reading) - 0 (no mask) if a given
        // index is out of range, e.g. for Shapes built directly rather
        // than read from a LEF file that never set one.
        for (size_t i = 0; i < shape.rects.size(); i++)
        {
            const Rect &rect = shape.rects[i];
            const int mask = i < shape.rect_masks.size() ? shape.rect_masks[i] : 0;
            status = is_pin_port
                         ? lefwMacroPinPortLayerRect(to_um(rect.ll.x), to_um(rect.ll.y), to_um(rect.ur.x), to_um(rect.ur.y), 0, 0, 0, 0, mask)
                         : lefwMacroObsLayerRect(to_um(rect.ll.x), to_um(rect.ll.y), to_um(rect.ur.x), to_um(rect.ur.y), 0, 0, 0, 0, mask);
            if (status)
                return status;
        }

        for (const RectIterate &it : shape.rect_iterates)
        {
            const int mask = it.mask.value_or(0);
            status = is_pin_port
                         ? lefwMacroPinPortLayerRect(to_um(it.rect.ll.x), to_um(it.rect.ll.y), to_um(it.rect.ur.x), to_um(it.rect.ur.y), it.num_x, it.num_y, to_um(it.space_x), to_um(it.space_y), mask)
                         : lefwMacroObsLayerRect(to_um(it.rect.ll.x), to_um(it.rect.ll.y), to_um(it.rect.ur.x), to_um(it.rect.ur.y), it.num_x, it.num_y, to_um(it.space_x), to_um(it.space_y), mask);
            if (status)
                return status;
        }

        auto write_path = [&](const Path &path, int num_x, int num_y, double space_x_um, double space_y_um, int mask) -> int
        {
            if (!has_current_width || current_width != path.width)
            {
                const double width_um = to_um(static_cast<int64_t>(path.width));
                const int width_status = is_pin_port ? lefwMacroPinPortLayerWidth(width_um) : lefwMacroObsLayerWidth(width_um);
                if (width_status)
                    return width_status;
                has_current_width = true;
                current_width = path.width;
            }

            std::vector<double> xs;
            std::vector<double> ys;
            xs.reserve(path.polygon.points.size());
            ys.reserve(path.polygon.points.size());
            for (const Point &point : path.polygon.points)
            {
                xs.push_back(to_um(point.x));
                ys.push_back(to_um(point.y));
            }

            return is_pin_port
                       ? lefwMacroPinPortLayerPath(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, mask)
                       : lefwMacroObsLayerPath(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, mask);
        };

        for (size_t i = 0; i < shape.paths.size(); i++)
        {
            const int mask = i < shape.path_masks.size() ? shape.path_masks[i] : 0;
            status = write_path(shape.paths[i], 0, 0, 0, 0, mask);
            if (status)
                return status;
        }

        for (const PathIterate &it : shape.path_iterates)
        {
            status = write_path(it.path, it.num_x, it.num_y, to_um(it.space_x), to_um(it.space_y), it.mask.value_or(0));
            if (status)
                return status;
        }

        auto write_polygon = [&](const Polygon &polygon, int num_x, int num_y, double space_x_um, double space_y_um, int mask) -> int
        {
            std::vector<double> xs;
            std::vector<double> ys;
            xs.reserve(polygon.points.size());
            ys.reserve(polygon.points.size());
            for (const Point &point : polygon.points)
            {
                xs.push_back(to_um(point.x));
                ys.push_back(to_um(point.y));
            }

            return is_pin_port
                       ? lefwMacroPinPortLayerPolygon(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, mask)
                       : lefwMacroObsLayerPolygon(static_cast<int>(xs.size()), xs.data(), ys.data(), num_x, num_y, space_x_um, space_y_um, mask);
        };

        for (size_t i = 0; i < shape.polygons.size(); i++)
        {
            const int mask = i < shape.polygon_masks.size() ? shape.polygon_masks[i] : 0;
            status = write_polygon(shape.polygons[i], 0, 0, 0, 0, mask);
            if (status)
                return status;
        }

        for (const PolygonIterate &it : shape.polygon_iterates)
        {
            status = write_polygon(it.polygon, it.num_x, it.num_y, to_um(it.space_x), to_um(it.space_y), 0);
            if (status)
                return status;
        }

        // lefwMacroObsVia/lefwMacroPinPortVia are callable at any point
        // once the section is open (lefwIsMacroObs/lefwIsMacroPinPort are
        // simple flags, not fine-grained state) - matches
        // complete.5.8.lef's own usage of VIA nested after LAYER/RECT
        // within one PORT block.
        for (const ShapeVia &via : shape.vias)
        {
            status = is_pin_port
                         ? lefwMacroPinPortVia(to_um(via.origin.x), to_um(via.origin.y), via.via_name.c_str(), 0, 0, 0, 0, via.mask.value_or(0))
                         : lefwMacroObsVia(to_um(via.origin.x), to_um(via.origin.y), via.via_name.c_str(), 0, 0, 0, 0, via.mask.value_or(0));
            if (status)
                return status;
        }
        for (const ShapeViaIterate &via_iter : shape.via_iterates)
        {
            status = is_pin_port
                         ? lefwMacroPinPortVia(to_um(via_iter.origin.x), to_um(via_iter.origin.y), via_iter.via_name.c_str(), via_iter.num_x, via_iter.num_y, to_um(via_iter.space_x), to_um(via_iter.space_y), via_iter.mask.value_or(0))
                         : lefwMacroObsVia(to_um(via_iter.origin.x), to_um(via_iter.origin.y), via_iter.via_name.c_str(), via_iter.num_x, via_iter.num_y, to_um(via_iter.space_x), to_um(via_iter.space_y), via_iter.mask.value_or(0));
            if (status)
                return status;
        }

        return 0;
    }

    int LEFWriter::write_terminal(const Root &root, TerminalId terminal_id, double dbu_per_micron)
    {
        const TerminalData *terminal = root.get_terminal(terminal_id);
        if (!terminal)
            return 0;

        int status = lefwStartMacroPin(terminal->name.c_str());
        if (status)
            return status;

        if (const char *direction = signal_direction_to_string(terminal->direction))
        {
            status = lefwMacroPinDirection(direction);
            if (status)
                return status;
        }
        if (terminal->use)
        {
            status = lefwMacroPinUse(terminal->use->c_str());
            if (status)
                return status;
        }
        if (terminal->shape)
        {
            status = lefwMacroPinShape(terminal->shape->c_str());
            if (status)
                return status;
        }
        if (terminal->must_join)
        {
            status = lefwMacroPinMustjoin(terminal->must_join->c_str());
            if (status)
                return status;
        }
        if (terminal->net_expr)
        {
            status = lefwMacroPinNetExpr(terminal->net_expr->c_str());
            if (status)
                return status;
        }
        // terminal->leq is deliberately never written - lefwMacroPinLEQ is
        // obsoleted for VERSION >= 5.6 with no replacement (same
        // unreachable-at-5.8 gap as the macro-level LEQ above).
        if (terminal->taper_rule)
        {
            status = lefwMacroPinTaperRule(terminal->taper_rule->c_str());
            if (status)
                return status;
        }
        if (terminal->supply_sensitivity)
        {
            status = lefwMacroPinSupplySensitivity(terminal->supply_sensitivity->c_str());
            if (status)
                return status;
        }
        if (terminal->ground_sensitivity)
        {
            status = lefwMacroPinGroundSensitivity(terminal->ground_sensitivity->c_str());
            if (status)
                return status;
        }
        // terminal->rise_slew_limit/fall_slew_limit/max_load are
        // deliberately never written - lefwWriter.hpp/.cpp contain zero
        // lefwMacroPin* functions for RISESLEWLIMIT/FALLSLEWLIMIT/MAXLOAD
        // (confirmed by grep) even though lefiPin fully reads all three -
        // no vendored writer entry point exists at all, same class of gap
        // as split_wire_width.
        // terminal doesn't have power/leakage/pin_capacitance/
        // pin_resistance/pulldown_resistance/tie_off_resistance/vhi/vlo/
        // rise_voltage_threshold/fall_voltage_threshold/rise_thresh/
        // fall_thresh/rise_sat_cur/fall_sat_cur/current_source fields at
        // all - this project only supports LEF >= 5.4 (read_lef's own
        // version check), and lef.y's own grammar action for every one of
        // those only calls its setter when versionNum < 5.4, so the
        // vendored reader itself can never populate them for a file this
        // project would accept. See LEFDEF_BUGS.md's "Reader-side:
        // intentional version-obsolescence".
        // terminal->max_delay (MAXDELAY) is deliberately never written -
        // no lefwMacroPinMaxdelay exists in lefwWriter.hpp/.cpp at all
        // (confirmed by grep), same "fully readable, no vendored writer
        // entry point" gap as rise_slew_limit/fall_slew_limit/max_load
        // just above.

        // Flat pre-5.5 (value, layer) antenna fields - AntennaGateArea has
        // no such flat form (see lefrPinCbkFn's own comment), only the
        // oxide-scoped one below.
        status = write_pin_antenna_values(terminal->antenna_partial_metal_area, lefwMacroPinAntennaPartialMetalArea);
        if (status)
            return status;
        status = write_pin_antenna_values(terminal->antenna_partial_metal_side_area, lefwMacroPinAntennaPartialMetalSideArea);
        if (status)
            return status;
        status = write_pin_antenna_values(terminal->antenna_partial_cut_area, lefwMacroPinAntennaPartialCutArea);
        if (status)
            return status;
        status = write_pin_antenna_values(terminal->antenna_diff_area, lefwMacroPinAntennaDiffArea);
        if (status)
            return status;

        // 5.5 oxide-scoped antenna models - lefwMacroPinAntennaModel sets
        // "current oxide" state, no explicit end call (same flat
        // sequential-call pattern as lefwLayerAntennaModel above).
        for (const PinAntennaModelId model_id : root.get_terminal_antenna_models(terminal_id))
        {
            const PinAntennaModelData *model_ptr = root.get_pin_antenna_model(model_id);
            if (!model_ptr)
                continue;
            const PinAntennaModelData &model = *model_ptr;
            status = lefwMacroPinAntennaModel(model.oxide.c_str());
            if (status)
                return status;
            status = write_pin_antenna_values(model.gate_area, lefwMacroPinAntennaGateArea);
            if (status)
                return status;
            status = write_pin_antenna_values(model.max_area_car, lefwMacroPinAntennaMaxAreaCar);
            if (status)
                return status;
            status = write_pin_antenna_values(model.max_side_area_car, lefwMacroPinAntennaMaxSideAreaCar);
            if (status)
                return status;
            status = write_pin_antenna_values(model.max_cut_car, lefwMacroPinAntennaMaxCutCar);
            if (status)
                return status;
        }

        // lefwStartMacroPin doesn't change lefwState (it stays LEFW_MACRO,
        // tracked instead via the separate lefwIsMacroPin flag) - PIN
        // properties work via the same generic functions MACRO properties
        // use, unlike SITE/NONDEFAULTRULE/GENERATE-VIARULE.
        status = write_properties(terminal->properties);
        if (status)
            return status;

        for (TerminalPortId port_id : root.get_terminal_ports(terminal_id))
        {
            const TerminalPortData *port = root.get_terminal_port(port_id);
            if (!port)
                continue;

            status = lefwStartMacroPinPort(port->port_class ? port->port_class->c_str() : nullptr);
            if (status)
                return status;

            for (ShapeId shape_id : root.get_terminal_port_shapes(port_id))
            {
                const Shape *shape = root.get_shape(shape_id);
                if (!shape)
                    continue;
                status = write_shape_geometry(*shape, dbu_per_micron, true);
                if (status)
                    return status;
            }

            status = lefwEndMacroPinPort();
            if (status)
                return status;
        }

        return lefwEndMacroPin(terminal->name.c_str());
    }

    int LEFWriter::write_obstruction(const Root &root, ObstructionId obstruction_id, double dbu_per_micron)
    {
        const ObstructionData *obstruction = root.get_obstruction(obstruction_id);
        if (!obstruction)
            return 0;

        int status = lefwStartMacroObs();
        if (status)
            return status;

        for (ShapeId shape_id : root.get_obstruction_shapes(obstruction_id))
        {
            const Shape *shape = root.get_shape(shape_id);
            if (!shape)
                continue;
            status = write_shape_geometry(*shape, dbu_per_micron, false);
            if (status)
                return status;
        }

        return lefwEndMacroObs();
    }

    int LEFWriter::write_macro(const Root &root, AbstractId abstract_id, double dbu_per_micron)
    {
        const AbstractData *abstract = root.get_abstract(abstract_id);
        if (!abstract)
            return 0;

        const DesignData *design = root.get_design(abstract->design);
        if (!design)
            return 0;

        auto to_um = [&](int64_t v)
        { return to_microns(v, dbu_per_micron); };

        int status = lefwStartMacro(design->name.c_str());
        if (status)
            return status;

        if (abstract->type)
        {
            // abstract->type stores LEF's `CLASS <base> [<subtype>] ;`
            // verbatim as ONE space-joined string (confirmed by reading
            // lef.y's own class_type grammar rule - e.g. "CORE WELLTAP",
            // "BLOCK BLACKBOX" are single strings from lef_macro->
            // macroClass(), not two separate fields), but lefwMacroClass
            // takes base/subtype as two separate arguments and validates
            // the base against a fixed known list - passing the combined
            // string as a single argument never matches and fails the
            // whole MACRO write with LEFW_BAD_DATA, silently losing 100%
            // of that macro's content. Split back apart here.
            const size_t space_pos = abstract->type->find(' ');
            const std::string macro_class = space_pos == std::string::npos ? *abstract->type : abstract->type->substr(0, space_pos);
            const std::string macro_subclass = space_pos == std::string::npos ? std::string() : abstract->type->substr(space_pos + 1);
            status = lefwMacroClass(macro_class.c_str(), macro_subclass.empty() ? nullptr : macro_subclass.c_str());
            if (status)
                return status;
        }

        // abstract->leq/power/source are deliberately never written: for a
        // VERSION >= 5.6 file (this writer always writes 5.8), MACRO LEQ
        // is obsoleted with no replacement (lefwMacroLEQ's own
        // "versionNum >= 5.6 -> LEFW_OBSOLETE" check) and POWER/SOURCE are
        // obsolete at >= 5.4/5.6 respectively - and lef.y's own grammar
        // drops all three silently on READ at those versions too (never
        // populated in the first place), so this is a consistent,
        // unreachable-at-5.8 gap on both sides, not an asymmetry to work
        // around.
        if (abstract->eeq)
        {
            status = lefwMacroEEQ(abstract->eeq->c_str());
            if (status)
                return status;
        }
        if (abstract->is_fixed_mask)
        {
            status = lefwMacroFixedMask();
            if (status)
                return status;
        }

        // lefwMacroForeignStr's own xl/yl/orient are each independently
        // optional(0.0)/optional("") - same convention write_via_foreign
        // relies on for the VIA-context FOREIGN sites, just a different
        // vendored function (a MACRO can have several FOREIGNs, a VIA only
        // one, so there's no single shared call site to factor this into).
        for (const ForeignId foreign_id : root.get_abstract_foreigns(abstract_id))
        {
            const ForeignData *foreign = root.get_foreign(foreign_id);
            if (!foreign)
                continue;
            const double xl = foreign->origin ? to_um(foreign->origin->x) : 0.0;
            const double yl = foreign->origin ? to_um(foreign->origin->y) : 0.0;
            const std::string orient = foreign->orient ? orientation_to_string(*foreign->orient) : "";
            status = lefwMacroForeignStr(foreign->name.c_str(), xl, yl, orient.c_str());
            if (status)
                return status;
        }

        // value_or(Point{})/value_or(Symmetry{}) - all three are now
        // optional (create_abstract/update_abstract can leave any of them
        // unset), same zero-value default this writer already emitted
        // unconditionally before they were optional.
        const Point origin = abstract->origin.value_or(Point{});
        status = lefwMacroOrigin(to_um(origin.x), to_um(origin.y));
        if (status)
            return status;

        const Point size = abstract->size.value_or(Point{});
        status = lefwMacroSize(to_um(size.x), to_um(size.y));
        if (status)
            return status;

        const Symmetry abstract_symmetry = abstract->symmetry.value_or(Symmetry{});
        std::string symmetry;
        if (abstract_symmetry.x)
            symmetry += "X ";
        if (abstract_symmetry.y)
            symmetry += "Y ";
        if (abstract_symmetry.r90)
            symmetry += "R90 ";
        if (!symmetry.empty())
        {
            symmetry.pop_back(); // trailing space
            status = lefwMacroSymmetry(symmetry.c_str());
            if (status)
                return status;
        }

        if (abstract->site)
        {
            status = lefwMacroSite(abstract->site->c_str());
            if (status)
                return status;
        }

        // Distinct, mutually-exclusive grammar alternative from the
        // singular site name above (see lefrMacroCbkFn's own comment on
        // setSiteName vs setSitePattern).
        for (const MacroSitePlacementId placement_id : root.get_abstract_site_placements(abstract_id))
        {
            const MacroSitePlacementData *placement = root.get_macro_site_placement(placement_id);
            if (!placement)
                continue;
            const Point placement_origin = placement->origin.value_or(Point{});
            status = lefwMacroSitePatternStr(placement->site_name.c_str(), to_um(placement_origin.x), to_um(placement_origin.y), orientation_to_string(placement->orient),
                                              placement->num_x.value_or(0), placement->num_y.value_or(0),
                                              placement->step_x ? to_um(*placement->step_x) : 0.0, placement->step_y ? to_um(*placement->step_y) : 0.0);
            if (status)
                return status;
        }

        status = write_properties(abstract->properties);
        if (status)
            return status;

        for (TerminalId terminal_id : root.get_abstract_terminals(abstract_id))
        {
            status = write_terminal(root, terminal_id, dbu_per_micron);
            if (status)
                return status;
        }

        for (ObstructionId obstruction_id : root.get_abstract_obstructions(abstract_id))
        {
            status = write_obstruction(root, obstruction_id, dbu_per_micron);
            if (status)
                return status;
        }

        // abstract->densities is deliberately never written: the vendored
        // lefwStartMacroDensity(layerName) prints "DENSITY <layerName>\n"
        // directly, with no "LAYER" keyword - but lef.y's own macro_density
        // grammar rule requires "DENSITY" alone, then one "LAYER name ;"
        // statement per layer group (density_layer) - so the written text
        // can never be re-parsed as a DENSITY statement at all, for any
        // layer count. lefwStartMacroDensity also flatly refuses a second
        // call in the same macro (lefwIsMacroDensity guard), so even a
        // syntax-correct workaround couldn't cover more than one layer.
        // Fully readable (lefrDensityCbkFn) - not writable via this
        // vendored version. Not fixed (vendored code).

        return lefwEndMacro(design->name.c_str());
    }

    int LEFWriter::write_lef(const std::string &path, const Root &root, AbstractId abstract_id, LayerWriteMode mode)
    {
        messages_.clear();

        std::unique_ptr<FILE, int (*)(FILE *)> file(fopen(path.c_str(), "w"), &fclose);
        if (!file)
        {
            messages_.push_back(fmt::format("ERROR: Could not open {} for writing.", path));
            return 1;
        }

        int status = lefwInit(file.get());
        if (status)
        {
            messages_.push_back(fmt::format("ERROR: lefwInit failed with status {}.", status));
            return status;
        }

        status = lefwVersion(5, 8);
        if (status)
        {
            messages_.push_back(fmt::format("ERROR: lefwVersion failed with status {}.", status));
            return status;
        }

        const auto technology_ids = root.get_technology_ids();
        const TechnologyId technology_id = technology_ids.empty() ? TechnologyId{} : technology_ids.front();
        const TechnologyData *technology = technology_id.valid() ? root.get_technology(technology_id) : nullptr;
        const double dbu_per_micron = technology ? technology->database_units_microns : 0.0;

        // Written unconditionally (like VERSION above), not gated on
        // `mode` - these are top-level header statements, not layer/macro
        // content, and there's no case where writing a legal LEF file
        // should omit them if we have them.
        if (technology && technology->fixed_mask)
        {
            status = lefwFixedMask();
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: lefwFixedMask failed with status {}.", status));
                return status;
            }
        }
        if (technology && technology->bus_bit_chars)
        {
            status = lefwBusBitChars(technology->bus_bit_chars->c_str());
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: lefwBusBitChars failed with status {}.", status));
                return status;
            }
        }
        if (technology && technology->divider_char)
        {
            status = lefwDividerChar(technology->divider_char->c_str());
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: lefwDividerChar failed with status {}.", status));
                return status;
            }
        }
        if (technology && technology->use_min_spacing_obs)
        {
            status = lefwUseMinSpacing("OBS", *technology->use_min_spacing_obs ? "ON" : "OFF");
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: lefwUseMinSpacing(OBS) failed with status {}.", status));
                return status;
            }
        }
        if (technology && technology->use_min_spacing_pin)
        {
            status = lefwUseMinSpacing("PIN", *technology->use_min_spacing_pin ? "ON" : "OFF");
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: lefwUseMinSpacing(PIN) failed with status {}.", status));
                return status;
            }
        }
        if (technology && technology->clearance_measure)
        {
            status = lefwClearanceMeasure(technology->clearance_measure->c_str());
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: lefwClearanceMeasure failed with status {}.", status));
                return status;
            }
        }

        if (technology_id.valid() && mode != LayerWriteMode::None)
        {
            status = write_units(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing UNITS failed with status {}.", status));
                return status;
            }

            if (technology->manufacturing_grid)
            {
                status = lefwManufacturingGrid(*technology->manufacturing_grid);
                if (status)
                {
                    messages_.push_back(fmt::format("ERROR: lefwManufacturingGrid failed with status {}.", status));
                    return status;
                }
            }

            // After UNITS, before LAYER/VIA/... - matches complete.5.8.lef's
            // own real ordering, and LEF's own requirement that
            // PROPERTYDEFINITIONS precede the first use of any property
            // name it declares.
            status = write_property_definitions(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing PROPERTYDEFINITIONS failed with status {}.", status));
                return status;
            }

            status = write_technology_layers(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing LAYERs failed with status {}.", status));
                return status;
            }

            // lefwMaxviastack's own doc comment: "must be called only once
            // after all the layers" - matches complete.5.8.lef's own
            // MAXVIASTACK placement (mid-LAYER-section in the source, but
            // the vendored writer's own ordering requirement wins here).
            if (technology->max_via_stack)
            {
                status = lefwMaxviastack(*technology->max_via_stack,
                                          technology->max_via_stack_bottom_layer ? technology->max_via_stack_bottom_layer->c_str() : nullptr,
                                          technology->max_via_stack_top_layer ? technology->max_via_stack_top_layer->c_str() : nullptr);
                if (status)
                {
                    messages_.push_back(fmt::format("ERROR: lefwMaxviastack failed with status {}.", status));
                    return status;
                }
            }

            // Legacy pre-5.0 top-level antenna defaults - no ordering
            // constraint beyond "after lefwInit" per their own doc
            // comments, so grouped here with the other simple Technology
            // scalars rather than matching complete.5.8.lef's own
            // after-every-MACRO placement (which our multi-file writer,
            // one Technology-only file plus one file per MACRO, has no
            // equivalent position for anyway).
            if (technology->antenna_input_gate_area)
            {
                status = lefwAntennaInputGateArea(*technology->antenna_input_gate_area);
                if (status)
                {
                    messages_.push_back(fmt::format("ERROR: lefwAntennaInputGateArea failed with status {}.", status));
                    return status;
                }
            }
            if (technology->antenna_inout_diff_area)
            {
                status = lefwAntennaInOutDiffArea(*technology->antenna_inout_diff_area);
                if (status)
                {
                    messages_.push_back(fmt::format("ERROR: lefwAntennaInOutDiffArea failed with status {}.", status));
                    return status;
                }
            }
            if (technology->antenna_output_diff_area)
            {
                status = lefwAntennaOutputDiffArea(*technology->antenna_output_diff_area);
                if (status)
                {
                    messages_.push_back(fmt::format("ERROR: lefwAntennaOutputDiffArea failed with status {}.", status));
                    return status;
                }
            }

            // After LAYER (VIA/VIARULE reference layers by name), before
            // MACRO (LEF's own required ordering).
            status = write_vias(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing VIAs failed with status {}.", status));
                return status;
            }

            status = write_via_rules(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing VIARULEs failed with status {}.", status));
                return status;
            }

            // NONDEFAULTRULE then SITE, matching complete.5.8.lef's own
            // real ordering (after VIA/VIARULE, before MACRO).
            status = write_non_default_rules(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing NONDEFAULTRULEs failed with status {}.", status));
                return status;
            }

            status = write_sites(root, technology_id);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing SITEs failed with status {}.", status));
                return status;
            }
        }

        if (mode != LayerWriteMode::TechnologyOnly)
        {
            status = write_macro(root, abstract_id, dbu_per_micron);
            if (status)
            {
                messages_.push_back(fmt::format("ERROR: Writing MACRO failed with status {}.", status));
                return status;
            }
        }

        status = lefwEnd();
        if (status)
        {
            messages_.push_back(fmt::format("ERROR: lefwEnd failed with status {}.", status));
            return status;
        }

        return 0;
    }
}
