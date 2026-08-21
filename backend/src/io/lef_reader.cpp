#include <fmt/format.h>
#include <fmt/ostream.h>
#include "lef_reader.hpp"
#include "../geometry/geometry.hpp"
#include <cmath>
#include <utility>

namespace
{
    // Staging buffer for LEFReader::lefrLogFn - the vendored parser's
    // LEFI_LOG_FUNCTION/LEFI_WARNING_LOG_FUNCTION callbacks carry no
    // userData (see lefrReader.hpp), so there's no way to route a
    // message straight to the LEFReader instance that's mid-read_lef();
    // this file-local buffer bridges that gap instead, moved into the
    // instance's own messages_ right after lefrRead returns. thread_local
    // (not just a plain static) so two different LeHandles calling
    // read_lef concurrently on different threads can't corrupt each
    // other's capture - though the vendored parser's own global callback
    // registration (lefrSetLogFunction et al., process-wide state) is
    // inherently single-parse-at-a-time regardless, a pre-existing
    // property of this vendored library, not something this fixes.
    //
    // log_warning/log_error below (this class's own internal
    // diagnostics, as opposed to the vendored parser's own text captured
    // by lefrLogFn) append here too, not directly to LEFReader::messages_
    // - both this class's own callbacks (lefrLayerCbkFn etc.) and the
    // vendored parser's own log function run *during* lefrRead(), and
    // read_lef unconditionally does `messages_ =
    // std::move(g_pending_lef_messages)` right after lefrRead() returns
    // - if log_warning/log_error wrote straight to messages_ instead,
    // that move-assignment would silently wipe out anything they'd
    // already added. Routing everything through this one staging buffer
    // keeps every diagnostic from a single read_lef() call in one place,
    // in true chronological order, with no separate merge step needed.
    thread_local std::vector<std::string> g_pending_lef_messages;

    // See the comment above for why these target g_pending_lef_messages,
    // not a LEFReader instance - free functions (not LEFReader methods)
    // since they need no instance state, only this file-local buffer.
    // Templated to mirror spdlog::warn/error's own format-string+args
    // signature, so call sites need only a name change. Formats once
    // and logs the *already-formatted* text via a safe "{}" passthrough,
    // not the raw template a second time - reformatting dynamic,
    // file-controlled content (e.g. a LEF layer/design name) a second
    // time would misinterpret any literal `{`/`}` it happens to contain
    // as a format placeholder.
    template <typename... Args>
    void log_warning(fmt::format_string<Args...> fmt_str, Args &&...args)
    {
        std::string msg = fmt::format(fmt_str, std::forward<Args>(args)...);
        spdlog::warn("{}", msg);
        g_pending_lef_messages.push_back("WARNING: " + msg);
    }

    template <typename... Args>
    void log_error(fmt::format_string<Args...> fmt_str, Args &&...args)
    {
        std::string msg = fmt::format(fmt_str, std::forward<Args>(args)...);
        spdlog::error("{}", msg);
        g_pending_lef_messages.push_back("ERROR: " + msg);
    }

    // The vendored reader splits a VIA/VIA ITERATE placement's MASK digits
    // into three separate lefiGeomVia(Iter)::topMaskNum/cutMaskNum/
    // bottomMaskNum fields (see lefiMisc.cpp's own `topMaskNum = viaMask /
    // 100` etc.), but the vendored writer (lefwMacroObsVia/
    // lefwMacroPinPortVia) only ever accepts the combined 3-digit number
    // back, not the three digits separately - this recombines them the
    // same way the reader split them, so ShapeVia/ShapeViaIterate.mask can
    // stay the single value the writer needs. 0 for any digit means that
    // digit wasn't set (per-cut-layer masking is itself optional).
    int combine_via_mask(int top, int cut, int bottom)
    {
        return top * 100 + cut * 10 + bottom;
    }
}

namespace le
{
    void LEFReader::lefrLogFn(const char *msg)
    {
        if (!msg)
            return;
        std::string s(msg);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        if (!s.empty())
            g_pending_lef_messages.push_back(std::move(s));
    }

    int LEFReader::read_lef(std::string filename, Root &root, std::string library_name)
    {
        lefrInit();
        messages_.clear();
        g_pending_lef_messages.clear();
        used_dbu_before_units_declared_ = false;
        version_ = 0.0;

        // Setup callbacks
        lefrSetVersionCbk(lefrVersionCbkFn);
        lefrSetUnitsCbk(lefrUnitsCbkFn);
        lefrSetLayerCbk(lefrLayerCbkFn);
        lefrSetMacroBeginCbk(lefrMacroBeginCbkFn);
        lefrSetMacroCbk(lefrMacroCbkFn);
        lefrSetPinCbk(lefrPinCbkFn);
        lefrSetObstructionCbk(lefrObstructionCbkFn);
        lefrSetBusBitCharsCbk(lefrBusBitCharsCbkFn);
        lefrSetDividerCharCbk(lefrDividerCharCbkFn);
        lefrSetViaCbk(lefrViaCbkFn);
        lefrSetViaRuleCbk(lefrViaRuleCbkFn);
        lefrSetSiteCbk(lefrSiteCbkFn);
        lefrSetNonDefaultCbk(lefrNonDefaultCbkFn);
        lefrSetDensityCbk(lefrDensityCbkFn);
        lefrSetPropCbk(lefrPropCbkFn);
        lefrSetFixedMaskCbk(lefrFixedMaskCbkFn);
        lefrSetUseMinSpacingCbk(lefrUseMinSpacingCbkFn);
        lefrSetClearanceMeasureCbk(lefrClearanceMeasureCbkFn);
        lefrSetManufacturingCbk(lefrManufacturingCbkFn);
        lefrSetMaxStackViaCbk(lefrMaxStackViaCbkFn);
        lefrSetAntennaInputCbk(lefrAntennaInputCbkFn);
        lefrSetAntennaInoutCbk(lefrAntennaInoutCbkFn);
        lefrSetAntennaOutputCbk(lefrAntennaOutputCbkFn);
        lefrSetRegisterUnusedCallbacks();
        lefrSetLogFunction(&LEFReader::lefrLogFn);
        lefrSetWarningLogFunction(&LEFReader::lefrLogFn);

        // If there is no technology, create a new one. Otherwise get the first technology.
        TechnologyId technology_id = root.is_technology_empty() ? root.create_technology(TechnologyData{}) : root.get_technology_ids().front();

        // Initialize
        root_ = &root;
        technology_id_ = technology_id;
        technology_ = root.get_technology(technology_id);
        library_name_ = library_name;

        // Open file - RAII (fclose via unique_ptr's deleter) rather than a
        // manual fclose() at each return path below, so the handle is
        // still closed if an exception propagates out of lefrRead (which
        // invokes our own callbacks - e.g. a std::vector::reserve deep in
        // shapes_from_parser could throw on a malformed file) rather than
        // returning through here normally.
        std::unique_ptr<FILE, int (*)(FILE *)> file(fopen(filename.c_str(), "r"), &fclose);
        if (!file)
        {
            // Happens before lefrRead() ever runs, so the usual
            // post-lefrRead `messages_ = std::move(g_pending_lef_messages)`
            // (see read_lef's own comment further down) never executes on
            // this path - flush explicitly here instead, or this message
            // would sit in g_pending_lef_messages forever unseen.
            log_error("Could not open LEF file {}.", filename);
            messages_ = std::move(g_pending_lef_messages);
            return 1;
        }

        // Read file
        int result = lefrRead(file.get(), filename.c_str(), (void *)this);
        messages_ = std::move(g_pending_lef_messages);
        if (result != 0)
        {
            if (messages_.empty())
                messages_.push_back(fmt::format("ERROR: Could not parse LEF file {}.", filename));
            spdlog::error("Could not parse LEF file {}.", filename);
            return 2;
        }
        lefrPrintUnusedCallbacks(stdout);

        // Checked once here, same not-aborted-mid-parse convention as
        // used_dbu_before_units_declared_ below - this project only
        // supports LEF >= 5.4 (UPDATES.md item 12), so a version below
        // that is a real error, not a silent downgrade to reading it
        // partially (the vendored parser itself would already be
        // discarding several PIN-level statements at >= 5.4 that it
        // wouldn't at, say, 5.3 - see LEFDEF_BUGS.md's "Reader-side:
        // intentional version-obsolescence" - accepting < 5.4 input would
        // just mean this project's own database silently disagrees with
        // what a real 5.3-reading tool would see).
        if (version_ > 0.0 && version_ < 5.4)
        {
            const std::string msg = fmt::format("ERROR: {} declares VERSION {:.1f}, but this project only supports LEF 5.4 and later.", filename, version_);
            spdlog::error("{}", msg);
            messages_.push_back(msg);
            return 4;
        }

        // Checked once here, not aborted mid-parse from inside a callback
        // (see used_dbu_before_units_declared_'s own comment) - the file
        // parsed successfully overall, but at least one dbu conversion
        // ran before DATABASE MICRONS was ever set (from this file or an
        // earlier one sharing the same Root/Technology), so every
        // coordinate it touched is silently wrong (multiplied by the 0
        // sentinel). Pushed directly to messages_, not via log_error/
        // g_pending_lef_messages - that staging buffer was already moved
        // into messages_ above.
        if (used_dbu_before_units_declared_)
        {
            const std::string msg = fmt::format("ERROR: {} used geometry that required DATABASE MICRONS before it was ever declared (from this file or an earlier one read into the same database).", filename);
            spdlog::error("{}", msg);
            messages_.push_back(msg);
            return 3;
        }

        return 0;
    }

    /// @brief Build boundary from OVERLAP or SIZE and ORIGIN
    void LEFReader::post_process(LEFReader *reader)
    {
        // Look for OVERLAP obstructions
        std::vector<const Shape *> overlap_shapes;

        for (auto const &obstruction_id : reader->root_->get_abstract_obstructions(reader->abstract_id_))
        {
            for (auto const &shape_id : reader->root_->get_obstruction_shapes(obstruction_id))
            {
                auto const *shape = reader->root_->get_shape(shape_id);
                if (shape && shape->layer_name == "OVERLAP")
                {
                    overlap_shapes.push_back(shape);
                }
            }
        }

        bool did_use_overlap = false;

        if (overlap_shapes.size() > 0)
        {
            // Union overlap_shapes
            auto boundary = Geometry::union_shapes(overlap_shapes);

            if (boundary)
            {
                reader->abstract_data_.boundary = *boundary;
                did_use_overlap = true;
            }
        }
        if (!did_use_overlap)
        {
            // Use size and origin to determine boundary - value_or(Point{})
            // since both are now optional (unset if the LEF file never had
            // a MACRO ORIGIN/SIZE statement), same zero-point fallback this
            // computation already used implicitly before they were optional.
            const Point origin = reader->abstract_data_.origin.value_or(Point{});
            const Point size = reader->abstract_data_.size.value_or(Point{});
            Rect bbox;
            bbox.ll.x = origin.x;
            bbox.ll.y = origin.y;
            bbox.ur.x = origin.x + size.x;
            bbox.ur.y = origin.y + size.y;
            reader->abstract_data_.boundary.push_back(Geometry::rect_to_polygon(bbox));
        }

        for (auto const &polygon : reader->abstract_data_.boundary)
            spdlog::debug("boundary polygon {}", fmt::streamed(polygon));
    }

