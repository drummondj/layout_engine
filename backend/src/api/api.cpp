#include "api.hpp"
#include "../database/database.hpp"
#include "../database/filter.hpp"
#include "../editing/editing.hpp"
#include "../geometry/geometry.hpp"
#include "../core/placement_geometry.hpp"
#include "../core/row_geometry.hpp"
#include "../io/lef_reader.hpp"
#include "../io/def_reader.hpp"
#include "../pipelines/abstract_shape_pipeline.hpp"
#include "../pipelines/frame_render_pipeline.hpp"
#include "../pipelines/hierarchy_resolver.hpp"
#include "../pipelines/hit_test.hpp"
#include "../pipelines/layout_shape_pipeline.hpp"
#include "../pipelines/synchronous_stage_runner.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
// Generated apply_<snake>_snapshot(Root&, <Klass>Id, const <Klass>Data&)
// helpers (UPDATES.md item 21) - a real standalone header, unlike every
// other generated_tcl/*.inc fragment, so it's included here with the
// rest of api.cpp's top-level includes rather than spliced into a
// specific scope. Never edit generated_tcl/snapshot_appliers.hpp
// directly - regenerate via the regen-tcl skill.
#include "generated_tcl/snapshot_appliers.hpp"
#include <fmt/format.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <unordered_set>
#include <vector>

// The real, C++-only definition behind the opaque LeHandle - never exposed
// in api.hpp. Owns everything needed to load a LEF file and render it: one
// each of the oneTBB-based pipelines module's own owner objects per handle
// (not one per call), matching the "reuse across repeated calls" lifetime
// their own internal MemoizingStage caching is designed around
// (backend/ONETBB_INTEGRATION.md migration, Phase 5 cutover - replaces the
// former Pipeline/Renderer/InstanceRenderer trio).
//
// `mutex_` exists because this handle genuinely is called from more than
// one thread today, not as defensive-but-unnecessary caution: Flutter's
// external-texture API invokes le_render_pixel_buffer (via
// LeTexture.copyPixelBuffer()) from its own dedicated raster thread once
// per frame, while ordinary pointer/FFI calls (le_set_mouse_position,
// le_mouse_down/up, le_zoom, ...) run on the platform thread - both
// threads reach the same pipelines/Scene/Root state. See every exported
// function's own std::lock_guard for the actual enforcement; see
// le_destroy's doc comment in api.hpp for the one function that can't be
// covered by the handle's own mutex.
struct LeHandle
{
    le::Root root;
    le::ViewLayerSet view_layers;
    le::Scene scene;
    le::AbstractShapePipeline abstract_shape_pipeline;
    le::LayoutShapePipeline layout_shape_pipeline;
    le::FrameRenderPipeline frame_render_pipeline;

    // Resolves Placement -> Design into cached SkPictures and renders the
    // full displayable frame for a Layout view (Migration Step 3 Phase C)
    // - le_render_pixel_buffer calls its own render_layout_frame instead
    // of abstract_shape_pipeline/frame_render_pipeline above when
    // scene.current_layout() is active. Fully self-contained: it owns its
    // own private MouseOverlayStage/RulerOverlayStage/SelectionOverlayStage
    // trio AND its own private RasterizeComposePipeline, entirely separate
    // from frame_render_pipeline's own equivalents (see HierarchyResolver's
    // own class comment for why - a pipeline is a self-contained graph of
    // stages, so this never reaches into frame_render_pipeline's internal
    // nodes).
    le::HierarchyResolver hierarchy_resolver;

    // Undo/redo stack + command-recall log (UPDATES.md item 21) - every
    // generated le_create_X/le_update_X/le_delete_X function records
    // itself into whatever transaction is currently recording (see
    // command_history.is_recording()); Move
    // (le_mouse_up's Edit-mode branch) and le_repl_eval (the Tcl-side
    // wrapper every typed console command goes through) are the two
    // callers that bracket one with begin()/end().
    le::editing::CommandHistory command_history;
    std::mutex mutex_;

    // BUGS_AND_ENHANCEMENTS.md E17 - whether le_render_pixel_buffer is
    // currently doing real work on this handle, for a caller (a Dart-side
    // poll driving the status bar's own spinner) that wants to know
    // without blocking behind the render itself. Deliberately a plain
    // std::atomic<bool>, read/written with no lock - le_is_rendering()
    // must NOT take mutex_ (that's the exact mutex the render itself
    // holds for its own entire duration - see mutex_'s own doc comment -
    // so a lock-taking getter would just block until the render it's
    // reporting on already finished, defeating the whole point). Safe
    // without one: mutex_ already serializes every real le_* call
    // including le_render_pixel_buffer itself, so at most one thread is
    // ever writing this for a given handle at a time; a plain atomic is
    // exactly the right tool for "one writer under a different lock,
    // arbitrary lock-free readers".
    std::atomic<bool> is_rendering_{false};

    // Single-slot cache backing le_object_property_count/le_object_
    // property_at - rebuilt whenever a different LeObjectRef is
    // requested. le::PropertyValue (generated/property.hpp) doubles as
    // LeProperty's string-owning backing store directly - no separate
    // wrapper type needed, its shape already matches LeProperty
    // field-for-field.
    LeObjectRef cached_object_property_ref{.kind = -1, .index = UINT32_MAX, .generation = 0};
    std::vector<le::PropertyValue> cached_object_properties;

    // Backs every le_X_property_path function (UPDATES.md item 19.2's
    // dot-notation/chaining follow-up). le::PropertyValue owns its own
    // std::string storage, and the LeProperty handed back to the caller
    // is just raw c_str() pointers into that storage (same convention as
    // every to_c(PropertyValue) call in this file) - those pointers must
    // point somewhere that outlives the function call, not a local
    // std::optional<PropertyValue>/vector that gets destroyed the moment
    // le_X_property_path returns. This single slot is that backing
    // store, "valid until the next call" like every other single-slot
    // cache above - confirmed the hard way: a resolved value short
    // enough for std::string's small-string optimization ("IN0") kept
    // "working" by accident (its bytes were still sitting, unclobbered,
    // in the just-freed stack slot), while a longer one (a formatted
    // rects/polygons/paths coordinate list, heap-allocated) came back
    // corrupted, since libc++ actually reused/overwrote that freed heap
    // block before the caller read it.
    le::PropertyValue cached_property_path_value;

    // Backs le_message_count/le_message_at (UPDATES.md item 3) - every
    // error/warning/info message produced by this handle's backend
    // operations so far (currently just le_read_lef), in order, never
    // cleared or reordered. std::deque, deliberately not std::vector:
    // le_message_at() hands out a `const char*` promised valid "until
    // the handle is destroyed" (this API's usual string-ownership
    // convention), but std::vector::push_back can reallocate on growth,
    // move-relocating every contained std::string - for a short (SSO)
    // string that relocates its character buffer inline, invalidating
    // any .c_str() a caller is still holding from an earlier call.
    // std::deque::push_back never invalidates references/pointers to
    // existing elements (only iterators), so it's the correct container
    // here.
    std::deque<std::string> messages;

    // Generated TCL property-reading cache - one cached_X_property_id/
    // cached_X_properties pair per TCL-readable class not already covered
    // by hand-written code above. Never edit generated_tcl/
    // handle_fields.inc directly - regenerate via the regen-tcl skill.
#include "generated_tcl/handle_fields.inc"
};

namespace
{
    // Tracy's FrameMarkStart/FrameMarkEnd send the raw pointer value of
    // `name`, not a copy of the string (see TracyProfiler.hpp's
    // SendFrameMark) - the server pairs a Start with its matching End by
    // that pointer, so both call sites in le_render_pixel_buffer must pass
    // the exact same pointer, not two separately-compiled string literals
    // (which the compiler is free, but not guaranteed, to pool into one
    // address). A single named constant guarantees that regardless of
    // compiler/optimization level.
    constexpr const char *kRenderFrameName = "le_render_pixel_buffer";

    // Builds a PipelineOptions snapshot of `handle`'s own current root/
    // view_layers/scene state - every pipelines-module call site below
    // needs one of these (backend/ONETBB_INTEGRATION.md migration, Phase
    // 5 cutover). Read-only (just copies counters/pointers out of
    // `handle`), safe to call regardless of lock state, though every real
    // call site already holds handle->mutex_ by the time it does.
    le::PipelineOptions pipeline_options_for(const LeHandle *handle)
    {
        le::PipelineOptions options;
        options.ctx.root = &handle->root;
        options.ctx.view_layers = &handle->view_layers;
        options.ctx.scene = &handle->scene;
        options.epoch.root_mutation_version = handle->root.mutation_version();
        options.epoch.view_layers_generation = handle->view_layers.generation();
        options.viewport.viewport_version = handle->scene.viewport_version();
        options.viewport.visibility_version = handle->scene.visibility_version();
        options.viewport.scale = handle->scene.scale();
        options.interaction.mouse_version = handle->scene.mouse_version();
        options.interaction.selection_version = handle->scene.selection_version();
        options.interaction.ruler_version = handle->scene.ruler_version();
        return options;
    }

    // Below this many pixels of down-to-up movement, le_mouse_up treats a
    // gesture as a click rather than a drag-select - small enough that an
    // intended click with a little hand tremor still registers as one,
    // large enough that a real rubber-band drag never gets misread as a
    // click. Not exposed/configurable - an implementation detail of the
    // click-vs-drag decision, not a Scene-persistent concern.
    constexpr int32_t kClickDragThresholdPx = 4;

    // Fixed step sizes for the keyboard-triggered canvas-navigation
    // commands (le_key_down's LE_KEY_ZOOM/LE_KEY_FIT/LE_KEY_PAN_*) - not
    // exposed/configurable, same reasoning as kClickDragThresholdPx.
    constexpr double kKeyZoomFactor = 0.3;
    constexpr int32_t kKeyFitPaddingPx = 10;
    constexpr double kKeyPanFactor = 0.25;

    // LE_KEY_SELECT_ALL's own cap (UPDATES.md 9.1) - a design can have
    // far more selectable shapes than are reasonable to hold in the
    // selection at once (Scene::select() is O(1) average per call, but
    // the resulting selection itself, and every later FFI round-trip
    // over it, still scales with however many objects are in it).
    constexpr int32_t kMaxSelectAllCount = 10000;

    // le_tooltip_message's own text (UPDATES.md item 7.3), one constant
    // per Scene::Mode (UPDATES.md item 11) - le_tooltip_message branches
    // on the current mode rather than returning a single fixed string.
    constexpr const char *kSelectModeTooltip =
        "Left click to select. Shift for multi-select. Left click and drag for rectangle multi-select.";
    // UPDATES.md item 21 - Move is the only real editing semantic
    // implemented so far (Resize/Rotate/Align/Delete remain inert UI
    // stubs), so this text describes only that flow.
    constexpr const char *kEditModeTooltip =
        "Ctrl-M or the Move button to arm a move. Click to set the start point, move the mouse, click again "
        "to commit - stays armed for another move until Esc. Shift for free-form (non-orthogonal).";
    constexpr const char *kRulerModeTooltip =
        "Click to add a ruler point. Shift for a non-orthogonal segment. Esc to finish the ruler.";

    // Maps le::PropertyValue::Type (generated/property.hpp) to the C API's
    // LePropertyType - kept as an explicit switch rather than a bare
    // static_cast so a future reordering of either enum fails to compile
    // here instead of silently mislabeling a row's type.
    int32_t to_c_property_type(le::PropertyValue::Type type)
    {
        switch (type)
        {
        case le::PropertyValue::Type::STRING:
            return LE_PROPERTY_TYPE_STRING;
        case le::PropertyValue::Type::INT:
            return LE_PROPERTY_TYPE_INT;
        case le::PropertyValue::Type::DOUBLE:
            return LE_PROPERTY_TYPE_DOUBLE;
        }
        return LE_PROPERTY_TYPE_STRING;
    }

    // LeProperty conversion - shared by every by-id property accessor
    // (le_terminal_property_at et al) and le_object_property_at, so they
    // all build the same row shape from a le::PropertyValue without
    // duplicating the field-by-field mapping.
    LeProperty to_c(const le::PropertyValue &property)
    {
        return LeProperty{
            .name = property.name.c_str(),
            .type = to_c_property_type(property.type),
            .string_value = property.string_value.c_str(),
            .int_value = property.int_value,
            .double_value = property.double_value,
        };
    }

    // UPDATES.md item 19.1's `-filter` validation (every le_get_* function
    // below) - a hand-maintained allowlist of each class's filterable leaf
    // fields and hops, cross-checked directly against each class's own
    // generated get_field()/match_hop() (src/database/generated/*.hpp).
    // Hand-duplicated from schema.py rather than adding a cmg-generated
    // runtime enumeration - a short static list not worth a cross-repo
    // codegen change for. Deliberately narrower than what get_field/
    // match_hop actually dispatch: excludes hops into non-pooled
    // value-list/embedded types (Abstract's bbox/boundary/densities/
    // foreigns/origin/properties/site_placements/size/symmetry; Terminal's
    // antenna_*/properties; Shape's paths/polygons/rects/texts/vias/
    // *_iterates; Design's schematic hop, Schematic not being one of the
    // seven get_* types) - a filter naming one of these is rejected by
    // get_* even though it still works via the unscoped le_search_terminal/
    // etc. escape hatch (item 17).
    struct FilterFieldTable
    {
        std::unordered_set<std::string> leaf_fields;
        std::unordered_map<std::string, std::string> hops; // hop name -> target class name
    };

    const std::unordered_map<std::string, FilterFieldTable> &filter_field_tables()
    {
        static const std::unordered_map<std::string, FilterFieldTable> tables =
#include "generated_tcl/filter_tables.inc"
            return tables;
    }

