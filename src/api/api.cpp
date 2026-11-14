#include "api.hpp"
#include "../database/database.hpp"
#include "../database/filter.hpp"
#include "../database/hierarchical_resolver.hpp"
#include "../database/library_helpers.hpp"
#include "../database/schematic_layout_linker.hpp"
#include "../database/rename_propagation.hpp"
#include "../editing/editing.hpp"
#include "../geometry/geometry.hpp"
#include "shape_ops.hpp"
#include "../io/lef_reader.hpp"
#include "../io/def_reader.hpp"
#include "../io/sv_reader.hpp"
#include "../io/verilog_stub_writer.hpp"
#include "../io/lef_writer.hpp"
#include "../io/def_writer.hpp"
#include "../io/native_format.hpp"
#include "generated/database/schema_version.hpp"
#include "../pipelines/view_style.hpp"
#include "../pipelines/view_render_pipeline.hpp"
#include "../pipelines/pipeline_options.hpp"
#include "edit_ops.hpp"
#include "api_internal.hpp"
#include "read_command.hpp"
#include "le_handle.hpp"
#include "le/extension.hpp"
// Generated apply_<snake>_snapshot(Root&, <Klass>Id, const <Klass>Data&)
// helpers - a real standalone header, unlike every
// other generated/api/*.inc fragment, so it's included here with the
// rest of api.cpp's top-level includes rather than spliced into a
// specific scope. Never edit generated/api/snapshot_appliers.hpp
// directly - the build regenerates it.
#include "generated/api/snapshot_appliers.hpp"
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace le::api_detail;

namespace
{
    // The generated id conversions (generated/api/id_conversions.hpp), the
    // same ones extensions use, overloaded with this file's own to_c's.
    using le::ext::from_c;
    using le::ext::to_c;

    // Tracy's FrameMarkStart/FrameMarkEnd send the raw pointer value of
    // `name`, not a copy of the string (see TracyProfiler.hpp's
    // SendFrameMark) - the server pairs a Start with its matching End by
    // that pointer, so both call sites in le_render_pixel_buffer must pass
    // the exact same pointer, not two separately-compiled string literals
    // (which the compiler is free, but not guaranteed, to pool into one
    // address). A single named constant guarantees that regardless of
    // compiler/optimization level.
    constexpr const char *kRenderFrameName = "le_render_pixel_buffer";
}

namespace le::api_detail
{
    // Logs the exception being handled. The entry points that parse, read,
    // write, link or render catch everything with it, so no exception
    // crosses the C boundary.
    void log_exception(const char *function) noexcept
    {
        try
        {
            throw;
        }
        catch (const std::exception &error)
        {
            spdlog::error("{}: {}", function, error.what());
        }
        catch (...)
        {
            spdlog::error("{}: unknown exception", function);
        }
    }
}

namespace
{
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

    LeProperty invalid_property()
    {
        return LeProperty{.name = nullptr, .type = LE_PROPERTY_TYPE_STRING, .string_value = nullptr, .int_value = 0, .double_value = 0.0};
    }

    // The handle and database state a per-thread cache was built from.
    struct CacheStamp
    {
        uint64_t handle_serial = 0; // LeHandle::serial starts at 1
        uint64_t mutation_version = 0;
        uint64_t change_log_end = 0;

        bool operator==(const CacheStamp &) const = default;
    };

    CacheStamp cache_stamp(const LeHandle &handle)
    {
        return CacheStamp{.handle_serial = handle.serial,
                          .mutation_version = handle.root.mutation_version(),
                          .change_log_end = handle.root.change_log().end_sequence()};
    }

    // Which object a per-thread property table was built for (`kind`
    // only for le_object_property_*, whose table serves every class).
    struct PropertyTableKey
    {
        int32_t kind = -1;
        uint32_t index = UINT32_MAX;
        uint32_t generation = 0;

        bool operator==(const PropertyTableKey &) const = default;
    };

    struct PropertyTable
    {
        CacheStamp stamp;
        PropertyTableKey key;
        std::vector<le::PropertyValue> properties;
    };

    // What the C API hands back by pointer on this thread, so a result
    // stays valid after the handle lock is released.
    struct ThreadResults
    {
        // One string per returning function, keyed by its name.
        std::unordered_map<std::string_view, std::string> strings;

        // le_*_property_count/_at's tables, one per builder: per class,
        // plus le_object_property_*'s.
        std::unordered_map<const void *, PropertyTable> property_tables;

        // le_<type>_token_name's Instance paths from `token_paths_from`.
        CacheStamp token_paths_stamp;
        le::SchematicId token_paths_from{};
        std::unordered_map<le::SchematicId, std::string> token_paths;

        // le_<type>_property_path's result, and whether the last such
        // call failed (le_property_path_failed).
        le::PropertyValue property_path_value;
        bool property_path_failed = false;
    };

    // Allocated per thread on first use: le_tcl.so is dlopen'ed with
    // static TLS, which has room for a pointer but not for these.
    ThreadResults &thread_results()
    {
        thread_local std::unique_ptr<ThreadResults> results;
        if (!results)
            results = std::make_unique<ThreadResults>();
        return *results;
    }
}

namespace le::api_detail
{
    // Copies `value` into `function`'s string for this thread: valid until
    // that function's next call on this thread.
    const char *hold_string(std::string_view function, std::string_view value)
    {
        std::string &slot = thread_results().strings[function];
        slot.assign(value);
        return slot.c_str();
    }
}

namespace
{
    template <typename Tag>
    PropertyTableKey property_table_key(le::Id<Tag> id)
    {
        return PropertyTableKey{.kind = -1, .index = id.index, .generation = id.generation};
    }

    PropertyTableKey property_table_key(LeObjectRef ref)
    {
        return PropertyTableKey{.kind = ref.kind, .index = ref.index, .generation = ref.generation};
    }

    // This thread's `build` table for `key`, rebuilt when it was last
    // built for another object or the database has changed since. Built
    // under a shared lock; its rows' strings stay valid until it rebuilds.
    template <typename Key>
    const std::vector<le::PropertyValue> &thread_properties(const LeHandle &handle, Key key,
                                                            std::vector<le::PropertyValue> (*build)(const le::Root &, Key))
    {
        PropertyTable &table = thread_results().property_tables[reinterpret_cast<const void *>(build)];
        const CacheStamp stamp = cache_stamp(handle);
        const PropertyTableKey table_key = property_table_key(key);
        if (table.stamp != stamp || table.key != table_key)
        {
            table.properties = build(handle.root, key);
            table.stamp = stamp;
            table.key = table_key;
        }
        return table.properties;
    }

    LeProperty property_row(const std::vector<le::PropertyValue> &properties, int32_t index)
    {
        if (index < 0 || static_cast<size_t>(index) >= properties.size())
            return invalid_property();
        return to_c(properties[static_cast<size_t>(index)]);
    }

    // `-filter` validation (every le_get_* function
    // below) - a hand-maintained allowlist of each class's filterable leaf
    // fields and hops, cross-checked directly against each class's own
    // generated get_field()/match_hop() (the generated struct headers).
    // Hand-duplicated from schema.py rather than adding a codegen-generated
    // runtime enumeration - a short static list not worth a cross-repo
    // codegen change for. Deliberately narrower than what get_field/
    // match_hop actually dispatch: excludes hops into non-pooled
    // value-list/embedded types (Abstract's bbox/boundary/densities/
    // foreigns/origin/properties/site_placements/size/symmetry; Terminal's
    // antenna_*/properties; Shape's paths/polygons/rects/texts/vias/
    // *_iterates; Design's schematic hop, Schematic not being one of the
    // seven get_* types) - a filter naming one of these is rejected by
    // get_* even though it still works via the unscoped le_search_terminal/
    // etc. escape hatch.
    struct FilterFieldTable
    {
        std::unordered_set<std::string> leaf_fields;
        std::unordered_map<std::string, std::string> hops; // hop name -> target class name
    };

    const std::unordered_map<std::string, FilterFieldTable> &filter_field_tables()
    {
        static const std::unordered_map<std::string, FilterFieldTable> tables =
#include "generated/api/filter_tables.inc"
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

    // Shared parse+validate+log sequence every le_get_* function below
    // needs for its `-filter` axis - std::nullopt with `ok` left true
    // means "no -filter given" (skip that axis entirely); std::nullopt
    // with `ok` set false means a parse or validation error already
    // logged via spdlog::error (caller returns -1). `handle` is unused
    // now (spdlog needs no LeHandle) but kept in the signature - every
    // generated le_get_<type> call site (search_inc_j2.py) passes it
    // positionally, and there's no reason to touch that codegen template
    // just to drop one now-decorative parameter.
    std::optional<le::FilterExpr> parse_and_validate_filter(LeHandle *, const char *caller, const std::string &root_class, const char *filter_expression, bool &ok)
    {
        ok = true;
        if (!filter_expression || filter_expression[0] == '\0')
            return std::nullopt;

        auto parsed = le::parse_filter_expression(filter_expression);
        if (!parsed)
        {
            spdlog::error("{}: {}", caller, parsed.error());
            ok = false;
            return std::nullopt;
        }
        if (auto error = validate_filter_expr(root_class, *parsed))
        {
            spdlog::error("{}: {}", caller, *error);
            ok = false;
            return std::nullopt;
        }
        return std::move(*parsed);
    }
}

namespace le::api_detail
{
    // Single shared/global Technology, same assumption
    // le_snapped_mouse_position's own lookup makes - nullopt if none has
    // been read yet (le_read_lef) or it has no usable scale.
    std::optional<double> database_units_microns(const le::Root &root)
    {
        return le::edit::dbu_per_um(root);
    }

    // Rounds to the nearest dbu rather than truncating - a caller passing
    // e.g. 0.1um at 1000 dbu/um should get exactly 100 dbu, not silently
    // lose precision to a fractional-dbu rounding direction they didn't
    // choose.
    int64_t to_dbu(double value_um, double dbu_per_um)
    {
        return static_cast<int64_t>(std::llround(value_um * dbu_per_um));
    }
}

namespace
{
    // to_dbu() for an area: square microns to database units squared (a
    // `dbu2` field).
    int64_t to_dbu2(double value_um2, double dbu_per_um)
    {
        return static_cast<int64_t>(std::llround(value_um2 * dbu_per_um * dbu_per_um));
    }


