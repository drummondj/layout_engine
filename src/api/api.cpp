#include "../database/database.hpp"
#include "../database/filter.hpp"
#include "../database/hierarchical_resolver.hpp"
#include "../database/library_helpers.hpp"
#include "../database/schematic_layout_linker.hpp"
#include "../database/signal_use_inference.hpp"
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
#include "views.hpp"
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
    // Each class's filterable leaf fields and hops (filter_tables.inc,
    // generated from the schema), so a -filter naming an unknown field or
    // hop is an error rather than a silent no-match. Hops into value types
    // (an Abstract's bbox, a Shape's rects, ...) aren't filterable.
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

}

namespace le::api_detail
{
    std::expected<std::optional<le::FilterExpr>, std::string> parse_search_filter(const std::string &root_class, const std::string &filter)
    {
        if (filter.empty())
            return std::nullopt;
        std::expected<le::FilterExpr, std::string> parsed = le::parse_filter_expression(filter);
        if (!parsed)
            return std::unexpected(parsed.error());
        if (std::optional<std::string> error = validate_filter_expr(root_class, *parsed))
            return std::unexpected(*error);
        return std::optional<le::FilterExpr>(std::move(*parsed));
    }
}

namespace le::api_detail
{
    // Single shared/global Technology, same assumption
    // snapped_mouse_position's own lookup makes - nullopt if none has
    // been read yet (read_lef) or it has no usable scale.
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

    // Looks up `name` among an object's property table rows - the rows a
    // bare `get_properties $token` shows. A single-segment property path
    // resolves this way, so a name like Shape's "rects" works too: the
    // -filter DSL's get_field() (what resolve_property_path() uses for a
    // chained path) only recognizes scalar leaf fields, not list-of-object
    // fields like rects/polygons/paths.
    std::optional<le::PropertyValue> find_property_by_name(const std::vector<le::PropertyValue> &properties, std::string_view name)
    {
        for (const le::PropertyValue &property : properties)
        {
            if (property.name == name)
                return property;
        }
        return std::nullopt;
    }

    // build_<type>_properties for every readable class: its property
    // table as the Property Viewer and get_properties show it.
#include "generated/api/property_accessors_internal.inc"

    // kObjectKinds, and properties, parents and property paths by ObjectId.
#include "generated/api/object_dispatch.inc"
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

    // An undo's or redo's result: logs the steps that couldn't be applied.
    std::expected<bool, std::string> replay_outcome(LeHandle *handle, const le::editing::ReplayResult &result, std::string_view verb)
    {
        if (!result.replayed)
            return false;
        refresh_armed_move_geometry_unlocked(handle);
        if (result.failed_steps == 0)
            return true;
        const std::string error = fmt::format("{} '{}': {} of its {} steps could not be applied", verb, result.label, result.failed_steps, result.steps);
        spdlog::error("{}", error);
        return std::unexpected(error);
    }
}

namespace
{
    // A failed read's or write's error text: the ERROR lines it reported,
    // else a generic line.
    std::string io_failure(std::string_view verb, const std::string &path, const std::vector<std::string> &messages, int code)
    {
        std::string text;
        for (const std::string &message : messages)
            if (message.starts_with("ERROR"))
                text += (text.empty() ? "" : "\n") + message;
        return text.empty() ? fmt::format("{} {} failed (code {})", verb, path, code) : text;
    }

    // Logs `message` as `verb`'s error and returns it as one.
    std::unexpected<std::string> logged_error(std::string_view verb, std::string message)
    {
        spdlog::error("{}: {}", verb, message);
        return std::unexpected(std::move(message));
    }

    // Opens one view of `design`: both "current view" trackers move together
    // - LeHandle's (what the design view renders) and the generated
    // current_<type> one (Tcl's default scope) - and the other view closes.
    std::expected<void, std::string> open_view(LeHandle &handle, le::DesignId design, bool layout)
    {
        if (!handle.root.get_design(design))
            return std::unexpected("unknown Design");
        const le::AbstractId abstract_id = layout ? le::AbstractId{} : handle.root.get_design_abstract(design);
        const le::LayoutId layout_id = layout ? handle.root.get_design_layout(design) : le::LayoutId{};
        handle.set_current_abstract(abstract_id);
        handle.set_current_layout(layout_id);
        handle.current_abstract_id = abstract_id;
        handle.current_layout_id = layout_id;
        return {};
    }
}

namespace le::edit
{
    std::expected<void, std::string> open_abstract(LeHandle &handle, DesignId design) { return open_view(handle, design, false); }