    // Walks one Comparison's path (the last segment must be a leaf field of
    // whatever class the path has hopped to by then; every earlier segment
    // must be a hop of the class it's checked against, advancing the
    // "current class" to the hop's target). filter.hpp itself never
    // validates field/hop names - an unrecognized one just silently
    // evaluates to no-match (get_field returns nullopt / match_hop returns
    // false) - this is what turns that into a real, reported error instead.
    std::optional<std::string> validate_filter_path(const std::string &root_class, const std::vector<std::string> &path)
    {
        std::string current_class = root_class;
        for (size_t i = 0; i < path.size(); ++i)
        {
            const auto table_it = filter_field_tables().find(current_class);
            if (table_it == filter_field_tables().end())
                return fmt::format("unknown class '{}'", current_class);

            const std::string &segment = path[i];
            const bool is_last = (i + 1 == path.size());
            if (is_last)
            {
                if (table_it->second.leaf_fields.count(segment))
                    return std::nullopt;
                return fmt::format("unknown field '{}' on {}", segment, current_class);
            }

            const auto hop_it = table_it->second.hops.find(segment);
            if (hop_it == table_it->second.hops.end())
                return fmt::format("unknown hop '{}' on {}", segment, current_class);
            current_class = hop_it->second;
        }
        return std::string("empty field path");
    }