    int LEFReader::lefrLayerCbkFn(lefrCallbackType_e typ, lefiLayer *lef_layer, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        auto layer_name = lef_layer->name();

        if (reader->root_->get_layer_by_name(layer_name).valid())
        {
            log_warning("Layer {} already exists. Ignoring new definition.", layer_name);
            return 0;
        }

        // The parser reuses one scratch lefiLayer across every LAYER statement
        // and never resets fields between them, so a layer that omits a given
        // property would otherwise silently inherit whichever prior layer
        // last set it - every field below is has*()-guarded for exactly that
        // reason (UPDATES.md 12 Phase 1: basic scalar LAYER properties only -
        // multi-value SPACING beyond the first entry, SPACINGTABLE,
        // ENCLOSURE, ANTENNAMODEL, MINSTEP and other nested sub-rules are
        // deferred to a later iteration, per this task's own diff-driven
        // process).
        LayerData layer{
            .technology = reader->technology_id_,
            .name = layer_name,
            .type = lef_layer->type(),
            .direction = lef_layer->hasDirection() ? routing_direction_from_parser(lef_layer->direction()) : RoutingDirection::NONE,
        };

        if (lef_layer->hasWidth())
            layer.width = reader->microns_to_dbu(lef_layer->width());
        // hasXYPitch()/hasXYOffset() (two-value form) are mutually
        // exclusive with hasPitch()/hasOffset() (single-value form) per
        // lefiLayer's own setPitch() vs setPitchXY() - never both set.
        if (lef_layer->hasXYPitch())
            layer.pitch_xy = Point{.x = reader->microns_to_dbu(lef_layer->pitchX()), .y = reader->microns_to_dbu(lef_layer->pitchY())};
        else if (lef_layer->hasPitch())
            layer.pitch = reader->microns_to_dbu(lef_layer->pitch());
        if (lef_layer->hasXYOffset())
            layer.offset_xy = Point{.x = reader->microns_to_dbu(lef_layer->offsetX()), .y = reader->microns_to_dbu(lef_layer->offsetY())};
        else if (lef_layer->hasOffset())
            layer.offset = reader->microns_to_dbu(lef_layer->offset());
        if (lef_layer->hasArea())
            layer.area = reader->microns_squared_to_dbu(lef_layer->area());
        if (lef_layer->hasXYDiagPitch())
            layer.diag_pitch_xy = Point{.x = reader->microns_to_dbu(lef_layer->diagPitchX()), .y = reader->microns_to_dbu(lef_layer->diagPitchY())};
        else if (lef_layer->hasDiagPitch())
            layer.diag_pitch = reader->microns_to_dbu(lef_layer->diagPitch());
        if (lef_layer->hasDiagSpacing())
            layer.diag_spacing = reader->microns_to_dbu(lef_layer->diagSpacing());
        if (lef_layer->hasDiagWidth())
            layer.diag_width = reader->microns_to_dbu(lef_layer->diagWidth());
        if (lef_layer->hasDiagMinEdgeLength())
            layer.diag_min_edge_length = reader->microns_to_dbu(lef_layer->diagMinEdgeLength());
        if (lef_layer->hasMask())
            layer.default_mask = lef_layer->mask();
        if (lef_layer->hasMaxwidth())
            layer.max_width = reader->microns_to_dbu(lef_layer->maxwidth());
        if (lef_layer->hasMinwidth())
            layer.min_width = reader->microns_to_dbu(lef_layer->minwidth());
        layer.min_sizes.reserve(static_cast<size_t>(lef_layer->numMinSize()));
        for (int i = 0; i < lef_layer->numMinSize(); i++)
        {
            layer.min_sizes.push_back(MinSizeEntry{
                .width = reader->microns_to_dbu(lef_layer->minSizeWidth(i)),
                .length = reader->microns_to_dbu(lef_layer->minSizeLength(i)),
            });
        }
        layer.min_enclosed_areas.reserve(static_cast<size_t>(lef_layer->numMinenclosedarea()));
        for (int i = 0; i < lef_layer->numMinenclosedarea(); i++)
        {
            MinEnclosedAreaEntry entry{.area = reader->microns_squared_to_dbu(lef_layer->minenclosedarea(i))};
            if (lef_layer->hasMinenclosedareaWidth(i))
                entry.width = reader->microns_to_dbu(lef_layer->minenclosedareaWidth(i));
            layer.min_enclosed_areas.push_back(entry);
        }
        if (lef_layer->hasProtrusion())
        {
            layer.protrusion_width1 = reader->microns_to_dbu(lef_layer->protrusionWidth1());
            layer.protrusion_length = reader->microns_to_dbu(lef_layer->protrusionLength());
            layer.protrusion_width2 = reader->microns_to_dbu(lef_layer->protrusionWidth2());
        }
        layer.array_cuts.reserve(static_cast<size_t>(lef_layer->numArrayCuts()));
        for (int i = 0; i < lef_layer->numArrayCuts(); i++)
        {
            layer.array_cuts.push_back(ArrayCutsEntry{
                .cuts = lef_layer->arrayCuts(i),
                .spacing = reader->microns_to_dbu(lef_layer->arraySpacing(i)),
            });
        }
        // The following fields moved to pooled classes (Phase 2 - see
        // schema.py) - LayerData no longer has data members for them, so
        // each is staged into a local "pending_*" collection here and
        // attached via root_->create_X() once layer_id exists (below),
        // rather than assigned directly onto `layer`.
        std::optional<ArraySpacingData> pending_array_spacing;
        std::vector<PreferEnclosureEntryData> pending_prefer_enclosures;
        std::vector<EnclosureEntryData> pending_enclosures;
        std::vector<LayerDensityEntryData> pending_ac_current_density;
        std::vector<LayerDensityEntryData> pending_dc_current_density;
        std::vector<SpacingRuleData> pending_spacing_rules;
        std::vector<MinimumCutData> pending_minimum_cuts;
        std::vector<MinStepData> pending_min_steps;
        std::vector<InfluenceSpacingEntryData> pending_spacing_table_influence;
        std::vector<TwoWidthsSpacingEntryData> pending_spacing_table_two_widths;
        std::vector<AntennaModelData> pending_antenna_models;

        if (lef_layer->hasArraySpacing())
        {
            ArraySpacingData array_spacing{
                .long_array = static_cast<bool>(lef_layer->hasLongArray()),
                .cut_spacing = reader->microns_to_dbu(lef_layer->cutSpacing()),
            };
            if (lef_layer->hasViaWidth())
                array_spacing.via_width = reader->microns_to_dbu(lef_layer->viaWidth());
            pending_array_spacing = std::move(array_spacing);
        }
        pending_prefer_enclosures.reserve(static_cast<size_t>(lef_layer->numPreferEnclosure()));
        for (int i = 0; i < lef_layer->numPreferEnclosure(); i++)
        {
            PreferEnclosureEntryData entry{
                .location = lef_layer->hasPreferEnclosureRule(i) ? std::make_optional<std::string>(lef_layer->preferEnclosureRule(i)) : std::nullopt,
                .overhang1 = reader->microns_to_dbu(lef_layer->preferEnclosureOverhang1(i)),
                .overhang2 = reader->microns_to_dbu(lef_layer->preferEnclosureOverhang2(i)),
            };
            if (lef_layer->hasPreferEnclosureWidth(i))
                entry.min_width = reader->microns_to_dbu(lef_layer->preferEnclosureMinWidth(i));
            pending_prefer_enclosures.push_back(std::move(entry));
        }
        pending_enclosures.reserve(static_cast<size_t>(lef_layer->numEnclosure()));
        for (int i = 0; i < lef_layer->numEnclosure(); i++)
        {
            EnclosureEntryData entry{
                .location = lef_layer->hasEnclosureRule(i) ? std::make_optional<std::string>(lef_layer->enclosureRule(i)) : std::nullopt,
                .overhang1 = reader->microns_to_dbu(lef_layer->enclosureOverhang1(i)),
                .overhang2 = reader->microns_to_dbu(lef_layer->enclosureOverhang2(i)),
            };
            // width/except_extra_cut and min_length are mutually exclusive
            // per the vendored writer's own three ENCLOSURE variants (see
            // write_technology_layers's own comment).
            if (lef_layer->hasEnclosureWidth(i))
            {
                entry.width = reader->microns_to_dbu(lef_layer->enclosureMinWidth(i));
                if (lef_layer->hasEnclosureExceptExtraCut(i))
                    entry.except_extra_cut = reader->microns_to_dbu(lef_layer->enclosureExceptExtraCut(i));
            }
            else if (lef_layer->hasEnclosureMinLength(i))
                entry.min_length = reader->microns_to_dbu(lef_layer->enclosureMinLength(i));
            pending_enclosures.push_back(std::move(entry));
        }
        if (lef_layer->hasSplitWireWidth())
            layer.split_wire_width = reader->microns_to_dbu(lef_layer->splitWireWidth());
        if (lef_layer->hasMinimumDensity())
            layer.minimum_density = lef_layer->minimumDensity();
        if (lef_layer->hasMaximumDensity())
            layer.maximum_density = lef_layer->maximumDensity();
        if (lef_layer->hasDensityCheckStep())
            layer.density_check_step = reader->microns_to_dbu(lef_layer->densityCheckStep());
        if (lef_layer->hasDensityCheckWindow())
        {
            layer.density_check_window = DensityCheckWindow{
                .length = reader->microns_to_dbu(lef_layer->densityCheckWindowLength()),
                .width = reader->microns_to_dbu(lef_layer->densityCheckWindowWidth()),
            };
        }
        if (lef_layer->hasFillActiveSpacing())
            layer.fill_active_spacing = reader->microns_to_dbu(lef_layer->fillActiveSpacing());

        // AC/DC CURRENTDENSITY - lefiLayerDensity's type() is always set
        // ("PEAK"/"AVERAGE"/"RMS"); hasOneEntry() selects the plain-scalar
        // form, otherwise it's the table form (frequency/width-or-cutarea +
        // table_entries, mutually exclusive with one_entry per grammar).
        // table_entries is a single flat array per block (addTableEntry()
        // is called once per grammar rule, no 2D structure in the API) -
        // stored as-is, matching the vendored reader's own lack of any
        // cross-count validation between it and frequency/width/cutarea.
        auto read_current_density = [reader](lefiLayerDensity *density) -> LayerDensityEntryData
        {
            LayerDensityEntryData entry{.type = density->type()};
            if (density->hasOneEntry())
            {
                entry.one_entry = density->oneEntry();
                return entry;
            }
            entry.frequency.reserve(static_cast<size_t>(density->numFrequency()));
            for (int f = 0; f < density->numFrequency(); f++)
                entry.frequency.push_back(density->frequency(f));
            entry.width.reserve(static_cast<size_t>(density->numWidths()));
            for (int w = 0; w < density->numWidths(); w++)
                entry.width.push_back(reader->microns_to_dbu(density->width(w)));
            entry.cutarea.reserve(static_cast<size_t>(density->numCutareas()));
            for (int c = 0; c < density->numCutareas(); c++)
                entry.cutarea.push_back(reader->microns_squared_to_dbu(density->cutArea(c)));
            entry.table_entries.reserve(static_cast<size_t>(density->numTableEntries()));
            for (int t = 0; t < density->numTableEntries(); t++)
                entry.table_entries.push_back(density->tableEntry(t));
            return entry;
        };
        pending_ac_current_density.reserve(static_cast<size_t>(lef_layer->numAccurrentDensity()));
        for (int i = 0; i < lef_layer->numAccurrentDensity(); i++)
            pending_ac_current_density.push_back(read_current_density(lef_layer->accurrent(i)));
        pending_dc_current_density.reserve(static_cast<size_t>(lef_layer->numDccurrentDensity()));
        for (int i = 0; i < lef_layer->numDccurrentDensity(); i++)
            pending_dc_current_density.push_back(read_current_density(lef_layer->dccurrent(i)));
        pending_spacing_rules.reserve(static_cast<size_t>(lef_layer->numSpacing()));
        for (int i = 0; i < lef_layer->numSpacing(); i++)
        {
            SpacingRuleData rule{.distance = reader->microns_to_dbu(lef_layer->spacing(i))};

            if (lef_layer->hasSpacingRange(i))
            {
                rule.range_min = reader->microns_to_dbu(lef_layer->spacingRangeMin(i));
                rule.range_max = reader->microns_to_dbu(lef_layer->spacingRangeMax(i));
            }
            if (lef_layer->hasSpacingRangeUseLengthThreshold(i))
                rule.range_use_length_threshold = true;
            if (lef_layer->hasSpacingRangeInfluence(i))
                rule.range_influence = reader->microns_to_dbu(lef_layer->spacingRangeInfluence(i));
            if (lef_layer->hasSpacingRangeInfluenceRange(i))
            {
                rule.range_influence_range_min = reader->microns_to_dbu(lef_layer->spacingRangeInfluenceMin(i));
                rule.range_influence_range_max = reader->microns_to_dbu(lef_layer->spacingRangeInfluenceMax(i));
            }
            if (lef_layer->hasSpacingRangeRange(i))
            {
                rule.range_range_min = reader->microns_to_dbu(lef_layer->spacingRangeRangeMin(i));
                rule.range_range_max = reader->microns_to_dbu(lef_layer->spacingRangeRangeMax(i));
            }
            if (lef_layer->hasSpacingLengthThreshold(i))
                rule.length_threshold = reader->microns_to_dbu(lef_layer->spacingLengthThreshold(i));
            if (lef_layer->hasSpacingLengthThresholdRange(i))
            {
                rule.length_threshold_range_min = reader->microns_to_dbu(lef_layer->spacingLengthThresholdRangeMin(i));
                rule.length_threshold_range_max = reader->microns_to_dbu(lef_layer->spacingLengthThresholdRangeMax(i));
            }
            if (lef_layer->hasSpacingCenterToCenter(i))
                rule.center_to_center = true;
            if (lef_layer->hasSpacingSamenet(i))
            {
                rule.same_net = true;
                if (lef_layer->hasSpacingSamenetPGonly(i))
                    rule.same_net_pg_only = true;
            }
            if (lef_layer->hasSpacingParallelOverlap(i))
                rule.parallel_overlap = true;
            if (lef_layer->hasSpacingEndOfLine(i))
            {
                rule.end_of_line_width = reader->microns_to_dbu(lef_layer->spacingEolWidth(i));
                rule.end_of_line_within = reader->microns_to_dbu(lef_layer->spacingEolWithin(i));
                if (lef_layer->hasSpacingParellelEdge(i))
                {
                    rule.parallel_edge_space = reader->microns_to_dbu(lef_layer->spacingParSpace(i));
                    rule.parallel_edge_within = reader->microns_to_dbu(lef_layer->spacingParWithin(i));
                    if (lef_layer->hasSpacingTwoEdges(i))
                        rule.two_edges = true;
                }
            }
            if (lef_layer->hasSpacingName(i))
                rule.second_layer_name = lef_layer->spacingName(i);
            if (lef_layer->hasSpacingLayerStack(i))
                rule.second_layer_stack = true;
            if (lef_layer->hasSpacingAdjacent(i))
            {
                rule.adjacent_cuts = lef_layer->spacingAdjacentCuts(i);
                rule.adjacent_within = reader->microns_to_dbu(lef_layer->spacingAdjacentWithin(i));
                if (lef_layer->hasSpacingAdjacentExcept(i))
                    rule.adjacent_except_same_pg_net = true;
            }
            if (lef_layer->hasSpacingArea(i))
                rule.area = reader->microns_to_dbu(lef_layer->spacingArea(i));
            // Read-only (see write_technology_layers's own comment on the
            // vendored writer bug - these are never written back out).
            if (lef_layer->hasSpacingNotchLength(i))
                rule.notch_length = reader->microns_to_dbu(lef_layer->spacingNotchLength(i));
            if (lef_layer->hasSpacingEndOfNotchWidth(i))
            {
                rule.end_of_notch_width = reader->microns_to_dbu(lef_layer->spacingEndOfNotchWidth(i));
                rule.end_of_notch_spacing = reader->microns_to_dbu(lef_layer->spacingEndOfNotchSpacing(i));
                rule.end_of_notch_length = reader->microns_to_dbu(lef_layer->spacingEndOfNotchLength(i));
            }

            pending_spacing_rules.push_back(std::move(rule));
        }

        pending_minimum_cuts.reserve(static_cast<size_t>(lef_layer->numMinimumcut()));
        for (int i = 0; i < lef_layer->numMinimumcut(); i++)
        {
            MinimumCutData cut{
                .cuts = lef_layer->minimumcut(i),
                .width = reader->microns_to_dbu(lef_layer->minimumcutWidth(i)),
            };
            if (lef_layer->hasMinimumcutWithin(i))
                cut.within = reader->microns_to_dbu(lef_layer->minimumcutWithin(i));
            if (lef_layer->hasMinimumcutConnection(i))
                cut.connection = lef_layer->minimumcutConnection(i);
            if (lef_layer->hasMinimumcutNumCuts(i))
            {
                cut.length = reader->microns_to_dbu(lef_layer->minimumcutLength(i));
                cut.distance = reader->microns_to_dbu(lef_layer->minimumcutDistance(i));
            }
            pending_minimum_cuts.push_back(cut);
        }

        pending_min_steps.reserve(static_cast<size_t>(lef_layer->numMinstep()));
        for (int i = 0; i < lef_layer->numMinstep(); i++)
        {
            MinStepData step{.distance = reader->microns_to_dbu(lef_layer->minstep(i))};
            if (lef_layer->hasMinstepType(i))
                step.min_step_type = lef_layer->minstepType(i);
            if (lef_layer->hasMinstepLengthsum(i))
                step.lengthsum = reader->microns_to_dbu(lef_layer->minstepLengthsum(i));
            if (lef_layer->hasMinstepMaxedges(i))
                step.max_edges = lef_layer->minstepMaxedges(i);
            pending_min_steps.push_back(step);
        }

        for (int i = 0; i < lef_layer->numSpacingTable(); i++)
        {
            lefiSpacingTable *table = lef_layer->spacingTable(i);
            if (table->isParallel())
            {
                lefiParallel *parallel = table->parallel();
                ParallelRunLengthSpacingTable prl;
                prl.lengths.reserve(static_cast<size_t>(parallel->numLength()));
                for (int l = 0; l < parallel->numLength(); l++)
                    prl.lengths.push_back(reader->microns_to_dbu(parallel->length(l)));
                prl.widths.reserve(static_cast<size_t>(parallel->numWidth()));
                prl.spacings.reserve(static_cast<size_t>(parallel->numWidth()) * prl.lengths.size());
                for (int w = 0; w < parallel->numWidth(); w++)
                {
                    prl.widths.push_back(reader->microns_to_dbu(parallel->width(w)));
                    for (int l = 0; l < parallel->numLength(); l++)
                        prl.spacings.push_back(reader->microns_to_dbu(parallel->widthSpacing(w, l)));
                }
                layer.spacing_table_parallel_run_length = std::move(prl);
            }
            else if (table->isInfluence())
            {
                lefiInfluence *influence = table->influence();
                for (int e = 0; e < influence->numInfluenceEntry(); e++)
                {
                    pending_spacing_table_influence.push_back(InfluenceSpacingEntryData{
                        .width = reader->microns_to_dbu(influence->width(e)),
                        .distance = reader->microns_to_dbu(influence->distance(e)),
                        .spacing = reader->microns_to_dbu(influence->spacing(e)),
                    });
                }
            }
            else if (lefiTwoWidths *two_widths = table->twoWidths())
            {
                // No isTwoWidths() guard exists on lefiSpacingTable - the
                // null-check on twoWidths() itself is the only way to
                // detect this form (mutually exclusive with isParallel()/
                // isInfluence() per the grammar).
                for (int w = 0; w < two_widths->numWidth(); w++)
                {
                    TwoWidthsSpacingEntryData entry{.width = reader->microns_to_dbu(two_widths->width(w))};
                    if (two_widths->hasWidthPRL(w))
                        entry.prl = reader->microns_to_dbu(two_widths->widthPRL(w));
                    entry.spacings.reserve(static_cast<size_t>(two_widths->numWidthSpacing(w)));
                    for (int s = 0; s < two_widths->numWidthSpacing(w); s++)
                        entry.spacings.push_back(reader->microns_to_dbu(two_widths->widthSpacing(w, s)));
                    pending_spacing_table_two_widths.push_back(std::move(entry));
                }
            }
        }

        if (lef_layer->hasSpacingTableOrtho())
        {
            lefiOrthogonal *orthogonal = lef_layer->orthogonal();
            layer.spacing_table_orthogonal.reserve(static_cast<size_t>(orthogonal->numOrthogonal()));
            for (int i = 0; i < orthogonal->numOrthogonal(); i++)
            {
                layer.spacing_table_orthogonal.push_back(OrthogonalSpacingEntry{
                    .cut_within = reader->microns_to_dbu(orthogonal->cutWithin(i)),
                    .ortho_spacing = reader->microns_to_dbu(orthogonal->orthoSpacing(i)),
                });
            }
        }
        if (lef_layer->hasResistance())
            layer.resistance = lef_layer->resistance();
        // hasResistancePerCut()/resistancePerCut() is a distinct
        // accessor pair from hasResistance()/resistance() - the CUT-layer
        // RESISTANCE statement (5.6) parses differently from ROUTING's own
        // RESISTANCE, but both land in the same layer.resistance field
        // (mutually exclusive per layer type, never both set on one layer).
        else if (lef_layer->hasResistancePerCut())
            layer.resistance = lef_layer->resistancePerCut();
        if (lef_layer->hasCapacitance())
            layer.capacitance = lef_layer->capacitance();
        if (lef_layer->hasHeight())
            layer.height = reader->microns_to_dbu(lef_layer->height());
        if (lef_layer->hasThickness())
            layer.thickness = reader->microns_to_dbu(lef_layer->thickness());
        if (lef_layer->hasWireExtension())
            layer.wire_extension = reader->microns_to_dbu(lef_layer->wireExtension());
        if (lef_layer->hasShrinkage())
            layer.shrinkage = reader->microns_to_dbu(lef_layer->shrinkage());
        if (lef_layer->hasCapMultiplier())
            layer.cap_multiplier = lef_layer->capMultiplier();
        if (lef_layer->hasEdgeCap())
            layer.edge_cap = lef_layer->edgeCap();
        // hasAntennaArea()/antennaArea() are dead code in the vendored
        // parser - confirmed by reading lefiLayer.cpp and lef.y: setAntennaArea()
        // is defined but no grammar rule ever calls it (only
        // ANTENNAAREARATIO/ANTENNAAREAFACTOR exist as real LEF statements,
        // not a bare ANTENNAAREA), so this field is never populated by any
        // real LEF file and isn't tracked here.
        if (lef_layer->hasAntennaLength())
            layer.antenna_length = reader->microns_to_dbu(lef_layer->antennaLength());

        layer.properties.reserve(static_cast<size_t>(lef_layer->numProps()));
        for (int i = 0; i < lef_layer->numProps(); i++)
        {
            layer.properties.push_back(LefProperty{
                .name = lef_layer->propName(i),
                .is_number = static_cast<bool>(lef_layer->propIsNumber(i)),
                .string_value = lef_layer->propIsString(i) ? lef_layer->propValue(i) : "",
                .number_value = lef_layer->propIsNumber(i) ? lef_layer->propNumber(i) : 0.0,
            });
        }

        pending_antenna_models.reserve(static_cast<size_t>(lef_layer->numAntennaModel()));
        for (int i = 0; i < lef_layer->numAntennaModel(); i++)
        {
            lefiAntennaModel *lef_model = lef_layer->antennaModel(i);
            AntennaModelData model{.oxide = lef_model->antennaOxide()};

            if (lef_model->hasAntennaAreaRatio())
                model.area_ratio = lef_model->antennaAreaRatio();
            if (lef_model->hasAntennaCumAreaRatio())
                model.cum_area_ratio = lef_model->antennaCumAreaRatio();
            if (lef_model->hasAntennaAreaFactor())
            {
                model.area_factor = lef_model->antennaAreaFactor();
                if (lef_model->hasAntennaAreaFactorDUO())
                    model.area_factor_diffuse_only = true;
            }
            if (lef_model->hasAntennaSideAreaRatio())
                model.side_area_ratio = lef_model->antennaSideAreaRatio();
            if (lef_model->hasAntennaCumSideAreaRatio())
                model.cum_side_area_ratio = lef_model->antennaCumSideAreaRatio();
            if (lef_model->hasAntennaSideAreaFactor())
            {
                model.side_area_factor = lef_model->antennaSideAreaFactor();
                if (lef_model->hasAntennaSideAreaFactorDUO())
                    model.side_area_factor_diffuse_only = true;
            }

            // Each of these four is EITHER the scalar OR a PWL list, never
            // both (lefiAntennaModel's own mutually-exclusive has* flags).
            if (lef_model->hasAntennaDiffAreaRatioPWL())
            {
                lefiAntennaPWL *pwl = lef_model->antennaDiffAreaRatioPWL();
                model.diff_area_ratio_pwl.reserve(static_cast<size_t>(pwl->numPWL()));
                for (int j = 0; j < pwl->numPWL(); j++)
                    model.diff_area_ratio_pwl.push_back(AntennaPWLEntry{.diffusion = pwl->PWLdiffusion(j), .ratio = pwl->PWLratio(j)});
            }
            else if (lef_model->hasAntennaDiffAreaRatio())
                model.diff_area_ratio = lef_model->antennaDiffAreaRatio();

            if (lef_model->hasAntennaCumDiffAreaRatioPWL())
            {
                lefiAntennaPWL *pwl = lef_model->antennaCumDiffAreaRatioPWL();
                model.cum_diff_area_ratio_pwl.reserve(static_cast<size_t>(pwl->numPWL()));
                for (int j = 0; j < pwl->numPWL(); j++)
                    model.cum_diff_area_ratio_pwl.push_back(AntennaPWLEntry{.diffusion = pwl->PWLdiffusion(j), .ratio = pwl->PWLratio(j)});
            }
            else if (lef_model->hasAntennaCumDiffAreaRatio())
                model.cum_diff_area_ratio = lef_model->antennaCumDiffAreaRatio();

            if (lef_model->hasAntennaDiffSideAreaRatioPWL())
            {
                lefiAntennaPWL *pwl = lef_model->antennaDiffSideAreaRatioPWL();
                model.diff_side_area_ratio_pwl.reserve(static_cast<size_t>(pwl->numPWL()));
                for (int j = 0; j < pwl->numPWL(); j++)
                    model.diff_side_area_ratio_pwl.push_back(AntennaPWLEntry{.diffusion = pwl->PWLdiffusion(j), .ratio = pwl->PWLratio(j)});
            }
            else if (lef_model->hasAntennaDiffSideAreaRatio())
                model.diff_side_area_ratio = lef_model->antennaDiffSideAreaRatio();

            if (lef_model->hasAntennaCumDiffSideAreaRatioPWL())
            {
                lefiAntennaPWL *pwl = lef_model->antennaCumDiffSideAreaRatioPWL();
                model.cum_diff_side_area_ratio_pwl.reserve(static_cast<size_t>(pwl->numPWL()));
                for (int j = 0; j < pwl->numPWL(); j++)
                    model.cum_diff_side_area_ratio_pwl.push_back(AntennaPWLEntry{.diffusion = pwl->PWLdiffusion(j), .ratio = pwl->PWLratio(j)});
            }
            else if (lef_model->hasAntennaCumDiffSideAreaRatio())
                model.cum_diff_side_area_ratio = lef_model->antennaCumDiffSideAreaRatio();

            pending_antenna_models.push_back(std::move(model));
        }

        const LayerId layer_id = reader->root_->create_layer(std::move(layer));
        if (pending_array_spacing)
        {
            pending_array_spacing->layer = layer_id;
            reader->root_->create_array_spacing(std::move(*pending_array_spacing));
        }
        for (auto &entry : pending_prefer_enclosures)
        {
            entry.layer = layer_id;
            reader->root_->create_prefer_enclosure_entry(std::move(entry));
        }
        for (auto &entry : pending_enclosures)
        {
            entry.layer = layer_id;
            reader->root_->create_enclosure_entry(std::move(entry));
        }
        for (auto &entry : pending_ac_current_density)
        {
            entry.ac_layer = layer_id;
            reader->root_->create_layer_density_entry(std::move(entry));
        }
        for (auto &entry : pending_dc_current_density)
        {
            entry.dc_layer = layer_id;
            reader->root_->create_layer_density_entry(std::move(entry));
        }
        for (auto &rule : pending_spacing_rules)
        {
            rule.layer = layer_id;
            reader->root_->create_spacing_rule(std::move(rule));
        }
        for (auto &cut : pending_minimum_cuts)
        {
            cut.layer = layer_id;
            reader->root_->create_minimum_cut(std::move(cut));
        }
        for (auto &step : pending_min_steps)
        {
            step.layer = layer_id;
            reader->root_->create_min_step(std::move(step));
        }
        for (auto &entry : pending_spacing_table_influence)
        {
            entry.layer = layer_id;
            reader->root_->create_influence_spacing_entry(std::move(entry));
        }
        for (auto &entry : pending_spacing_table_two_widths)
        {
            entry.layer = layer_id;
            reader->root_->create_two_widths_spacing_entry(std::move(entry));
        }
        for (auto &model : pending_antenna_models)
        {
            model.layer = layer_id;
            reader->root_->create_antenna_model(std::move(model));
        }

        return 0;
    }