    std::expected<void, std::string> open_layout(LeHandle &handle, DesignId design) { return open_view(handle, design, true); }

    std::expected<void, std::string> read_lef(LeHandle &handle, const std::string &path, const std::string &library)
    {
        LeHandle *const h = &handle;
        if (library.empty())
            return logged_error("read_lef", "a library name is required");

        // The Technology's layer count before this read, so the default-
        // visibility pass below touches only the layers it adds: a layer
        // the user has since made visible stays so across later reads.
        size_t old_layer_count = 0;
        {
            const auto existing_technology_ids = h->root.get_technology_ids();
            if (!existing_technology_ids.empty())
                old_layer_count = h->root.get_technology_layers(existing_technology_ids.front()).size();
        }

        const CleanAcrossRead clean(h);
        le::LEFReader reader;
        int result = 0;
        {
            RecordedRead recorded(h->root, h->command_history.current(), /*tracks_links=*/false);
            result = reader.read_lef(std::filesystem::path(path).string(), h->root, library);
            if (result == 0)
                recorded.commit();
        }
        // The readers create through Root directly, which never bumps the
        // version itself - without this, anything keyed on it (the render
        // graph's LayerGenerationStage, ensure_view_layers_current) keeps
        // what it computed before the read. Also on failure: the rollback
        // put the content back, but saturated the change log.
        h->root.bump_mutation_version();
        if (result != 0)
        {
            clean.commit_failed(h);
            return std::unexpected(io_failure("read_lef", path, reader.messages(), result));
        }

        // Rebuilt after every successful read, so a later file's new
        // layers are picked up too.
        const auto technology_ids = h->root.get_technology_ids();
        if (!technology_ids.empty())
        {
            // Every physical layer this read added defaults to hidden
            // unless it's ROUTING or CUT.
            const auto &layers = h->root.get_technology_layers(technology_ids.front());
            for (size_t i = old_layer_count; i < layers.size(); ++i)
            {
                const le::LayerData *layer = h->root.get_layer(layers[i]);
                if (layer && layer->type != "ROUTING" && layer->type != "CUT")
                    h->set_layer_name_visible(layer->name, false);
            }

            rebuild_view_layers(h, technology_ids.front());
            apply_pending_grid_um_unlocked(h); // a settings file read before any Technology existed

            // The session's one Technology is the current one: it has no
            // separate "open" step, so a read is where it becomes current.
            h->current_technology_id = technology_ids.front();
        }

        clean.commit(h);
        return {};
    }

    std::expected<void, std::string> read_def(LeHandle &handle, const std::string &path, const std::string &library)
    {
        LeHandle *const h = &handle;
        if (library.empty())
            return logged_error("read_def", "a library name is required");

        const CleanAcrossRead clean(h);
        le::DEFReader reader;
        int result = 0;
        {
            RecordedRead recorded(h->root, h->command_history.current(), /*tracks_links=*/false);
            result = reader.read_def(std::filesystem::path(path).string(), h->root, library);
            if (result == 0)
                recorded.commit();
        }
        h->root.bump_mutation_version(); // see read_lef
        if (result != 0)
        {
            clean.commit_failed(h);
            return std::unexpected(io_failure("read_def", path, reader.messages(), result));
        }

        // NONDEFAULTRULES may have created the session's Technology even
        // with no LEF read yet; DEF adds no layers, so the view layers stay.
        const auto technology_ids = h->root.get_technology_ids();
        if (!technology_ids.empty())
            h->current_technology_id = technology_ids.front();
        apply_pending_grid_um_unlocked(h); // a settings file read before any Technology existed

        clean.commit(h);
        return {};
    }