    // Recurses through a parsed FilterExpr's And/Or tree, validating every
    // leaf Comparison's path against root_class via validate_filter_path.
    std::optional<std::string> validate_filter_expr(const std::string &root_class, const le::FilterExpr &expr)
    {
        switch (expr.kind)
        {
        case le::FilterExpr::Kind::Comparison:
            return validate_filter_path(root_class, expr.path);
        case le::FilterExpr::Kind::And:
        case le::FilterExpr::Kind::Or:
            for (const le::FilterExpr &child : expr.children)
            {
                if (auto error = validate_filter_expr(root_class, child))
                    return error;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    // Shared parse+validate+push-message sequence every le_get_* function
    // below needs for its `-filter` axis - std::nullopt with `ok` left
    // true means "no -filter given" (skip that axis entirely);
    // std::nullopt with `ok` set false means a parse or validation error
    // already pushed to handle->messages (caller returns -1). Caller must
    // already hold handle->mutex_.
    std::optional<le::FilterExpr> parse_and_validate_filter(LeHandle *handle, const char *caller, const std::string &root_class, const char *filter_expression, bool &ok)
    {
        ok = true;
        if (!filter_expression || filter_expression[0] == '\0')
            return std::nullopt;

        auto parsed = le::parse_filter_expression(filter_expression);
        if (!parsed)
        {
            handle->messages.push_back(fmt::format("ERROR: {}: {}", caller, parsed.error()));
            ok = false;
            return std::nullopt;
        }
        if (auto error = validate_filter_expr(root_class, *parsed))
        {
            handle->messages.push_back(fmt::format("ERROR: {}: {}", caller, *error));
            ok = false;
            return std::nullopt;
        }
        return std::move(*parsed);
    }

    // Single shared/global Technology, same assumption
    // le_snapped_mouse_position's own lookup makes - nullopt if none has
    // been read yet (le_read_lef) or it has no usable scale.
    std::optional<double> database_units_microns(const le::Root &root)
    {
        const auto technology_ids = root.get_technology_ids();
        if (technology_ids.empty())
            return std::nullopt;

        const le::TechnologyData *technology = root.get_technology(technology_ids.front());
        if (!technology || technology->database_units_microns <= 0.0)
            return std::nullopt;

        return technology->database_units_microns;
    }

    // Rounds to the nearest dbu rather than truncating - a caller passing
    // e.g. 0.1um at 1000 dbu/um should get exactly 100 dbu, not silently
    // lose precision to a fractional-dbu rounding direction they didn't
    // choose.
    int64_t to_dbu(double value_um, double dbu_per_um)
    {
        return static_cast<int64_t>(std::llround(value_um * dbu_per_um));
    }

    // A `dbu` field's own dbu-per-micron ratio, for property tables built
    // before any Technology has been loaded (or with an invalid scale) -
    // falls back to 1.0 (no scaling, same raw magnitude the field would
    // have shown before dbu-aware formatting existed) rather than leaving
    // every build_X_properties() call site to invent its own fallback.
    double display_dbu_per_um(const le::Root &root)
    {
        return database_units_microns(root).value_or(1.0);
    }

    // Looks up `name` among an object's already-built to_properties()-
    // style rows - the same rows a bare `get_properties $token` (no
    // property name) already shows. Used by every le_X_property_path for
    // a single-segment (non-chained) path, so a name like Shape's "rects"
    // resolves the same way there - the -filter DSL's get_field() (what
    // resolve_property_path() uses for everything else) only recognizes
    // scalar leaf fields, not list-of-object fields like rects/polygons/
    // paths, so a bare `.rects` used to fail with "unknown field" even
    // though `get_properties $token` (no name) happily showed it.
    std::optional<le::PropertyValue> find_property_by_name(const std::vector<le::PropertyValue> &properties, std::string_view name)
    {
        for (const le::PropertyValue &property : properties)
        {
            if (property.name == name)
                return property;
        }
        return std::nullopt;
    }

    // Generated TCL property-reading surface (internal helpers only -
    // build_X_properties/to_c/from_c overloads, for every TCL-readable
    // class - these stay inside this anonymous namespace since they're
    // never called from another translation unit; see
    // generated_tcl/property_accessors_public.inc, included later in
    // this file inside extern "C", for the externally-linked
    // le_X_property_count/_at/_path etc. build_terminal_properties/
    // build_library_properties/etc.'s own derived "<field>_count" rows
    // (e.g. Terminal's "ports_count") come from here now too - matches
    // every other is_child-field-derived count row exactly (was
    // hand-abbreviated to "port_count" before this migration; see
    // api_test.cpp). Never edit generated_tcl/property_accessors_internal.inc
    // directly - regenerate via the regen-tcl skill instead.
#include "generated_tcl/property_accessors_internal.inc"

    // Lock-free bodies of le_zoom/le_pan/le_fit_scene - factored out so
    // le_key_down's LE_KEY_ZOOM/FIT/PAN_* handling can call them directly
    // while it's already holding handle->mutex_ (std::mutex isn't
    // recursive - calling back into le_zoom/le_pan/le_fit_scene itself
    // from inside le_key_down would deadlock the calling thread against
    // itself). Callers must have already null-checked `handle` and locked
    // its mutex; the public le_zoom/le_pan/le_fit_scene below do exactly
    // that and then delegate here, so the real logic exists in exactly
    // one place either way.
    void zoom_unlocked(LeHandle *handle, double factor, int32_t x, int32_t y)
    {
        const double old_scale = handle->scene.scale();
        const double new_scale = old_scale * (1.0 + factor);
        if (new_scale <= 0.0)
            return;

        const le::Point old_pan = handle->scene.pan();
        const double viewport_height = handle->scene.viewport_height_px();

        // Undo rasterize()'s Y-flip to get from the caller's image-pixel
        // (x, y) - top-left origin, y down - to the dbu point it currently
        // shows, using the *old* scale/pan (see render.hpp's PixelShape /
        // Renderer::rasterize comments for why pan/scale describe the
        // pre-flip transform while (x, y) here is post-flip).
        const double dbu_x = static_cast<double>(old_pan.x) + static_cast<double>(x) / old_scale;
        const double dbu_y = static_cast<double>(old_pan.y) + (viewport_height - static_cast<double>(y)) / old_scale;

        // Re-solve pan so that same dbu point still lands under (x, y) at
        // the new scale, keeping the zoom visually anchored there.
        const double pan_x_double = dbu_x - static_cast<double>(x) / new_scale;
        const double pan_y_double = dbu_y - (viewport_height - static_cast<double>(y)) / new_scale;

        // A `factor` close enough to -1.0 (an ordinary finite double, not
        // just the already-rejected exact -1.0 above) drives new_scale
        // toward zero, blowing up x/new_scale - reject before casting to
        // int64_t below, since casting an out-of-range (or non-finite)
        // double to an integer type is undefined behavior in C++, not a
        // safe wrap or saturation. Matches new_scale <= 0.0's existing
        // "invalid zoom, no-op" behavior rather than clamping to some
        // arbitrary minimum scale.
        constexpr double kInt64Max = static_cast<double>(std::numeric_limits<int64_t>::max());
        if (!std::isfinite(pan_x_double) || !std::isfinite(pan_y_double) ||
            std::abs(pan_x_double) >= kInt64Max || std::abs(pan_y_double) >= kInt64Max)
            return;

        const int64_t pan_x = static_cast<int64_t>(pan_x_double);
        const int64_t pan_y = static_cast<int64_t>(pan_y_double);

        handle->scene.set_scale(new_scale);
        handle->scene.set_pan(le::Point{.x = pan_x, .y = pan_y});
    }

    void pan_unlocked(LeHandle *handle, double x_factor, double y_factor)
    {
        const double scale = handle->scene.scale();
        const le::Point pan = handle->scene.pan();

        const int64_t dx = static_cast<int64_t>(x_factor * handle->scene.viewport_width_px() / scale);
        const int64_t dy = static_cast<int64_t>(y_factor * handle->scene.viewport_height_px() / scale);

        handle->scene.set_pan(le::Point{.x = pan.x + dx, .y = pan.y + dy});
    }

    void fit_scene_unlocked(LeHandle *handle, int32_t padding_px)
    {
        // A Layout view has no current_abstract() (Phase C's own
        // convention: the two "current view" trackers are mutually
        // exclusive) - generate_shapes against an invalid AbstractId
        // returns nothing, so fit_to_content(nullopt, ...) used to reset
        // to scale=1.0/pan={0,0} instead of framing the Layout's own
        // content. Uses the Layout's own declared diearea bbox (same
        // "declared size" convention as layout_declared_bbox in
        // instance_renderer.hpp/layout_die_area_bbox in
        // generate_layout_shapes_stage.hpp) rather than unioning every
        // Placement's own transformed bbox - O(1) instead of
        // O(placement count), and diearea is the DEF-standard bound of
        // everything in it anyway.
        if (handle->scene.current_layout().valid())
        {
            const le::Shape *diearea = handle->root.get_shape(handle->root.get_layout_diearea(handle->scene.current_layout()));
            handle->scene.fit_to_content(diearea ? le::Geometry::bbox(*diearea) : std::nullopt, padding_px);
            return;
        }

        const auto &generated = handle->abstract_shape_pipeline.run_generate_shapes(handle->scene.current_abstract(), pipeline_options_for(handle));

        std::vector<const le::Shape *> shape_ptrs;
        shape_ptrs.reserve(generated.size());
        for (const auto &rs : generated)
            shape_ptrs.push_back(&rs.shape);

        handle->scene.fit_to_content(le::Geometry::bbox(shape_ptrs), padding_px);
    }

    // Widens `bbox` to also enclose `r` - a plain min/max union, same
    // shape as Geometry::expand_bbox (private to that class - this is
    // the small hand-rolled equivalent api.cpp needs at its own call
    // sites for the bare-id (no Shape) selectable kinds, E1).
    void expand_bbox_unlocked(std::optional<le::Rect> &bbox, const le::Rect &r)
    {
        if (!bbox)
        {
            bbox = r;
            return;
        }
        bbox->ll.x = std::min(bbox->ll.x, r.ll.x);
        bbox->ll.y = std::min(bbox->ll.y, r.ll.y);
        bbox->ur.x = std::max(bbox->ur.x, r.ur.x);
        bbox->ur.y = std::max(bbox->ur.y, r.ur.y);
    }

    // LE_KEY_FIT's Ctrl-held branch (UPDATES.md 9.6) - fits the viewport
    // to the current selection's own combined bbox instead of the whole
    // design's. One Root::get_shape(selected.shape_id) lookup per
    // ShapePiece selection entry (owned by `root`, outlives this call, no
    // copy needed), then a single Geometry::bbox call unions them -
    // mirrors fit_scene_unlocked's own shape_ptrs pattern above. E1's own
    // bare-id kinds (Row/Region/Placement - no backing Shape) resolve
    // their own bbox directly and union in via expand_bbox_unlocked
    // instead. A no-op (view unchanged) if nothing is selected, unlike
    // fit_scene_unlocked, which always has the whole design to fall back
    // to.
    void fit_selected_unlocked(LeHandle *handle, int32_t padding_px)
    {
        std::vector<const le::Shape *> shape_ptrs;
        std::optional<le::Rect> bbox;
        const int remaining_depth = std::max(0, handle->scene.hierarchy_depth() - 1);

        for (const le::SelectedObject &selected : handle->scene.selection())
        {
            std::visit([&](const auto &s)
                       {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, le::ShapePiece>)
                {
                    if (const le::Shape *shape = handle->root.get_shape(s.shape_id))
                        shape_ptrs.push_back(shape);
                }
                else if constexpr (std::is_same_v<T, le::RowId>)
                {
                    if (auto row_bbox = le::row_footprint_bbox(handle->root, s))
                        expand_bbox_unlocked(bbox, *row_bbox);
                }
                else if constexpr (std::is_same_v<T, le::RegionId>)
                {
                    if (const le::RegionData *region = handle->root.get_region(s))
                        for (const le::Rect &r : region->rects)
                            expand_bbox_unlocked(bbox, r);
                }
                else if constexpr (std::is_same_v<T, le::PlacementId>)
                {
                    if (auto placement_bbox = le::placement_world_bbox(handle->root, s, remaining_depth))
                        expand_bbox_unlocked(bbox, *placement_bbox);
                } }, selected);
        }

        if (const std::optional<le::Rect> shapes_bbox = le::Geometry::bbox(shape_ptrs))
            expand_bbox_unlocked(bbox, *shapes_bbox);

        if (!bbox)
            return;

        handle->scene.fit_to_content(bbox, padding_px);
    }

    // Builds the one-piece ghost-preview geometry for a single selected
    // piece (UPDATES.md item 21) - a fresh Root lookup plus
    // Geometry::extract_piece, shared by arm_move_unlocked and
    // refresh_armed_move_geometry_unlocked. An empty one-piece Shape
    // (drawing nothing) if the shape itself or the piece index has gone
    // stale since it was selected, rather than crashing or substituting
    // the wrong piece. Also empty (a documented no-op, not a gap) for
    // E1's Row/Placement/Region alternatives - Move only ever operates on
    // ShapePiece selections; moving a Row/Placement/Region is explicitly
    // out of scope for E1 (BUGS_AND_ENHANCEMENTS.md - "selectable", not
    // "movable").
    le::Shape move_ghost_piece_unlocked(LeHandle *handle, const le::SelectedObject &selected)
    {
        const le::ShapePiece *piece = std::get_if<le::ShapePiece>(&selected);
        if (!piece)
            return le::Shape{};

        const le::ShapeData *data = handle->root.get_shape(piece->shape_id);
        if (!data)
            return le::Shape{};
        return le::Geometry::extract_piece(*data, piece->piece_kind, piece->piece_index);
    }

    // LE_KEY_MOVE/le_arm_move's own body (UPDATES.md item 21) - unlocked
    // variant, same reasoning as fit_selected_unlocked/select_all_unlocked
    // below (called from inside le_key_down, which already holds
    // handle->mutex_). Only meaningful in Edit mode with a non-empty
    // selection - the Mode::EDIT check lives here rather than inside
    // Scene::arm_move itself (Scene stays mode-agnostic - see
    // Scene::set_mode's own comment on the analogous Ruler-mode split).
    // Snapshots each selected *piece's* current geometry for the ghost
    // overlay (Scene::MoveState::moving_geometry), piece-granular
    // (UPDATES.md item 21's follow-up) - Move moves exactly whichever
    // pieces are selected, not necessarily every piece of their owning
    // Shapes.
    void arm_move_unlocked(LeHandle *handle)
    {
        if (handle->scene.mode() != le::Scene::Mode::EDIT)
            return;

        std::vector<le::Shape> geometry;
        geometry.reserve(handle->scene.selection().size());
        for (const le::SelectedObject &selected : handle->scene.selection())
            geometry.push_back(move_ghost_piece_unlocked(handle, selected));

        handle->scene.arm_move(std::move(geometry));
    }

    // le_undo/le_redo's own follow-up (UPDATES.md item 21) - unlocked
    // variant, called right after handle->command_history.undo()/redo()
    // succeeds. If Move is currently armed (including the "stays armed
    // after a commit" case - see move_click_unlocked), the moving
    // pieces' geometry may have just changed out from under its own
    // moving_geometry snapshot (taken at the last arm/re-arm, not
    // continuously) - re-snapshot it via Scene::refresh_move_geometry so
    // a subsequent move's ghost preview starts from the actual
    // post-undo/redo position rather than a stale one. A no-op (via
    // refresh_move_geometry's own guard) if Move isn't armed.
    void refresh_armed_move_geometry_unlocked(LeHandle *handle)
    {
        if (!handle->scene.move().armed)
            return;

        std::vector<le::Shape> geometry;
        geometry.reserve(handle->scene.move().moving_pieces.size());
        for (const le::SelectedObject &selected : handle->scene.move().moving_pieces)
            geometry.push_back(move_ghost_piece_unlocked(handle, selected));
        handle->scene.refresh_move_geometry(std::move(geometry));
    }

    // le_mouse_up's Edit-mode branch (UPDATES.md item 21) - unlocked
    // variant, called with handle->mutex_ already held. First click (no
    // anchor yet) sets the move's anchor (Scene::move_set_anchor, which
    // - like Scene::add_ruler_point's own click handling just below in
    // le_mouse_up - reads the separately-tracked *stored* mouse position,
    // not an x/y passed in here); second click (anchor already set)
    // computes the delta and commits: for each moving *piece*, re-fetches
    // its owning Shape's *current* full geometry from Root (not the
    // arm-time snapshot, which is ghost-rendering-only - see
    // arm_move_unlocked), translates only that one piece in place
    // (Geometry::transform_piece_in_place - leaving every sibling piece,
    // including other entries of the same owning Shape, untouched),
    // applies it via Root::update_shape (bypassing the micron-conversion
    // C API layer - Move already works in dbu), and records the whole
    // set as one undo/redo transaction. A no-op if Move isn't armed.
    //
    // Stays armed after a successful commit (re-arms with each moved
    // piece's now-current geometry, ready for an immediate follow-up
    // move) rather than fully clearing Move state - only Escape
    // (le_cancel_move/LE_KEY_FINISH_RULER) or leaving Edit mode
    // (Scene::set_mode's own end_move() call) actually disarms it, so a
    // user moving several pieces in sequence doesn't have to re-press
    // the Move button/Ctrl-M between each one.
    void move_click_unlocked(LeHandle *handle)
    {
        if (!handle->scene.move().armed)
            return;

        if (!handle->scene.move().anchor)
        {
            handle->scene.move_set_anchor();
            return;
        }

        const std::optional<le::Point> delta = handle->scene.move_delta(handle->scene.move_free_form());
        if (!delta)
        {
            handle->scene.end_move();
            return;
        }

        handle->command_history.begin("move");
        const std::vector<le::SelectedObject> moving_pieces = handle->scene.move().moving_pieces;
        for (const le::SelectedObject &selected : moving_pieces)
        {
            // Move only ever commits a ShapePiece - E1's Row/Placement/
            // Region alternatives never reach moving_pieces with real
            // geometry to move (move_ghost_piece_unlocked's own no-op
            // above already keeps them out of ghost rendering); this
            // guard is the matching no-op on the commit side.
            const le::ShapePiece *piece = std::get_if<le::ShapePiece>(&selected);
            if (!piece)
                continue;

            const le::ShapeData *existing = handle->root.get_shape(piece->shape_id);
            if (!existing || !le::Geometry::piece_in_range(*existing, piece->piece_kind, piece->piece_index))
                continue; // stale shape or piece index - skip rather than corrupt an unrelated piece

            const le::ShapeData before = *existing;
            le::ShapeData after = before;
            le::Geometry::transform_piece_in_place(after, piece->piece_kind, piece->piece_index, *delta);
            handle->root.update_shape(piece->shape_id, after.layer, after.purpose, after.paths, after.polygons, after.rects,
                                      after.spacing, after.design_rule_width, after.except_pg_net);
            handle->root.bump_mutation_version();

            if (le::editing::Transaction *txn = handle->command_history.current())
                txn->record_update<le::ShapeId, le::ShapeData>(piece->shape_id, before, after, &le::apply_shape_snapshot);
        }
        handle->command_history.end(/*succeeded=*/true);

        std::vector<le::Shape> geometry;
        geometry.reserve(moving_pieces.size());
        for (const le::SelectedObject &selected : moving_pieces)
            geometry.push_back(move_ghost_piece_unlocked(handle, selected));
        handle->scene.arm_move(std::move(geometry));
    }

    // LE_KEY_SELECT_ALL's own body (UPDATES.md 9.1) - unlocked variant,
    // same reasoning as zoom_unlocked/pan_unlocked/fit_scene_unlocked
    // above (called from inside le_key_down, which already holds
    // handle->mutex_). Deliberately uses generate_shapes +
    // filter_by_layer_visibility directly, *not* pipeline.run() - run()
    // also applies filter_by_viewport_and_size, which would silently
    // exclude anything currently off-screen or sub-pixel from "select
    // all" (the same viewport-independence fit_scene_unlocked's own
    // generate_shapes-direct call above needs, for the same reason).
    // Every selectable shape's own bbox is trivially inside the whole
    // Abstract's bbox, so hit_test_rect against that bbox correctly
    // enumerates every *candidate* Shape (still respecting layer
    // visibility/selectability), with no new traversal.
    //
    // hit_test_rect's own pieces (UPDATES.md item 21) are Pipeline's
    // *rendered* geometry, not necessarily addressable in Root (e.g. an
    // ITERATE-expanded piece has no raw index at all - see HoverTarget's
    // own comment) - only used here to find which ShapeIds are candidates
    // at all; every raw rect/polygon/path of each candidate's actual
    // Root::get_shape() data is then selected directly (select-all means
    // "select everything", not a geometric containment test - no second
    // fully_enclosed_pieces call needed, unlike drag-select).
    void select_all_unlocked(LeHandle *handle)
    {
        const auto &generated = handle->abstract_shape_pipeline.run_generate_shapes(handle->scene.current_abstract(), pipeline_options_for(handle));

        // Deliberately a fresh, standalone LayerVisibilityFilterStage
        // call directly off `generated` (bypassing viewport filtering) -
        // AbstractShapePipeline's own wired chain has no path from
        // AbstractGeometryStage straight to LayerVisibilityFilterStage
        // without going through ViewportFilterStage first (see this
        // function's own comment above for why viewport filtering must
        // be skipped here). Same "fresh, call-local filter instance"
        // pattern HierarchyResolver already uses for its own per-call
        // culling passes.
        le::SynchronousStageRunner<le::LayerVisibilityFilterStage, std::vector<le::RenderedShape>, std::map<le::ViewLayerId, std::vector<le::RenderedShape>>> layer_visibility_runner{"select_all_layer_visibility_filter"};
        const auto &filtered = layer_visibility_runner.run(generated, handle->abstract_shape_pipeline.shapes_version(), pipeline_options_for(handle));

        std::vector<const le::Shape *> shape_ptrs;
        shape_ptrs.reserve(generated.size());
        for (const auto &rs : generated)
            shape_ptrs.push_back(&rs.shape);
        const auto bbox = le::Geometry::bbox(shape_ptrs);
        if (!bbox)
            return;

        handle->scene.clear_selection();

        std::set<le::ShapeId> candidate_ids;
        for (const le::HoverTarget &hit : le::hit_test_rect(filtered, handle->view_layers, handle->scene, *bbox))
            if (hit.shape_id)
                candidate_ids.insert(*hit.shape_id);

        bool capped = false;
        for (const le::ShapeId shape_id : candidate_ids)
        {
            if (capped)
                break;

            const le::ShapeData *data = handle->root.get_shape(shape_id);
            if (!data)
                continue;

            auto select_piece = [&](le::PieceKind kind, size_t index)
            {
                if (capped)
                    return;
                if (static_cast<int32_t>(handle->scene.selection().size()) >= kMaxSelectAllCount)
                {
                    capped = true;
                    return;
                }
                handle->scene.select(shape_id, kind, index);
            };

            for (size_t i = 0; i < data->rects.size(); ++i)
                select_piece(le::PieceKind::RECT, i);
            for (size_t i = 0; i < data->polygons.size(); ++i)
                select_piece(le::PieceKind::POLYGON, i);
            for (size_t i = 0; i < data->paths.size(); ++i)
                select_piece(le::PieceKind::PATH, i);
        }

        if (capped)
            handle->messages.push_back(fmt::format("WARNING: Selection capped at {} objects - design has more.", kMaxSelectAllCount));
    }

    // Every ROUTING-type layer in `technology_id`'s own declaration
    // order (UPDATES.md 9.4) - LE_KEY_1 maps to index 0 here, LE_KEY_2
    // to index 1, etc.
    std::vector<le::LayerId> ordered_routing_layers(const le::Root &root, le::TechnologyId technology_id)
    {
        std::vector<le::LayerId> result;
        for (le::LayerId layer_id : root.get_technology_layers(technology_id))
        {
            const le::LayerData *layer = root.get_layer(layer_id);
            if (layer && layer->type == "ROUTING")
                result.push_back(layer_id);
        }
        return result;
    }

    // Every CUT-type layer strictly between `a` and `b`'s own positions
    // in root.get_technology_layers(technology_id)'s declaration order
    // (UPDATES.md 9.4 - LEF has no distinct "VIA" layer type, vias are
    // TYPE CUT layers - see LeKeyCode's own doc comment). Order-
    // independent (a/b can be passed either way); usually exactly one,
    // but every CUT layer in the gap is returned, not just the first,
    // for an unusual technology that declares more than one.
    std::vector<le::LayerId> cut_layers_between(const le::Root &root, le::TechnologyId technology_id, le::LayerId a, le::LayerId b)
    {
        const auto &layers = root.get_technology_layers(technology_id);

        auto index_of = [&](le::LayerId id) -> std::optional<size_t>
        {
            for (size_t i = 0; i < layers.size(); ++i)
                if (layers[i] == id)
                    return i;
            return std::nullopt;
        };

        const auto index_a = index_of(a);
        const auto index_b = index_of(b);
        if (!index_a || !index_b)
            return {};

        const size_t lo = std::min(*index_a, *index_b);
        const size_t hi = std::max(*index_a, *index_b);

        std::vector<le::LayerId> result;
        for (size_t i = lo + 1; i < hi; ++i)
        {
            const le::LayerData *layer = root.get_layer(layers[i]);
            if (layer && layer->type == "CUT")
                result.push_back(layers[i]);
        }
        return result;
    }

    // LE_KEY_0..LE_KEY_9's own body (UPDATES.md 9.4/9.7) - unlocked-style
    // helper (already inside le_key_down's held mutex, matches the other
    // *_unlocked helpers' own convention above). `routing_index` is
    // 0-based (e.g. LE_KEY_1 with Ctrl not held -> 0, LE_KEY_1 with Ctrl
    // held -> 10, LE_KEY_0 -> 9 - see le_key_down's own switch for the
    // full mapping). No-op if there's no ROUTING layer at that index or
    // no Technology has been read yet.
    void toggle_routing_layer_visibility_unlocked(LeHandle *handle, int routing_index)
    {
        if (handle->root.get_technology_ids().empty())
            return;
        const le::TechnologyId technology_id = handle->root.get_technology_ids().front();

        const auto routing_layers = ordered_routing_layers(handle->root, technology_id);
        if (routing_index < 0 || static_cast<size_t>(routing_index) >= routing_layers.size())
            return;

        const le::LayerData *toggled = handle->root.get_layer(routing_layers[static_cast<size_t>(routing_index)]);
        if (!toggled)
            return;

        handle->scene.set_layer_name_visible(toggled->name, !handle->scene.is_layer_name_visible(toggled->name));

        // Only on this keyboard path (never from a direct
        // le_set_layer_name_visible() call) - re-check every adjacent
        // routing-layer pair and sync the CUT layer(s) between them to
        // "both visible" (UPDATES.md 9.4). Recomputed as a full pass,
        // not just the pairs touching the just-toggled layer - simpler
        // to reason about/test, and a technology has at most a few
        // dozen routing layers so the cost is trivial.
        for (size_t i = 0; i + 1 < routing_layers.size(); ++i)
        {
            const le::LayerData *first = handle->root.get_layer(routing_layers[i]);
            const le::LayerData *second = handle->root.get_layer(routing_layers[i + 1]);
            if (!first || !second)
                continue;

            const bool both_visible = handle->scene.is_layer_name_visible(first->name) && handle->scene.is_layer_name_visible(second->name);
            for (le::LayerId cut_id : cut_layers_between(handle->root, technology_id, routing_layers[i], routing_layers[i + 1]))
            {
                const le::LayerData *cut = handle->root.get_layer(cut_id);
                if (cut)
                    handle->scene.set_layer_name_visible(cut->name, both_visible);
            }
        }
    }

    // --- Generic LeObjectRef dispatch (UPDATES.md 7.2's database-hierarchy
    // Property Viewer redesign) - le_object_property_count/_at/
    // le_object_parent/le_selected_object_ref's own internal machinery.
    // Every ref's `index`/`generation` pair is exactly one of the seven
    // LeXxxId structs' own fields, so converting is a bare field copy. ---

    LeObjectRef invalid_object_ref()
    {
        return LeObjectRef{.kind = LE_OBJECT_KIND_LIBRARY, .index = UINT32_MAX, .generation = 0};
    }

    bool same_object_ref(LeObjectRef a, LeObjectRef b)
    {
        return a.kind == b.kind && a.index == b.index && a.generation == b.generation;
    }

    template <typename IdT>
    IdT id_from_ref(LeObjectRef ref)
    {
        return IdT{.index = ref.index, .generation = ref.generation};
    }

    LeObjectRef ref_from_id(LeObjectKind kind, le::LibraryId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::DesignId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::AbstractId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::TerminalId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::TerminalPortId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::ObstructionId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::ShapeId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    // E1 (BUGS_AND_ENHANCEMENTS.md) - the six top-level Layout-view kinds
    // Scene::SelectedObject's own variant grew, plus PhysicalPortSegment
    // (an intermediate parent-hop node only, mirroring TerminalPort).
    LeObjectRef ref_from_id(LeObjectKind kind, le::RowId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::PlacementId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::BlockageId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::RouteId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::PhysicalPortId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::RegionId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::PhysicalPortSegmentId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }
    LeObjectRef ref_from_id(LeObjectKind kind, le::LayoutId id) { return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation}; }

    // Dispatches to the same by-id property builder each class's own
    // le_X_property_count/_at already uses (build_library_properties et
    // al, all lock-free, defined earlier in this file) - never the public
    // le_X_property_at functions themselves, which each take
    // handle->mutex_ on their own; le_object_property_count/_at take it
    // exactly once, so calling back into a lock-taking function here
    // would self-deadlock (std::mutex isn't recursive).
    std::vector<le::PropertyValue> build_object_properties(const le::Root &root, LeObjectRef ref)
    {
        switch (static_cast<LeObjectKind>(ref.kind))
        {
        case LE_OBJECT_KIND_LIBRARY:
            return build_library_properties(root, id_from_ref<le::LibraryId>(ref));
        case LE_OBJECT_KIND_DESIGN:
            return build_design_properties(root, id_from_ref<le::DesignId>(ref));
        case LE_OBJECT_KIND_ABSTRACT:
            return build_abstract_properties(root, id_from_ref<le::AbstractId>(ref));
        case LE_OBJECT_KIND_TERMINAL:
            return build_terminal_properties(root, id_from_ref<le::TerminalId>(ref));
        case LE_OBJECT_KIND_TERMINAL_PORT:
            return build_terminal_port_properties(root, id_from_ref<le::TerminalPortId>(ref));
        case LE_OBJECT_KIND_OBSTRUCTION:
            return build_obstruction_properties(root, id_from_ref<le::ObstructionId>(ref));
        case LE_OBJECT_KIND_SHAPE:
            return build_shape_properties(root, id_from_ref<le::ShapeId>(ref));
        case LE_OBJECT_KIND_ROW:
            return build_row_properties(root, id_from_ref<le::RowId>(ref));
        case LE_OBJECT_KIND_PLACEMENT:
            return build_placement_properties(root, id_from_ref<le::PlacementId>(ref));
        case LE_OBJECT_KIND_BLOCKAGE:
            return build_blockage_properties(root, id_from_ref<le::BlockageId>(ref));
        case LE_OBJECT_KIND_ROUTE:
            return build_route_properties(root, id_from_ref<le::RouteId>(ref));
        case LE_OBJECT_KIND_PHYSICAL_PORT:
            return build_physical_port_properties(root, id_from_ref<le::PhysicalPortId>(ref));
        case LE_OBJECT_KIND_REGION:
            return build_region_properties(root, id_from_ref<le::RegionId>(ref));
        case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
            return build_physical_port_segment_properties(root, id_from_ref<le::PhysicalPortSegmentId>(ref));
        case LE_OBJECT_KIND_LAYOUT:
            return build_layout_properties(root, id_from_ref<le::LayoutId>(ref));
        }
        return {};
    }

    // `ref`'s immediate parent - the same parent-hop graph
    // filter_field_tables() already declares for -filter validation
    // (Shape->terminal_port/obstruction, TerminalPort->terminal,
    // Terminal/Obstruction->abstract, Abstract->design, Design->library),
    // read directly off each class's own schema parent field. Library has
    // no parent. Degrades to invalid_object_ref() if `ref` doesn't
    // resolve to a real object (rather than asserting) - same graceful-
    // degradation convention as every other lookup in this file.
    LeObjectRef object_ref_parent(const le::Root &root, LeObjectRef ref)
    {
        switch (static_cast<LeObjectKind>(ref.kind))
        {
        case LE_OBJECT_KIND_LIBRARY:
            return invalid_object_ref();
        case LE_OBJECT_KIND_DESIGN:
        {
            const le::DesignData *design = root.get_design(id_from_ref<le::DesignId>(ref));
            return design ? ref_from_id(LE_OBJECT_KIND_LIBRARY, design->library) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_ABSTRACT:
        {
            const le::AbstractData *abstract = root.get_abstract(id_from_ref<le::AbstractId>(ref));
            return abstract ? ref_from_id(LE_OBJECT_KIND_DESIGN, abstract->design) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_TERMINAL:
        {
            const le::TerminalData *terminal = root.get_terminal(id_from_ref<le::TerminalId>(ref));
            return terminal ? ref_from_id(LE_OBJECT_KIND_ABSTRACT, terminal->abstract) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_TERMINAL_PORT:
        {
            const le::TerminalPortData *port = root.get_terminal_port(id_from_ref<le::TerminalPortId>(ref));
            return port ? ref_from_id(LE_OBJECT_KIND_TERMINAL, port->terminal) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_OBSTRUCTION:
        {
            const le::ObstructionData *obstruction = root.get_obstruction(id_from_ref<le::ObstructionId>(ref));
            return obstruction ? ref_from_id(LE_OBJECT_KIND_ABSTRACT, obstruction->abstract) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_SHAPE:
        {
            const le::ShapeData *shape = root.get_shape(id_from_ref<le::ShapeId>(ref));
            if (!shape)
                return invalid_object_ref();
            if (shape->terminal_port.valid())
                return ref_from_id(LE_OBJECT_KIND_TERMINAL_PORT, shape->terminal_port);
            if (shape->obstruction.valid())
                return ref_from_id(LE_OBJECT_KIND_OBSTRUCTION, shape->obstruction);
            // E1 (BUGS_AND_ENHANCEMENTS.md) - the three Layout-view
            // multi-parent fields Shape gained alongside terminal_port/
            // obstruction (schema.py).
            if (shape->blockage.valid())
                return ref_from_id(LE_OBJECT_KIND_BLOCKAGE, shape->blockage);
            if (shape->route.valid())
                return ref_from_id(LE_OBJECT_KIND_ROUTE, shape->route);
            if (shape->physical_port_segment.valid())
                return ref_from_id(LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT, shape->physical_port_segment);
            return invalid_object_ref(); // shouldn't happen - mutually exclusive per schema.py - degrade gracefully anyway
        }
        case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
        {
            const le::PhysicalPortSegmentData *segment = root.get_physical_port_segment(id_from_ref<le::PhysicalPortSegmentId>(ref));
            return segment ? ref_from_id(LE_OBJECT_KIND_PHYSICAL_PORT, segment->physical_port) : invalid_object_ref();
        }
        // E1 - Row/Placement/Blockage/Route/PhysicalPort/Region's own
        // parent is a Layout, which now has its own LeObjectKind (see
        // LE_OBJECT_KIND_LAYOUT below) so this hop continues on up to
        // Design -> Library instead of dead-ending here.
        case LE_OBJECT_KIND_ROW:
        {
            const le::RowData *row = root.get_row(id_from_ref<le::RowId>(ref));
            return row ? ref_from_id(LE_OBJECT_KIND_LAYOUT, row->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_PLACEMENT:
        {
            const le::PlacementData *placement = root.get_placement(id_from_ref<le::PlacementId>(ref));
            return placement ? ref_from_id(LE_OBJECT_KIND_LAYOUT, placement->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_BLOCKAGE:
        {
            const le::BlockageData *blockage = root.get_blockage(id_from_ref<le::BlockageId>(ref));
            return blockage ? ref_from_id(LE_OBJECT_KIND_LAYOUT, blockage->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_ROUTE:
        {
            const le::RouteData *route = root.get_route(id_from_ref<le::RouteId>(ref));
            return route ? ref_from_id(LE_OBJECT_KIND_LAYOUT, route->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_PHYSICAL_PORT:
        {
            const le::PhysicalPortData *port = root.get_physical_port(id_from_ref<le::PhysicalPortId>(ref));
            return port ? ref_from_id(LE_OBJECT_KIND_LAYOUT, port->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_REGION:
        {
            const le::RegionData *region = root.get_region(id_from_ref<le::RegionId>(ref));
            return region ? ref_from_id(LE_OBJECT_KIND_LAYOUT, region->layout) : invalid_object_ref();
        }
        case LE_OBJECT_KIND_LAYOUT:
        {
            const le::LayoutData *layout = root.get_layout(id_from_ref<le::LayoutId>(ref));
            return layout ? ref_from_id(LE_OBJECT_KIND_DESIGN, layout->design) : invalid_object_ref();
        }
        }
        return invalid_object_ref();
    }
}

extern "C"
{
    // Generated TCL property-reading surface (public, externally-linked
    // functions - le_X_property_count/_at/_path, friendly-id-by-name
    // lookups, is_child-field enumeration - declared in api.hpp's own
    // generated_tcl/declarations.inc, called from le_tcl_shim.cpp) for
    // every TCL-readable class not already covered by hand-written code.
    // Must live inside this extern "C" block, not the anonymous namespace
    // above (internal linkage there would make these unresolvable from
    // other translation units) - see
    // generated_tcl/property_accessors_public.inc's own header comment.
    // Never edit that file directly - regenerate via the regen-tcl skill.
#include "generated_tcl/property_accessors_public.inc"

    // Generated get_<type> search (le_get_X/le_search_result_X_at) for
    // every TCL-readable class - same external-linkage requirement as
    // property_accessors_public.inc above. Never edit that file directly
    // - regenerate via the regen-tcl skill.
#include "generated_tcl/search.inc"

    LeHandle *le_create(void)
    {
        return new LeHandle();
    }

    void le_destroy(LeHandle *handle)
    {
        delete handle;
    }

    int le_read_lef(LeHandle *handle, const char *path)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        if (!path)
        {
            handle->messages.push_back("ERROR: le_read_lef: path is null");
            return 1;
        }

        // UPDATES.md 10 - snapshot the Technology's own layer count before
        // this read, so the default-visibility pass below (after the read)
        // can tell which physical layers this specific call newly
        // introduced, as opposed to ones a prior le_read_lef call already
        // defaulted - LEF layers are only ever appended to a Technology
        // within a session (see LEFReader's own is_technology_empty()-
        // gated create-vs-reuse), so get_technology_layers' existing
        // declaration-order prefix stays stable across reads and re-
        // running the default over it would silently re-hide a layer the
        // user has since made visible via the layer manager.
        size_t old_layer_count = 0;
        {
            const auto existing_technology_ids = handle->root.get_technology_ids();
            if (!existing_technology_ids.empty())
                old_layer_count = handle->root.get_technology_layers(existing_technology_ids.front()).size();
        }

        const std::filesystem::path lef_path(path);
        le::LEFReader reader;
        const int result = reader.read_lef(lef_path.string(), handle->root, lef_path.stem().string());
        for (const auto &msg : reader.messages())
            handle->messages.push_back(msg);
        // if (result == 0)
        //     handle->messages.push_back(fmt::format("INFO: Loaded {}.", path));
        if (result != 0)
            return result;

        // Rebuilt after every successful read, not just the first, so a
        // later LEF file's own new physical layers (e.g. a second macro
        // file with inline LAYER declarations) are picked up too - cheap
        // relative to a full LEF parse, so correctness here wins over the
        // small extra cost without needing a benchmark to justify it.
        const auto technology_ids = handle->root.get_technology_ids();
        if (!technology_ids.empty())
        {
            // UPDATES.md 10 - every physical layer this read newly
            // introduced defaults to hidden unless it's ROUTING or CUT.
            // BOUNDARY isn't a physical layer at all (a Shape with
            // purpose=BOUNDARY instead - see Shape.layer/.purpose's own
            // schema.py comments), so it never reaches this loop - stays
            // visible via Scene::is_layer_name_visible's own default-
            // true-until-toggled behavior, same as it always has.
            const auto &layers = handle->root.get_technology_layers(technology_ids.front());
            for (size_t i = old_layer_count; i < layers.size(); ++i)
            {
                const le::LayerData *layer = handle->root.get_layer(layers[i]);
                if (layer && layer->type != "ROUTING" && layer->type != "CUT")
                    handle->scene.set_layer_name_visible(layer->name, false);
            }

            handle->view_layers = le::ViewLayerSet::build_for_technology(handle->root, technology_ids.front());

            // Also selects the singleton Technology as the current one for
            // the generated TCL current-instance mechanism (see
            // codegen/codegen/tcl_scope.py's own module docstring for why
            // get_layers/get_vias/etc.'s default scope needs this) -
            // Technology has no separate "open" step the way a Design/
            // Abstract does (le_tcl_procs.tcl's open_design), it's
            // implicitly read alongside everything else in a LEF file, so
            // this is the one natural chokepoint - shared by every caller
            // (Dart FFI direct, or TCL's own read_lef, which calls this
            // same function), not just the TCL-facing shim. Idempotent to
            // repeat across multiple le_read_lef calls on the same handle.
            handle->current_technology_id = technology_ids.front();
        }

        return 0;
    }

    int le_read_def(LeHandle *handle, const char *path)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        if (!path)
        {
            handle->messages.push_back("ERROR: le_read_def: path is null");
            return 1;
        }

        const std::filesystem::path def_path(path);
        le::DEFReader reader;
        const int result = reader.read_def(def_path.string(), handle->root, def_path.stem().string());
        for (const auto &msg : reader.messages())
            handle->messages.push_back(msg);
        if (result != 0)
            return result;

        // NONDEFAULTRULES may have resolved/created the shared Technology
        // (DEFReader::technology_id_, same reuse-or-create pattern
        // le_read_lef's own LEFReader uses) even if no LEF has been read
        // into this handle yet - mirror le_read_lef's own
        // current_technology_id selection so the generated TCL
        // current-instance mechanism still works. Layer-visibility
        // defaults and ViewLayerSet aren't touched here - DEF doesn't
        // introduce new physical Layers of its own (see this function's
        // own api.hpp comment).
        const auto technology_ids = handle->root.get_technology_ids();
        if (!technology_ids.empty())
            handle->current_technology_id = technology_ids.front();

        return 0;
    }

    int32_t le_message_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->messages.size());
    }

    const char *le_message_at(LeHandle *handle, int32_t index)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        if (index < 0 || static_cast<size_t>(index) >= handle->messages.size())
            return nullptr;
        return handle->messages[static_cast<size_t>(index)].c_str();
    }

    int32_t le_design_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_design_size());
    }

    const char *le_design_name(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return nullptr;

        const le::DesignData *design = handle->root.get_design(design_ids[static_cast<size_t>(index)]);
        return design ? design->name.c_str() : nullptr;
    }

    int le_set_current_design_abstract(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return 1;

        // Moves the Abstract-view "current view" trackers together -
        // Scene's own (drives GUI rendering) and the generated
        // has_current_access one (handle->current_abstract_id, what
        // get_terminals/get_shapes/etc.'s own default -of-omitted scope
        // and resolve_terminal_id derive from - see current_abstract_id's
        // own declaration comment). Selecting a Design from either FFI
        // caller (a Dart-driven GUI) or a TCL script (open_design,
        // itself calling le_set_current_design_abstract_by_id below - the same
        // shared entry point) should mean the same thing to both.
        // Clears current_layout_id and Scene's own current_layout
        // (Migration Step 3 Phase C): only one view is "open" at a time
        // (see le_set_current_design_layout's own comment) - selecting
        // the Abstract view deactivates the Layout one, same as the
        // reverse.
        const le::DesignId design_id = design_ids[static_cast<size_t>(index)];
        const le::AbstractId abstract_id = handle->root.get_design_abstract(design_id);
        handle->scene.set_current_abstract(abstract_id);
        handle->scene.set_current_layout(le::LayoutId{});
        handle->current_abstract_id = abstract_id;
        handle->current_layout_id = le::LayoutId{};
        return 0;
    }

    int32_t le_library_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_library_size());
    }

    LeLibraryInfo le_library_at(LeHandle *handle, int32_t index)
    {
        const LeLibraryInfo invalid{.id = {UINT32_MAX, 0}, .name = nullptr};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto library_ids = handle->root.get_library_ids();
        if (static_cast<size_t>(index) >= library_ids.size())
            return invalid;

        const le::LibraryId id = library_ids[static_cast<size_t>(index)];
        const le::LibraryData *library = handle->root.get_library(id);
        return LeLibraryInfo{.id = to_c(id), .name = library ? library->name.c_str() : nullptr};
    }

    int32_t le_library_design_count(LeHandle *handle, int32_t library_index)
    {
        if (!handle || library_index < 0)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto library_ids = handle->root.get_library_ids();
        if (static_cast<size_t>(library_index) >= library_ids.size())
            return 0;

        return static_cast<int32_t>(handle->root.get_library_designs(library_ids[static_cast<size_t>(library_index)]).size());
    }

    LeDesignInfo le_library_design_at(LeHandle *handle, int32_t library_index, int32_t design_index)
    {
        const LeDesignInfo invalid{.library_id = {UINT32_MAX, 0}, .id = {UINT32_MAX, 0}, .abstract_id = {UINT32_MAX, 0}, .layout_id = {UINT32_MAX, 0}, .name = nullptr};
        if (!handle || library_index < 0 || design_index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto library_ids = handle->root.get_library_ids();
        if (static_cast<size_t>(library_index) >= library_ids.size())
            return invalid;

        const le::LibraryId library_id = library_ids[static_cast<size_t>(library_index)];
        const auto &design_ids = handle->root.get_library_designs(library_id);
        if (static_cast<size_t>(design_index) >= design_ids.size())
            return invalid;

        const le::DesignId design_id = design_ids[static_cast<size_t>(design_index)];
        const le::DesignData *design = handle->root.get_design(design_id);
        return LeDesignInfo{
            .library_id = to_c(library_id),
            .id = to_c(design_id),
            .abstract_id = to_c(handle->root.get_design_abstract(design_id)),
            .layout_id = to_c(handle->root.get_design_layout(design_id)),
            .name = design ? design->name.c_str() : nullptr,
        };
    }

    // Technology is a single shared per-session instance, same assumption
    // database_units_microns() already makes (root.get_technology_ids().front()).
    // Hand-written, not generated - it's a session/singleton lookup, not
    // per-class CRUD (see backend/CLAUDE.md's TCL section).
    LeTechnologyId le_technology_id(LeHandle *handle)
    {
        const LeTechnologyId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto technology_ids = handle->root.get_technology_ids();
        if (technology_ids.empty())
            return invalid;
        return to_c(technology_ids.front());
    }

    int le_set_current_design_abstract_by_id(LeHandle *handle, LeDesignId design_id)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::DesignId id = from_c(design_id);
        if (!handle->root.get_design(id))
            return 1;

        // See le_set_current_design_abstract's own comment above - the
        // Abstract-view trackers move together, current_layout_id clears.
        const le::AbstractId abstract_id = handle->root.get_design_abstract(id);
        handle->scene.set_current_abstract(abstract_id);
        handle->scene.set_current_layout(le::LayoutId{});
        handle->current_abstract_id = abstract_id;
        handle->current_layout_id = le::LayoutId{};
        return 0;
    }

    int le_set_current_design_layout(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return 1;

        // Mirror image of le_set_current_design_abstract: activates the
        // Layout view's own current-instance tracker (what get_rows/
        // get_placements/get_blockages/etc.'s own default -of-omitted
        // scope derives from) and Scene's own current_layout (Migration
        // Step 3 Phase C - what le_render_pixel_buffer now actually
        // renders, via InstanceRenderer::render_layout_frame), and
        // deactivates the Abstract-view ones - only one view is "open" at
        // a time, matching a real GUI showing one editor.
        const le::DesignId design_id = design_ids[static_cast<size_t>(index)];
        const le::LayoutId layout_id = handle->root.get_design_layout(design_id);
        handle->scene.set_current_layout(layout_id);
        handle->scene.set_current_abstract(le::AbstractId{});
        handle->current_layout_id = layout_id;
        handle->current_abstract_id = le::AbstractId{};
        return 0;
    }

    int le_set_current_design_layout_by_id(LeHandle *handle, LeDesignId design_id)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::DesignId id = from_c(design_id);
        if (!handle->root.get_design(id))
            return 1;

        // See le_set_current_design_layout's own comment above.
        const le::LayoutId layout_id = handle->root.get_design_layout(id);
        handle->scene.set_current_layout(layout_id);
        handle->scene.set_current_abstract(le::AbstractId{});
        handle->current_layout_id = layout_id;
        handle->current_abstract_id = le::AbstractId{};
        return 0;
    }

    int32_t le_hierarchy_depth(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->scene.hierarchy_depth());
    }

    void le_set_hierarchy_depth(LeHandle *handle, int32_t depth)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_hierarchy_depth(depth);
    }

    int32_t le_layer_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->view_layers.rows().size());
    }

    LeLayerRow le_layer_at(LeHandle *handle, int32_t row_index)
    {
        const LeLayerRow invalid{.name = nullptr, .color_r = 0, .color_g = 0, .color_b = 0, .has_physical_layer = 0};
        if (!handle || row_index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto &rows = handle->view_layers.rows();
        if (static_cast<size_t>(row_index) >= rows.size())
            return invalid;

        const le::ViewLayerRow &row = rows[static_cast<size_t>(row_index)];
        const le::ViewLayerData *first_column = row.columns.empty() ? nullptr : handle->view_layers.get(row.columns.front().id);

        return LeLayerRow{
            .name = row.name.c_str(),
            .color_r = first_column ? first_column->style.outline_color.r : uint8_t{0},
            .color_g = first_column ? first_column->style.outline_color.g : uint8_t{0},
            .color_b = first_column ? first_column->style.outline_color.b : uint8_t{0},
            .has_physical_layer = (first_column && first_column->layer.valid()) ? 1 : 0,
        };
    }

    int32_t le_purpose_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->view_layers.purposes().size());
    }

    int32_t le_purpose_at(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return -1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto purposes = handle->view_layers.purposes();
        if (static_cast<size_t>(index) >= purposes.size())
            return -1;

        return static_cast<int32_t>(purposes[static_cast<size_t>(index)]);
    }

    bool le_is_layer_name_visible(LeHandle *handle, const char *layer_name)
    {
        if (!handle || !layer_name)
            return true;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.is_layer_name_visible(layer_name);
    }

    void le_set_layer_name_visible(LeHandle *handle, const char *layer_name, bool visible)
    {
        if (!handle || !layer_name)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_layer_name_visible(layer_name, visible);
    }

    bool le_is_antialiasing_enabled(LeHandle *handle)
    {
        if (!handle)
            return false;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.antialiasing_enabled();
    }

    void le_set_antialiasing_enabled(LeHandle *handle, bool enabled)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_antialiasing_enabled(enabled);
    }

    int32_t le_is_purpose_visible(LeHandle *handle, int32_t purpose)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.is_purpose_visible(static_cast<le::ViewLayerPurpose>(purpose)) ? 1 : 0;
    }