    int LEFReader::lefrVersionCbkFn(lefrCallbackType_e typ, double version, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        reader->version_ = version;
        return 0;
    }

    int LEFReader::lefrUnitsCbkFn(lefrCallbackType_e typ, lefiUnits *lef_units, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        if (lef_units->hasDatabase())
        {
            auto lef_database_units = lef_units->databaseNumber();
            auto const technology = reader->technology_;
            if (technology->database_units_microns == 0)
            {
                technology->database_units_microns = lef_database_units;
            }
            else
            {
                if (technology->database_units_microns != lef_database_units)
                {
                    log_warning("Database UNITS in LEF {} does not equal current technology units {}. Ignoring new definition.", lef_database_units, technology->database_units_microns);
                }
            }
        }

        auto const technology = reader->technology_;
        if (lef_units->hasCapacitance())
            technology->capacitance_units_pf = lef_units->capacitance();
        if (lef_units->hasResistance())
            technology->resistance_units_ohms = lef_units->resistance();
        if (lef_units->hasPower())
            technology->power_units_mw = lef_units->power();
        if (lef_units->hasCurrent())
            technology->current_units_ma = lef_units->current();
        if (lef_units->hasVoltage())
            technology->voltage_units_v = lef_units->voltage();
        if (lef_units->hasFrequency())
            technology->frequency_units_mhz = lef_units->frequency();

        return 0;
    }