    std::expected<DefPlacementsReport, std::string> read_def_placements(LeHandle &handle, const std::string &path, LayoutId layout)
    {
        Root &root = handle.root;
        if (!layout.valid())
            layout = handle.current_layout_id;
        if (!root.get_layout(layout))
            return logged_error("read_def", "no Layout given and no current Layout set");

        const auto technology_ids = root.get_technology_ids();
        const TechnologyData *technology = technology_ids.empty() ? nullptr : root.get_technology(technology_ids.front());
        le::DEFReader reader;
        std::vector<le::DefComponentPlacement> components;
        if (const int result = reader.read_component_placements(path, technology ? technology->database_units_microns : 0, components);
            result != 0)
            return std::unexpected(io_failure("read_def", path, reader.messages(), result));

        // Planned in full before anything changes.
        DefPlacementsReport report;
        std::vector<std::pair<PlacementId, PlacementChanges>> plan;
        for (le::DefComponentPlacement &component : components)
        {
            const PlacementId id = root.get_placement_by_name(layout, component.name);
            const PlacementData *placement = root.get_placement(id);
            if (!placement)
            {
                report.unmatched.push_back(std::move(component.name));
                continue;
            }
            const DesignData *cell = root.get_design(placement->reference_design);
            if (!cell || cell->name != component.cell)
                report.cell_mismatches.push_back(component.name);
            const bool moves = component.location && (!placement->location || placement->location->x != component.location->x ||
                                                       placement->location->y != component.location->y);
            const bool turns = component.orientation && *component.orientation != placement->orientation;
            if (!moves && !turns && component.status == placement->placement_status)
                continue;
            plan.emplace_back(id, PlacementChanges{.placement_status = component.status,
                                                   .location = component.location,
                                                   .orientation = component.orientation});
        }

        const auto names = [](const std::vector<std::string> &all)
        {
            constexpr size_t kShown = 10;
            std::string text;
            for (size_t i = 0; i < std::min(all.size(), kShown); ++i)
                text += (i ? ", " : "") + all[i];
            return all.size() > kShown ? fmt::format("{} and {} more", text, all.size() - kShown) : text;
        };
        if (!report.unmatched.empty())
            spdlog::warn("read_def: {} component(s) match no placement: {}", report.unmatched.size(), names(report.unmatched));
        if (!report.cell_mismatches.empty())
            spdlog::warn("read_def: {} placement(s) place a different cell than their component: {}", report.cell_mismatches.size(),
                         names(report.cell_mismatches));

        api_detail::OwnStep step(handle, "read_def");
        for (auto &[id, changes] : plan)
        {
            // The DEF's placement wins over a lock: unlock first, so the
            // update may move it and lock it again.
            const PlacementData before = *root.get_placement(id);
            if (check_placement_update(root, before, changes))
                if (const auto unlocked = update_placement(handle, id, PlacementChanges{.placement_status = PlacementStatus::PLACED}); !unlocked)
                {
                    step.fail();
                    return logged_error("read_def", unlocked.error());
                }
            if (const auto updated = update_placement(handle, id, std::move(changes)); !updated)
            {
                step.fail();
                return logged_error("read_def", updated.error());
            }
            ++report.updated;
        }
        spdlog::info("read_def: updated {} placement(s) from '{}'", report.updated, path);
        return report;
    }

    std::expected<void, std::string> read_verilog(LeHandle &handle, std::span<const std::string> files, bool netlist, const std::string &library)
    {
        LeHandle *const h = &handle;
        if (files.empty())
            return logged_error("read_verilog", "no files given");
        if (library.empty())
            return logged_error("read_verilog", "a library name is required");

        std::vector<std::string> filenames(files.begin(), files.end());

        // A netlist elaborates gate-level instances of LEF-only cells (an
        // Abstract, no Schematic) only if a module for each is elaborated
        // with it, so stubs for them go into a temporary file read in the
        // same call - only files elaborated together share one
        // Compilation. RTL reads never elaborate, so need none.
        std::string stub_path;
        if (netlist)
        {
            std::vector<std::string> ambiguous;
            const std::string stub_source = le::generate_verilog_stubs_for_read(h->root, h->root.get_library_by_name(library), ambiguous);
            for (const std::string &name : ambiguous)
            {
                const std::vector<le::DesignId> designs = le::designs_named(h->root, name);
                spdlog::warn("read_verilog: libraries {} all have a cell {} - no stub module for it, so give it a module "
                             "or read into one of those libraries",
                             le::library_names_of(h->root, designs), name);
            }

            if (!stub_source.empty())
            {
                const std::filesystem::path candidate = std::filesystem::temp_directory_path() /
                    fmt::format("le_verilog_stubs_{}_{}.v", getpid(), reinterpret_cast<uintptr_t>(h));
                std::ofstream stub_file(candidate);
                if (stub_file)
                {
                    stub_file << stub_source;
                    stub_file.close();
                    stub_path = candidate.string();
                    filenames.push_back(stub_path);
                }
                else
                {
                    spdlog::warn("read_verilog: could not write temporary stub file {} - LEF-only leaf cells may fail to elaborate",
                                 candidate.string());
                }
            }
        }

        const CleanAcrossRead clean(h);
        le::SVReader reader;
        int result = 0;
        {
            RecordedRead recorded(h->root, h->command_history.current(), /*tracks_links=*/true);
            result = netlist ? reader.read_netlist(filenames, h->root, library) : reader.read_rtl(filenames, h->root, library);
            if (result == 0)
                recorded.commit();
        }
        h->root.bump_mutation_version(); // see read_lef

        if (!stub_path.empty())
        {
            std::error_code ec;
            std::filesystem::remove(stub_path, ec);
        }

        if (result != 0)
        {
            clean.commit_failed(h);
            return std::unexpected(io_failure("read_verilog", files.front(), reader.messages(), result));
        }
        clean.commit(h);
        return {};
    }