    // Looks up `name` among an object's already-built to_properties()-
    // style rows - the same rows a bare `get_properties $token` (no
    // property name) already shows. Used by every le_X_property_path for
    // a single-segment (non-chained) path, so a name like Shape's "rects"
    // resolves the same way there - the -filter DSL's get_field() (what
    // resolve_property_path() uses for everything else) only recognizes
    // scalar leaf fields, not list-of-object fields like rects/polygons/
    // paths, so a bare `.rects` must be resolved here, as
    // `get_properties $token` (no name) shows it.
    // A chained property path's resolved value (resolve_property_path), as
    // get_properties shows it: a length/area arrives as raw, unit-tagged
    // dbu (get_field()), so it's converted to the same micron string a
    // property table shows.
    le::PropertyValue display_path_value(const le::Root &root, le::PropertyValue value)
    {
        if (value.unit == le::PropertyValue::Unit::NONE)
            return value;
        return le::PropertyValue::make_string(value.name, le::format_coordinate_um(le::property_value_in_um(value, display_dbu_per_um(root))));
    }

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
    // build_X_properties, for every TCL-readable
    // class - these stay inside this anonymous namespace since they're
    // never called from another translation unit; see
    // generated/api/property_accessors_public.inc, included later in
    // this file inside extern "C", for the externally-linked
    // le_X_property_count/_at/_path etc. build_terminal_properties/
    // build_library_properties/etc.'s own derived "<field>_count" rows
    // (e.g. Terminal's "ports_count") come from here now too - matches
    // every other is_child-field-derived count row exactly (was
    // hand-abbreviated to "port_count" before this migration; see
    // api_test.cpp). Never edit generated/api/property_accessors_internal.inc
    // directly - the build regenerates it.
#include "generated/api/property_accessors_internal.inc"

    // --- Generic LeObjectRef dispatch (for the Property Viewer) -
    // le_object_property_count/_at/
    // le_object_parent/le_selected_object_ref's own internal machinery. ---

    bool same_object_ref(LeObjectRef a, LeObjectRef b)
    {
        return a.kind == b.kind && a.index == b.index && a.generation == b.generation;
    }

    // build_object_properties, object_ref_parent and kObjectKinds.
#include "generated/api/object_dispatch.inc"

    // --- shape_* operations ---

        // Shape and Wire refs; any other kind becomes an invalid id, which
        // the shape op reports as unknown.
        std::vector<le::GeometryId> shape_ids_from_c(const LeObjectRef *refs, int32_t count)
        {
            std::vector<le::GeometryId> out;
            if (!refs || count <= 0)
                return out;
            out.reserve(static_cast<size_t>(count));
            for (int32_t i = 0; i < count; ++i)
            {
                if (refs[i].kind == LE_OBJECT_KIND_WIRE)
                    out.push_back(id_from_ref<le::WireId>(refs[i]));
                else if (refs[i].kind == LE_OBJECT_KIND_SHAPE)
                    out.push_back(id_from_ref<le::ShapeId>(refs[i]));
                else
                    out.push_back(le::GeometryId{});
            }
            return out;
        }

        // A shape_* command's target: a real layer, else a layer-less purpose
        // (e.g. "DEBUG"), else nullopt to keep each input's own; an unknown
        // purpose string is an error.
        std::expected<std::optional<le::shape_ops::LayerOrPurpose>, std::string> target_from_c(LeLayerId layer, const char *purpose)
        {
            if (layer.index != UINT32_MAX)
                return le::shape_ops::LayerOrPurpose{.layer = from_c(layer)};
            if (purpose && purpose[0])
            {
                const std::optional<le::ShapePurpose> parsed = le::shape_purpose_from_string(purpose);
                if (!parsed)
                    return std::unexpected(fmt::format("unknown purpose \"{}\"", purpose));
                return le::shape_ops::LayerOrPurpose{.purpose = *parsed};
            }
            return std::optional<le::shape_ops::LayerOrPurpose>{};
        }

        // An explicit parent (any kind shape_ops::ShapeParent supports), else
        // nullopt for le::edit's default (the open view's free shapes).
        std::expected<std::optional<le::shape_ops::ShapeParent>, std::string> shape_parent_from_c(LeObjectRef parent)
        {
            if (parent.index == UINT32_MAX)
                return std::optional<le::shape_ops::ShapeParent>{};
            switch (parent.kind)
            {
            case LE_OBJECT_KIND_ABSTRACT:
                return id_from_ref<le::AbstractId>(parent);
            case LE_OBJECT_KIND_LAYOUT:
                return id_from_ref<le::LayoutId>(parent);
            case LE_OBJECT_KIND_OBSTRUCTION:
                return id_from_ref<le::ObstructionId>(parent);
            case LE_OBJECT_KIND_TERMINAL_PORT:
                return id_from_ref<le::TerminalPortId>(parent);
            case LE_OBJECT_KIND_ROUTE:
                return id_from_ref<le::RouteId>(parent);
            case LE_OBJECT_KIND_BLOCKAGE:
                return id_from_ref<le::BlockageId>(parent);
            case LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT:
                return id_from_ref<le::PhysicalPortSegmentId>(parent);
            default:
                return std::unexpected("-parent must be an abstract, layout, obstruction, terminal_port, route, blockage or physical_port_segment");
            }
        }

        // Every creating le_shape_* function: converts its target and parent,
        // runs `op` (an le::edit shape op) under the write lock, keeps the
        // result for le_shape_op_result_at, and logs an error. Returns how
        // many Shapes were created, or -1.
        template <typename Op>
        int32_t run_shape_op(LeHandle *handle, const char *command, LeLayerId layer, const char *purpose, LeObjectRef parent, Op &&op)
        try
        {
            if (!handle)
                return -1;
            HandleWriteLock lock(handle);
            handle->shape_op_results.clear();
            auto fail = [&](const std::string &error)
            {
                spdlog::error("{}: {}", command, error);
                return -1;
            };
            const auto target = target_from_c(layer, purpose);
            if (!target)
                return fail(target.error());
            const auto owner = shape_parent_from_c(parent);
            if (!owner)
                return fail(owner.error());
            const le::edit::ShapeOpResult result = op(*target, *owner);
            if (!result)
                return fail(result.error());
            handle->shape_op_results = *result;
            return static_cast<int32_t>(result->size());
        }
        catch (...)
        {
            log_exception(command);
            return -1;
        }

    // A Schematic object's token name (le_<type>_token_name), held as
    // `function`'s string: its own name when the current Schematic owns
    // it, else its escaped Instance path from there, else (the current
    // Schematic doesn't reach `owner`) its plain name.
    const char *schematic_object_token_name(const LeHandle *handle, le::SchematicId owner, const std::string &name, std::string_view function)
    {
        if (owner == handle->current_schematic_id)
            return hold_string(function, name);
        ThreadResults &results = thread_results();
        const CacheStamp stamp = cache_stamp(*handle);
        if (results.token_paths_stamp != stamp || results.token_paths_from != handle->current_schematic_id)
        {
            results.token_paths = le::hierarchy::schematic_paths(handle->root, handle->current_schematic_id);
            results.token_paths_stamp = stamp;
            results.token_paths_from = handle->current_schematic_id;
        }
        const auto it = results.token_paths.find(owner);
        if (it == results.token_paths.end())
            return hold_string(function, name);
        return hold_string(function, it->second + le::hierarchy::escape_segment(name));
    }

    // The object a token name names: an exact name in the current
    // Schematic first, so a name that itself contains "/" still resolves,
    // else an exact Instance path from there.
    template <typename Id, typename ByNameFn>
    Id schematic_object_by_token_name(const LeHandle *handle, const char *name, ByNameFn &&by_name)
    {
        const le::Root &root = handle->root;
        if (const Id id = by_name(root, handle->current_schematic_id, std::string(name)); id.valid())
            return id;
        const std::vector<std::string> segments = le::hierarchy::split_path(name);
        if (segments.size() < 2)
            return Id{};
        const le::SchematicId owner = le::hierarchy::descend_exact(
            root, handle->current_schematic_id, std::span(segments).first(segments.size() - 1));
        return owner.valid() ? by_name(root, owner, segments.back()) : Id{};
    }
}

namespace le::api_detail
{
    // A `dbu` field's own dbu-per-micron ratio, for property tables built
    // before any Technology has been loaded (or with an invalid scale) -
    // falls back to 1.0 (no scaling, same raw magnitude the field would
    // have shown before dbu-aware formatting existed) rather than leaving
    // every build_X_properties() call site to invent its own fallback.
    double display_dbu_per_um(const le::Root &root)
    {
        return database_units_microns(root).value_or(1.0);
    }

    // le_undo/le_redo's result: logs the steps that couldn't be applied.
    int32_t report_replay(LeHandle *handle, const le::editing::ReplayResult &result, std::string_view verb)
    {
        if (!result.replayed)
            return 0;
        refresh_armed_move_geometry_unlocked(handle);
        if (result.failed_steps == 0)
            return 1;
        spdlog::error("{} '{}': {} of its {} steps could not be applied", verb, result.label, result.failed_steps, result.steps);
        return -1;
    }
}

extern "C"
{
    // Generated TCL property-reading surface (public, externally-linked
    // functions - le_X_property_count/_at/_path, friendly-id-by-name
    // lookups, is_child-field enumeration - declared in api.hpp's own
    // generated/api/declarations.inc, called from le_tcl_shim.cpp) for
    // every TCL-readable class not already covered by hand-written code.
    // Must live inside this extern "C" block, not the anonymous namespace
    // above (internal linkage there would make these unresolvable from
    // other translation units) - see
    // generated/api/property_accessors_public.inc's own header comment.
    // Never edit that file directly - the build regenerates it.
#include "generated/api/property_accessors_public.inc"

    // Generated get_<type> search (le_get_X/le_search_result_X_at) for
    // every TCL-readable class - same external-linkage requirement as
    // property_accessors_public.inc above. Never edit that file directly
    // - the build regenerates it.
#include "generated/api/search.inc"

    LeHandle *le_create(void)
    {
        LeHandle *handle = new LeHandle();
        handle->saved_mutation_version = handle->root.mutation_version();
        handle->saved_settings_json = settings_snapshot(handle);
        return handle;
    }

    void le_destroy(LeHandle *handle)
    {
        delete handle;
    }

    int le_read_lef(LeHandle *handle, const char *path, const char *library_name)
    try
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!path)
        {
            spdlog::error("read_lef: path is null");
            return 1;
        }
        if (!library_name || !library_name[0])
        {
            spdlog::error("read_lef: a library name is required");
            return 1;
        }

        // Snapshot the Technology's own layer count before
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