    void le_set_purpose_visible(LeHandle *handle, int32_t purpose, int32_t visible)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_purpose_visible(static_cast<le::ViewLayerPurpose>(purpose), visible != 0);
    }

    int32_t le_get_mode(LeHandle *handle)
    {
        if (!handle)
            return LE_MODE_SELECT;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->scene.mode());
    }

    void le_set_mode(LeHandle *handle, int32_t mode)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        if (mode == LE_MODE_RULER)
            handle->scene.reset_ruler_mode();
        else
            handle->scene.set_mode(static_cast<le::Scene::Mode>(mode));
    }

    int32_t le_ruler_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->scene.rulers().size());
    }

    int32_t le_ruler_point_count(LeHandle *handle, int32_t ruler_index)
    {
        if (!handle || ruler_index < 0)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        const auto &rulers = handle->scene.rulers();
        if (static_cast<size_t>(ruler_index) >= rulers.size())
            return 0;
        return static_cast<int32_t>(rulers[ruler_index].points.size());
    }

    LeRulerPoint le_ruler_point_at(LeHandle *handle, int32_t ruler_index, int32_t point_index)
    {
        constexpr LeRulerPoint kInvalid{.x_um = 0.0, .y_um = 0.0};
        if (!handle || ruler_index < 0 || point_index < 0)
            return kInvalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        const auto &rulers = handle->scene.rulers();
        if (static_cast<size_t>(ruler_index) >= rulers.size())
            return kInvalid;
        const auto &points = rulers[ruler_index].points;
        if (static_cast<size_t>(point_index) >= points.size())
            return kInvalid;
        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return kInvalid;
        const le::Point &p = points[point_index];
        return LeRulerPoint{.x_um = static_cast<double>(p.x) / *dbu_per_um, .y_um = static_cast<double>(p.y) / *dbu_per_um};
    }

    void le_finish_ruler(LeHandle *handle)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.finish_active_ruler();
    }

    void le_clear_rulers(LeHandle *handle)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.clear_rulers();
    }

    // --- Editing / undo-redo (UPDATES.md item 21) ---

    void le_begin_command(LeHandle *handle, const char *label)
    {
        if (!handle || !label)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->command_history.begin(label);
    }

    void le_end_command(LeHandle *handle, int32_t succeeded)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->command_history.end(succeeded != 0);
    }

    int32_t le_undo(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        const bool undone = handle->command_history.undo(handle->root);
        if (undone)
            refresh_armed_move_geometry_unlocked(handle);
        return undone ? 1 : 0;
    }

    int32_t le_redo(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        const bool redone = handle->command_history.redo(handle->root);
        if (redone)
            refresh_armed_move_geometry_unlocked(handle);
        return redone ? 1 : 0;
    }

    int32_t le_can_undo(LeHandle *handle)
    {
        return handle && handle->command_history.can_undo() ? 1 : 0;
    }

    int32_t le_can_redo(LeHandle *handle)
    {
        return handle && handle->command_history.can_redo() ? 1 : 0;
    }

    int32_t le_command_history_count(LeHandle *handle)
    {
        return handle ? static_cast<int32_t>(handle->command_history.recall_count()) : 0;
    }

    const char *le_command_history_at(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0 || static_cast<size_t>(index) >= handle->command_history.recall_count())
            return nullptr;
        return handle->command_history.recall_at(static_cast<size_t>(index)).c_str();
    }

    void le_select_all(LeHandle *handle)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        select_all_unlocked(handle);
    }

    void le_deselect_all(LeHandle *handle)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.clear_selection();
    }

    void le_arm_move(LeHandle *handle)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        arm_move_unlocked(handle);
    }

    void le_cancel_move(LeHandle *handle)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.end_move();
    }

    int32_t le_is_move_armed(LeHandle *handle)
    {
        return handle && handle->scene.move().armed ? 1 : 0;
    }

    int32_t le_is_layer_name_selectable(LeHandle *handle, const char *layer_name)
    {
        if (!handle || !layer_name)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.is_layer_name_selectable(layer_name) ? 1 : 0;
    }

    void le_set_layer_name_selectable(LeHandle *handle, const char *layer_name, int32_t selectable)
    {
        if (!handle || !layer_name)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_layer_name_selectable(layer_name, selectable != 0);
    }

    int32_t le_is_purpose_selectable(LeHandle *handle, int32_t purpose)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.is_purpose_selectable(static_cast<le::ViewLayerPurpose>(purpose)) ? 1 : 0;
    }

    void le_set_purpose_selectable(LeHandle *handle, int32_t purpose, int32_t selectable)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_purpose_selectable(static_cast<le::ViewLayerPurpose>(purpose), selectable != 0);
    }

    void le_zoom(LeHandle *handle, double factor, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        zoom_unlocked(handle, factor, x, y);
    }

    void le_pan(LeHandle *handle, double x_factor, double y_factor)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        pan_unlocked(handle, x_factor, y_factor);
    }

    void le_set_viewport_size(LeHandle *handle, int32_t width_px, int32_t height_px)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_viewport_size(width_px, height_px);
    }

    void le_fit_scene(LeHandle *handle, int32_t padding_px)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        fit_scene_unlocked(handle, padding_px);
    }

    void le_fit_rect(LeHandle *handle, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um, int32_t padding_px)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        const double dbu_per_um = display_dbu_per_um(handle->root);
        handle->scene.fit_to_content(le::Rect{.ll = {.x = to_dbu(ll_x_um, dbu_per_um), .y = to_dbu(ll_y_um, dbu_per_um)}, .ur = {.x = to_dbu(ur_x_um, dbu_per_um), .y = to_dbu(ur_y_um, dbu_per_um)}}, padding_px);
    }

    int64_t le_minor_grid_spacing(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.minor_grid_spacing();
    }

    void le_set_minor_grid_spacing(LeHandle *handle, int64_t dbu)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_minor_grid_spacing(dbu);
    }

    int64_t le_major_grid_spacing(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.major_grid_spacing();
    }

    void le_set_major_grid_spacing(LeHandle *handle, int64_t dbu)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_major_grid_spacing(dbu);
    }

    double le_ruler_label_size(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.ruler_label_size_px();
    }

    void le_set_ruler_label_size(LeHandle *handle, double px)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_ruler_label_size_px(px);
    }

    void le_set_mouse_position(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        handle->scene.set_mouse_position(x, y);

        // The hover outline is a Select-mode-only affordance (see
        // Scene::set_mode's own comment) - skip the hit-test entirely
        // outside Select mode rather than computing and immediately
        // discarding it.
        if (handle->scene.mode() == le::Scene::Mode::SELECT)
        {
            // E1 (BUGS_AND_ENHANCEMENTS.md) - this used to unconditionally
            // hit-test the Abstract path even in Layout view, hovering
            // stale/irrelevant content; same branch as le_mouse_up below.
            // No Placement fallback here (unlike le_mouse_up's own click/
            // drag handling) - HoverTarget's own SelectionRef variant
            // deliberately excludes PlacementId (see its own comment: a
            // Placement never enters the RenderedShape map at all), so
            // hovering a Placement specifically isn't representable by
            // this mechanism - a real, separate follow-up, not a gap this
            // fix silently introduces (Placement was never hoverable
            // before this fix either).
            if (handle->scene.current_layout().valid())
            {
                const auto &shapes = handle->layout_shape_pipeline.run(handle->scene.current_layout(), pipeline_options_for(handle));
                handle->scene.set_hover(le::hit_test_point(shapes, handle->view_layers, handle->scene, *handle->scene.mouse_dbu_position()));
            }
            else
            {
                const auto &shapes = handle->abstract_shape_pipeline.run(handle->scene.current_abstract(), pipeline_options_for(handle));
                handle->scene.set_hover(le::hit_test_point(shapes, handle->view_layers, handle->scene, *handle->scene.mouse_dbu_position()));
            }
        }
        else
        {
            handle->scene.clear_hover();
        }
    }

    void le_clear_mouse_position(LeHandle *handle)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.clear_mouse_position();
        handle->scene.clear_hover();
    }

    LeSnappedMousePosition le_snapped_mouse_position(LeHandle *handle)
    {
        if (!handle)
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::optional<le::Point> snapped = handle->scene.snapped_mouse_position();
        if (!snapped)
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};

        // Single shared/global Technology, same assumption le_read_lef's
        // own ViewLayerSet::build_for_technology(..., technology_ids.front())
        // call already makes.
        const auto technology_ids = handle->root.get_technology_ids();
        if (technology_ids.empty())
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};

        const le::TechnologyData *technology = handle->root.get_technology(technology_ids.front());
        if (!technology || technology->database_units_microns <= 0.0)
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};

        return LeSnappedMousePosition{
            .x_um = static_cast<double>(snapped->x) / technology->database_units_microns,
            .y_um = static_cast<double>(snapped->y) / technology->database_units_microns,
            .has_position = 1,
        };
    }

    void le_key_down(LeHandle *handle, int32_t key_code)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.press_key(key_code);
        const bool ctrl = handle->scene.is_key_held(LE_KEY_CTRL);
        const bool shift = handle->scene.is_key_held(LE_KEY_SHIFT);
        handle->scene.set_ruler_free_form(shift);
        handle->scene.set_move_free_form(shift);

        // Every case below fires only for the *exact* modifier
        // combination its own action is actually defined for - an
        // unexpected extra modifier (e.g. Shift held for a key with no
        // Shift-specific meaning) is a no-op, not a silent fall-through
        // to the bare/Ctrl behavior. Regression: pressing 's'/'e'/'r' to
        // switch modes used to fire even with Ctrl or Shift held (e.g.
        // Ctrl-S), stealing the keystroke from whatever the modifier was
        // actually meant for. LE_KEY_ZOOM is the one exception that's
        // deliberately exhaustive instead (Ctrl and Shift each already
        // select a real, distinct action of their own - see below) and
        // LE_KEY_FINISH_RULER, which is deliberately *not* modifier-
        // gated at all (see its own case).
        switch (key_code)
        {
        case LE_KEY_ZOOM:
        {
            // UPDATES.md item 21 - Ctrl-Z/Ctrl-Shift-Z undo/redo, branched
            // here rather than a separate key code (see LE_KEY_ZOOM's own
            // api.hpp doc comment for why). Falls through to the ordinary
            // zoom action when Ctrl isn't held. Every one of the four
            // Ctrl/Shift combinations already has a real, distinct
            // meaning (undo/redo, zoom in/out), unlike every other action
            // key below - nothing to suppress here.
            if (ctrl)
            {
                if (shift)
                    handle->command_history.redo(handle->root);
                else
                    handle->command_history.undo(handle->root);
                refresh_armed_move_geometry_unlocked(handle);
                break;
            }
            // Unlocked variants (see their own comment) - handle->mutex_
            // is already held above; le_zoom/le_fit_scene/le_pan
            // themselves would re-lock it and deadlock.
            const double factor = shift ? -kKeyZoomFactor : kKeyZoomFactor;
            zoom_unlocked(handle, factor, handle->scene.mouse_x_px(), handle->scene.mouse_y_px());
            break;
        }
        case LE_KEY_FIT:
            // Shift has no meaning for Fit - held at all (with or without
            // Ctrl) suppresses the action rather than falling through to
            // the bare/Ctrl behavior.
            if (shift)
                break;
            if (ctrl)
                fit_selected_unlocked(handle, kKeyFitPaddingPx);
            else
                fit_scene_unlocked(handle, kKeyFitPaddingPx);
            break;
        case LE_KEY_PAN_LEFT:
            if (!ctrl && !shift)
                pan_unlocked(handle, -kKeyPanFactor, 0.0);
            break;
        case LE_KEY_PAN_RIGHT:
            if (!ctrl && !shift)
                pan_unlocked(handle, kKeyPanFactor, 0.0);
            break;
        case LE_KEY_PAN_UP:
            if (!ctrl && !shift)
                pan_unlocked(handle, 0.0, kKeyPanFactor);
            break;
        case LE_KEY_PAN_DOWN:
            if (!ctrl && !shift)
                pan_unlocked(handle, 0.0, -kKeyPanFactor);
            break;
        case LE_KEY_SELECT_ALL:
            // UPDATES.md item 21 - Select-mode-only, in addition to the
            // existing Ctrl-held gate (switch back to Select mode to
            // change the selection from Edit/Ruler mode). Shift has no
            // meaning here - Ctrl-Shift-A is a no-op, not "same as
            // Ctrl-A".
            if (handle->scene.mode() == le::Scene::Mode::SELECT && ctrl && !shift)
                select_all_unlocked(handle);
            break;
        case LE_KEY_1:
        case LE_KEY_2:
        case LE_KEY_3:
        case LE_KEY_4:
        case LE_KEY_5:
        case LE_KEY_6:
        case LE_KEY_7:
        case LE_KEY_8:
        case LE_KEY_9:
        {
            // UPDATES.md 9.7 - the same physical 1-9 keys address the
            // 11th..19th ROUTING layer instead of the 1st..9th while Ctrl
            // is held (LE_KEY_0 covers the 10th - see its own case).
            // Shift has no meaning for either - held at all suppresses
            // the action.
            if (shift)
                break;
            const int base_index = key_code - LE_KEY_1; // 0-8
            const int routing_index = ctrl ? base_index + 10 : base_index;
            toggle_routing_layer_visibility_unlocked(handle, routing_index);
            break;
        }
        case LE_KEY_0:
            // Same "no modifier meaning at all" shape as the mode keys
            // below - LE_KEY_1..9's own Ctrl-held branch has no digit
            // left over to double up on for the 10th layer, so this key
            // is bare-only, not Ctrl-gated.
            if (!ctrl && !shift)
                toggle_routing_layer_visibility_unlocked(handle, 9); // the 10th ROUTING layer
            break;
        case LE_KEY_DESELECT_ALL:
            // UPDATES.md item 21 - same Select-mode-only gate and
            // Shift-suppresses shape as LE_KEY_SELECT_ALL above.
            if (handle->scene.mode() == le::Scene::Mode::SELECT && ctrl && !shift)
                handle->scene.clear_selection();
            break;
        case LE_KEY_MOVE:
            if (ctrl && !shift)
                arm_move_unlocked(handle);
            break;
        case LE_KEY_SELECT_MODE:
            if (!ctrl && !shift)
                handle->scene.set_mode(le::Scene::Mode::SELECT);
            break;
        case LE_KEY_EDIT_MODE:
            if (!ctrl && !shift)
                handle->scene.set_mode(le::Scene::Mode::EDIT);
            break;
        case LE_KEY_RULER_MODE:
            if (!ctrl && !shift)
                handle->scene.reset_ruler_mode();
            break;
        case LE_KEY_FINISH_RULER:
            // Deliberately *not* modifier-gated, unlike every other bare
            // key above: Escape is a pure cancel/finish gesture, and a
            // real ruler-drawing sequence routinely ends with Shift
            // (free-form) still physically held right up to the Escape
            // press - suppressing it here would make "finish the ruler"
            // unreliable in exactly the workflow that uses Shift most.
            handle->scene.finish_active_ruler();
            handle->scene.end_move(); // UPDATES.md item 21 - Escape also cancels an in-progress move
            break;
        default:
            break;
        }
    }

    void le_key_up(LeHandle *handle, int32_t key_code)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.release_key(key_code);
        handle->scene.set_ruler_free_form(handle->scene.is_key_held(LE_KEY_SHIFT));
        handle->scene.set_move_free_form(handle->scene.is_key_held(LE_KEY_SHIFT));
    }

    int32_t le_is_key_held(LeHandle *handle, int32_t key_code)
    {
        return handle && handle->scene.is_key_held(key_code) ? 1 : 0;
    }

    void le_clear_all_keys(LeHandle *handle)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.clear_all_keys();
    }

    void le_mouse_down(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.begin_drag(x, y);
    }

    void le_zoom_drag_down(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.begin_drag(x, y, le::Scene::DragKind::ZOOM);
    }

    // le_mouse_up's Select-mode, Abstract-view branch - exactly the
    // click/drag hit-testing logic that lived directly in le_mouse_up
    // before E1 (BUGS_AND_ENHANCEMENTS.md) split it out to make room for
    // select_in_layout_view_unlocked below, which needs an entirely
    // different hit-test (Layout content has no per-piece geometry
    // addressable the same way an Abstract's Terminal/Obstruction Shapes
    // are). Called with handle->mutex_ already held, `x`/`y` the same
    // release-point le_mouse_up itself received, `is_click` its own
    // click-vs-drag threshold result.
    void select_in_abstract_view_unlocked(LeHandle *handle, int32_t x, int32_t y, bool is_click)
    {
        const auto &shapes = handle->abstract_shape_pipeline.run(handle->scene.current_abstract(), pipeline_options_for(handle));

        if (is_click)
        {
            // Computed straight from this call's own x/y, not
            // Scene::drag_rect_dbu()/mouse_dbu_position() - those read the
            // separately-tracked *stored* mouse position (see
            // le_set_mouse_position), which this call has no guaranteed
            // ordering against.
            const le::Point dbu_point = handle->scene.pixel_to_dbu(x, y);
            const auto hit = le::hit_test_point(shapes, handle->view_layers, handle->scene, dbu_point);
            if (hit && hit->shape_id)
            {
                // UPDATES.md item 21 - hit_test_point's own piece is
                // Pipeline's *rendered* geometry, not always
                // addressable in Root (e.g. an ITERATE-expanded piece
                // has no raw index - see HoverTarget's own comment) -
                // re-hit-test the same point against the shape's real,
                // raw geometry to find the piece selection/Move
                // actually operate on. Selects the whole shape id with
                // the default piece (0) in the vanishingly unlikely
                // case the raw geometry doesn't hit at the exact same
                // point the rendered geometry did (e.g. a boundary
                // rounding difference, or a pure ITERATE-expanded hit
                // with no raw counterpart at all) rather than silently
                // selecting nothing.
                if (const le::ShapeData *data = handle->root.get_shape(*hit->shape_id))
                {
                    if (const auto raw_piece = le::Geometry::find_hit_piece(*data, dbu_point))
                        handle->scene.select(*hit->shape_id, raw_piece->kind, raw_piece->index);
                    else
                        handle->scene.select(*hit->shape_id);
                }
            }
        }
        else
        {
            const le::Point start = handle->scene.pixel_to_dbu(handle->scene.drag_start_x_px(), handle->scene.drag_start_y_px());
            const le::Point end = handle->scene.pixel_to_dbu(x, y);
            const le::Rect drag_rect{
                .ll = le::Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                .ur = le::Point{std::max(start.x, end.x), std::max(start.y, end.y)},
            };

            // UPDATES.md item 21 - same "re-test against raw geometry"
            // reasoning as the click branch above: hit_test_rect's own
            // pieces are only used to find which ShapeIds are
            // candidates at all; each candidate's actual enclosed
            // pieces are then found by re-running fully_enclosed_pieces
            // against its real Root::get_shape() data.
            std::set<le::ShapeId> candidate_ids;
            for (const le::HoverTarget &hit : le::hit_test_rect(shapes, handle->view_layers, handle->scene, drag_rect))
                if (hit.shape_id)
                    candidate_ids.insert(*hit.shape_id);

            for (const le::ShapeId shape_id : candidate_ids)
            {
                const le::ShapeData *data = handle->root.get_shape(shape_id);
                if (!data)
                    continue;

                for (const auto &piece : le::Geometry::fully_enclosed_pieces(drag_rect, *data))
                    handle->scene.select(shape_id, piece.kind, piece.index);
            }
        }
    }

    // le_mouse_up's Select-mode, Layout-view branch (E1,
    // BUGS_AND_ENHANCEMENTS.md) - top-level Layout content only, never
    // recursing into a Placement's own reference_design (matches this
    // codebase's own existing, documented "whole-placement only"
    // deferral - see InstanceRenderer's own class comment). Placement
    // hit-testing (hit_test_placements_point/_rect, src/core/
    // placement_geometry.hpp) is checked first/unioned first since a
    // Placement is always drawn topmost (BuildLayoutPictureStage::run
    // draws own_shapes - Blockage/Route/PhysicalPort/Row/Region - first,
    // then instances on top of them) - a click only ever selects the one
    // topmost object, a drag unions everything enclosed regardless of
    // stacking. Blockage/Route/PhysicalPort share the exact same
    // ShapeId+piece re-resolution as the Abstract branch above (they
    // have real backing Shapes); Row/Region have no Shape at all, so a
    // hit with `origin` set but `shape_id` unset gets its own small fork
    // straight into scene.select(RowId)/scene.select(RegionId) - there's
    // no separate Root-owned geometry to re-validate a piece against, the
    // synthesized rect *is* the geometry.
    void select_in_layout_view_unlocked(LeHandle *handle, int32_t x, int32_t y, bool is_click)
    {
        const le::LayoutId layout_id = handle->scene.current_layout();
        const int remaining_depth = std::max(0, handle->scene.hierarchy_depth() - 1);
        const auto &shapes = handle->layout_shape_pipeline.run(layout_id, pipeline_options_for(handle));

        if (is_click)
        {
            const le::Point dbu_point = handle->scene.pixel_to_dbu(x, y);

            if (const auto placement_id = le::hit_test_placements_point(handle->root, layout_id, remaining_depth, dbu_point))
            {
                handle->scene.select(*placement_id);
                return;
            }

            const auto hit = le::hit_test_point(shapes, handle->view_layers, handle->scene, dbu_point);
            if (!hit)
                return;

            if (hit->shape_id)
            {
                if (const le::ShapeData *data = handle->root.get_shape(*hit->shape_id))
                {
                    if (const auto raw_piece = le::Geometry::find_hit_piece(*data, dbu_point))
                        handle->scene.select(*hit->shape_id, raw_piece->kind, raw_piece->index);
                    else
                        handle->scene.select(*hit->shape_id);
                }
            }
            else if (const auto *row_id = std::get_if<le::RowId>(&hit->origin))
                handle->scene.select(*row_id);
            else if (const auto *region_id = std::get_if<le::RegionId>(&hit->origin))
                handle->scene.select(*region_id);
        }
        else
        {
            const le::Point start = handle->scene.pixel_to_dbu(handle->scene.drag_start_x_px(), handle->scene.drag_start_y_px());
            const le::Point end = handle->scene.pixel_to_dbu(x, y);
            const le::Rect drag_rect{
                .ll = le::Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                .ur = le::Point{std::max(start.x, end.x), std::max(start.y, end.y)},
            };

            for (le::PlacementId placement_id : le::hit_test_placements_rect(handle->root, layout_id, remaining_depth, drag_rect))
                handle->scene.select(placement_id);

            std::set<le::ShapeId> candidate_ids;
            for (const le::HoverTarget &hit : le::hit_test_rect(shapes, handle->view_layers, handle->scene, drag_rect))
            {
                if (hit.shape_id)
                    candidate_ids.insert(*hit.shape_id);
                else if (const auto *row_id = std::get_if<le::RowId>(&hit.origin))
                    handle->scene.select(*row_id);
                else if (const auto *region_id = std::get_if<le::RegionId>(&hit.origin))
                    handle->scene.select(*region_id);
            }

            for (const le::ShapeId shape_id : candidate_ids)
            {
                const le::ShapeData *data = handle->root.get_shape(shape_id);
                if (!data)
                    continue;

                for (const auto &piece : le::Geometry::fully_enclosed_pieces(drag_rect, *data))
                    handle->scene.select(shape_id, piece.kind, piece.index);
            }
        }
    }

    void le_mouse_up(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle || !handle->scene.is_dragging())
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const int32_t dx = x - handle->scene.drag_start_x_px();
        const int32_t dy = y - handle->scene.drag_start_y_px();
        const bool is_click = dx * dx + dy * dy < kClickDragThresholdPx * kClickDragThresholdPx;

        if (handle->scene.drag_kind() == le::Scene::DragKind::ZOOM)
        {
            // Rectangle-zoom (UPDATES.md 9.3) - purely navigational,
            // selection is untouched. A click-sized release is a no-op
            // (fitting to a near-zero-size rect would produce an absurd
            // scale) - same threshold used for the select gesture below.
            if (!is_click)
            {
                const le::Point start = handle->scene.pixel_to_dbu(handle->scene.drag_start_x_px(), handle->scene.drag_start_y_px());
                const le::Point end = handle->scene.pixel_to_dbu(x, y);
                const le::Rect zoom_rect{
                    .ll = le::Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                    .ur = le::Point{std::max(start.x, end.x), std::max(start.y, end.y)},
                };
                handle->scene.fit_to_content(zoom_rect, 0);
            }
            handle->scene.end_drag();
            return;
        }

        // UPDATES.md item 11 - only Select mode changes the selection; in
        // Edit mode a click/drag is left for editing the existing
        // selection (behavior TBD, a later item), so this whole block -
        // including the pipeline run it only needs for hit-testing - is
        // skipped. end_drag() below stays unconditional so drag state
        // always resets regardless of mode.
        if (handle->scene.mode() == le::Scene::Mode::SELECT)
        {
            const bool shift = handle->scene.is_key_held(LE_KEY_SHIFT);

            if (!shift)
                handle->scene.clear_selection();

            // E1 (BUGS_AND_ENHANCEMENTS.md) - this used to unconditionally
            // hit-test the Abstract path even in Layout view (a real bug:
            // clicking in Layout view hit whatever stale/irrelevant
            // Abstract content happened to exist, never the Layout's own).
            if (handle->scene.current_layout().valid())
                select_in_layout_view_unlocked(handle, x, y, is_click);
            else
                select_in_abstract_view_unlocked(handle, x, y, is_click);
        }
        else if (handle->scene.mode() == le::Scene::Mode::RULER)
        {
            // UPDATES.md item 13 - only a click (not a drag) commits a
            // ruler point; a drag in Ruler mode does nothing beyond
            // ending the gesture below.
            if (is_click)
                handle->scene.add_ruler_point(handle->scene.is_key_held(LE_KEY_SHIFT));
        }
        else if (handle->scene.mode() == le::Scene::Mode::EDIT)
        {
            // UPDATES.md item 21 - only a click (not a drag) sets the
            // move's anchor / commits it; a drag in Edit mode does
            // nothing beyond ending the gesture below, same as Ruler
            // mode's own click-only handling just above. A no-op if
            // Move isn't armed (see move_click_unlocked).
            if (is_click)
                move_click_unlocked(handle);
        }

        handle->scene.end_drag();
    }

    const char *le_tooltip_message(LeHandle *handle)
    {
        if (!handle)
            return nullptr;
        switch (handle->scene.mode())
        {
        case le::Scene::Mode::EDIT:
            return kEditModeTooltip;
        case le::Scene::Mode::RULER:
            return kRulerModeTooltip;
        case le::Scene::Mode::SELECT:
        default:
            return kSelectModeTooltip;
        }
    }

    int32_t le_selection_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        return static_cast<int32_t>(handle->scene.selection().size());
    }

    int64_t le_selection_version(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        return static_cast<int64_t>(handle->scene.selection_version());
    }

    LeObjectRef le_object_invalid_ref(void)
    {
        return invalid_object_ref();
    }

    int32_t le_object_property_count(LeHandle *handle, LeObjectRef ref)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        handle->cached_object_properties = build_object_properties(handle->root, ref);
        handle->cached_object_property_ref = ref;
        return static_cast<int32_t>(handle->cached_object_properties.size());
    }

    LeProperty le_object_property_at(LeHandle *handle, LeObjectRef ref, int32_t index)
    {
        const LeProperty invalid{.name = nullptr, .type = LE_PROPERTY_TYPE_STRING, .string_value = nullptr, .int_value = 0, .double_value = 0.0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        if (!same_object_ref(handle->cached_object_property_ref, ref))
        {
            handle->cached_object_properties = build_object_properties(handle->root, ref);
            handle->cached_object_property_ref = ref;
        }

        if (static_cast<size_t>(index) >= handle->cached_object_properties.size())
            return invalid;

        return to_c(handle->cached_object_properties[static_cast<size_t>(index)]);
    }

    LeObjectRef le_object_parent(LeHandle *handle, LeObjectRef ref)
    {
        if (!handle)
            return invalid_object_ref();
        std::lock_guard<std::mutex> lock(handle->mutex_);

        return object_ref_parent(handle->root, ref);
    }

    LeObjectRef le_selected_object_ref(LeHandle *handle, int32_t selection_index)
    {
        if (!handle || selection_index < 0)
            return invalid_object_ref();
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::vector<le::SelectedObject> &selection = handle->scene.selection();
        if (static_cast<size_t>(selection_index) >= selection.size())
            return invalid_object_ref();

        // E1 (BUGS_AND_ENHANCEMENTS.md) - dispatches every SelectedObject
        // alternative to its own LeObjectKind; ShapePiece (Terminal/
        // Obstruction/Blockage/Route/PhysicalPort) always resolves to
        // LE_OBJECT_KIND_SHAPE, unchanged from before this variant grew -
        // a Property Viewer wanting the owning Blockage/Route/PhysicalPort
        // instead walks up via le_object_parent (see object_ref_parent's
        // own new Shape->blockage/route/physical_port_segment hops).
        return std::visit([](const auto &s) -> LeObjectRef
                          {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, le::ShapePiece>)
                return ref_from_id(LE_OBJECT_KIND_SHAPE, s.shape_id);
            else if constexpr (std::is_same_v<T, le::RowId>)
                return ref_from_id(LE_OBJECT_KIND_ROW, s);
            else if constexpr (std::is_same_v<T, le::PlacementId>)
                return ref_from_id(LE_OBJECT_KIND_PLACEMENT, s);
            else if constexpr (std::is_same_v<T, le::RegionId>)
                return ref_from_id(LE_OBJECT_KIND_REGION, s); }, selection[static_cast<size_t>(selection_index)]);
    }

    LeTerminalId le_terminal_by_name(LeHandle *handle, const char *name)
    {
        const LeTerminalId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        // handle->current_abstract_id (le_set_current_abstract/
        // le_current_abstract's own generated field), not
        // handle->scene.current_abstract() - the latter is a genuinely
        // separate "GUI current view" tracker. le_set_current_design_abstract/
        // le_set_current_design_abstract_by_id move both together (selecting a
        // Design should mean the same thing whether it came from a
        // Dart-driven GUI or a TCL script's open_design), but they can
        // still diverge: a script that builds an Abstract from scratch
        // and calls set_current_abstract directly (no Design to
        // open_design into at all) only ever touches
        // handle->current_abstract_id, never Scene. Terminal is the one
        // class with a hand-written friendly-id resolver
        // (unique_per_parent means it can't use the generated by-name
        // lookup pair - see Field.unique_per_parent's own docstring), so
        // it's the one place reading the wrong one of the two was easy
        // to get wrong: reading handle->scene.current_abstract() here
        // would leave resolve_terminal_id unable to find any Terminal a
        // from-scratch script just created, even though get_terminals
        // (same "current" concept, already reading current_abstract_id)
        // already sees it correctly.
        for (const le::TerminalId id : handle->root.get_abstract_terminals(handle->current_abstract_id))
        {
            const le::TerminalData *terminal = handle->root.get_terminal(id);
            if (terminal && terminal->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_terminal_name(LeHandle *handle, LeTerminalId id)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::TerminalData *terminal = handle->root.get_terminal(from_c(id));
        return terminal ? terminal->name.c_str() : nullptr;
    }

    LeRowId le_row_by_name(LeHandle *handle, const char *name)
    {
        const LeRowId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        for (const le::RowId id : handle->root.get_layout_rows(handle->current_layout_id))
        {
            const le::RowData *row = handle->root.get_row(id);
            if (row && row->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_row_name(LeHandle *handle, LeRowId id)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::RowData *row = handle->root.get_row(from_c(id));
        return row ? row->name.c_str() : nullptr;
    }

    LePlacementId le_placement_by_name(LeHandle *handle, const char *name)
    {
        const LePlacementId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        for (const le::PlacementId id : handle->root.get_layout_placements(handle->current_layout_id))
        {
            const le::PlacementData *placement = handle->root.get_placement(id);
            if (placement && placement->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_placement_name(LeHandle *handle, LePlacementId id)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::PlacementData *placement = handle->root.get_placement(from_c(id));
        return placement ? placement->name.c_str() : nullptr;
    }

    LePhysicalPortId le_physical_port_by_name(LeHandle *handle, const char *name)
    {
        const LePhysicalPortId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        for (const le::PhysicalPortId id : handle->root.get_layout_physical_ports(handle->current_layout_id))
        {
            const le::PhysicalPortData *physical_port = handle->root.get_physical_port(id);
            if (physical_port && physical_port->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_physical_port_name(LeHandle *handle, LePhysicalPortId id)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::PhysicalPortData *physical_port = handle->root.get_physical_port(from_c(id));
        return physical_port ? physical_port->name.c_str() : nullptr;
    }

    LeRouteId le_route_by_name(LeHandle *handle, const char *name)
    {
        const LeRouteId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        for (const le::RouteId id : handle->root.get_layout_routes(handle->current_layout_id))
        {
            const le::RouteData *route = handle->root.get_route(id);
            if (route && route->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_route_name(LeHandle *handle, LeRouteId id)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::RouteData *route = handle->root.get_route(from_c(id));
        return route ? route->name.c_str() : nullptr;
    }

    LeRegionId le_region_by_name(LeHandle *handle, const char *name)
    {
        const LeRegionId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        for (const le::RegionId id : handle->root.get_layout_regions(handle->current_layout_id))
        {
            const le::RegionData *region = handle->root.get_region(id);
            if (region && region->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_region_name(LeHandle *handle, LeRegionId id)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::RegionData *region = handle->root.get_region(from_c(id));
        return region ? region->name.c_str() : nullptr;
    }

    LeLayoutViaId le_layout_via_by_name(LeHandle *handle, const char *name)
    {
        const LeLayoutViaId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        for (const le::LayoutViaId id : handle->root.get_layout_vias(handle->current_layout_id))
        {
            const le::LayoutViaData *layout_via = handle->root.get_layout_via(id);
            if (layout_via && layout_via->name == name)
                return to_c(id);
        }
        return invalid;
    }

    const char *le_layout_via_name(LeHandle *handle, LeLayoutViaId id)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::LayoutViaData *layout_via = handle->root.get_layout_via(from_c(id));
        return layout_via ? layout_via->name.c_str() : nullptr;
    }

    int32_t le_search_terminal(LeHandle *handle, const char *filter_expression)
    {
        if (!handle || !filter_expression)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            handle->messages.push_back(fmt::format("ERROR: le_search_terminal: {}", expr.error()));
            return -1;
        }

        handle->terminal_search_results = handle->root.search_terminal(
            [&expr](const le::Root &root, le::TerminalId id, const le::TerminalData &data)
            { return le::evaluate_filter(*expr, root, id, data); });
        return static_cast<int32_t>(handle->terminal_search_results.size());
    }

    int32_t le_search_terminal_port(LeHandle *handle, const char *filter_expression)
    {
        if (!handle || !filter_expression)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            handle->messages.push_back(fmt::format("ERROR: le_search_terminal_port: {}", expr.error()));
            return -1;
        }

        handle->terminal_port_search_results = handle->root.search_terminal_port(
            [&expr](const le::Root &root, le::TerminalPortId id, const le::TerminalPortData &data)
            { return le::evaluate_filter(*expr, root, id, data); });
        return static_cast<int32_t>(handle->terminal_port_search_results.size());
    }

    int32_t le_search_obstruction(LeHandle *handle, const char *filter_expression)
    {
        if (!handle || !filter_expression)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            handle->messages.push_back(fmt::format("ERROR: le_search_obstruction: {}", expr.error()));
            return -1;
        }

        handle->obstruction_search_results = handle->root.search_obstruction(
            [&expr](const le::Root &root, le::ObstructionId id, const le::ObstructionData &data)
            { return le::evaluate_filter(*expr, root, id, data); });
        return static_cast<int32_t>(handle->obstruction_search_results.size());
    }

    int32_t le_terminal_port_shape_count(LeHandle *handle, LeTerminalPortId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_terminal_port_shapes(from_c(id)).size());
    }

    LeShapeId le_terminal_port_shape_at(LeHandle *handle, LeTerminalPortId id, int32_t index)
    {
        const LeShapeId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::vector<le::ShapeId> &shapes = handle->root.get_terminal_port_shapes(from_c(id));
        if (static_cast<size_t>(index) >= shapes.size())
            return invalid;
        return to_c(shapes[static_cast<size_t>(index)]);
    }

    int32_t le_obstruction_shape_count(LeHandle *handle, LeObstructionId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_obstruction_shapes(from_c(id)).size());
    }

    LeShapeId le_obstruction_shape_at(LeHandle *handle, LeObstructionId id, int32_t index)
    {
        const LeShapeId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::vector<le::ShapeId> &shapes = handle->root.get_obstruction_shapes(from_c(id));
        if (static_cast<size_t>(index) >= shapes.size())
            return invalid;
        return to_c(shapes[static_cast<size_t>(index)]);
    }

    const char *le_shape_layer_name(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape)
            return nullptr;
        const le::LayerData *layer = handle->root.get_layer(shape->layer);
        return layer ? layer->name.c_str() : nullptr;
    }

    int32_t le_shape_rect_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->rects.size()) : 0;
    }

    LeRectUm le_shape_rect_at(LeHandle *handle, LeShapeId id, int32_t index)
    {
        const LeRectUm invalid{.ll_x_um = 0, .ll_y_um = 0, .ur_x_um = 0, .ur_y_um = 0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(index) >= shape->rects.size())
            return invalid;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return invalid;

        const le::Rect &rect = shape->rects[static_cast<size_t>(index)];
        return LeRectUm{
            .ll_x_um = le::to_um(rect.ll.x, *dbu_per_um),
            .ll_y_um = le::to_um(rect.ll.y, *dbu_per_um),
            .ur_x_um = le::to_um(rect.ur.x, *dbu_per_um),
            .ur_y_um = le::to_um(rect.ur.y, *dbu_per_um),
        };
    }

    int le_remove_shape_rect(LeHandle *handle, LeShapeId id, int32_t index)
    {
        if (!handle || index < 0)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(index) >= shape->rects.size())
            return 1;
        shape->rects.erase(shape->rects.begin() + index);
        handle->root.bump_mutation_version();
        return 0;
    }

    int32_t le_shape_polygon_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->polygons.size()) : 0;
    }

    int32_t le_shape_polygon_point_count(LeHandle *handle, LeShapeId id, int32_t polygon_index)
    {
        if (!handle || polygon_index < 0)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(polygon_index) >= shape->polygons.size())
            return 0;
        return static_cast<int32_t>(shape->polygons[static_cast<size_t>(polygon_index)].points.size());
    }

    LePointUm le_shape_polygon_point_at(LeHandle *handle, LeShapeId id, int32_t polygon_index, int32_t point_index)
    {
        const LePointUm invalid{.x_um = 0, .y_um = 0};
        if (!handle || polygon_index < 0 || point_index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(polygon_index) >= shape->polygons.size())
            return invalid;
        const std::vector<le::Point> &points = shape->polygons[static_cast<size_t>(polygon_index)].points;
        if (static_cast<size_t>(point_index) >= points.size())
            return invalid;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return invalid;

        const le::Point &point = points[static_cast<size_t>(point_index)];
        return LePointUm{.x_um = le::to_um(point.x, *dbu_per_um), .y_um = le::to_um(point.y, *dbu_per_um)};
    }

    int le_remove_shape_polygon(LeHandle *handle, LeShapeId id, int32_t polygon_index)
    {
        if (!handle || polygon_index < 0)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(polygon_index) >= shape->polygons.size())
            return 1;
        shape->polygons.erase(shape->polygons.begin() + polygon_index);
        handle->root.bump_mutation_version();
        return 0;
    }

    int32_t le_shape_path_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->paths.size()) : 0;
    }

    double le_shape_path_width_um(LeHandle *handle, LeShapeId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return 0;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return 0;
        return le::to_um(static_cast<int64_t>(shape->paths[static_cast<size_t>(path_index)].width), *dbu_per_um);
    }

    int32_t le_shape_path_point_count(LeHandle *handle, LeShapeId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return 0;
        return static_cast<int32_t>(shape->paths[static_cast<size_t>(path_index)].polygon.points.size());
    }

    LePointUm le_shape_path_point_at(LeHandle *handle, LeShapeId id, int32_t path_index, int32_t point_index)
    {
        const LePointUm invalid{.x_um = 0, .y_um = 0};
        if (!handle || path_index < 0 || point_index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return invalid;
        const std::vector<le::Point> &points = shape->paths[static_cast<size_t>(path_index)].polygon.points;
        if (static_cast<size_t>(point_index) >= points.size())
            return invalid;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return invalid;

        const le::Point &point = points[static_cast<size_t>(point_index)];
        return LePointUm{.x_um = le::to_um(point.x, *dbu_per_um), .y_um = le::to_um(point.y, *dbu_per_um)};
    }

    int le_remove_shape_path(LeHandle *handle, LeShapeId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return 1;
        shape->paths.erase(shape->paths.begin() + path_index);
        handle->root.bump_mutation_version();
        return 0;
    }

    int frame_mark_count = 0;

    LePixelBuffer le_render_pixel_buffer(LeHandle *handle)
    {
        if (!handle)
            return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};
        std::lock_guard<std::mutex> lock(handle->mutex_);

        // BUGS_AND_ENHANCEMENTS.md E17 - is_rendering_'s own doc comment
        // (LeHandle) explains why this is a plain atomic, not mutex_-
        // guarded. RAII, not a manual reset-before-every-return, so a
        // future early return (or a real exception, however unlikely
        // given this codebase's own "no exceptions for expected-missing-
        // data paths" convention) can't leave this stuck true.
        handle->is_rendering_.store(true, std::memory_order_relaxed);
        struct RenderingGuard
        {
            LeHandle *handle;
            ~RenderingGuard() { handle->is_rendering_.store(false, std::memory_order_relaxed); }
        } rendering_guard{handle};

        // FrameMarkStart/End (named), not plain FrameMark - a frame here
        // only ever happens on demand (whenever something changed and the
        // native texture callback next pulls a frame), not once per
        // engine vsync like a game's continuous main loop, which is what
        // plain FrameMark assumes (see Tracy's own manual). A named
        // start/end pair correctly represents this as a discontinuous
        // frame set with its own explicit duration in the Tracy timeline,
        // instead of a single zero-width instant marker that would make
        // every render look free.

        FrameMarkStart(kRenderFrameName);

        // Migration Step 3 Phase C: a Layout view (scene.current_layout()
        // valid - set by le_set_current_design_layout(_by_id), which
        // clears scene.current_abstract() at the same time, so the two
        // are always mutually exclusive) renders through HierarchyResolver
        // instead of AbstractShapePipeline/FrameRenderPipeline - see
        // LeHandle::hierarchy_resolver's own comment.
        const le::PixelBuffer *buffer = nullptr;
        if (handle->scene.current_layout().valid())
        {
            buffer = &handle->hierarchy_resolver.render_layout_frame(handle->root, handle->scene.current_layout(), handle->scene.hierarchy_depth(), handle->view_layers, handle->scene);
        }
        else
        {
            const le::PipelineOptions options = pipeline_options_for(handle);
            const auto &shapes = handle->abstract_shape_pipeline.run(handle->scene.current_abstract(), options);
            const auto &tiny_shapes = handle->abstract_shape_pipeline.run_tiny_shapes(handle->scene.current_abstract(), options);
            buffer = &handle->frame_render_pipeline.run(handle->scene.current_abstract(), shapes, tiny_shapes, options);
        }

        FrameMarkEnd(kRenderFrameName);
        return LePixelBuffer{
            .data = buffer->data,
            .width = buffer->width,
            .height = buffer->height,
            .row_bytes = static_cast<int64_t>(buffer->row_bytes),
        };
    }

    int32_t le_is_rendering(LeHandle *handle)
    {
        // No lock - see this function's own doc comment (api.hpp) and
        // is_rendering_'s own doc comment (LeHandle) for why not.
        if (!handle)
            return 0;
        return handle->is_rendering_.load(std::memory_order_relaxed) ? 1 : 0;
    }
}