    std::expected<void, std::string> write_verilog_stubs(LeHandle &handle, const std::string &path, LibraryId library)
    {
        if (!library.valid())
        {
            const auto library_ids = handle.root.get_library_ids();
            if (library_ids.size() != 1)
                return logged_error("write_verilog_stubs",
                                    fmt::format("no -library given and {} Libraries exist - need exactly 1 to default to", library_ids.size()));
            library = library_ids.front();
        }
        else if (!handle.root.get_library(library))
            return logged_error("write_verilog_stubs", "unknown library");

        const std::string stub_source = le::generate_verilog_stubs(handle.root, library);
        std::ofstream out(path);
        if (!out)
            return logged_error("write_verilog_stubs", fmt::format("could not open {} for writing", path));
        out << stub_source;
        return {};
    }

    std::expected<void, std::string> write_lef(LeHandle &handle, const std::string &path, std::span<const AbstractId> abstracts,
                                               LibraryId library, LefLayerWriteMode layers)
    {
        le::LEFWriter::LayerWriteMode mode = le::LEFWriter::LayerWriteMode::None;
        if (layers == LefLayerWriteMode::INCLUDE_WITH_ABSTRACT)
            mode = le::LEFWriter::LayerWriteMode::IncludeWithAbstract;
        else if (layers == LefLayerWriteMode::TECHNOLOGY_ONLY)
            mode = le::LEFWriter::LayerWriteMode::TechnologyOnly;

        // An explicit list, else the Library's Abstracts, else the current
        // one; none for a Technology-only write.
        std::vector<le::AbstractId> abstract_ids;
        if (mode != le::LEFWriter::LayerWriteMode::TechnologyOnly)
        {
            if (!abstracts.empty())
                abstract_ids.assign(abstracts.begin(), abstracts.end());
            else if (library.valid())
            {
                for (const le::DesignId design_id : handle.root.get_library_designs(library))
                {
                    const le::AbstractId design_abstract_id = handle.root.get_design_abstract(design_id);
                    if (design_abstract_id.valid())
                        abstract_ids.push_back(design_abstract_id);
                }
            }
            else if (handle.current_abstract_id.valid())
                abstract_ids.push_back(handle.current_abstract_id);
            else
                return logged_error("write_lef", "no Abstract or Library given and no current Abstract set");
        }

        le::LEFWriter writer;
        const int result = writer.write_lef(path, handle.root, abstract_ids, mode);
        if (result != 0)
            return std::unexpected(io_failure("write_lef", path, writer.messages(), result));
        return {};
    }

    std::expected<void, std::string> write_def(LeHandle &handle, const std::string &path, LayoutId layout)
    {
        if (!layout.valid())
            layout = handle.current_layout_id;
        if (!layout.valid())
            return logged_error("write_def", "no Layout given and no current Layout set");

        le::DEFWriter writer;
        const int result = writer.write_def(path, handle.root, layout);
        if (result != 0)
            return std::unexpected(io_failure("write_def", path, writer.messages(), result));
        return {};
    }

    std::expected<void, std::string> write_db(LeHandle &handle, const std::string &path, bool with_session)
    {
        if (path.empty())
            return logged_error("write_db", "a file path is required");
        le::persistence::SaveOptions options;
        if (with_session)
            options.session_json = session_to_json(&handle);
        const le::persistence::SaveReport report = le::persistence::save_native(handle.root, path, options);
        if (!report.ok())
            return logged_error("write_db", report.error);
        if (report.dangling_references)
            spdlog::warn("write_db: {} reference(s) to objects that no longer exist were written as unset", report.dangling_references);
        spdlog::info("write_db: wrote {} objects ({} bytes) to {}", report.objects, report.file_bytes, path);
        handle.saved_mutation_version = handle.root.mutation_version(); // the design is saved
        handle.db_path = path;
        return {};
    }