    int LEFReader::lefrBusBitCharsCbkFn(lefrCallbackType_e typ, const char *bus_bit_chars, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        if (bus_bit_chars)
            reader->technology_->bus_bit_chars = bus_bit_chars;
        return 0;
    }

    int LEFReader::lefrDividerCharCbkFn(lefrCallbackType_e typ, const char *divider_char, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        if (divider_char)
            reader->technology_->divider_char = divider_char;
        return 0;
    }

    int LEFReader::lefrFixedMaskCbkFn(lefrCallbackType_e typ, int fixed_mask, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        reader->technology_->fixed_mask = static_cast<bool>(fixed_mask);
        return 0;
    }

    int LEFReader::lefrUseMinSpacingCbkFn(lefrCallbackType_e typ, lefiUseMinSpacing *lef_use_min_spacing, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        const std::string type = lef_use_min_spacing->name();
        const bool on = static_cast<bool>(lef_use_min_spacing->value());
        if (type == "OBS")
            reader->technology_->use_min_spacing_obs = on;
        else if (type == "PIN")
            reader->technology_->use_min_spacing_pin = on;
        return 0;
    }

    int LEFReader::lefrClearanceMeasureCbkFn(lefrCallbackType_e typ, const char *clearance_measure, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        if (clearance_measure)
            reader->technology_->clearance_measure = clearance_measure;
        return 0;
    }

    int LEFReader::lefrManufacturingCbkFn(lefrCallbackType_e typ, double manufacturing_grid, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        reader->technology_->manufacturing_grid = manufacturing_grid;
        return 0;
    }

    int LEFReader::lefrMaxStackViaCbkFn(lefrCallbackType_e typ, lefiMaxStackVia *lef_max_stack_via, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        reader->technology_->max_via_stack = lef_max_stack_via->maxStackVia();
        if (lef_max_stack_via->hasMaxStackViaRange())
        {
            reader->technology_->max_via_stack_bottom_layer = lef_max_stack_via->maxStackViaBottomLayer();
            reader->technology_->max_via_stack_top_layer = lef_max_stack_via->maxStackViaTopLayer();
        }
        return 0;
    }

    int LEFReader::lefrAntennaInputCbkFn(lefrCallbackType_e typ, double antenna_input_gate_area, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        reader->technology_->antenna_input_gate_area = antenna_input_gate_area;
        return 0;
    }

    int LEFReader::lefrAntennaInoutCbkFn(lefrCallbackType_e typ, double antenna_inout_diff_area, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        reader->technology_->antenna_inout_diff_area = antenna_inout_diff_area;
        return 0;
    }

    int LEFReader::lefrAntennaOutputCbkFn(lefrCallbackType_e typ, double antenna_output_diff_area, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        reader->technology_->antenna_output_diff_area = antenna_output_diff_area;
        return 0;
    }

    std::vector<ViaLayerData> LEFReader::via_layers_from_parser(LEFReader *reader, lefiVia *lef_via)
    {
        std::vector<ViaLayerData> layers;
        layers.reserve(static_cast<size_t>(lef_via->numLayers()));
        for (int i = 0; i < lef_via->numLayers(); i++)
        {
            auto layer = ViaLayerData{.layer_name = lef_via->layerName(i)};

            layer.rects.reserve(static_cast<size_t>(lef_via->numRects(i)));
            layer.rect_masks.reserve(static_cast<size_t>(lef_via->numRects(i)));
            for (int j = 0; j < lef_via->numRects(i); j++)
            {
                layer.rects.push_back(rect_from_parser(reader, lef_via->xl(i, j), lef_via->yl(i, j), lef_via->xh(i, j), lef_via->yh(i, j)));
                layer.rect_masks.push_back(lef_via->rectColorMask(i, j));
            }

            layer.polygons.reserve(static_cast<size_t>(lef_via->numPolygons(i)));
            layer.polygon_masks.reserve(static_cast<size_t>(lef_via->numPolygons(i)));
            for (int j = 0; j < lef_via->numPolygons(i); j++)
            {
                const lefiGeomPolygon poly = lef_via->getPolygon(i, j);
                layer.polygons.push_back(polygon_from_parser(reader, poly.numPoints, poly.x, poly.y));
                layer.polygon_masks.push_back(lef_via->polyColorMask(i, j));
            }

            layers.push_back(std::move(layer));
        }
        return layers;
    }