        const CleanAcrossRead clean(handle);
        const std::filesystem::path lef_path(path);
        le::LEFReader reader;
        int result = 0;
        {
            RecordedRead recorded(handle->root, handle->command_history.current(), /*tracks_links=*/false);
            result = reader.read_lef(lef_path.string(), handle->root, library_name);
            if (result == 0)
                recorded.commit();
        }
        // The readers create through Root directly, which never bumps the
        // version itself - without this, anything keyed on it (the render
        // graph's LayerGenerationStage, ensure_view_layers_current) kept
        // whatever it computed before the read - a GUI opened before any
        // LEF would stay blank. Also on failure: the rollback put the
        // content back, but saturated the change log.
        handle->root.bump_mutation_version();
        if (result != 0)
        {
            clean.commit_failed(handle);
            return result;
        }

        // Rebuilt after every successful read, not just the first, so a
        // later LEF file's own new physical layers (e.g. a second macro
        // file with inline LAYER declarations) are picked up too - cheap
        // relative to a full LEF parse, so correctness here wins over the
        // small extra cost without needing a benchmark to justify it.
        const auto technology_ids = handle->root.get_technology_ids();
        if (!technology_ids.empty())
        {
            // Every physical layer this read newly
            // introduced defaults to hidden unless it's ROUTING or CUT.
            // BOUNDARY isn't a physical layer at all (a Shape with
            // purpose=BOUNDARY instead - see Shape.layer/.purpose's own
            // schema.py comments), so it never reaches this loop - stays
            // visible via LeHandle::is_layer_name_visible's own default-
            // true-until-toggled behavior, same as it always has.
            const auto &layers = handle->root.get_technology_layers(technology_ids.front());
            for (size_t i = old_layer_count; i < layers.size(); ++i)
            {
                const le::LayerData *layer = handle->root.get_layer(layers[i]);
                if (layer && layer->type != "ROUTING" && layer->type != "CUT")
                    handle->set_layer_name_visible(layer->name, false);
            }

            rebuild_view_layers(handle, technology_ids.front());
            apply_pending_grid_um_unlocked(handle); // a settings file read before any Technology existed

            // Also selects the singleton Technology as the current one for
            // the generated TCL current-instance mechanism (see
            // codegen/codegen/tcl_scope.py's own module docstring for why
            // get_layers/get_vias/etc.'s default scope needs this) -
            // Technology has no separate "open" step the way a Design/
            // Abstract does (le_tcl_procs.tcl's open_design), it's
            // implicitly read alongside everything else in a LEF file, so
            // this is the one natural chokepoint - shared by every caller
            // (the GUI directly, or TCL's own read_lef, which calls this
            // same function), not just the TCL-facing shim. Idempotent to
            // repeat across multiple le_read_lef calls on the same handle.
            handle->current_technology_id = technology_ids.front();
        }