    std::expected<void, std::string> read_db(LeHandle &handle, const std::string &path, bool with_session)
    {
        LeHandle *const h = &handle;
        if (path.empty())
            return logged_error("read_db", "a file path is required");
        // Loading replaces the database wholesale, so it would silently
        // discard anything already read or created - only into an empty
        // one.
        if (!le::persistence::database_is_empty(h->root))
            return logged_error("read_db", "the database already has data in it - read_db only loads into an empty session");

        const le::persistence::LoadReport report = le::persistence::load_native(h->root, path);
        if (!report.ok())
            return logged_error("read_db", report.error);
        for (const std::string &warning : report.warnings)
            spdlog::warn("read_db: {}", warning);
        if (!report.schema_matches)
            spdlog::info("read_db: {} was written with {}; its fields were matched by name", path,
                         report.file_schema_version == le::schema_info::kVersion
                             ? std::string("older extension schemas")
                             : fmt::format("schema version {} (this build: {})", report.file_schema_version, le::schema_info::kVersion));

        // Every object is new (Ids restart at {i, 0}): nothing that held an
        // Id from before may survive.
        h->root.bump_mutation_version();
        h->command_history.clear_undo_redo();
        h->clear_selection();
        h->current_technology_id = {};
        h->current_abstract_id = {};
        h->current_schematic_id = {};
        h->current_layout_id = {};

        // What read_lef sets up after a Technology first appears.
        const auto technology_ids = h->root.get_technology_ids();
        if (!technology_ids.empty())
        {
            for (le::LayerId layer_id : h->root.get_technology_layers(technology_ids.front()))
            {
                const le::LayerData *layer = h->root.get_layer(layer_id);
                if (layer && layer->type != "ROUTING" && layer->type != "CUT")
                    h->set_layer_name_visible(layer->name, false);
            }
            rebuild_view_layers(h, technology_ids.front());
            apply_pending_grid_um_unlocked(h);
            h->current_technology_id = technology_ids.front();
        }

        if (with_session && !report.session_json.empty())
            apply_session_json(h, report.session_json, path);

        spdlog::info("read_db: read {} objects from {}", report.objects, path);
        h->saved_mutation_version = h->root.mutation_version(); // matches the file
        h->db_path = path;
        return {};
    }
}

namespace le::edit
{
#include "generated/api/search_defs.inc"

    std::vector<PropertyValue> object_properties(const LeHandle &handle, const ObjectId &id) { return build_object_id_properties(handle.root, id); }

    std::optional<ObjectId> object_parent(const LeHandle &handle, const ObjectId &id) { return object_id_parent(handle.root, id); }

    std::expected<std::optional<PropertyValue>, std::string> object_property_path(const LeHandle &handle, const ObjectId &id, const std::string &path)
    {
        std::expected<std::vector<std::string>, std::string> parsed = parse_property_path(path);
        if (!parsed)
            return std::unexpected(parsed.error());
        return object_id_property_path(handle.root, id, *parsed);
    }

    namespace
    {
        // A hierarchical path search (hierarchical_resolver.hpp's syntax)
        // under `of`, or the current Schematic if `of` doesn't exist,
        // narrowed by a -filter expression in microns.
        template <typename IdT, typename ResolveFn, typename GetFn>
        std::expected<std::vector<IdT>, std::string> search_by_path(const LeHandle &handle, const char *klass, SchematicId of,
                                                                    const std::string &path, const std::string &filter,
                                                                    ResolveFn &&resolve, GetFn &&get)
        {
            std::expected<std::optional<FilterExpr>, std::string> expr = api_detail::parse_search_filter(klass, filter);
            if (!expr)
                return std::unexpected(expr.error());
            const SchematicId scope = handle.root.get_schematic(of) ? of : handle.current_schematic_id;
            std::vector<IdT> found = resolve(handle.root, scope, hierarchy::split_path(path));
            if (*expr)
            {
                const double dbu_per_um = api_detail::display_dbu_per_um(handle.root);
                std::erase_if(found, [&](IdT id)
                              {
                                  const auto *data = get(handle.root, id);
                                  return !data || !evaluate_filter(**expr, handle.root, id, *data, dbu_per_um); });
            }
            return found;
        }
    }

    std::expected<std::vector<InstanceId>, std::string> get_instances_by_path(const LeHandle &handle, SchematicId of, const std::string &path,
                                                                              const std::string &filter)
    {
        return search_by_path<InstanceId>(handle, "Instance", of, path, filter, hierarchy::resolve_instances,
                                          [](const Root &root, InstanceId id)
                                          { return root.get_instance(id); });
    }