    int LEFReader::lefrViaCbkFn(lefrCallbackType_e typ, lefiVia *lef_via, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        auto via_name = lef_via->name();

        if (reader->root_->get_via_by_name(via_name).valid())
        {
            log_warning("Via {} already exists. Ignoring new definition.", via_name);
            return 0;
        }

        ViaData via{
            .technology = reader->technology_id_,
            .name = via_name,
            .is_default = static_cast<bool>(lef_via->hasDefault()),
        };

        if (lef_via->hasResistance())
            via.resistance = lef_via->resistance();

        // foreign/layers/via_rule moved to pooled classes (Phase 2) -
        // staged here, attached via root_->create_X() once via_id exists
        // (below), same pattern as lefrLayerCbkFn's own pending_* locals.
        std::optional<ForeignData> pending_foreign;
        if (lef_via->hasForeign())
        {
            ForeignData foreign{.name = lef_via->foreign()};

            if (lef_via->hasForeignPnt())
                foreign.origin = Point{
                    .x = reader->microns_to_dbu(lef_via->foreignX()),
                    .y = reader->microns_to_dbu(lef_via->foreignY()),
                };

            if (lef_via->hasForeignOrient())
                foreign.orient = orientation_from_parser(lef_via->foreignOrient());

            pending_foreign = std::move(foreign);
        }

        std::vector<ViaLayerData> pending_layers = via_layers_from_parser(reader, lef_via);

        // 5.6 VIARULE-inside-VIA - a materially different, smaller thing
        // than a VIARULE block itself (see ViaRuleReference's own doc
        // comment) - mutually exclusive with resistance above (a real via
        // has one or the other, matching lefwViaViarule/lefwViaResistance's
        // own "either...or" contract on the writer side).
        std::optional<ViaRuleReferenceData> pending_via_rule;
        if (lef_via->hasViaRule())
        {
            pending_via_rule = ViaRuleReferenceData{
                .via_rule_name = lef_via->viaRuleName(),
                .cut_size = Point{
                    .x = reader->microns_to_dbu(lef_via->xCutSize()),
                    .y = reader->microns_to_dbu(lef_via->yCutSize()),
                },
                .bot_layer_name = lef_via->botMetalLayer(),
                .cut_layer_name = lef_via->cutLayer(),
                .top_layer_name = lef_via->topMetalLayer(),
                .cut_spacing = Point{
                    .x = reader->microns_to_dbu(lef_via->xCutSpacing()),
                    .y = reader->microns_to_dbu(lef_via->yCutSpacing()),
                },
                .bot_enclosure = Point{
                    .x = reader->microns_to_dbu(lef_via->xBotEnc()),
                    .y = reader->microns_to_dbu(lef_via->yBotEnc()),
                },
                .top_enclosure = Point{
                    .x = reader->microns_to_dbu(lef_via->xTopEnc()),
                    .y = reader->microns_to_dbu(lef_via->yTopEnc()),
                },
            };
        }

        // lefiVia's own accessor is numProperties(), not numProps() -
        // otherwise identical shape to the other property-carrying
        // vendored classes.
        via.properties.reserve(static_cast<size_t>(lef_via->numProperties()));
        for (int i = 0; i < lef_via->numProperties(); i++)
        {
            via.properties.push_back(LefProperty{
                .name = lef_via->propName(i),
                .is_number = static_cast<bool>(lef_via->propIsNumber(i)),
                .string_value = lef_via->propIsString(i) ? lef_via->propValue(i) : "",
                .number_value = lef_via->propIsNumber(i) ? lef_via->propNumber(i) : 0.0,
            });
        }

        const ViaId via_id = reader->root_->create_via(std::move(via));
        if (pending_foreign)
        {
            pending_foreign->via = via_id;
            reader->root_->create_foreign(std::move(*pending_foreign));
        }
        for (auto &layer : pending_layers)
        {
            layer.via = via_id;
            reader->root_->create_via_layer(std::move(layer));
        }
        if (pending_via_rule)
        {
            pending_via_rule->via = via_id;
            reader->root_->create_via_rule_reference(std::move(*pending_via_rule));
        }

        return 0;
    }

    int LEFReader::lefrViaRuleCbkFn(lefrCallbackType_e typ, lefiViaRule *lef_via_rule, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        auto via_rule_name = lef_via_rule->name();

        if (reader->root_->get_via_rule_by_name(via_rule_name).valid())
        {
            log_warning("ViaRule {} already exists. Ignoring new definition.", via_rule_name);
            return 0;
        }

        ViaRuleData via_rule{
            .technology = reader->technology_id_,
            .name = via_rule_name,
            .is_generate = static_cast<bool>(lef_via_rule->hasGenerate()),
            .is_default = static_cast<bool>(lef_via_rule->hasDefault()),
        };

        // layers moved to a pooled class (Phase 2) - staged here, attached
        // via root_->create_via_rule_layer() once via_rule_id exists
        // (below). 2 layers (non-GENERATE) or 3 (GENERATE, the 3rd being
        // the cut layer) - see lefiViaRule.hpp's own numLayers()/layer()
        // comment.
        std::vector<ViaRuleLayerData> pending_layers;
        pending_layers.reserve(static_cast<size_t>(lef_via_rule->numLayers()));
        for (int i = 0; i < lef_via_rule->numLayers(); i++)
        {
            const lefiViaRuleLayer *lef_layer = lef_via_rule->layer(i);

            auto layer = ViaRuleLayerData{.layer_name = lef_layer->name()};

            if (lef_layer->hasDirection())
                layer.direction = lef_layer->isVertical() ? RoutingDirection::V : RoutingDirection::H;
            else
                layer.direction = RoutingDirection::NONE;

            if (lef_layer->hasWidth())
            {
                layer.width_min = reader->microns_to_dbu(lef_layer->widthMin());
                layer.width_max = reader->microns_to_dbu(lef_layer->widthMax());
            }
            if (lef_layer->hasOverhang())
                layer.overhang = reader->microns_to_dbu(lef_layer->overhang());
            if (lef_layer->hasMetalOverhang())
                layer.metal_overhang = reader->microns_to_dbu(lef_layer->metalOverhang());
            if (lef_layer->hasEnclosure())
            {
                layer.enclosure_overhang1 = reader->microns_to_dbu(lef_layer->enclosureOverhang1());
                layer.enclosure_overhang2 = reader->microns_to_dbu(lef_layer->enclosureOverhang2());
            }
            if (lef_layer->hasSpacing())
            {
                layer.spacing_step_x = reader->microns_to_dbu(lef_layer->spacingStepX());
                layer.spacing_step_y = reader->microns_to_dbu(lef_layer->spacingStepY());
            }
            if (lef_layer->hasRect())
                layer.rect = rect_from_parser(reader, lef_layer->xl(), lef_layer->yl(), lef_layer->xh(), lef_layer->yh());
            if (lef_layer->hasResistance())
                layer.resistance = lef_layer->resistance();

            pending_layers.push_back(std::move(layer));
        }

        // VIA name list is only meaningful for a non-GENERATE VIARULE (a
        // GENERATE rule produces vias algorithmically, it doesn't list
        // them) - lefiViaRule::numVias()/viaName() would just be empty
        // for a GENERATE rule anyway, but skip the loop entirely to make
        // that explicit rather than relying on it happening to be empty.
        if (!via_rule.is_generate)
        {
            via_rule.via_names.reserve(static_cast<size_t>(lef_via_rule->numVias()));
            for (int i = 0; i < lef_via_rule->numVias(); i++)
                via_rule.via_names.push_back(lef_via_rule->viaName(i));
        }

        via_rule.properties.reserve(static_cast<size_t>(lef_via_rule->numProps()));
        for (int i = 0; i < lef_via_rule->numProps(); i++)
        {
            via_rule.properties.push_back(LefProperty{
                .name = lef_via_rule->propName(i),
                .is_number = static_cast<bool>(lef_via_rule->propIsNumber(i)),
                .string_value = lef_via_rule->propIsString(i) ? lef_via_rule->propValue(i) : "",
                .number_value = lef_via_rule->propIsNumber(i) ? lef_via_rule->propNumber(i) : 0.0,
            });
        }

        const ViaRuleId via_rule_id = reader->root_->create_via_rule(std::move(via_rule));
        for (auto &layer : pending_layers)
        {
            layer.via_rule = via_rule_id;
            reader->root_->create_via_rule_layer(std::move(layer));
        }

        return 0;
    }

    int LEFReader::lefrSiteCbkFn(lefrCallbackType_e typ, lefiSite *lef_site, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        auto site_name = lef_site->name();

        if (reader->root_->get_site_by_name(site_name).valid())
        {
            log_warning("Site {} already exists. Ignoring new definition.", site_name);
            return 0;
        }

        SiteData site{
            .technology = reader->technology_id_,
            .name = site_name,
        };

        if (lef_site->hasClass())
            site.site_class = lef_site->siteClass();

        if (lef_site->hasSize())
            site.size = Point{
                .x = reader->microns_to_dbu(lef_site->sizeX()),
                .y = reader->microns_to_dbu(lef_site->sizeY()),
            };

        // Independent, combinable bit flags (a site can be X- and
        // Y-symmetric and 90-rotatable all at once) - matches
        // Abstract.symmetry's own X/Y/R90 reading in lefrMacroCbkFn.
        site.symmetry = Symmetry{
            .r90 = static_cast<bool>(lef_site->has90Symmetry()),
            .x = static_cast<bool>(lef_site->hasXSymmetry()),
            .y = static_cast<bool>(lef_site->hasYSymmetry()),
        };

        if (lef_site->hasRowPattern())
        {
            site.row_pattern.reserve(static_cast<size_t>(lef_site->numSites()));
            for (int i = 0; i < lef_site->numSites(); i++)
            {
                site.row_pattern.push_back(RowPatternEntry{
                    .site_name = lef_site->siteName(i),
                    .orient = orientation_from_parser(lef_site->siteOrient(i)),
                });
            }
        }

        // lefiSite is structurally different from the other property-
        // carrying vendored classes - it stores full lefiProp* objects
        // (numProps()/prop(i)) rather than parallel arrays, the same
        // lefiProp class PROPERTYDEFINITIONS itself uses (see
        // lefrPropCbkFn) - hasNumber()/number()/hasString()/string()
        // instead of propIsNumber(i)/propNumber(i)/propIsString(i)/propValue(i).
        site.properties.reserve(static_cast<size_t>(lef_site->numProps()));
        for (int i = 0; i < lef_site->numProps(); i++)
        {
            lefiProp *lef_prop = lef_site->prop(i);
            site.properties.push_back(LefProperty{
                .name = lef_prop->propName(),
                .is_number = static_cast<bool>(lef_prop->hasNumber()),
                .string_value = lef_prop->hasString() ? lef_prop->string() : "",
                .number_value = lef_prop->hasNumber() ? lef_prop->number() : 0.0,
            });
        }

        reader->root_->create_site(std::move(site));

        return 0;
    }