        clean.commit(handle);
        return 0;
    }
    catch (...)
    {
        log_exception("read_lef");
        return 1;
    }

    int le_read_def(LeHandle *handle, const char *path, const char *library_name)
    try
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!path)
        {
            spdlog::error("read_def: path is null");
            return 1;
        }
        if (!library_name || !library_name[0])
        {
            spdlog::error("read_def: a library name is required");
            return 1;
        }

        const CleanAcrossRead clean(handle);
        const std::filesystem::path def_path(path);
        le::DEFReader reader;
        int result = 0;
        {
            RecordedRead recorded(handle->root, handle->command_history.current(), /*tracks_links=*/false);
            result = reader.read_def(def_path.string(), handle->root, library_name);
            if (result == 0)
                recorded.commit();
        }
        handle->root.bump_mutation_version(); // see le_read_lef
        if (result != 0)
        {
            clean.commit_failed(handle);
            return result;
        }

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
        apply_pending_grid_um_unlocked(handle); // a settings file read before any Technology existed

        clean.commit(handle);
        return 0;
    }
    catch (...)
    {
        log_exception("read_def");
        return 1;
    }

    int le_read_verilog(LeHandle *handle, const char *const *filenames, int32_t filename_count, int32_t is_netlist, const char *library_name)
    try
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!filenames || filename_count <= 0)
        {
            spdlog::error("read_verilog: filenames is null or empty");
            return 1;
        }
        if (!library_name || !library_name[0])
        {
            spdlog::error("read_verilog: a library name is required");
            return 1;
        }

        std::vector<std::string> filename_strings;
        filename_strings.reserve(static_cast<size_t>(filename_count));
        for (int32_t i = 0; i < filename_count; ++i)
            filename_strings.emplace_back(filenames[i] ? filenames[i] : "");

        // Netlist flavor only - the RTL flavor (read_rtl) never
        // elaborates against a real slang::ast::Compilation at all (see
        // SVReader's own class comment), so it has no "unknown module"
        // failure mode a stub could address. Automatically covers every
        // LEF-only Design (Abstract but no Schematic) already in this
        // handle's Root, from any read_lef so far, one per name - a Design that
        // already has a Schematic (a prior real Verilog read) is
        // skipped by generate_verilog_stubs itself, so re-running this
        // on a later read_verilog call never produces a duplicate
        // module definition. See verilog_stub_writer.hpp's own
        // top-of-file comment for why this has to be a real file
        // appended to the same read (not a separate prior read_verilog
        // call) - only files elaborated together share one Compilation.
        std::string stub_path;
        if (is_netlist)
        {
            std::vector<std::string> ambiguous;
            const std::string stub_source =
                le::generate_verilog_stubs_for_read(handle->root, handle->root.get_library_by_name(library_name), ambiguous);
            for (const std::string &name : ambiguous)
            {
                const std::vector<le::DesignId> designs = le::designs_named(handle->root, name);
                spdlog::warn("read_verilog: libraries {} all have a cell {} - no stub module for it, so give it a module "
                             "or read into one of those libraries",
                             le::library_names_of(handle->root, designs), name);
            }

            if (!stub_source.empty())
            {
                const std::filesystem::path candidate = std::filesystem::temp_directory_path() /
                    fmt::format("le_verilog_stubs_{}_{}.v", getpid(), reinterpret_cast<uintptr_t>(handle));
                std::ofstream stub_file(candidate);
                if (stub_file)
                {
                    stub_file << stub_source;
                    stub_file.close();
                    stub_path = candidate.string();
                    filename_strings.push_back(stub_path);
                }
                else
                {
                    spdlog::warn("read_verilog: could not write temporary stub file {} - LEF-only leaf cells may fail to elaborate",
                                 candidate.string());
                }
            }
        }

        const CleanAcrossRead clean(handle);
        le::SVReader reader;
        int result = 0;
        {
            RecordedRead recorded(handle->root, handle->command_history.current(), /*tracks_links=*/true);
            result = is_netlist ? reader.read_netlist(filename_strings, handle->root, library_name)
                                : reader.read_rtl(filename_strings, handle->root, library_name);
            if (result == 0)
                recorded.commit();
        }
        handle->root.bump_mutation_version(); // see le_read_lef

        if (!stub_path.empty())
        {
            std::error_code ec;
            std::filesystem::remove(stub_path, ec);
        }

        if (result == 0)
            clean.commit(handle);
        else
            clean.commit_failed(handle);
        return result;
    }
    catch (...)
    {
        log_exception("read_verilog");
        return 1;
    }

    int le_write_verilog_stubs(LeHandle *handle, const char *path, LeLibraryId library_id_c)
    try
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!path)
        {
            spdlog::error("write_verilog_stubs: path is null");
            return 1;
        }

        le::LibraryId library_id{.index = library_id_c.index, .generation = library_id_c.generation};
        if (library_id.index == UINT32_MAX)
        {
            const auto library_ids = handle->root.get_library_ids();
            if (library_ids.size() != 1)
            {
                spdlog::error("write_verilog_stubs: no -library given and {} Libraries exist - need exactly 1 to default to",
                               library_ids.size());
                return 1;
            }
            library_id = library_ids.front();
        }
        else if (!handle->root.get_library(library_id))
        {
            spdlog::error("write_verilog_stubs: unknown library");
            return 1;
        }

        const std::string stub_source = le::generate_verilog_stubs(handle->root, library_id);
        std::ofstream out(path);
        if (!out)
        {
            spdlog::error("write_verilog_stubs: could not open {} for writing", path);
            return 1;
        }
        out << stub_source;
        return 0;
    }
    catch (...)
    {
        log_exception("write_verilog_stubs");
        return 1;
    }

    int32_t le_link_unresolved_instances(LeHandle *handle)
    try
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        // Linking derives connectivity from what the reads loaded, changing
        // content (see le_read_lef's own bump) but no setting the user made.
        const CleanAcrossRead clean(handle);
        const size_t resolved = le::SVReader::link_unresolved_instances(handle->root);

        // Physical-side linking (Placement/Route/PhysicalPort <-> sibling
        // Schematic) runs
        // *after* the instance-resolution call above reaches its own
        // fixed point - an Instance whose own reference_design is still
        // unresolved can't be reached by link_physical's own hierarchical
        // descent (see that function's own "ordering dependency" comment,
        // schematic_layout_linker.hpp). Its own counts don't change this
        // function's return value (still Instance-resolution-count only,
        // matching sv_reader_test.cpp's own existing assertions) -
        // link_physical logs its own WARNING/ERROR messages via spdlog
        // directly, same as LEFReader/DEFReader.
        le::link_physical(handle->root);

        handle->root.bump_mutation_version();
        clean.commit(handle);
        return static_cast<int32_t>(resolved);
    }
    catch (...)
    {
        log_exception("link_unresolved_instances");
        return 0;
    }

    // --- Schematic<->Layout link mutation side-effects - unlike
    // link_physical/link_unresolved_instances above
    // (bulk, re-derivable, deliberately not undoable), these are direct
    // user edits, so each is batched into one undo/redo transaction the
    // same way move_click_unlocked is: raw Root:: calls (not the
    // generated public le_update_x/le_delete_x - those can't express
    // "clear this reference field back to unset", which the Net-delete
    // cascade specifically needs) under one held lock, with every step
    // recorded by hand (record_update, le::editing::delete_object).
    // handle->command_history.begin/end is called unconditionally in
    // each - a no-op if a caller (le_repl_eval) already has one open,
    // and otherwise starts/ends one itself (le_shell's own console isn't
    // wired through le_repl_eval - see le_tcl_procs.tcl's own comment).

    int le_delete_net_cascade(LeHandle *handle, LeNetId id)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        const le::NetId net_id = from_c(id);
        const le::NetData *existing_net = handle->root.get_net(net_id);
        if (!existing_net)
        {
            spdlog::error("delete_net: unknown id - no such Net exists");
            return 1;
        }
        const le::SchematicId schematic_id = existing_net->schematic;

        handle->command_history.begin("delete_net");
        le::editing::Transaction *txn = handle->command_history.current();

        const le::SchematicData *schematic = handle->root.get_schematic(schematic_id);
        if (schematic)
        {
            for (const le::InstanceId instance_id : handle->root.get_schematic_instances(schematic_id))
            {
                for (const le::PinId pin_id : handle->root.get_instance_pins(instance_id))
                {
                    const le::PinData *pin = handle->root.get_pin(pin_id);
                    if (!pin || pin->net != net_id)
                        continue;
                    const le::PinData before = *pin;
                    handle->root.update_pin(pin_id, le::InstanceId{}, std::optional<le::NetId>(le::NetId{}),
                                             std::nullopt, std::nullopt, std::nullopt);
                    if (txn)
                        txn->record_update<le::PinId, le::PinData>(pin_id, before, *handle->root.get_pin(pin_id),
                                                                    &le::apply_pin_snapshot);
                }
            }

            for (const le::PortId port_id : handle->root.get_schematic_ports(schematic_id))
            {
                const le::PortData *port = handle->root.get_port(port_id);
                if (!port || port->net != net_id)
                    continue;
                const le::PortData before = *port;
                handle->root.update_port(port_id, le::SchematicId{}, std::nullopt, std::optional<le::NetId>(le::NetId{}),
                                          std::nullopt, std::nullopt, std::nullopt);
                if (txn)
                    txn->record_update<le::PortId, le::PortData>(port_id, before, *handle->root.get_port(port_id),
                                                                  &le::apply_port_snapshot);
            }
        }

        const le::LayoutId layout_id = schematic ? handle->root.get_design_layout(schematic->design) : le::LayoutId{};
        if (layout_id.valid())
        {
            std::vector<le::RouteId> routes_to_delete;
            for (const le::RouteId route_id : handle->root.get_layout_routes(layout_id))
            {
                const le::RouteData *route = handle->root.get_route(route_id);
                if (route && route->net == net_id)
                    routes_to_delete.push_back(route_id);
            }
            // Newest first, so each comes off the end of its parent's list.
            for (auto route = routes_to_delete.rbegin(); route != routes_to_delete.rend(); ++route)
            {
                // Copies: deleting a child edits the list being walked.
                const std::vector<le::WireId> wire_ids = handle->root.get_route_wires(*route);
                for (auto wire = wire_ids.rbegin(); wire != wire_ids.rend(); ++wire)
                    le::editing::delete_object(handle->root, txn, *wire);
                const std::vector<le::ShapeId> shape_ids = handle->root.get_route_shapes(*route);
                for (auto shape = shape_ids.rbegin(); shape != shape_ids.rend(); ++shape)
                    le::editing::delete_object(handle->root, txn, *shape);
                le::editing::delete_object(handle->root, txn, *route);
            }

            for (const le::PhysicalPortId port_id : handle->root.get_layout_physical_ports(layout_id))
            {
                const le::PhysicalPortData *port = handle->root.get_physical_port(port_id);
                if (!port || port->net != net_id)
                    continue;
                const le::PhysicalPortData before = *port;
                handle->root.update_physical_port(port_id, le::LayoutId{}, std::optional<le::NetId>(le::NetId{}),
                                                   std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                                   std::nullopt, std::nullopt);
                if (txn)
                    txn->record_update<le::PhysicalPortId, le::PhysicalPortData>(
                        port_id, before, *handle->root.get_physical_port(port_id), &le::apply_physical_port_snapshot);
            }
        }

        le::editing::delete_object(handle->root, txn, net_id);
        handle->root.bump_mutation_version();
        handle->command_history.end(/*succeeded=*/true);
        return 0;
    }

    int le_rename_net_propagate(LeHandle *handle, LeNetId id, const char *new_name)
    {
        if (!handle)
            return 1;
        if (!new_name || !new_name[0])
        {
            spdlog::error("update_net: -name may not be empty");
            return 1;
        }
        HandleWriteLock lock(handle);

        const le::NetId net_id = from_c(id);
        const le::NetData *existing_net = handle->root.get_net(net_id);
        if (!existing_net)
        {
            spdlog::error("update_net: unknown id - no such Net exists");
            return 1;
        }

        const le::RenamePlan plan = le::plan_net_rename(handle->root, net_id, new_name);
        if (!plan.clashes.empty())
        {
            for (const std::string &clash : plan.clashes)
                spdlog::error("update_net: renaming to '{}' would rename a linked object onto a taken name - {}", new_name, clash);
            return 1;
        }

        handle->command_history.begin("update_net");
        le::editing::Transaction *txn = handle->command_history.current();

        const le::NetData before_net = *existing_net;
        const bool renamed =
            handle->root.update_net(net_id, le::SchematicId{}, std::nullopt, std::optional<std::string>(new_name),
                                     std::nullopt);
        if (!renamed)
        {
            spdlog::error("update_net: a sibling Net with this name already exists");
            handle->command_history.end(false);
            return 1;
        }
        if (txn)
            txn->record_update<le::NetId, le::NetData>(net_id, before_net, *handle->root.get_net(net_id),
                                                        &le::apply_net_snapshot);

        le::apply_rename_plan(
            handle->root, plan, [](le::PlacementId, const le::PlacementData &, const le::PlacementData &) {},
            [&](le::RouteId route_id, const le::RouteData &b, const le::RouteData &a)
            {
                if (txn)
                    txn->record_update<le::RouteId, le::RouteData>(route_id, b, a, &le::apply_route_snapshot);
            },
            [&](le::PhysicalPortId port_id, const le::PhysicalPortData &b, const le::PhysicalPortData &a)
            {
                if (txn)
                    txn->record_update<le::PhysicalPortId, le::PhysicalPortData>(port_id, b, a, &le::apply_physical_port_snapshot);
            });

        handle->root.bump_mutation_version();
        handle->command_history.end(/*succeeded=*/true);
        return 0;
    }

    int le_rename_instance_propagate(LeHandle *handle, LeInstanceId id, const char *new_name)
    {
        if (!handle)
            return 1;
        if (!new_name || !new_name[0])
        {
            spdlog::error("update_instance: -name may not be empty");
            return 1;
        }
        HandleWriteLock lock(handle);

        const le::InstanceId instance_id = from_c(id);
        const le::InstanceData *existing = handle->root.get_instance(instance_id);
        if (!existing)
        {
            spdlog::error("update_instance: unknown id - no such Instance exists");
            return 1;
        }

        const le::RenamePlan plan = le::plan_instance_rename(handle->root, instance_id, new_name);
        if (!plan.clashes.empty())
        {
            for (const std::string &clash : plan.clashes)
                spdlog::error("update_instance: renaming to '{}' would rename a linked object onto a taken name - {}", new_name, clash);
            return 1;
        }

        handle->command_history.begin("update_instance");
        le::editing::Transaction *txn = handle->command_history.current();

        const le::InstanceData before = *existing;
        const bool renamed = handle->root.update_instance(instance_id, le::SchematicId{}, std::nullopt,
                                                            std::optional<std::string>(new_name), std::nullopt,
                                                            std::nullopt, std::nullopt, std::nullopt, std::nullopt);
        if (!renamed)
        {
            spdlog::error("update_instance: a sibling Instance with this name already exists");
            handle->command_history.end(false);
            return 1;
        }
        if (txn)
            txn->record_update<le::InstanceId, le::InstanceData>(
                instance_id, before, *handle->root.get_instance(instance_id), &le::apply_instance_snapshot);

        const auto record_placement = [&](le::PlacementId placement_id, const le::PlacementData &b, const le::PlacementData &a)
        {
            if (txn)
                txn->record_update<le::PlacementId, le::PlacementData>(placement_id, b, a, &le::apply_placement_snapshot);
        };
        const auto record_route = [&](le::RouteId route_id, const le::RouteData &b, const le::RouteData &a)
        {
            if (txn)
                txn->record_update<le::RouteId, le::RouteData>(route_id, b, a, &le::apply_route_snapshot);
        };
        const auto record_physical_port =
            [&](le::PhysicalPortId port_id, const le::PhysicalPortData &b, const le::PhysicalPortData &a)
        {
            if (txn)
                txn->record_update<le::PhysicalPortId, le::PhysicalPortData>(port_id, b, a,
                                                                              &le::apply_physical_port_snapshot);
        };
        le::apply_rename_plan(handle->root, plan, record_placement, record_route, record_physical_port);

        handle->root.bump_mutation_version();
        handle->command_history.end(/*succeeded=*/true);
        return 0;
    }

    int le_write_lef(LeHandle *handle, const char *path,
                     const LeAbstractId *abstract_ids_c, int32_t abstract_id_count,
                     LeLibraryId library_id_c, int32_t layer_write_mode)
    try
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!path)
        {
            spdlog::error("write_lef: path is null");
            return 1;
        }

        le::LEFWriter::LayerWriteMode mode;
        switch (layer_write_mode)
        {
        case LE_LEF_LAYER_WRITE_MODE_INCLUDE_WITH_ABSTRACT:
            mode = le::LEFWriter::LayerWriteMode::IncludeWithAbstract;
            break;
        case LE_LEF_LAYER_WRITE_MODE_TECHNOLOGY_ONLY:
            mode = le::LEFWriter::LayerWriteMode::TechnologyOnly;
            break;
        default:
            mode = le::LEFWriter::LayerWriteMode::None;
            break;
        }

        // Abstract resolution order - see this
        // function's own api.hpp doc comment for the full 4-step
        // rationale. Skipped entirely in TechnologyOnly mode. Reads
        // handle->current_abstract_id directly rather than calling
        // le_current_abstract(handle) - that function takes
        // handle->mutex_ itself, and this function already holds it
        // (non-recursive std::mutex, so re-locking here would deadlock).
        std::vector<le::AbstractId> abstract_ids;
        if (mode != le::LEFWriter::LayerWriteMode::TechnologyOnly)
        {
            if (abstract_id_count > 0)
            {
                abstract_ids.reserve(static_cast<size_t>(abstract_id_count));
                for (int32_t i = 0; i < abstract_id_count; i++)
                    abstract_ids.push_back(le::AbstractId{.index = abstract_ids_c[i].index, .generation = abstract_ids_c[i].generation});
            }
            else
            {
                const le::LibraryId library_id{.index = library_id_c.index, .generation = library_id_c.generation};
                if (library_id.index != UINT32_MAX)
                {
                    for (const le::DesignId design_id : handle->root.get_library_designs(library_id))
                    {
                        const le::AbstractId design_abstract_id = handle->root.get_design_abstract(design_id);
                        if (design_abstract_id.index != UINT32_MAX)
                            abstract_ids.push_back(design_abstract_id);
                    }
                }
                else if (handle->current_abstract_id.index != UINT32_MAX)
                {
                    abstract_ids.push_back(handle->current_abstract_id);
                }
                else
                {
                    spdlog::error("write_lef: no Abstract or Library given and no current Abstract set");
                    return 1;
                }
            }
        }

        le::LEFWriter writer;
        return writer.write_lef(path, handle->root, abstract_ids, mode);
    }
    catch (...)
    {
        log_exception("write_lef");
        return 1;
    }

    int le_write_def(LeHandle *handle, const char *path, LeLayoutId layout_id_c)
    try
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        if (!path)
        {
            spdlog::error("write_def: path is null");
            return 1;
        }

        // Same "invalid id means use current" convention as le_write_lef
        // above - read handle->current_layout_id directly rather than
        // calling le_current_layout(handle), which would re-lock
        // handle->mutex_ and deadlock (see le_write_lef's own comment).
        le::LayoutId layout_id{.index = layout_id_c.index, .generation = layout_id_c.generation};
        if (layout_id.index == UINT32_MAX)
        {
            layout_id = handle->current_layout_id;
        }
        if (layout_id.index == UINT32_MAX)
        {
            spdlog::error("write_def: no Layout given and no current Layout set");
            return 1;
        }

        le::DEFWriter writer;
        return writer.write_def(path, handle->root, layout_id);
    }
    catch (...)
    {
        log_exception("write_def");
        return 1;
    }

    int le_write_db(LeHandle *handle, const char *path, int32_t with_session)
    try
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);
        if (!path || !path[0])
        {
            spdlog::error("write_db: a file path is required");
            return 1;
        }
        le::persistence::SaveOptions options;
        if (with_session)
            options.session_json = session_to_json(handle);
        const le::persistence::SaveReport report = le::persistence::save_native(handle->root, path, options);
        if (!report.ok())
        {
            spdlog::error("write_db: {}", report.error);
            return 1;
        }
        if (report.dangling_references)
            spdlog::warn("write_db: {} reference(s) to objects that no longer exist were written as unset", report.dangling_references);
        spdlog::info("write_db: wrote {} objects ({} bytes) to {}", report.objects, report.file_bytes, path);
        handle->saved_mutation_version = handle->root.mutation_version(); // the design is saved
        handle->db_path = path;
        return 0;
    }
    catch (...)
    {
        log_exception("write_db");
        return 1;
    }

    int le_read_db(LeHandle *handle, const char *path, int32_t with_session)
    try
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);
        if (!path || !path[0])
        {
            spdlog::error("read_db: a file path is required");
            return 1;
        }
        // Loading replaces the database wholesale, so it would silently
        // discard anything already read or created - only into an empty
        // one. (A "close/new design" command would lift this.)
        if (!le::persistence::database_is_empty(handle->root))
        {
            spdlog::error("read_db: the database already has data in it - read_db only loads into an empty session");
            return 1;
        }

        const le::persistence::LoadReport report = le::persistence::load_native(handle->root, path);
        if (!report.ok())
        {
            spdlog::error("read_db: {}", report.error);
            return 1;
        }
        for (const std::string &warning : report.warnings)
            spdlog::warn("read_db: {}", warning);
        if (!report.schema_matches)
            spdlog::info("read_db: {} was written with {}; its fields were matched by name", path,
                         report.file_schema_version == le::schema_info::kVersion
                             ? std::string("older extension schemas")
                             : fmt::format("schema version {} (this build: {})", report.file_schema_version, le::schema_info::kVersion));

        // Every object is new (Ids restart at {i, 0}): nothing that held an
        // Id from before may survive.
        handle->root.bump_mutation_version();
        handle->command_history.clear_undo_redo();
        handle->clear_selection();
        handle->current_technology_id = {};
        handle->current_abstract_id = {};
        handle->current_schematic_id = {};
        handle->current_layout_id = {};

        // What le_read_lef sets up after a Technology first appears.
        const auto technology_ids = handle->root.get_technology_ids();
        if (!technology_ids.empty())
        {
            for (le::LayerId layer_id : handle->root.get_technology_layers(technology_ids.front()))
            {
                const le::LayerData *layer = handle->root.get_layer(layer_id);
                if (layer && layer->type != "ROUTING" && layer->type != "CUT")
                    handle->set_layer_name_visible(layer->name, false);
            }
            rebuild_view_layers(handle, technology_ids.front());
            apply_pending_grid_um_unlocked(handle);
            handle->current_technology_id = technology_ids.front();
        }

        if (with_session && !report.session_json.empty())
            apply_session_json(handle, report.session_json, path);

        spdlog::info("read_db: read {} objects from {}", report.objects, path);
        handle->saved_mutation_version = handle->root.mutation_version(); // matches the file
        handle->db_path = path;
        return 0;
    }
    catch (...)
    {
        log_exception("read_db");
        return 1;
    }

    const char *le_db_path(LeHandle *handle)
    {
        if (!handle)
            return "";
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return hold_string(__func__, handle->db_path);
    }

    int32_t le_database_is_empty(LeHandle *handle)
    {
        if (!handle)
            return 1;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return le::persistence::database_is_empty(handle->root) ? 1 : 0;
    }

    const char *le_db_info(const char *path)
    {
        thread_local std::string text;
        if (!path || !path[0])
        {
            text = "error: a file path is required";
            return text.c_str();
        }
        const le::persistence::FileInfo info = le::persistence::inspect_native(path);
        if (!info.ok())
        {
            text = "error: " + info.error;
            return text.c_str();
        }
        text = fmt::format("file: {}\nsize: {} bytes\ncontainer version: {}\nschema version: {} (fingerprint {})\nthis build: {} (fingerprint {}) - {}\n", path,
                           info.file_bytes, info.container_version, info.schema_version, info.fingerprint, le::schema_info::kVersion, le::schema_info::kFingerprint,
                           info.schema_matches ? "same schema" : "different schema, fields matched by name on load");
        for (const auto &ext : info.extensions)
            text += fmt::format("extension {} {} (schema {}): {}\n", ext.name, ext.package_version, ext.schema_version,
                                ext.built_schema_version.empty() ? "not in this build - the file can't be opened here"
                                                                 : "this build has schema " + ext.built_schema_version);
        text += info.migrations.empty() ? "migrations to run: none\n" : fmt::format("migrations to run: {}\n", info.migrations.size());
        for (const auto &m : info.migrations)
            text += fmt::format("  {} {}: {}{}\n", m.extension.empty() ? "core" : m.extension, m.to_version, m.description,
                                m.unsupported ? " (can't be applied yet - the file can't be opened here)" : "");
        uint64_t total = 0;
        for (const auto &[name, rows] : info.classes)
        {
            text += fmt::format("  {:<32} {}\n", name, rows);
            total += rows;
        }
        text += fmt::format("objects: {}\nsession: {}", total, info.has_session ? "yes" : "no");
        return text.c_str();
    }

    const char *le_migrate_db(const char *in_path, const char *out_path)
    {
        thread_local std::string text;
        if (!in_path || !in_path[0] || !out_path || !out_path[0])
        {
            text = "error: an input and an output file path are required";
            return text.c_str();
        }
        const le::persistence::MigrateReport report = le::persistence::migrate_native(in_path, out_path);
        if (!report.ok())
        {
            text = "error: " + report.error;
            return text.c_str();
        }
        text = fmt::format("migrated {} (schema {}) to {} (schema {}): {} objects, {} bytes", in_path, report.from_schema_version, out_path,
                           le::schema_info::kVersion, report.objects, report.file_bytes);
        for (const std::string &warning : report.warnings)
            text += "\n" + warning;
        spdlog::info("migrate_db: {}", text);
        return text.c_str();
    }

    int32_t le_design_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_design_size());
    }

    const char *le_design_name(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return nullptr;

        const le::DesignData *design = handle->root.get_design(design_ids[static_cast<size_t>(index)]);
        return design ? hold_string(__func__, design->name) : nullptr;
    }

    int32_t le_property_path_failed(LeHandle *handle)
    {
        if (!handle)
            return 0;
        return thread_results().property_path_failed ? 1 : 0;
    }

    int le_set_current_design_abstract(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return 1;
        HandleWriteLock lock(handle);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return 1;

        // Moves the Abstract-view "current view" trackers together - two
        // genuinely separate concepts that happen to both live on
        // LeHandle: `handle->current_abstract()` (drives GUI rendering)
        // and the generated has_current_access
        // one (handle->current_abstract_id, what get_terminals/get_shapes/
        // etc.'s own default -of-omitted scope and resolve_terminal_id
        // derive from - see current_abstract_id's own declaration
        // comment). Selecting a Design from either the GUI or a TCL
        // script (open_design, itself calling
        // le_set_current_design_abstract_by_id below - the same shared
        // entry point) should mean the same thing to both.
        // Clears current_layout_id and handle->current_layout(): only one
        // view is "open" at a time
        // (see le_set_current_design_layout's own comment) - selecting
        // the Abstract view deactivates the Layout one, same as the
        // reverse.
        const le::DesignId design_id = design_ids[static_cast<size_t>(index)];
        const le::AbstractId abstract_id = handle->root.get_design_abstract(design_id);
        handle->set_current_abstract(abstract_id);
        handle->set_current_layout(le::LayoutId{});
        handle->current_abstract_id = abstract_id;
        handle->current_layout_id = le::LayoutId{};
        return 0;
    }

    int32_t le_library_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_library_size());
    }

    LeLibraryInfo le_library_at(LeHandle *handle, int32_t index)
    {
        const LeLibraryInfo invalid{.id = {UINT32_MAX, 0}, .name = nullptr};
        if (!handle || index < 0)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const auto library_ids = handle->root.get_library_ids();
        if (static_cast<size_t>(index) >= library_ids.size())
            return invalid;

        const le::LibraryId id = library_ids[static_cast<size_t>(index)];
        const le::LibraryData *library = handle->root.get_library(id);
        return LeLibraryInfo{.id = to_c(id), .name = library ? hold_string(__func__, library->name) : nullptr};
    }

    int32_t le_library_design_count(LeHandle *handle, int32_t library_index)
    {
        if (!handle || library_index < 0)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

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
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

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
            .name = design ? hold_string(__func__, design->name) : nullptr,
        };
    }

    // Technology is a single shared per-session instance, same assumption
    // database_units_microns() already makes (root.get_technology_ids().front()).
    // Hand-written, not generated - it's a session/singleton lookup, not
    // per-class CRUD (see CLAUDE.md's TCL section).
    LeTechnologyId le_technology_id(LeHandle *handle)
    {
        const LeTechnologyId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const auto technology_ids = handle->root.get_technology_ids();
        if (technology_ids.empty())
            return invalid;
        return to_c(technology_ids.front());
    }

    int le_set_current_design_abstract_by_id(LeHandle *handle, LeDesignId design_id)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        const le::DesignId id = from_c(design_id);
        if (!handle->root.get_design(id))
            return 1;

        // See le_set_current_design_abstract's own comment above - the
        // Abstract-view trackers move together, current_layout_id clears.
        const le::AbstractId abstract_id = handle->root.get_design_abstract(id);
        handle->set_current_abstract(abstract_id);
        handle->set_current_layout(le::LayoutId{});
        handle->current_abstract_id = abstract_id;
        handle->current_layout_id = le::LayoutId{};
        return 0;
    }

    int le_set_current_design_layout(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return 1;
        HandleWriteLock lock(handle);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return 1;

        // Mirror image of le_set_current_design_abstract: activates the
        // Layout view's own current-instance tracker (what get_rows/
        // get_placements/get_blockages/etc.'s own default -of-omitted
        // scope derives from) and handle->current_layout() (what
        // le_render_pixel_buffer renders), and
        // deactivates the Abstract-view ones - only one view is "open" at
        // a time, matching a real GUI showing one editor.
        const le::DesignId design_id = design_ids[static_cast<size_t>(index)];
        const le::LayoutId layout_id = handle->root.get_design_layout(design_id);
        handle->set_current_layout(layout_id);
        handle->set_current_abstract(le::AbstractId{});
        handle->current_layout_id = layout_id;
        handle->current_abstract_id = le::AbstractId{};
        return 0;
    }

    int le_set_current_design_layout_by_id(LeHandle *handle, LeDesignId design_id)
    {
        if (!handle)
            return 1;
        HandleWriteLock lock(handle);

        const le::DesignId id = from_c(design_id);
        if (!handle->root.get_design(id))
            return 1;

        // See le_set_current_design_layout's own comment above.
        const le::LayoutId layout_id = handle->root.get_design_layout(id);
        handle->set_current_layout(layout_id);
        handle->set_current_abstract(le::AbstractId{});
        handle->current_layout_id = layout_id;
        handle->current_abstract_id = le::AbstractId{};
        return 0;
    }

    // --- Editing / undo-redo ---

    void le_begin_command(LeHandle *handle, const char *label)
    {
        if (!handle || !label)
            return;
        HandleWriteLock lock(handle);
        handle->command_history.begin(label);
        handle->hold_renders();
    }

    void le_end_command(LeHandle *handle, int32_t succeeded)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->command_history.end(succeeded != 0);
        handle->release_renders();
    }

    int32_t le_undo(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return report_replay(handle, handle->command_history.undo(), "undo");
    }

    int32_t le_redo(LeHandle *handle)
    {
        if (!handle)
            return 0;
        HandleWriteLock lock(handle);
        return report_replay(handle, handle->command_history.redo(), "redo");
    }

    int32_t le_can_undo(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->command_history.can_undo() ? 1 : 0;
    }

    int32_t le_can_redo(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->command_history.can_redo() ? 1 : 0;
    }

    int32_t le_undo_limit(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(std::min<size_t>(handle->command_history.undo_limit(), INT32_MAX));
    }

    void le_set_undo_limit(LeHandle *handle, int32_t limit)
    {
        if (!handle || limit < 0)
            return;
        HandleWriteLock lock(handle);
        handle->command_history.set_undo_limit(static_cast<size_t>(limit));
    }

    int32_t le_command_history_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->command_history.recall_count());
    }

    const char *le_command_history_at(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        if (static_cast<size_t>(index) >= handle->command_history.recall_count())
            return nullptr;
        return hold_string(__func__, handle->command_history.recall_at(static_cast<size_t>(index)));
    }

    LeObjectRef le_object_invalid_ref(void)
    {
        return invalid_object_ref();
    }

    const char *le_object_kind_name(int32_t kind)
    {
        if (kind < 0 || static_cast<size_t>(kind) >= std::size(kObjectKinds))
            return nullptr;
        return kObjectKinds[kind].name;
    }

    int32_t le_object_kind_is_named(int32_t kind)
    {
        if (kind < 0 || static_cast<size_t>(kind) >= std::size(kObjectKinds))
            return 0;
        return kObjectKinds[kind].named ? 1 : 0;
    }

    int32_t le_object_property_count(LeHandle *handle, LeObjectRef ref)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(thread_properties(*handle, ref, build_object_properties).size());
    }

    LeProperty le_object_property_at(LeHandle *handle, LeObjectRef ref, int32_t index)
    {
        if (!handle)
            return invalid_property();
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return property_row(thread_properties(*handle, ref, build_object_properties), index);
    }

    LeObjectRef le_object_parent(LeHandle *handle, LeObjectRef ref)
    {
        if (!handle)
            return invalid_object_ref();
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        return object_ref_parent(handle->root, ref);
    }

    LeDesignId le_design_by_name(LeHandle *handle, const char *name)
    {
        const LeDesignId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const le::DesignId id = le::resolve_design_reference(handle->root, name);
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_design_name_by_id(LeHandle *handle, LeDesignId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const le::DesignData *data = handle->root.get_design(from_c(id));
        return data ? hold_string(__func__, data->name) : nullptr;
    }

    LeLibraryId le_design_library_by_id(LeHandle *handle, LeDesignId id)
    {
        const LeLibraryId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const le::DesignData *data = handle->root.get_design(from_c(id));
        return data ? to_c(data->library) : invalid;
    }

    LeTerminalId le_terminal_by_name(LeHandle *handle, const char *name)
    {
        const LeTerminalId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        // handle->current_abstract_id (le_set_current_abstract/
        // le_current_abstract's own generated field), not
        // handle->current_abstract() - the latter is a genuinely
        // separate "GUI current view" tracker. le_set_current_design_abstract/
        // le_set_current_design_abstract_by_id move both together (selecting a
        // Design should mean the same thing whether it came from the
        // GUI or a TCL script's open_design), but they can
        // still diverge: a script that builds an Abstract from scratch
        // and calls set_current_abstract directly (no Design to
        // open_design into at all) only ever touches
        // handle->current_abstract_id, never handle->current_abstract().
        // Reading handle->current_abstract() here instead would leave
        // resolve_terminal_id unable to find any Terminal a from-scratch
        // script just created, even though get_terminals (same "current"
        // concept, already reading current_abstract_id) sees it.
        // get_terminal_by_name is the per-Abstract unique_per_parent
        // index lookup, not a scan.
        const le::TerminalId id = handle->root.get_terminal_by_name(handle->current_abstract_id, name);
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_terminal_name(LeHandle *handle, LeTerminalId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::TerminalData *terminal = handle->root.get_terminal(from_c(id));
        return terminal ? hold_string(__func__, terminal->name) : nullptr;
    }

    LeRowId le_row_by_name(LeHandle *handle, const char *name)
    {
        const LeRowId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::RowId id = handle->root.get_row_by_name(handle->current_layout_id, name);
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_row_name(LeHandle *handle, LeRowId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::RowData *row = handle->root.get_row(from_c(id));
        return row ? hold_string(__func__, row->name) : nullptr;
    }

    LePlacementId le_placement_by_name(LeHandle *handle, const char *name)
    {
        const LePlacementId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::PlacementId id = handle->root.get_placement_by_name(handle->current_layout_id, name);
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_placement_name(LeHandle *handle, LePlacementId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::PlacementData *placement = handle->root.get_placement(from_c(id));
        return placement ? hold_string(__func__, placement->name) : nullptr;
    }

    LePhysicalPortId le_physical_port_by_name(LeHandle *handle, const char *name)
    {
        const LePhysicalPortId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::PhysicalPortId id = handle->root.get_physical_port_by_name(handle->current_layout_id, name);
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_physical_port_name(LeHandle *handle, LePhysicalPortId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::PhysicalPortData *physical_port = handle->root.get_physical_port(from_c(id));
        return physical_port ? hold_string(__func__, physical_port->name) : nullptr;
    }

    LeRouteId le_route_by_name(LeHandle *handle, const char *name)
    {
        const LeRouteId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::RouteId id = handle->root.get_route_by_name(handle->current_layout_id, name);
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_route_name(LeHandle *handle, LeRouteId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::RouteData *route = handle->root.get_route(from_c(id));
        return route ? hold_string(__func__, route->name) : nullptr;
    }

    LeRegionId le_region_by_name(LeHandle *handle, const char *name)
    {
        const LeRegionId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::RegionId id = handle->root.get_region_by_name(handle->current_layout_id, name);
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_region_name(LeHandle *handle, LeRegionId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::RegionData *region = handle->root.get_region(from_c(id));
        return region ? hold_string(__func__, region->name) : nullptr;
    }

    LeLayoutViaId le_layout_via_by_name(LeHandle *handle, const char *name)
    {
        const LeLayoutViaId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::LayoutViaId id = handle->root.get_layout_via_by_name(handle->current_layout_id, name);
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_layout_via_name(LeHandle *handle, LeLayoutViaId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::LayoutViaData *layout_via = handle->root.get_layout_via(from_c(id));
        return layout_via ? hold_string(__func__, layout_via->name) : nullptr;
    }

    LePortId le_port_by_name(LeHandle *handle, const char *name)
    {
        const LePortId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::PortId id = schematic_object_by_token_name<le::PortId>(
            handle, name, [](const le::Root &root, le::SchematicId schematic, const std::string &n)
            { return root.get_port_by_name(schematic, n); });
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_port_name(LeHandle *handle, LePortId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::PortData *port = handle->root.get_port(from_c(id));
        return port ? hold_string(__func__, port->name) : nullptr;
    }

    const char *le_port_token_name(LeHandle *handle, LePortId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::PortData *port = handle->root.get_port(from_c(id));
        return port ? schematic_object_token_name(handle, port->schematic, port->name, __func__) : nullptr;
    }

    LeNetId le_net_by_name(LeHandle *handle, const char *name)
    {
        const LeNetId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::NetId id = schematic_object_by_token_name<le::NetId>(
            handle, name, [](const le::Root &root, le::SchematicId schematic, const std::string &n)
            { return root.get_net_by_name(schematic, n); });
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_net_name(LeHandle *handle, LeNetId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::NetData *net = handle->root.get_net(from_c(id));
        return net ? hold_string(__func__, net->name) : nullptr;
    }

    const char *le_net_token_name(LeHandle *handle, LeNetId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::NetData *net = handle->root.get_net(from_c(id));
        return net ? schematic_object_token_name(handle, net->schematic, net->name, __func__) : nullptr;
    }

    LeInstanceId le_instance_by_name(LeHandle *handle, const char *name)
    {
        const LeInstanceId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::InstanceId id = schematic_object_by_token_name<le::InstanceId>(
            handle, name, [](const le::Root &root, le::SchematicId schematic, const std::string &n)
            { return root.get_instance_by_name(schematic, n); });
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_instance_name(LeHandle *handle, LeInstanceId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::InstanceData *instance = handle->root.get_instance(from_c(id));
        return instance ? hold_string(__func__, instance->name) : nullptr;
    }

    const char *le_instance_token_name(LeHandle *handle, LeInstanceId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::InstanceData *instance = handle->root.get_instance(from_c(id));
        return instance ? schematic_object_token_name(handle, instance->schematic, instance->name, __func__) : nullptr;
    }

    LePortBusId le_port_bus_by_name(LeHandle *handle, const char *name)
    {
        const LePortBusId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::PortBusId id = schematic_object_by_token_name<le::PortBusId>(
            handle, name, [](const le::Root &root, le::SchematicId schematic, const std::string &n)
            { return root.get_port_bus_by_name(schematic, n); });
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_port_bus_name(LeHandle *handle, LePortBusId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::PortBusData *bus = handle->root.get_port_bus(from_c(id));
        return bus ? hold_string(__func__, bus->name) : nullptr;
    }

    const char *le_port_bus_token_name(LeHandle *handle, LePortBusId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::PortBusData *bus = handle->root.get_port_bus(from_c(id));
        return bus ? schematic_object_token_name(handle, bus->schematic, bus->name, __func__) : nullptr;
    }

    LeNetBusId le_net_bus_by_name(LeHandle *handle, const char *name)
    {
        const LeNetBusId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::NetBusId id = schematic_object_by_token_name<le::NetBusId>(
            handle, name, [](const le::Root &root, le::SchematicId schematic, const std::string &n)
            { return root.get_net_bus_by_name(schematic, n); });
        return id.valid() ? to_c(id) : invalid;
    }

    const char *le_net_bus_name(LeHandle *handle, LeNetBusId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::NetBusData *bus = handle->root.get_net_bus(from_c(id));
        return bus ? hold_string(__func__, bus->name) : nullptr;
    }

    const char *le_net_bus_token_name(LeHandle *handle, LeNetBusId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::NetBusData *bus = handle->root.get_net_bus(from_c(id));
        return bus ? schematic_object_token_name(handle, bus->schematic, bus->name, __func__) : nullptr;
    }

    // le_get_instances/nets/ports_by_path: the TCL-facing hierarchical
    // path syntax -
    // get_instances/get_nets/get_ports' own thin wrapper (le_tcl_procs.tcl)
    // calls one of these instead of the plain flat le_get_<type> whenever
    // a name-expr argument contains "/", reusing the exact same shared
    // resolver `link` itself uses internally (hierarchical_resolver.hpp),
    // now including `**` recursive descent (a genuine differentiator most
    // EDA tool TCL interfaces don't offer - see that section's own note).
    // `-filter` still applies on top, via the same generic evaluator/
    // allowlist-validation helper the generated flat search already uses
    // (parse_and_validate_filter) - a path and a filter aren't mutually
    // exclusive.
    int32_t le_get_instances_by_path(LeHandle *handle, LeSchematicId of_schematic, const char *path,
                                      const char *filter_expression)
    {
        if (!handle || !path)
            return 0;
        HandleWriteLock lock(handle);

        bool ok = true;
        auto expr = parse_and_validate_filter(handle, "le_get_instances_by_path", "Instance", filter_expression, ok);
        if (!ok)
            return -1;

        const le::SchematicId requested = from_c(of_schematic);
        const le::SchematicId root_schematic =
            handle->root.get_schematic(requested) ? requested : handle->current_schematic_id;

        std::vector<le::InstanceId> results =
            le::hierarchy::resolve_instances(handle->root, root_schematic, le::hierarchy::split_path(path));
        if (expr)
        {
            std::vector<le::InstanceId> filtered;
            for (const le::InstanceId id : results)
            {
                const le::InstanceData *data = handle->root.get_instance(id);
                if (data && le::evaluate_filter(*expr, handle->root, id, *data))
                    filtered.push_back(id);
            }
            results = std::move(filtered);
        }

        handle->instance_search_results = std::move(results);
        return static_cast<int32_t>(handle->instance_search_results.size());
    }

    int32_t le_get_nets_by_path(LeHandle *handle, LeSchematicId of_schematic, const char *path,
                                 const char *filter_expression)
    {
        if (!handle || !path)
            return 0;
        HandleWriteLock lock(handle);

        bool ok = true;
        auto expr = parse_and_validate_filter(handle, "le_get_nets_by_path", "Net", filter_expression, ok);
        if (!ok)
            return -1;

        const le::SchematicId requested = from_c(of_schematic);
        const le::SchematicId root_schematic =
            handle->root.get_schematic(requested) ? requested : handle->current_schematic_id;

        std::vector<le::NetId> results =
            le::hierarchy::resolve_nets(handle->root, root_schematic, le::hierarchy::split_path(path));
        if (expr)
        {
            std::vector<le::NetId> filtered;
            for (const le::NetId id : results)
            {
                const le::NetData *data = handle->root.get_net(id);
                if (data && le::evaluate_filter(*expr, handle->root, id, *data))
                    filtered.push_back(id);
            }
            results = std::move(filtered);
        }

        handle->net_search_results = std::move(results);
        return static_cast<int32_t>(handle->net_search_results.size());
    }

    int32_t le_get_ports_by_path(LeHandle *handle, LeSchematicId of_schematic, const char *path,
                                  const char *filter_expression)
    {
        if (!handle || !path)
            return 0;
        HandleWriteLock lock(handle);

        bool ok = true;
        auto expr = parse_and_validate_filter(handle, "le_get_ports_by_path", "Port", filter_expression, ok);
        if (!ok)
            return -1;

        const le::SchematicId requested = from_c(of_schematic);
        const le::SchematicId root_schematic =
            handle->root.get_schematic(requested) ? requested : handle->current_schematic_id;

        std::vector<le::PortId> results =
            le::hierarchy::resolve_ports(handle->root, root_schematic, le::hierarchy::split_path(path));
        if (expr)
        {
            std::vector<le::PortId> filtered;
            for (const le::PortId id : results)
            {
                const le::PortData *data = handle->root.get_port(id);
                if (data && le::evaluate_filter(*expr, handle->root, id, *data))
                    filtered.push_back(id);
            }
            results = std::move(filtered);
        }

        handle->port_search_results = std::move(results);
        return static_cast<int32_t>(handle->port_search_results.size());
    }

    int32_t le_search_terminal(LeHandle *handle, const char *filter_expression)
    {
        if (!handle || !filter_expression)
            return 0;
        HandleWriteLock lock(handle);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            spdlog::error("le_search_terminal: {}", expr.error());
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
        HandleWriteLock lock(handle);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            spdlog::error("le_search_terminal_port: {}", expr.error());
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
        HandleWriteLock lock(handle);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            spdlog::error("le_search_obstruction: {}", expr.error());
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
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_terminal_port_shapes(from_c(id)).size());
    }

    LeShapeId le_terminal_port_shape_at(LeHandle *handle, LeTerminalPortId id, int32_t index)
    {
        const LeShapeId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const std::vector<le::ShapeId> &shapes = handle->root.get_terminal_port_shapes(from_c(id));
        if (static_cast<size_t>(index) >= shapes.size())
            return invalid;
        return to_c(shapes[static_cast<size_t>(index)]);
    }

    int32_t le_obstruction_shape_count(LeHandle *handle, LeObstructionId id)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_obstruction_shapes(from_c(id)).size());
    }

    LeShapeId le_obstruction_shape_at(LeHandle *handle, LeObstructionId id, int32_t index)
    {
        const LeShapeId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const std::vector<le::ShapeId> &shapes = handle->root.get_obstruction_shapes(from_c(id));
        if (static_cast<size_t>(index) >= shapes.size())
            return invalid;
        return to_c(shapes[static_cast<size_t>(index)]);
    }

    const char *le_shape_layer_name(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return nullptr;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape)
            return nullptr;
        const le::LayerData *layer = handle->root.get_layer(shape->layer);
        return layer ? hold_string(__func__, layer->name) : nullptr;
    }

    int32_t le_shape_rect_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->rects.size()) : 0;
    }

    LeRectUm le_shape_rect_at(LeHandle *handle, LeShapeId id, int32_t index)
    {
        const LeRectUm invalid{.ll_x_um = 0, .ll_y_um = 0, .ur_x_um = 0, .ur_y_um = 0};
        if (!handle || index < 0)
            return invalid;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

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
        HandleWriteLock lock(handle);
        return le::edit::remove_shape_piece(*handle, from_c(id), le::PieceKind::RECT, static_cast<size_t>(index)) ? 0 : 1;
    }

    int32_t le_shape_polygon_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->polygons.size()) : 0;
    }

    int32_t le_shape_polygon_point_count(LeHandle *handle, LeShapeId id, int32_t polygon_index)
    {
        if (!handle || polygon_index < 0)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

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
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

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
        HandleWriteLock lock(handle);
        return le::edit::remove_shape_piece(*handle, from_c(id), le::PieceKind::POLYGON, static_cast<size_t>(polygon_index)) ? 0 : 1;
    }

    int32_t le_shape_path_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->paths.size()) : 0;
    }

    double le_shape_path_width_um(LeHandle *handle, LeShapeId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

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
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

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
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

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
        HandleWriteLock lock(handle);
        return le::edit::remove_shape_piece(*handle, from_c(id), le::PieceKind::PATH, static_cast<size_t>(path_index)) ? 0 : 1;
    }

    int le_remove_wire_path(LeHandle *handle, LeWireId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 1;
        HandleWriteLock lock(handle);
        return le::edit::remove_shape_piece(*handle, from_c(id), le::PieceKind::PATH, static_cast<size_t>(path_index)) ? 0 : 1;
    }

    int32_t le_shape_copy(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, LeLayerId layer, const char *purpose, LeObjectRef parent)
    {
        return run_shape_op(handle, "shape_copy", layer, purpose, parent, [&](const auto &target, const auto &owner)
                            {
            if (!target)
                return le::edit::ShapeOpResult(std::unexpected("-layer is required"));
            return le::edit::shape_copy(*handle, shape_ids_from_c(shapes, shape_count), *target, owner); });
    }

    int32_t le_shape_boolean(LeHandle *handle, const LeObjectRef *shapes_a, int32_t shape_a_count, const LeObjectRef *shapes_b,
                             int32_t shape_b_count, int32_t op, LeLayerId layer, const char *purpose, LeObjectRef parent)
    {
        const char *command = op == LE_SHAPE_BOOLEAN_AND ? "shape_and" : op == LE_SHAPE_BOOLEAN_NOT ? "shape_not" : "shape_or";
        return run_shape_op(handle, command, layer, purpose, parent, [&](const auto &target, const auto &owner)
                            {
            const le::BooleanOp boolean_op = op == LE_SHAPE_BOOLEAN_AND ? le::BooleanOp::And
                                           : op == LE_SHAPE_BOOLEAN_NOT ? le::BooleanOp::Not
                                                                        : le::BooleanOp::Or;
            return le::edit::shape_boolean(*handle, shape_ids_from_c(shapes_a, shape_a_count), shape_ids_from_c(shapes_b, shape_b_count),
                                           boolean_op, target, owner); });
    }

    int32_t le_shape_to_polygon(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, LeLayerId layer, const char *purpose, LeObjectRef parent)
    {
        return run_shape_op(handle, "shape_to_polygon", layer, purpose, parent, [&](const auto &target, const auto &owner)
                            { return le::edit::shape_to_polygons(*handle, shape_ids_from_c(shapes, shape_count), target, owner); });
    }

    int32_t le_shape_to_rects(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, int32_t vertical, LeLayerId layer, const char *purpose,
                              LeObjectRef parent)
    {
        return run_shape_op(handle, "shape_to_rects", layer, purpose, parent, [&](const auto &target, const auto &owner)
                            {
            const le::FractureDirection direction = vertical ? le::FractureDirection::Vertical : le::FractureDirection::Horizontal;
            return le::edit::shape_to_rects(*handle, shape_ids_from_c(shapes, shape_count), direction, target, owner); });
    }

    int32_t le_shape_size(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, double dx_um, double dy_um, LeLayerId layer,
                          const char *purpose, LeObjectRef parent)
    {
        return run_shape_op(handle, "shape_size", layer, purpose, parent, [&](const auto &target, const auto &owner)
                            {
            const std::optional<double> dbu_per_um = database_units_microns(handle->root);
            if (!dbu_per_um)
                return le::edit::ShapeOpResult(std::unexpected("no Technology with a DATABASE MICRONS scale has been read yet"));
            return le::edit::shape_size(*handle, shape_ids_from_c(shapes, shape_count), to_dbu(dx_um, *dbu_per_um), to_dbu(dy_um, *dbu_per_um),
                                        target, owner); });
    }

    int32_t le_shape_path(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, double width_um, LeLayerId layer, const char *purpose,
                          LeObjectRef parent)
    {
        return run_shape_op(handle, "shape_path", layer, purpose, parent, [&](const auto &target, const auto &owner)
                            {
            const std::optional<double> dbu_per_um = database_units_microns(handle->root);
            if (!dbu_per_um)
                return le::edit::ShapeOpResult(std::unexpected("no Technology with a DATABASE MICRONS scale has been read yet"));
            return le::edit::shape_outline_paths(*handle, shape_ids_from_c(shapes, shape_count), to_dbu(width_um, *dbu_per_um), target, owner); });
    }

    int32_t le_shape_change_layer(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count, LeLayerId layer, const char *purpose)
    {
        if (!handle)
            return -1;
        HandleWriteLock lock(handle);
        const auto target = target_from_c(layer, purpose);
        if (!target || !*target)
        {
            spdlog::error("shape_change_layer: {}", target ? std::string("-layer is required") : target.error());
            return -1;
        }
        const std::vector<le::GeometryId> ids = shape_ids_from_c(shapes, shape_count);
        const auto changed = le::edit::shape_change_layer(*handle, ids, **target);
        if (!changed)
        {
            spdlog::error("shape_change_layer: {}", changed.error());
            return -1;
        }
        return static_cast<int32_t>(ids.size());
    }

    LeShapeId le_shape_op_result_at(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return LeShapeId{.index = UINT32_MAX, .generation = 0};
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        if (static_cast<size_t>(index) >= handle->shape_op_results.size())
            return LeShapeId{.index = UINT32_MAX, .generation = 0};
        return to_c(handle->shape_op_results[static_cast<size_t>(index)]);
    }

    LeShapeBbox le_shape_bbox(LeHandle *handle, const LeObjectRef *shapes, int32_t shape_count)
    {
        LeShapeBbox out{};
        if (!handle)
            return out;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        const std::expected<le::Rect, std::string> box = le::shape_ops::bbox(handle->root, shape_ids_from_c(shapes, shape_count));
        if (!box)
        {
            spdlog::error("shape_bbox: {}", box.error());
            return out;
        }
        const double dbu_per_um = display_dbu_per_um(handle->root);
        out.valid = 1;
        out.ll_x_um = static_cast<double>(box->ll.x) / dbu_per_um;
        out.ll_y_um = static_cast<double>(box->ll.y) / dbu_per_um;
        out.ur_x_um = static_cast<double>(box->ur.x) / dbu_per_um;
        out.ur_y_um = static_cast<double>(box->ur.y) / dbu_per_um;
        return out;
    }

    int frame_mark_count = 0;

    LePixelBuffer le_render_pixel_buffer(LeHandle *handle)
    try
    {
        if (!handle)
            return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};
        // The GUI render thread and the Tcl thread (dump_png) both render;
        // render_mutex_ keeps their pipeline runs apart. mutex_ is only
        // shared, so the GUI's panel reads stay live during a render: the
        // render mutates nothing but view_render_pipeline (and the
        // flightline cache, which has its own mutex).
        std::lock_guard<std::mutex> render_lock(handle->render_mutex_);
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);

        // A viewport that hasn't been sized yet (le_set_viewport_size
        // never called - LeHandle's own viewport_width_px_/
        // viewport_height_px_ both default to 0) has nothing to render -
        // short-circuit before the pipeline runs at all, rather than
        // letting it clamp a degenerate 0x0 request up to some minimum
        // internally (Blend2D itself can't construct a zero-sized
        // BLImage) and leak that clamped size back out as a misleading
        // non-empty result.
        if (handle->viewport_width_px() <= 0 || handle->viewport_height_px() <= 0)
            return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};

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

        // view_render_options_for maps LeHandle's pan/scale/viewport size,
        // selection, hover, rulers and other overlays onto
        // ViewRenderOptions (see its own comment).
        const le::ViewRenderOptions options = view_render_options_for(handle);

        // is_rendering_'s own doc comment
        // (LeHandle) explains why this is a plain atomic, not mutex_-
        // guarded. Bracketed around only the real recompute (would_recompute()
        // true), not this whole function - a call that finds nothing
        // changed (the common case, and now the *only* case whenever
        // this is called with nothing to do, since le_gui.cpp's render
        // thread no longer calls this speculatively on a timer - see
        // le_wait_for_render_needed's own doc comment, api.hpp) never
        // sets this at all, so a poller sees an accurate, precisely-timed
        // signal instead of one smeared across cheap no-op calls too.
        // RAII, not a manual reset-before-every-return, so a future early
        // return (or a real exception, however unlikely given this
        // codebase's own "no exceptions for expected-missing-data paths"
        // convention) can't leave this stuck true.
        const bool will_recompute = handle->view_render_pipeline.would_recompute(options);
        if (will_recompute)
            handle->is_rendering_.store(true, std::memory_order_relaxed);
        struct RenderingGuard
        {
            LeHandle *handle;
            bool active;
            ~RenderingGuard()
            {
                if (active)
                    handle->is_rendering_.store(false, std::memory_order_relaxed);
            }
        } rendering_guard{handle, will_recompute};

        const le::ViewRenderPipeline::WarmOutput output = handle->view_render_pipeline.run(&handle->root, options);

        FrameMarkEnd(kRenderFrameName);

        // The caller reads `data` after both locks are released, while
        // another thread's render may replace the pipeline's cached frame.
        // Frames are immutable, so holding this thread's last one keeps
        // `data` valid until this thread renders again (api.hpp).
        thread_local le::ComposeStage::OutputHandle this_thread_frame;
        this_thread_frame = output.frame;

        if (!output.frame || output.frame->empty)
            return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};

        return LePixelBuffer{
            .data = output.frame->buffer.data,
            .width = output.frame->buffer.width,
            .height = output.frame->buffer.height,
            .row_bytes = static_cast<int64_t>(output.frame->buffer.row_bytes),
        };
    }
    catch (...)
    {
        log_exception("render_pixel_buffer");
        return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};
    }

    int32_t le_is_rendering(LeHandle *handle)
    {
        // No lock - see this function's own doc comment (api.hpp) and
        // is_rendering_'s own doc comment (LeHandle) for why not.
        if (!handle)
            return 0;
        return handle->is_rendering_.load(std::memory_order_relaxed) ? 1 : 0;
    }

    int32_t le_is_command_running(LeHandle *handle)
    {
        if (!handle)
            return 0;
        return handle->renders_held() ? 1 : 0;
    }

    void le_wait_for_render_needed(LeHandle *handle)
    {
        if (!handle)
            return;
        handle->wait_for_render_needed();
    }

    void le_cancel_render_wait(LeHandle *handle)
    {
        if (!handle)
            return;
        handle->cancel_render_wait();
    }

    void le_request_show_gui(LeHandle *handle)
    {
        // No lock - see gui_show_requested_'s own doc comment (LeHandle).
        if (!handle)
            return;
        handle->gui_show_requested_.store(true, std::memory_order_relaxed);
    }

    int32_t le_take_show_gui_request(LeHandle *handle)
    {
        // exchange(false), not load() - see gui_show_requested_'s own doc
        // comment for why this consumes the request rather than just
        // peeking at it.
        if (!handle)
            return 0;
        return handle->gui_show_requested_.exchange(false, std::memory_order_relaxed) ? 1 : 0;
    }

    void le_request_close_gui(LeHandle *handle)
    {
        if (!handle)
            return;
        handle->gui_close_requested_.store(true, std::memory_order_relaxed);
    }

    int32_t le_take_close_gui_request(LeHandle *handle)
    {
        if (!handle)
            return 0;
        return handle->gui_close_requested_.exchange(false, std::memory_order_relaxed) ? 1 : 0;
    }

    void le_enqueue_tcl_command(LeHandle *handle, const char *command)
    {
        if (!handle || !command)
            return;
        std::lock_guard<std::mutex> lock(handle->pending_tcl_commands_mutex_);
        handle->pending_tcl_commands_.emplace_back(command);
    }

    const char *le_take_next_pending_tcl_command(LeHandle *handle)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->pending_tcl_commands_mutex_);
        if (handle->pending_tcl_commands_.empty())
            return nullptr;
        const char *command = hold_string(__func__, handle->pending_tcl_commands_.front());
        handle->pending_tcl_commands_.pop_front();
        return command;
    }
}