    std::expected<std::vector<NetId>, std::string> get_nets_by_path(const LeHandle &handle, SchematicId of, const std::string &path,
                                                                    const std::string &filter)
    {
        return search_by_path<NetId>(handle, "Net", of, path, filter, hierarchy::resolve_nets,
                                     [](const Root &root, NetId id)
                                     { return root.get_net(id); });
    }

    std::expected<std::vector<PortId>, std::string> get_ports_by_path(const LeHandle &handle, SchematicId of, const std::string &path,
                                                                      const std::string &filter)
    {
        return search_by_path<PortId>(handle, "Port", of, path, filter, hierarchy::resolve_ports,
                                      [](const Root &root, PortId id)
                                      { return root.get_port(id); });
    }

    std::expected<size_t, std::string> link_unresolved_instances(LeHandle &handle)
    try
    {
        // Linking derives connectivity from what the reads loaded, changing
        // content but no setting the user made.
        const api_detail::CleanAcrossRead clean(&handle);
        const size_t resolved = SVReader::link_unresolved_instances(handle.root);
        // Physical linking descends through Instances, so it runs once their
        // designs are resolved.
        link_physical(handle.root);
        // After link_physical, so a use from the layout's DEF comes first.
        infer_signal_uses(handle.root);
        handle.root.bump_mutation_version();
        clean.commit(&handle);
        return resolved;
    }
    catch (const std::exception &e)
    {
        return std::unexpected(std::string("link_unresolved_instances: ") + e.what());
    }

    // The netlist edits below change references that the generated
    // update_<type> can't clear back to unset, so they call Root directly
    // and record each change by hand into one undo step.