    int LEFReader::lefrNonDefaultCbkFn(lefrCallbackType_e typ, lefiNonDefault *lef_non_default, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        auto rule_name = lef_non_default->name();

        if (reader->root_->get_non_default_rule_by_name(rule_name).valid())
        {
            log_warning("NonDefaultRule {} already exists. Ignoring new definition.", rule_name);
            return 0;
        }

        NonDefaultRuleData rule{
            .technology = reader->technology_id_,
            .name = rule_name,
            .hard_spacing = static_cast<bool>(lef_non_default->hasHardspacing()),
        };

        // layers/vias moved to pooled classes (Phase 2) - staged here,
        // attached via root_->create_X() once rule_id exists (below).
        // Each pending via also carries its own nested foreign/layers
        // (also pooled, parented to the via itself, not the rule) -
        // those need the via's own id, so they wait for a second,
        // inner attach step once each NonDefaultRuleVia is created.
        struct PendingVia
        {
            NonDefaultRuleViaData data;
            std::optional<ForeignData> foreign;
            std::vector<ViaLayerData> layers;
        };
        std::vector<NonDefaultRuleLayerData> pending_layers;
        pending_layers.reserve(static_cast<size_t>(lef_non_default->numLayers()));
        for (int i = 0; i < lef_non_default->numLayers(); i++)
        {
            auto layer = NonDefaultRuleLayerData{.layer_name = lef_non_default->layerName(i)};

            if (lef_non_default->hasLayerWidth(i))
                layer.width = reader->microns_to_dbu(lef_non_default->layerWidth(i));
            if (lef_non_default->hasLayerSpacing(i))
                layer.spacing = reader->microns_to_dbu(lef_non_default->layerSpacing(i));
            if (lef_non_default->hasLayerWireExtension(i))
                layer.wire_extension = reader->microns_to_dbu(lef_non_default->layerWireExtension(i));
            if (lef_non_default->hasLayerResistance(i))
                layer.resistance = lef_non_default->layerResistance(i);
            if (lef_non_default->hasLayerCapacitance(i))
                layer.capacitance = lef_non_default->layerCapacitance(i);
            if (lef_non_default->hasLayerEdgeCap(i))
                layer.edge_cap = lef_non_default->layerEdgeCap(i);
            if (lef_non_default->hasLayerDiagWidth(i))
                layer.diag_width = reader->microns_to_dbu(lef_non_default->layerDiagWidth(i));

            pending_layers.push_back(std::move(layer));
        }

        std::vector<PendingVia> pending_vias;
        pending_vias.reserve(static_cast<size_t>(lef_non_default->numVias()));
        for (int i = 0; i < lef_non_default->numVias(); i++)
        {
            lefiVia *lef_via = lef_non_default->viaRule(i);

            PendingVia pending;
            pending.data = NonDefaultRuleViaData{
                .name = lef_via->name(),
                .is_default = static_cast<bool>(lef_via->hasDefault()),
            };
            if (lef_via->hasResistance())
                pending.data.resistance = lef_via->resistance();
            if (lef_via->hasForeign())
            {
                ForeignData foreign{.name = lef_via->foreign()};
                if (lef_via->hasForeignPnt())
                    foreign.origin = Point{
                        .x = reader->microns_to_dbu(lef_via->foreignX()),
                        .y = reader->microns_to_dbu(lef_via->foreignY()),
                    };
                if (lef_via->hasForeignOrient())
                    foreign.orient = orientation_from_parser(lef_via->foreignOrient());
                pending.foreign = std::move(foreign);
            }
            pending.layers = via_layers_from_parser(reader, lef_via);

            pending.data.properties.reserve(static_cast<size_t>(lef_via->numProperties()));
            for (int j = 0; j < lef_via->numProperties(); j++)
            {
                pending.data.properties.push_back(LefProperty{
                    .name = lef_via->propName(j),
                    .is_number = static_cast<bool>(lef_via->propIsNumber(j)),
                    .string_value = lef_via->propIsString(j) ? lef_via->propValue(j) : "",
                    .number_value = lef_via->propIsNumber(j) ? lef_via->propNumber(j) : 0.0,
                });
            }

            pending_vias.push_back(std::move(pending));
        }

        // spacing_rule (the embedded "SPACING ... SAMENET layer1 layer2
        // distance [STACK] ; ... END SPACING" section) is deliberately not
        // modeled - lef.y's own start_spacing grammar rule shows this
        // construct is not legal in a LEF >= 5.7 file at all (parsed only
        // as a no-op warning: "A SPACING SAMENET section is defined but it
        // is not legal in a LEF 5.7 version file... It will be ignored"),
        // so it can never be populated by this project's VERSION 5.8
        // fixtures/targets.

        rule.use_via_names.reserve(static_cast<size_t>(lef_non_default->numUseVia()));
        for (int i = 0; i < lef_non_default->numUseVia(); i++)
            rule.use_via_names.push_back(lef_non_default->viaName(i));

        rule.use_via_rule_names.reserve(static_cast<size_t>(lef_non_default->numUseViaRule()));
        for (int i = 0; i < lef_non_default->numUseViaRule(); i++)
            rule.use_via_rule_names.push_back(lef_non_default->viaRuleName(i));

        rule.min_cuts.reserve(static_cast<size_t>(lef_non_default->numMinCuts()));
        for (int i = 0; i < lef_non_default->numMinCuts(); i++)
        {
            rule.min_cuts.push_back(MinCutOverride{
                .cut_layer_name = lef_non_default->cutLayerName(i),
                .num_cuts = lef_non_default->numCuts(i),
            });
        }

        rule.properties.reserve(static_cast<size_t>(lef_non_default->numProps()));
        for (int i = 0; i < lef_non_default->numProps(); i++)
        {
            rule.properties.push_back(LefProperty{
                .name = lef_non_default->propName(i),
                .is_number = static_cast<bool>(lef_non_default->propIsNumber(i)),
                .string_value = lef_non_default->propIsString(i) ? lef_non_default->propValue(i) : "",
                .number_value = lef_non_default->propIsNumber(i) ? lef_non_default->propNumber(i) : 0.0,
            });
        }

        const NonDefaultRuleId rule_id = reader->root_->create_non_default_rule(std::move(rule));
        for (auto &layer : pending_layers)
        {
            layer.non_default_rule = rule_id;
            reader->root_->create_non_default_rule_layer(std::move(layer));
        }
        for (auto &pending : pending_vias)
        {
            pending.data.non_default_rule = rule_id;
            const NonDefaultRuleViaId via_id = reader->root_->create_non_default_rule_via(std::move(pending.data));
            if (pending.foreign)
            {
                pending.foreign->non_default_rule_via = via_id;
                reader->root_->create_foreign(std::move(*pending.foreign));
            }
            for (auto &layer : pending.layers)
            {
                layer.non_default_rule_via = via_id;
                reader->root_->create_via_layer(std::move(layer));
            }
        }

        return 0;
    }

    int LEFReader::lefrMacroBeginCbkFn(lefrCallbackType_e typ, const char *name, void *user_data)
    {
        spdlog::debug("processing macro {}", name);

        auto reader = static_cast<LEFReader *>(user_data);

        // If library has not been created, then create one
        if (!reader->library_id_.valid())
        {
            reader->library_id_ = reader->root_->create_library(LibraryData{.name = reader->library_name_});
        }

        // If design does not exist, then create it
        auto design_id = reader->root_->get_design_by_name(name);
        if (!design_id.valid())
        {
            design_id = reader->root_->create_design(
                DesignData{
                    .library = reader->library_id_,
                    .name = name,
                });
        }

        // If abstract view exists, then error
        auto abstract_id = reader->root_->get_design_abstract(design_id);
        if (abstract_id.valid())
        {
            log_error("Abstract view for design {} already exists.", name);
            return 1;
        }

        reader->abstract_data_ = AbstractData{.design = design_id};
        reader->abstract_id_ = reader->root_->create_abstract(reader->abstract_data_);

        return 0;
    }

    int LEFReader::lefrMacroCbkFn(lefrCallbackType_e typ, lefiMacro *lef_macro, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);

        if (lef_macro->hasClass())
            reader->abstract_data_.type = lef_macro->macroClass();

        // foreigns moved to a pooled class (Phase 2) - reader->abstract_id_
        // already exists at this point (created in lefrMacroBeginCbkFn),
        // so each Foreign is created directly here rather than staged.
        for (int i = 0; i < lef_macro->numForeigns(); i++)
        {
            ForeignData foreign{.abstract = reader->abstract_id_, .name = lef_macro->foreignName(i)};

            // Not lef_macro->hasForeignOrigin(i): the vendored parser's
            // hasForeignOrigin_ is actually populated from the orient code
            // (see lefiMacro::addForeign), not a real "has a point" flag -
            // hasForeignPoint(i) is the field that's genuinely wired to it.
            if (lef_macro->hasForeignPoint(i))
                foreign.origin = Point{
                    .x = reader->microns_to_dbu(lef_macro->foreignX(i)),
                    .y = reader->microns_to_dbu(lef_macro->foreignY(i)),
                };

            if (lef_macro->hasForeignOrient(i))
                foreign.orient = orientation_from_parser(lef_macro->foreignOrient(i));

            reader->root_->create_foreign(std::move(foreign));
        }

        if (lef_macro->hasSize())
            reader->abstract_data_.size = Point{
                .x = reader->microns_to_dbu(lef_macro->sizeX()),
                .y = reader->microns_to_dbu(lef_macro->sizeY()),
            };

        if (lef_macro->hasOrigin())
            reader->abstract_data_.origin = Point{
                .x = reader->microns_to_dbu(lef_macro->originX()),
                .y = reader->microns_to_dbu(lef_macro->originY()),
            };

        reader->abstract_data_.symmetry = Symmetry{
            .r90 = lef_macro->has90Symmetry() == 1,
            .x = lef_macro->hasXSymmetry() == 1,
            .y = lef_macro->hasYSymmetry() == 1,
        };

        if (lef_macro->hasSiteName())
        {
            reader->abstract_data_.site = lef_macro->siteName();
        }

        // Distinct, mutually-exclusive grammar alternative from the
        // singular hasSiteName()/siteName() above - a macro uses one form
        // or the other (lefiMacro's own setSiteName vs setSitePattern).
        // Also moved to a pooled class (Phase 2) - created directly here,
        // same reasoning as foreigns above.
        for (int i = 0; i < lef_macro->numSitePattern(); i++)
        {
            lefiSitePattern *pattern = lef_macro->sitePattern(i);
            MacroSitePlacementData placement{
                .abstract = reader->abstract_id_,
                .site_name = pattern->name(),
                .origin = Point{.x = reader->microns_to_dbu(pattern->x()), .y = reader->microns_to_dbu(pattern->y())},
                .orient = orientation_from_parser(pattern->orient()),
            };
            if (pattern->hasStepPattern())
            {
                placement.num_x = static_cast<int>(pattern->xStart());
                placement.num_y = static_cast<int>(pattern->yStart());
                placement.step_x = reader->microns_to_dbu(pattern->xStep());
                placement.step_y = reader->microns_to_dbu(pattern->yStep());
            }
            reader->root_->create_macro_site_placement(std::move(placement));
        }

        if (lef_macro->hasEEQ())
            reader->abstract_data_.eeq = lef_macro->EEQ();
        if (lef_macro->hasLEQ())
            reader->abstract_data_.leq = lef_macro->LEQ();
        if (lef_macro->hasPower())
            reader->abstract_data_.power = lef_macro->power();
        if (lef_macro->hasSource())
            reader->abstract_data_.source = lef_macro->source();
        if (lef_macro->isFixedMask())
            reader->abstract_data_.is_fixed_mask = true;

        // lefiMacro's own accessors are numProperties()/propNum(), not
        // numProps()/propNumber() - otherwise identical shape to the
        // other parallel-array property-carrying vendored classes.
        reader->abstract_data_.properties.reserve(static_cast<size_t>(lef_macro->numProperties()));
        for (int i = 0; i < lef_macro->numProperties(); i++)
        {
            reader->abstract_data_.properties.push_back(LefProperty{
                .name = lef_macro->propName(i),
                .is_number = static_cast<bool>(lef_macro->propIsNumber(i)),
                .string_value = lef_macro->propIsString(i) ? lef_macro->propValue(i) : "",
                .number_value = lef_macro->propIsNumber(i) ? lef_macro->propNum(i) : 0.0,
            });
        }

        // Post process
        post_process(reader);

        // Update stored AbstractData in pool
        // TODO: change this to build AbstractData first, then add child objects by
        // iterating through lef_macro data twice.
        if (reader->abstract_id_.valid())
        {
            if (auto stored = reader->root_->get_abstract(reader->abstract_id_))
            {
                *stored = reader->abstract_data_;
            }
        }
        auto stored = reader->root_->get_abstract(reader->abstract_id_);
        spdlog::debug("stored boundary size = {}", stored ? stored->boundary.size() : -1);
        spdlog::debug("stored site = {}", stored ? stored->site.value_or("NONE") : "NONE");

        return 0;
    }

    int LEFReader::lefrPinCbkFn(
        lefrCallbackType_e typ,
        lefiPin *lef_pin,
        void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);

        // TODO: Check that pin doesn't already exists with the same name
        // TODO: link to shematic depending if we are creating the schematic or not

        // The parser reuses one scratch lefiPin across every PIN statement
        // and never resets direction_ between them, so a pin that omits
        // DIRECTION would otherwise silently inherit whichever prior pin
        // last set it (same hazard as lefiLayer's direction_ above).
        auto terminal = TerminalData{
            .abstract = reader->abstract_id_,
            .name = lef_pin->name(),
            .direction = lef_pin->hasDirection() ? signal_direction_from_parser(lef_pin->direction()) : SignalDirection::NONE,
        };

        if (lef_pin->hasShape())
            terminal.shape = lef_pin->shape();
        if (lef_pin->hasUse())
            terminal.use = lef_pin->use();
        if (lef_pin->hasMustjoin())
            terminal.must_join = lef_pin->mustjoin();
        if (lef_pin->hasNetExpr())
            terminal.net_expr = lef_pin->netExpr();
        if (lef_pin->hasLEQ())
            terminal.leq = lef_pin->LEQ();
        if (lef_pin->hasTaperRule())
            terminal.taper_rule = lef_pin->taperRule();
        if (lef_pin->hasSupplySensitivity())
            terminal.supply_sensitivity = lef_pin->supplySensitivity();
        if (lef_pin->hasGroundSensitivity())
            terminal.ground_sensitivity = lef_pin->groundSensitivity();
        // rise_slew_limit/fall_slew_limit/max_load are read-only - no
        // vendored writer function exists for any of them (see
        // write_terminal's own comment).
        if (lef_pin->hasRiseSlewLimit())
            terminal.rise_slew_limit = lef_pin->riseSlewLimit();
        if (lef_pin->hasFallSlewLimit())
            terminal.fall_slew_limit = lef_pin->fallSlewLimit();
        if (lef_pin->hasMaxload())
            terminal.max_load = lef_pin->maxload();
        // POWER/LEAKAGE/CAPACITANCE/RESISTANCE/PULLDOWNRES/TIEOFFR/VHI/VLO/
        // RISEVOLTAGETHRESHOLD/FALLVOLTAGETHRESHOLD/RISETHRESH/FALLTHRESH/
        // RISESATCUR/FALLSATCUR/CURRENTSOURCE are deliberately not read here
        // (even though lefiPin has working has*()/value() accessors for
        // all of them) - lef.y's own grammar action for every one only
        // calls the matching setter when versionNum < 5.4; this project
        // only supports LEF >= 5.4 (see read_lef's own version check), so
        // the vendored reader itself would always discard these before
        // this callback ever runs. See LEFDEF_BUGS.md's "Reader-side:
        // intentional version-obsolescence".
        if (lef_pin->hasMaxdelay())
            terminal.max_delay = lef_pin->maxdelay();

        // lefiPin's own accessors are numProperties()/propNum(), matching
        // lefiMacro's own naming quirk (see lefrMacroCbkFn's own comment).
        terminal.properties.reserve(static_cast<size_t>(lef_pin->numProperties()));
        for (int i = 0; i < lef_pin->numProperties(); i++)
        {
            terminal.properties.push_back(LefProperty{
                .name = lef_pin->propName(i),
                .is_number = static_cast<bool>(lef_pin->propIsNumber(i)),
                .string_value = lef_pin->propIsString(i) ? lef_pin->propValue(i) : "",
                .number_value = lef_pin->propIsNumber(i) ? lef_pin->propNum(i) : 0.0,
            });
        }

        // Flat pre-5.5 (value, layer) antenna fields directly on lefiPin -
        // AntennaGateArea has no such flat form, only the oxide-scoped one
        // below (lefiPin has no numAntennaGateArea()/antennaGateArea()).
        // LAYER is optional on these statements (confirmed in
        // complete.5.8.lef, e.g. "ANTENNAGATEAREA 2 ;") - the vendored
        // parser leaves the matching *Layer(i) entry as a NULL char* (not
        // an empty string) when omitted (lefiMacro.cpp's own
        // addAntennaPartialMetalArea et al.), so every *Layer(i) call here
        // must go through safe_layer_name rather than std::string's
        // nullptr-crashing implicit conversion.
        auto safe_layer_name = [](const char *layer_name)
        { return layer_name ? layer_name : ""; };

        terminal.antenna_partial_metal_area.reserve(static_cast<size_t>(lef_pin->numAntennaPartialMetalArea()));
        for (int i = 0; i < lef_pin->numAntennaPartialMetalArea(); i++)
            terminal.antenna_partial_metal_area.push_back(PinAntennaValue{.value = lef_pin->antennaPartialMetalArea(i), .layer_name = safe_layer_name(lef_pin->antennaPartialMetalAreaLayer(i))});

        terminal.antenna_partial_metal_side_area.reserve(static_cast<size_t>(lef_pin->numAntennaPartialMetalSideArea()));
        for (int i = 0; i < lef_pin->numAntennaPartialMetalSideArea(); i++)
            terminal.antenna_partial_metal_side_area.push_back(PinAntennaValue{.value = lef_pin->antennaPartialMetalSideArea(i), .layer_name = safe_layer_name(lef_pin->antennaPartialMetalSideAreaLayer(i))});

        terminal.antenna_partial_cut_area.reserve(static_cast<size_t>(lef_pin->numAntennaPartialCutArea()));
        for (int i = 0; i < lef_pin->numAntennaPartialCutArea(); i++)
            terminal.antenna_partial_cut_area.push_back(PinAntennaValue{.value = lef_pin->antennaPartialCutArea(i), .layer_name = safe_layer_name(lef_pin->antennaPartialCutAreaLayer(i))});

        terminal.antenna_diff_area.reserve(static_cast<size_t>(lef_pin->numAntennaDiffArea()));
        for (int i = 0; i < lef_pin->numAntennaDiffArea(); i++)
            terminal.antenna_diff_area.push_back(PinAntennaValue{.value = lef_pin->antennaDiffArea(i), .layer_name = safe_layer_name(lef_pin->antennaDiffAreaLayer(i))});

        // 5.5 oxide-scoped antenna models - a distinct, narrower
        // lefiPinAntennaModel class from lefiLayer's own lefiAntennaModel.
        // Moved to a pooled class (Phase 2) - staged here, attached via
        // root_->create_pin_antenna_model() once terminal_id exists
        // (below), same pattern as lefrLayerCbkFn's own pending_* locals.
        std::vector<PinAntennaModelData> pending_antenna_models;
        pending_antenna_models.reserve(static_cast<size_t>(lef_pin->numAntennaModel()));
        for (int i = 0; i < lef_pin->numAntennaModel(); i++)
        {
            lefiPinAntennaModel *lef_pin_model = lef_pin->antennaModel(i);
            PinAntennaModelData pin_model{.oxide = lef_pin_model->antennaOxide()};

            pin_model.gate_area.reserve(static_cast<size_t>(lef_pin_model->numAntennaGateArea()));
            for (int j = 0; j < lef_pin_model->numAntennaGateArea(); j++)
                pin_model.gate_area.push_back(PinAntennaValue{.value = lef_pin_model->antennaGateArea(j), .layer_name = safe_layer_name(lef_pin_model->antennaGateAreaLayer(j))});

            pin_model.max_area_car.reserve(static_cast<size_t>(lef_pin_model->numAntennaMaxAreaCar()));
            for (int j = 0; j < lef_pin_model->numAntennaMaxAreaCar(); j++)
                pin_model.max_area_car.push_back(PinAntennaValue{.value = lef_pin_model->antennaMaxAreaCar(j), .layer_name = safe_layer_name(lef_pin_model->antennaMaxAreaCarLayer(j))});

            pin_model.max_side_area_car.reserve(static_cast<size_t>(lef_pin_model->numAntennaMaxSideAreaCar()));
            for (int j = 0; j < lef_pin_model->numAntennaMaxSideAreaCar(); j++)
                pin_model.max_side_area_car.push_back(PinAntennaValue{.value = lef_pin_model->antennaMaxSideAreaCar(j), .layer_name = safe_layer_name(lef_pin_model->antennaMaxSideAreaCarLayer(j))});

            pin_model.max_cut_car.reserve(static_cast<size_t>(lef_pin_model->numAntennaMaxCutCar()));
            for (int j = 0; j < lef_pin_model->numAntennaMaxCutCar(); j++)
                pin_model.max_cut_car.push_back(PinAntennaValue{.value = lef_pin_model->antennaMaxCutCar(j), .layer_name = safe_layer_name(lef_pin_model->antennaMaxCutCarLayer(j))});

            pending_antenna_models.push_back(std::move(pin_model));
        }

        auto terminal_id = reader->root_->create_terminal(terminal);
        for (auto &model : pending_antenna_models)
        {
            model.terminal = terminal_id;
            reader->root_->create_pin_antenna_model(std::move(model));
        }

        for (int i = 0; i < lef_pin->numPorts(); i++)
        {
            auto lef_port = lef_pin->port(i);
            auto shapes = shapes_from_parser(reader, lef_port);
            auto port = TerminalPortData{.terminal = terminal_id};

            // lefiGeomClassE doesn't belong to any one Shape (see
            // shapes_from_parser's own comment) - scan for it separately.
            for (int j = 0; j < lef_port->numItems(); j++)
            {
                if (lef_port->itemType(j) == lefiGeomEnum::lefiGeomClassE)
                {
                    port.port_class = lef_port->getClass(j);
                    break;
                }
            }

            auto port_id = reader->root_->create_terminal_port(port);
            for (auto &shape : shapes)
            {
                shape.terminal_port = port_id;
                reader->root_->create_shape(std::move(shape));
            }
        }

        return 0;
    }

    int LEFReader::lefrObstructionCbkFn(lefrCallbackType_e typ, lefiObstruction *lef_obs, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);

        auto shapes = shapes_from_parser(reader, lef_obs->geometries());
        spdlog::debug("shapes size = {}", shapes.size());

        auto obstruction_id = reader->root_->create_obstruction(ObstructionData{.abstract = reader->abstract_id_});
        for (auto &shape : shapes)
        {
            shape.obstruction = obstruction_id;
            reader->root_->create_shape(std::move(shape));
        }

        return 0;
    }

    int LEFReader::lefrDensityCbkFn(lefrCallbackType_e typ, lefiDensity *lef_density, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);

        // Fires between MacroBeginCbk and MacroCbk (same lifecycle as
        // Pin/Obstruction) - densities moved to a pooled class (Phase 2),
        // so like Pin/Obstruction it's now created directly here via
        // create_macro_density_layer() (reader->abstract_id_ already
        // exists, created in lefrMacroBeginCbkFn) rather than accumulated
        // on abstract_data_ for lefrMacroCbkFn's final sync to commit.
        for (int i = 0; i < lef_density->numLayer(); i++)
        {
            MacroDensityLayerData layer{.abstract = reader->abstract_id_, .layer_name = lef_density->layerName(i)};

            layer.rects.reserve(static_cast<size_t>(lef_density->numRects(i)));
            layer.values.reserve(static_cast<size_t>(lef_density->numRects(i)));
            for (int j = 0; j < lef_density->numRects(i); j++)
            {
                const lefiGeomRect lef_rect = lef_density->getRect(i, j);
                layer.rects.push_back(rect_from_parser(reader, lef_rect.xl, lef_rect.yl, lef_rect.xh, lef_rect.yh));
                layer.values.push_back(lef_density->densityValue(i, j));
            }

            reader->root_->create_macro_density_layer(std::move(layer));
        }

        return 0;
    }

    int LEFReader::lefrPropCbkFn(lefrCallbackType_e typ, lefiProp *lef_prop, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);

        PropertyDefinitionData def{
            .technology = reader->technology_id_,
            .owner_type = lef_prop->propType(),
            .name = lef_prop->propName(),
            .data_type = std::string(1, lef_prop->dataType()),
        };
        if (lef_prop->hasRange())
        {
            def.range_min = lef_prop->left();
            def.range_max = lef_prop->right();
        }
        if (lef_prop->hasNumber())
            def.default_number = lef_prop->number();
        if (lef_prop->hasString())
            def.default_string = lef_prop->string();

        reader->root_->create_property_definition(std::move(def));

        return 0;
    }

    int64_t LEFReader::microns_squared_to_dbu(const double microns_squared)
    {
        if (technology_->database_units_microns == 0)
            used_dbu_before_units_declared_ = true;
        return std::llround(microns_squared * technology_->database_units_microns * technology_->database_units_microns);
    }

    int64_t LEFReader::microns_to_dbu(const double microns)
    {
        if (technology_->database_units_microns == 0)
            used_dbu_before_units_declared_ = true;
        return std::llround(microns * technology_->database_units_microns);
    }

    Orientation LEFReader::orientation_from_parser(int v)
    {
        // Matches lef.y's `orientation` rule (R0=N, R90=W, R180=S, R270=E,
        // MY=FN, MYR90=FW, MX=FS, MXR90=FE) - a 90 degree rotation turns a
        // North-facing shape to face West, not South, so this is NOT the
        // naive 0=N,1=S,2=E,3=W ordering it's easy to assume instead.
        switch (v)
        {
        case 0:
            return Orientation::N;
        case 1:
            return Orientation::W;
        case 2:
            return Orientation::S;
        case 3:
            return Orientation::E;
        case 4:
            return Orientation::FN;
        case 5:
            return Orientation::FW;
        case 6:
            return Orientation::FS;
        case 7:
            return Orientation::FE;
        default:
            return Orientation::N;
        }
    }

    RoutingDirection LEFReader::routing_direction_from_parser(const char *name)
    {
        if (name == nullptr)
            return RoutingDirection::NONE;

        if (strcmp(name, "HORIZONTAL") == 0)
            return RoutingDirection::H;

        if (strcmp(name, "VERTICAL") == 0)
            return RoutingDirection::V;

        if (strcmp(name, "DIAG45") == 0)
            return RoutingDirection::DIAG45;

        if (strcmp(name, "DIAG135") == 0)
            return RoutingDirection::DIAG135;

        return RoutingDirection::NONE;
    }

    SignalDirection LEFReader::signal_direction_from_parser(const char *name)
    {
        if (name == nullptr)
            return SignalDirection::NONE;

        if (strcmp(name, "INPUT") == 0)
            return SignalDirection::INPUT;

        if (strcmp(name, "OUTPUT") == 0)
            return SignalDirection::OUTPUT;

        if (strcmp(name, "INOUT") == 0)
            return SignalDirection::INOUT;

        if (strcmp(name, "OUTPUT TRISTATE") == 0)
            return SignalDirection::OUTPUT_TRISTATE;

        if (strcmp(name, "FEEDTHRU") == 0)
            return SignalDirection::FEEDTHRU;

        return SignalDirection::NONE;
    }

    Polygon LEFReader::polygon_from_parser(LEFReader *reader, int count, double *x, double *y)
    {
        auto polygon = Polygon{};
        polygon.points.reserve(count);
        for (int k = 0; k < count; k++)
        {
            polygon.points.push_back(Point{
                .x = reader->microns_to_dbu(x[k]),
                .y = reader->microns_to_dbu(y[k]),
            });
        }
        return polygon;
    }

    Rect LEFReader::rect_from_parser(LEFReader *reader, double xl, double yl, double xh, double yh)
    {
        return Rect{
            .ll = Point{
                .x = reader->microns_to_dbu(xl),
                .y = reader->microns_to_dbu(yl),
            },
            .ur = Point{
                .x = reader->microns_to_dbu(xh),
                .y = reader->microns_to_dbu(yh),
            },
        };
    }

    size_t LEFReader::safe_iteration_count(double x_count, double y_count)
    {
        if (!std::isfinite(x_count) || !std::isfinite(y_count) || x_count < 0.0 || y_count < 0.0)
            return 0;

        // Generous but bounded - a real LEF ITERATE statement repeats a
        // handful of times (single/double digits), never anywhere close
        // to this. Checked against each factor individually, before
        // multiplying, so the product itself can't overflow double's
        // range either.
        constexpr double kMaxReasonableCount = 1'000'000.0;
        if (x_count > kMaxReasonableCount || y_count > kMaxReasonableCount)
            return 0;

        return static_cast<size_t>(x_count) * static_cast<size_t>(y_count);
    }

    // The `!shape.has_value()` guards below (RECT/RECT-ITERATE/PATH/
    // PATH-ITERATE/POLYGON/POLYGON-ITERATE without a preceding LAYER) are
    // unreachable in practice - the LEF grammar itself already rejects that
    // as a parse error before any such item is added to `geometries` - but
    // kept anyway as defense-in-depth at this file-parsing boundary rather
    // than removed as dead code.
    std::vector<Shape> LEFReader::shapes_from_parser(LEFReader *reader, lefiGeometries *geometries)
    {
        std::vector<Shape> shapes;
        int64_t width = 0;
        uint64_t geo_count = 0;

        shapes.reserve(geometries->numItems());

        std::optional<Shape> shape;
        for (int j = 0; j < geometries->numItems(); j++)
        {
            auto item_type = geometries->itemType(j);
            switch (item_type)
            {
            case lefiGeomEnum::lefiGeomLayerE:
            {
                if (shape.has_value() && geo_count > 0)
                {
                    shapes.push_back(shape.value());
                    geo_count = 0;
                }

                shape = Shape{.layer_name = geometries->getLayer(j)};
                break;
            }
            case lefiGeomEnum::lefiGeomWidthE:
            {
                width = reader->microns_to_dbu(geometries->getWidth(j));
                break;
            }
            case lefiGeomEnum::lefiGeomRectE:
            {
                if (!shape.has_value())
                {
                    log_error("RECT defined without previous LAYER definition.");
                    break;
                }
                auto lef_rect = geometries->getRect(j);
                auto rect = rect_from_parser(reader, lef_rect->xl, lef_rect->yl, lef_rect->xh, lef_rect->yh);
                shape.value().rects.push_back(rect);
                shape.value().rect_masks.push_back(lef_rect->colorMask);
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomRectIterE:
            {
                if (!shape.has_value())
                {
                    log_error("RECT ITERATE defined without previous LAYER definition.");
                    break;
                }
                auto lef_rect_iter = geometries->getRectIter(j);
                // Stored raw (UPDATES.md 12 Phase 1's ITERATE rework) rather
                // than expanded here - see Pipeline::generate_shapes for the
                // expansion, and LEFReader::safe_iteration_count's own
                // comment for why xStart/yStart (LEF-file-controlled
                // doubles, despite the misleading name they're iteration
                // counts, not start coordinates) need bounds-checking before
                // ever being cast to int.
                if (safe_iteration_count(lef_rect_iter->xStart, lef_rect_iter->yStart) == 0)
                    break;

                RectIterate rect_iter{
                    .rect = rect_from_parser(reader, lef_rect_iter->xl, lef_rect_iter->yl, lef_rect_iter->xh, lef_rect_iter->yh),
                    .num_x = static_cast<int>(lef_rect_iter->xStart),
                    .num_y = static_cast<int>(lef_rect_iter->yStart),
                    .space_x = reader->microns_to_dbu(lef_rect_iter->xStep),
                    .space_y = reader->microns_to_dbu(lef_rect_iter->yStep),
                };
                if (lef_rect_iter->colorMask)
                    rect_iter.mask = lef_rect_iter->colorMask;
                shape.value().rect_iterates.push_back(std::move(rect_iter));
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomPathE:
            {
                if (!shape.has_value())
                {
                    log_error("PATH defined without previous LAYER definition.");
                    break;
                }
                auto lef_path = geometries->getPath(j);
                auto polygon = polygon_from_parser(reader, lef_path->numPoints, lef_path->x, lef_path->y);
                auto path = Path{.polygon = polygon, .width = width};
                shape.value().paths.push_back(path);
                shape.value().path_masks.push_back(lef_path->colorMask);
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomPathIterE:
            {
                if (!shape.has_value())
                {
                    log_error("PATH ITERATE defined without previous LAYER definition.");
                    break;
                }
                auto lef_path_iter = geometries->getPathIter(j);
                if (safe_iteration_count(lef_path_iter->xStart, lef_path_iter->yStart) == 0)
                    break;

                auto polygon = polygon_from_parser(reader, lef_path_iter->numPoints, lef_path_iter->x, lef_path_iter->y);
                PathIterate path_iter{
                    .path = Path{.polygon = polygon, .width = width},
                    .num_x = static_cast<int>(lef_path_iter->xStart),
                    .num_y = static_cast<int>(lef_path_iter->yStart),
                    .space_x = reader->microns_to_dbu(lef_path_iter->xStep),
                    .space_y = reader->microns_to_dbu(lef_path_iter->yStep),
                };
                if (lef_path_iter->colorMask)
                    path_iter.mask = lef_path_iter->colorMask;
                shape.value().path_iterates.push_back(std::move(path_iter));
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomPolygonE:
            {
                if (!shape.has_value())
                {
                    log_error("POLYGON defined without previous LAYER definition.");
                    break;
                }
                auto lef_polygon = geometries->getPolygon(j);
                auto polygon = polygon_from_parser(reader, lef_polygon->numPoints, lef_polygon->x, lef_polygon->y);
                shape.value().polygons.push_back(polygon);
                shape.value().polygon_masks.push_back(lef_polygon->colorMask);
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomPolygonIterE:
            {
                if (!shape.has_value())
                {
                    log_error("POLYGON ITERATE defined without previous LAYER definition.");
                    break;
                }
                auto lef_polygon_iter = geometries->getPolygonIter(j);
                if (safe_iteration_count(lef_polygon_iter->xStart, lef_polygon_iter->yStart) == 0)
                    break;

                auto polygon = polygon_from_parser(reader, lef_polygon_iter->numPoints, lef_polygon_iter->x, lef_polygon_iter->y);
                shape.value().polygon_iterates.push_back(PolygonIterate{
                    .polygon = polygon,
                    .num_x = static_cast<int>(lef_polygon_iter->xStart),
                    .num_y = static_cast<int>(lef_polygon_iter->yStart),
                    .space_x = reader->microns_to_dbu(lef_polygon_iter->xStep),
                    .space_y = reader->microns_to_dbu(lef_polygon_iter->yStep),
                });
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomLayerMinSpacingE:
            {
                if (!shape.has_value())
                {
                    log_error("SPACING defined without previous LAYER definition.");
                    break;
                }
                shape.value().spacing = reader->microns_to_dbu(geometries->getLayerMinSpacing(j));
                // A LAYER occurrence can legitimately carry only a
                // SPACING/DESIGNRULEWIDTH/EXCEPTPGNET override with no
                // RECT/POLYGON/PATH/VIA at all (e.g. complete.5.8.lef's
                // own "PORT CLASS BUMP ; LAYER M2 SPACING 0.06 ; END") -
                // without this, the shape.has_value() && geo_count > 0
                // guard below would silently drop the whole Shape
                // (including this field) since nothing else ever
                // incremented geo_count.
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomLayerRuleWidthE:
            {
                if (!shape.has_value())
                {
                    log_error("DESIGNRULEWIDTH defined without previous LAYER definition.");
                    break;
                }
                shape.value().design_rule_width = reader->microns_to_dbu(geometries->getLayerRuleWidth(j));
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomLayerExceptPgNetE:
            {
                if (!shape.has_value())
                {
                    log_error("EXCEPTPGNET defined without previous LAYER definition.");
                    break;
                }
                shape.value().except_pg_net = true;
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomViaE:
            {
                if (!shape.has_value())
                {
                    log_error("VIA defined without previous LAYER definition.");
                    break;
                }
                lefiGeomVia *lef_via = geometries->getVia(j);
                ShapeVia via{
                    .via_name = lef_via->name,
                    .origin = Point{.x = reader->microns_to_dbu(lef_via->x), .y = reader->microns_to_dbu(lef_via->y)},
                };
                if (lef_via->topMaskNum || lef_via->cutMaskNum || lef_via->bottomMaskNum)
                    via.mask = combine_via_mask(lef_via->topMaskNum, lef_via->cutMaskNum, lef_via->bottomMaskNum);
                shape.value().vias.push_back(std::move(via));
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomViaIterE:
            {
                if (!shape.has_value())
                {
                    log_error("VIA ITERATE defined without previous LAYER definition.");
                    break;
                }
                lefiGeomViaIter *lef_via_iter = geometries->getViaIter(j);
                if (safe_iteration_count(lef_via_iter->xStart, lef_via_iter->yStart) == 0)
                    break;

                ShapeViaIterate via_iter{
                    .via_name = lef_via_iter->name,
                    .origin = Point{.x = reader->microns_to_dbu(lef_via_iter->x), .y = reader->microns_to_dbu(lef_via_iter->y)},
                    .num_x = static_cast<int>(lef_via_iter->xStart),
                    .num_y = static_cast<int>(lef_via_iter->yStart),
                    .space_x = reader->microns_to_dbu(lef_via_iter->xStep),
                    .space_y = reader->microns_to_dbu(lef_via_iter->yStep),
                };
                if (lef_via_iter->topMaskNum || lef_via_iter->cutMaskNum || lef_via_iter->bottomMaskNum)
                    via_iter.mask = combine_via_mask(lef_via_iter->topMaskNum, lef_via_iter->cutMaskNum, lef_via_iter->bottomMaskNum);
                shape.value().via_iterates.push_back(std::move(via_iter));
                geo_count++;
                break;
            }
            // lefiGeomClassE (PORT CLASS) doesn't belong to any one Shape -
            // extracted separately by the caller (see lefrPinCbkFn's own
            // PORT loop). lefiGeomPropE (a PROPERTY on one placed VIA
            // occurrence) and lefiGeomObsTypeE are deliberately unhandled -
            // out of this phase's scope.
            case lefiGeomEnum::lefiGeomClassE:
            case lefiGeomEnum::lefiGeomPropE:
            case lefiGeomEnum::lefiGeomObsTypeE:
            case lefiGeomEnum::lefiGeomUnknown:
            case lefiGeomEnum::lefiGeomEnd:
                break;
            }
        }
        if (shape.has_value() && geo_count > 0)
            shapes.push_back(shape.value());
        return shapes;
    }
}