    std::expected<void, std::string> delete_net_cascade(LeHandle &handle, NetId net_id)
    {
        Root &root = handle.root;
        const NetData *existing_net = root.get_net(net_id);
        if (!existing_net)
            return std::unexpected("no such Net exists");
        const SchematicId schematic_id = existing_net->schematic;

        api_detail::OwnStep step(handle, "delete_net");
        editing::Transaction *txn = handle.command_history.current();

        const SchematicData *schematic = root.get_schematic(schematic_id);
        if (schematic)
        {
            for (const InstanceId instance_id : root.get_schematic_instances(schematic_id))
            {
                for (const PinId pin_id : root.get_instance_pins(instance_id))
                {
                    const PinData *pin = root.get_pin(pin_id);
                    if (!pin || pin->net != net_id)
                        continue;
                    const PinData before = *pin;
                    root.update_pin(pin_id, InstanceId{}, std::optional<NetId>(NetId{}), std::nullopt, std::nullopt, std::nullopt);
                    if (txn)
                        txn->record_update<PinId, PinData>(pin_id, before, *root.get_pin(pin_id), &apply_pin_snapshot);
                }
            }

            for (const PortId port_id : root.get_schematic_ports(schematic_id))
            {
                const PortData *port = root.get_port(port_id);
                if (!port || port->net != net_id)
                    continue;
                const PortData before = *port;
                root.update_port(port_id, SchematicId{}, std::nullopt, std::optional<NetId>(NetId{}), std::nullopt, std::nullopt, std::nullopt, std::nullopt);
                if (txn)
                    txn->record_update<PortId, PortData>(port_id, before, *root.get_port(port_id), &apply_port_snapshot);
            }
        }

        const LayoutId layout_id = schematic ? root.get_design_layout(schematic->design) : LayoutId{};
        if (layout_id.valid())
        {
            std::vector<RouteId> routes_to_delete;
            for (const RouteId route_id : root.get_layout_routes(layout_id))
            {
                const RouteData *route = root.get_route(route_id);
                if (route && route->net == net_id)
                    routes_to_delete.push_back(route_id);
            }
            // Newest first, so each comes off the end of its parent's list.
            for (auto route = routes_to_delete.rbegin(); route != routes_to_delete.rend(); ++route)
            {
                // Copies: deleting a child edits the list being walked.
                const std::vector<WireId> wire_ids = root.get_route_wires(*route);
                for (auto wire = wire_ids.rbegin(); wire != wire_ids.rend(); ++wire)
                    editing::delete_object(root, txn, *wire);
                const std::vector<ShapeId> shape_ids = root.get_route_shapes(*route);
                for (auto shape = shape_ids.rbegin(); shape != shape_ids.rend(); ++shape)
                    editing::delete_object(root, txn, *shape);
                editing::delete_object(root, txn, *route);
            }

            for (const PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
            {
                const PhysicalPortData *port = root.get_physical_port(port_id);
                if (!port || port->net != net_id)
                    continue;
                const PhysicalPortData before = *port;
                root.update_physical_port(port_id, LayoutId{}, std::optional<NetId>(NetId{}), std::nullopt, std::nullopt, std::nullopt,
                                          std::nullopt, std::nullopt, std::nullopt, std::nullopt);
                if (txn)
                    txn->record_update<PhysicalPortId, PhysicalPortData>(port_id, before, *root.get_physical_port(port_id),
                                                                         &apply_physical_port_snapshot);
            }
        }

        editing::delete_object(root, txn, net_id);
        root.bump_mutation_version();
        return {};
    }

    namespace
    {
        std::expected<void, std::string> rename_clashes(const std::string &new_name, const RenamePlan &plan)
        {
            std::string message = "renaming to '" + new_name + "' would rename a linked object onto a taken name:";
            for (const std::string &clash : plan.clashes)
                message += " " + clash + ";";
            message.pop_back();
            return std::unexpected(std::move(message));
        }
    }

    std::expected<void, std::string> rename_net(LeHandle &handle, NetId net_id, const std::string &new_name)
    {
        Root &root = handle.root;
        if (new_name.empty())
            return std::unexpected("the new name may not be empty");
        const NetData *existing_net = root.get_net(net_id);
        if (!existing_net)
            return std::unexpected("no such Net exists");

        const RenamePlan plan = plan_net_rename(root, net_id, new_name);
        if (!plan.clashes.empty())
            return rename_clashes(new_name, plan);

        api_detail::OwnStep step(handle, "update_net");
        editing::Transaction *txn = handle.command_history.current();

        const NetData before_net = *existing_net;
        if (!root.update_net(net_id, SchematicId{}, std::nullopt, std::optional<std::string>(new_name), std::nullopt, std::nullopt))
        {
            step.fail();
            return std::unexpected("a sibling Net with this name already exists");
        }
        if (txn)
            txn->record_update<NetId, NetData>(net_id, before_net, *root.get_net(net_id), &apply_net_snapshot);

        apply_rename_plan(
            root, plan, [](PlacementId, const PlacementData &, const PlacementData &) {},
            [&](RouteId route_id, const RouteData &b, const RouteData &a)
            {
                if (txn)
                    txn->record_update<RouteId, RouteData>(route_id, b, a, &apply_route_snapshot);
            },
            [&](PhysicalPortId port_id, const PhysicalPortData &b, const PhysicalPortData &a)
            {
                if (txn)
                    txn->record_update<PhysicalPortId, PhysicalPortData>(port_id, b, a, &apply_physical_port_snapshot);
            });

        root.bump_mutation_version();
        return {};
    }

    std::expected<void, std::string> rename_instance(LeHandle &handle, InstanceId instance_id, const std::string &new_name)
    {
        Root &root = handle.root;
        if (new_name.empty())
            return std::unexpected("the new name may not be empty");
        const InstanceData *existing = root.get_instance(instance_id);
        if (!existing)
            return std::unexpected("no such Instance exists");

        const RenamePlan plan = plan_instance_rename(root, instance_id, new_name);
        if (!plan.clashes.empty())
            return rename_clashes(new_name, plan);

        api_detail::OwnStep step(handle, "update_instance");
        editing::Transaction *txn = handle.command_history.current();

        const InstanceData before = *existing;
        if (!root.update_instance(instance_id, SchematicId{}, std::nullopt, std::optional<std::string>(new_name), std::nullopt, std::nullopt,
                                  std::nullopt, std::nullopt, std::nullopt))
        {
            step.fail();
            return std::unexpected("a sibling Instance with this name already exists");
        }
        if (txn)
            txn->record_update<InstanceId, InstanceData>(instance_id, before, *root.get_instance(instance_id), &apply_instance_snapshot);

        apply_rename_plan(
            root, plan,
            [&](PlacementId placement_id, const PlacementData &b, const PlacementData &a)
            {
                if (txn)
                    txn->record_update<PlacementId, PlacementData>(placement_id, b, a, &apply_placement_snapshot);
            },
            [&](RouteId route_id, const RouteData &b, const RouteData &a)
            {
                if (txn)
                    txn->record_update<RouteId, RouteData>(route_id, b, a, &apply_route_snapshot);
            },
            [&](PhysicalPortId port_id, const PhysicalPortData &b, const PhysicalPortData &a)
            {
                if (txn)
                    txn->record_update<PhysicalPortId, PhysicalPortData>(port_id, b, a, &apply_physical_port_snapshot);
            });

        root.bump_mutation_version();
        return {};
    }
}

namespace le
{
    std::string_view object_kind_name(const ObjectId &id) { return kObjectKinds[id.index()].name; }

    bool object_kind_is_named(const ObjectId &id) { return kObjectKinds[id.index()].named; }
}

namespace le
{
    std::expected<std::string, std::string> db_info(const std::string &path)
    {
        if (path.empty())
            return std::unexpected("a file path is required");
        const persistence::FileInfo info = persistence::inspect_native(path);
        if (!info.ok())
            return std::unexpected(info.error);
        std::string text = fmt::format("file: {}\nsize: {} bytes\ncontainer version: {}\nschema version: {} (fingerprint {})\nthis build: {} (fingerprint {}) - {}\n",
                                       path, info.file_bytes, info.container_version, info.schema_version, info.fingerprint, schema_info::kVersion,
                                       schema_info::kFingerprint, info.schema_matches ? "same schema" : "different schema, fields matched by name on load");
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
        return text;
    }

    std::expected<std::string, std::string> migrate_db(const std::string &in_path, const std::string &out_path)
    {
        if (in_path.empty() || out_path.empty())
            return std::unexpected("an input and an output file path are required");
        const persistence::MigrateReport report = persistence::migrate_native(in_path, out_path);
        if (!report.ok())
            return std::unexpected(report.error);
        std::string text = fmt::format("migrated {} (schema {}) to {} (schema {}): {} objects, {} bytes", in_path, report.from_schema_version, out_path,
                                       schema_info::kVersion, report.objects, report.file_bytes);
        for (const std::string &warning : report.warnings)
            text += "\n" + warning;
        spdlog::info("migrate_db: {}", text);
        return text;
    }

    void SessionRequests::request_show_gui() const { handle_->gui_show_requested_.store(true, std::memory_order_relaxed); }

    bool SessionRequests::take_show_gui_request() const { return handle_->gui_show_requested_.exchange(false, std::memory_order_relaxed); }

    void SessionRequests::request_close_gui() const { handle_->gui_close_requested_.store(true, std::memory_order_relaxed); }

    bool SessionRequests::take_close_gui_request() const { return handle_->gui_close_requested_.exchange(false, std::memory_order_relaxed); }

    void SessionRequests::enqueue_tcl_command(std::string command) const
    {
        std::lock_guard<std::mutex> lock(handle_->pending_tcl_commands_mutex_);
        handle_->pending_tcl_commands_.push_back(std::move(command));
    }

    std::optional<std::string> SessionRequests::take_next_tcl_command() const
    {
        std::lock_guard<std::mutex> lock(handle_->pending_tcl_commands_mutex_);
        if (handle_->pending_tcl_commands_.empty())
            return std::nullopt;
        std::string command = std::move(handle_->pending_tcl_commands_.front());
        handle_->pending_tcl_commands_.pop_front();
        return command;
    }
}

namespace le::edit
{
    void begin_command(LeHandle &handle, const std::string &label)
    {
        handle.command_history.begin(label);
        handle.hold_renders();
    }

    void end_command(LeHandle &handle, bool succeeded)
    {
        handle.command_history.end(succeeded);
        handle.release_renders();
    }

    std::expected<bool, std::string> undo(LeHandle &handle) { return api_detail::replay_outcome(&handle, handle.command_history.undo(), "undo"); }

    std::expected<bool, std::string> redo(LeHandle &handle) { return api_detail::replay_outcome(&handle, handle.command_history.redo(), "redo"); }

    bool can_undo(const LeHandle &handle) { return handle.command_history.can_undo(); }

    bool can_redo(const LeHandle &handle) { return handle.command_history.can_redo(); }

    size_t command_history_size(const LeHandle &handle) { return handle.command_history.recall_count(); }

    const std::string &command_history_at(const LeHandle &handle, size_t index) { return handle.command_history.recall_at(index); }

    bool has_unsaved_database(const LeHandle &handle) { return api_detail::has_unsaved_database_unlocked(&handle); }

    const std::string &db_path(const LeHandle &handle) { return handle.db_path; }

    bool database_is_empty(const LeHandle &handle) { return persistence::database_is_empty(handle.root); }
}
