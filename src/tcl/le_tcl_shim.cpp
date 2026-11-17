#include "le_tcl_shim.hpp"
#include "le/extension_tcl.hpp"

#include "views.hpp"
#include "../core/themes.hpp"
#include "../database/hierarchical_resolver.hpp"
#include "../database/library_helpers.hpp"

#include <blend2d/blend2d.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <expected>
#include <format>
#include <span>
#include <variant>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
    // Whether Tcl's integer `value` names one of `first`..`last` of an enum.
    template <class Enum>
    bool in_range(int value, Enum first, Enum last)
    {
        return value >= static_cast<int>(first) && value <= static_cast<int>(last);
    }

    bool valid_piece_kind(int kind) { return in_range(kind, le::PieceKind::RECT, le::PieceKind::VIA_ITERATE); }
    bool valid_shape_snap_mode(int mode) { return in_range(mode, le::ShapeSnapMode::NONE, le::ShapeSnapMode::TRACKS); }

    // Set by set_session_handle() (see le_tcl_shim.hpp) - an externally-
    // owned handle wins over the lazy self-create below whenever one has
    // been injected (le_shell injects the handle its GUI renders).
    LeHandle *&injected_handle()
    {
        static LeHandle *handle = nullptr;
        return handle;
    }

    LeHandle *session()
    {
        if (injected_handle() != nullptr)
        {
            return injected_handle();
        }
        // Lives as long as the process (le_tcl loaded into a plain tclsh).
        static LeHandle *handle = le::Session().release();
        return handle;
    }

    // See le_tcl_shim.hpp's "IDs" comment for why AbstractId/DesignId
    // cross this shim packed into one int64_t rather than wrapped with a
    // custom SWIG struct typemap. Generic over every database id (all
    // identical {uint32_t index, generation} layouts) rather than one
    // pack/unpack pair per type - also still used internally here for
    // TerminalPortId/ObstructionId/ShapeId, whose friendly string form is
    // just this same packed integer with a type prefix (see
    // resolve_numeric_friendly_id/format_numeric_friendly_id below).
    template <typename IdT>
    int64_t pack(IdT id)
    {
        return (static_cast<int64_t>(id.generation) << 32) | static_cast<int64_t>(id.index);
    }

    template <typename IdT>
    IdT unpack(int64_t packed)
    {
        IdT id{};
        id.index = static_cast<uint32_t>(static_cast<uint64_t>(packed) & 0xFFFFFFFFu);
        id.generation = static_cast<uint32_t>((static_cast<uint64_t>(packed) >> 32) & 0xFFFFFFFFu);
        return id;
    }

    // Shared scratch buffer for shim functions that format and return a
    // `const char*` built on the fly here (not memory owned by Root, see
    // e.g. terminal_property_value/get_terminals_at/shape_rect_at below).
    // Safe to share across every such function despite the project's
    // usual "valid until the next call" pointer convention: SWIG's Tcl
    // typemap for `const char*` copies the bytes into a new Tcl_Obj
    // immediately on return, before the *next* shim call (a separate
    // Tcl statement) can ever run - so two of these functions called
    // back-to-back from Tcl never actually race over this buffer.
    std::string &scratch()
    {
        static thread_local std::string buffer;
        return buffer;
    }

    const char *return_string(std::string value)
    {
        scratch() = std::move(value);
        return scratch().c_str();
    }

    le::ReadView read_view() { return le::ReadView(session()); }
    le::WriteView write_view() { return le::WriteView(session()); }

    // The reason the last failing command gave, for last_error_cmd.
    std::string &last_error()
    {
        static thread_local std::string message;
        return message;
    }

    // 0, or 1 with the reason kept for last_error_cmd.
    int status(const std::expected<void, std::string> &result)
    {
        if (result)
            return 0;
        last_error() = result.error();
        return 1;
    }

    // 1, keeping `reason` for last_error_cmd.
    int fail(std::string reason)
    {
        last_error() = std::move(reason);
        return 1;
    }

    // "", keeping `reason` for last_error_cmd: a command returning a
    // token failed.
    const char *fail_text(std::string reason)
    {
        last_error() = std::move(reason);
        return return_string("");
    }

    // Microns to dbu at the Technology's scale, or 1:1 without one.
    template <typename View>
    double dbu_per_um(const View &view)
    {
        const std::expected<le::Units, std::string> units = view.units();
        return units ? units->dbu_per_um() : 1.0;
    }

    int64_t to_dbu(double um, double scale) { return static_cast<int64_t>(std::llround(um * scale)); }

    // --- Friendly ids (see le_tcl_shim.hpp's "IDs" comment) ---

    // What a friendly id resolves against: the database, and the current
    // objects that scope per-parent names (a Terminal's within the current
    // Abstract, a Row's within the current Layout, a Net's from the
    // current Schematic).
    struct Lookup
    {
        const le::Root &root;
        le::AbstractId abstract;
        le::LayoutId layout;
        le::SchematicId schematic;
    };

    template <typename View>
    Lookup lookup(const View &view)
    {
        return Lookup{view.root(), view.current_abstract(), view.current_layout(), view.current_schematic()};
    }

    // The text after `prefix`, if `s` starts with it. A fixed-prefix
    // compare, so a name containing ':' still parses; an empty or null `s`
    // (no -of token given, meaning the default scope) fails it like a
    // malformed one does.
    std::optional<std::string> strip_prefix(const char *s, std::string_view prefix)
    {
        if (!s)
            return std::nullopt;
        const std::string_view sv(s);
        if (!sv.starts_with(prefix))
            return std::nullopt;
        return std::string(sv.substr(prefix.size()));
    }

    // Obstruction/TerminalPort/Shape and every other class without a
    // name: the friendly id is its packed index and generation,
    // type-prefixed. A malformed token or wrong prefix gives an invalid id,
    // which every lookup treats as "no such object".
    template <typename IdT>
    IdT resolve_numeric_friendly_id(const char *s, std::string_view prefix)
    {
        const std::optional<std::string> digits = strip_prefix(s, prefix);
        if (!digits || digits->empty())
            return IdT{};
        int64_t packed = 0;
        const auto result = std::from_chars(digits->data(), digits->data() + digits->size(), packed);
        if (result.ec != std::errc{} || result.ptr != digits->data() + digits->size())
            return IdT{};
        return unpack<IdT>(packed);
    }

    template <typename IdT>
    std::string format_numeric_friendly_id(IdT id, std::string_view prefix)
    {
        return std::string(prefix) + std::to_string(pack(id));
    }

    // design:NAME, or design:LIB/NAME when several Libraries have a Design
    // of that name - resolve_design_reference accepts either.
    constexpr std::string_view kDesignPrefix = "design:";

    std::string format_design_id(const Lookup &lk, le::DesignId id)
    {
        const le::DesignData *design = lk.root.get_design(id);
        if (!design)
            return std::string(kDesignPrefix);
        if (le::resolve_design_reference(lk.root, design->name) == id)
            return std::string(kDesignPrefix) + design->name;
        const le::LibraryData *library = lk.root.get_library(design->library);
        return std::string(kDesignPrefix) + (library ? library->name : "") + "/" + design->name;
    }

    le::DesignId resolve_design_id(const Lookup &lk, const char *s)
    {
        const std::optional<std::string> name = strip_prefix(s, kDesignPrefix);
        return name ? le::resolve_design_reference(lk.root, *name) : le::DesignId{};
    }

    // Classes named uniquely within one parent: the name is looked up in
    // the current Abstract (Terminal) or Layout (Row, Placement, ...).
#define LE_TCL_PARENT_NAMED_ID(Klass, snake, scope)                                                \
    constexpr std::string_view k##Klass##Prefix = #snake ":";                                     \
    std::string format_##snake##_id(const Lookup &lk, le::Klass##Id id)                           \
    {                                                                                              \
        const le::Klass##Data *data = lk.root.get_##snake(id);                                    \
        return std::string(k##Klass##Prefix) + (data ? data->name : "");                          \
    }                                                                                              \
    le::Klass##Id resolve_##snake##_id(const Lookup &lk, const char *s)                           \
    {                                                                                              \
        const std::optional<std::string> name = strip_prefix(s, k##Klass##Prefix);                \
        return name ? lk.root.get_##snake##_by_name(lk.scope, *name) : le::Klass##Id{};           \
    }

    LE_TCL_PARENT_NAMED_ID(Terminal, terminal, abstract)
    LE_TCL_PARENT_NAMED_ID(Row, row, layout)
    LE_TCL_PARENT_NAMED_ID(Placement, placement, layout)
    LE_TCL_PARENT_NAMED_ID(PhysicalPort, physical_port, layout)
    LE_TCL_PARENT_NAMED_ID(Route, route, layout)
    LE_TCL_PARENT_NAMED_ID(Region, region, layout)
    LE_TCL_PARENT_NAMED_ID(LayoutVia, layout_via, layout)
#undef LE_TCL_PARENT_NAMED_ID

    // Schematic objects (Port, Net, Instance, PortBus, NetBus) are named
    // from the current Schematic: their own name when it owns them, else
    // their escaped Instance path from it (`instance:u_mid/u_leaf`).

    // Each Schematic the current one reaches, with its Instance path; kept
    // until the database or the current Schematic changes.
    const std::unordered_map<le::SchematicId, std::string> &schematic_paths(const Lookup &lk)
    {
        struct Cache
        {
            const le::Root *root = nullptr;
            uint64_t version = 0;
            le::SchematicId from;
            std::unordered_map<le::SchematicId, std::string> paths;
        };
        static thread_local Cache cache;
        if (cache.root != &lk.root || cache.version != lk.root.mutation_version() || cache.from != lk.schematic)
            cache = Cache{&lk.root, lk.root.mutation_version(), lk.schematic, le::hierarchy::schematic_paths(lk.root, lk.schematic)};
        return cache.paths;
    }

    std::string schematic_token_name(const Lookup &lk, le::SchematicId owner, const std::string &name)
    {
        if (owner == lk.schematic)
            return name;
        const auto &paths = schematic_paths(lk);
        const auto it = paths.find(owner);
        return it == paths.end() ? name : it->second + le::hierarchy::escape_segment(name);
    }

    // An exact name in the current Schematic first, so a name that itself
    // contains "/" still resolves, else an exact Instance path from there.
    template <typename IdT, typename ByNameFn>
    IdT schematic_object_by_token_name(const Lookup &lk, const std::string &name, ByNameFn &&by_name)
    {
        if (const IdT id = by_name(lk.root, lk.schematic, name); id.valid())
            return id;
        const std::vector<std::string> segments = le::hierarchy::split_path(name);
        if (segments.size() < 2)
            return IdT{};
        const le::SchematicId owner = le::hierarchy::descend_exact(lk.root, lk.schematic, std::span(segments).first(segments.size() - 1));
        return owner.valid() ? by_name(lk.root, owner, segments.back()) : IdT{};
    }

#define LE_TCL_SCHEMATIC_NAMED_ID(Klass, snake)                                                                  \
    constexpr std::string_view k##Klass##Prefix = #snake ":";                                                   \
    std::string format_##snake##_id(const Lookup &lk, le::Klass##Id id)                                         \
    {                                                                                                            \
        const le::Klass##Data *data = lk.root.get_##snake(id);                                                  \
        return std::string(k##Klass##Prefix) + (data ? schematic_token_name(lk, data->schematic, data->name) : ""); \
    }                                                                                                            \
    le::Klass##Id resolve_##snake##_id(const Lookup &lk, const char *s)                                         \
    {                                                                                                            \
        const std::optional<std::string> name = strip_prefix(s, k##Klass##Prefix);                              \
        if (!name)                                                                                               \
            return le::Klass##Id{};                                                                              \
        return schematic_object_by_token_name<le::Klass##Id>(lk, *name, [](const le::Root &root, le::SchematicId schematic, const std::string &n) \
                                                             { return root.get_##snake##_by_name(schematic, n); }); \
    }

    LE_TCL_SCHEMATIC_NAMED_ID(Port, port)
    LE_TCL_SCHEMATIC_NAMED_ID(Net, net)
    LE_TCL_SCHEMATIC_NAMED_ID(Instance, instance)
    LE_TCL_SCHEMATIC_NAMED_ID(PortBus, port_bus)
    LE_TCL_SCHEMATIC_NAMED_ID(NetBus, net_bus)
#undef LE_TCL_SCHEMATIC_NAMED_ID

    // Tcl is "everything is a string" (`expr {$v + 1}` works on a numeric
    // string), so every property value crosses as text.
    std::string format_property_value(const le::PropertyValue &prop)
    {
        switch (prop.type)
        {
        case le::PropertyValue::Type::STRING:
            return prop.string_value;
        case le::PropertyValue::Type::INT:
            return std::to_string(prop.int_value);
        case le::PropertyValue::Type::DOUBLE:
            return std::to_string(prop.double_value);
        }
        return "";
    }

    // `failure`, keeping `reason` for last_error_cmd.
    template <typename T>
    T fail(std::string reason, T failure)
    {
        last_error() = std::move(reason);
        return failure;
    }

    // The Technology's dbu per micron; nullopt before one is read.
    template <typename View>
    std::optional<double> units_dbu_per_um(const View &view)
    {
        const std::expected<le::Units, std::string> units = view.units();
        return units ? std::optional<double>(units->dbu_per_um()) : std::nullopt;
    }

    // Square microns to dbu squared.
    int64_t to_dbu2(double um2, double scale) { return static_cast<int64_t>(std::llround(um2 * scale * scale)); }

    // `ids` as a space-separated list of friendly ids. Every token is a
    // valid Tcl list element: no token contains whitespace.
    template <typename Ids, typename Format>
    std::string join_ids(const Ids &ids, Format &&format)
    {
        std::string out;
        for (const auto &id : ids)
        {
            if (!out.empty())
                out += ' ';
            out += format(id);
        }
        return out;
    }

    // The last get_<type> search's results as friendly ids, read back one
    // at a time by get_<type>_at.
    std::vector<std::string> &search_results()
    {
        static thread_local std::vector<std::string> results;
        return results;
    }

    template <typename Ids, typename Format>
    int keep_search_results(const Ids &ids, Format &&format)
    {
        std::vector<std::string> &results = search_results();
        results.clear();
        results.reserve(ids.size());
        for (const auto &id : ids)
            results.push_back(format(id));
        return static_cast<int>(results.size());
    }

    const char *search_result_at(int index)
    {
        const std::vector<std::string> &results = search_results();
        if (index < 0 || static_cast<size_t>(index) >= results.size())
            return return_string("");
        return results[static_cast<size_t>(index)].c_str();
    }

    // `id`'s property table, kept while Tcl reads it row by row (count,
    // then each name and value) and the database doesn't change.
    const std::vector<le::PropertyValue> &properties_of(const le::ReadView &view, const le::ObjectId &id)
    {
        struct Cache
        {
            const le::Root *root = nullptr;
            uint64_t version = 0;
            std::optional<le::ObjectId> id;
            std::vector<le::PropertyValue> rows;
        };
        static thread_local Cache cache;
        if (cache.root != &view.root() || cache.version != view.root().mutation_version() || cache.id != id)
            cache = Cache{&view.root(), view.root().mutation_version(), id, view.properties(id)};
        return cache.rows;
    }

    const char *property_name_at(const std::vector<le::PropertyValue> &rows, int index)
    {
        if (index < 0 || static_cast<size_t>(index) >= rows.size())
            return return_string("");
        return return_string(rows[static_cast<size_t>(index)].name);
    }

    const char *property_value_at(const std::vector<le::PropertyValue> &rows, int index)
    {
        if (index < 0 || static_cast<size_t>(index) >= rows.size())
            return return_string("");
        return return_string(format_property_value(rows[static_cast<size_t>(index)]));
    }

    // Whether the last *_property_path call had a path that didn't parse
    // or named no field (its reason in last_error_cmd), as opposed to one
    // that simply led to no value.
    bool &property_path_failed_flag()
    {
        static thread_local bool failed = false;
        return failed;
    }

    const char *property_at_path(const le::ReadView &view, const le::ObjectId &id, const char *path)
    {
        const std::expected<std::optional<le::PropertyValue>, std::string> value = view.property(id, path ? path : "");
        property_path_failed_flag() = !value;
        if (!value)
            return fail_text(value.error());
        return return_string(*value ? format_property_value(**value) : std::string());
    }
}

LeHandle *le::ext::tcl_session()
{
    return session();
}

// The generated commands for every readable class: friendly ids,
// property tables, child lists, current_<type>, get_<type> and
// create_/update_/delete_<type>. They use the helpers above.
#include "generated/tcl/le_tcl_shim_generated.inc"

namespace
{
    // Shared by both "open a view" commands: the same padding the GUI's
    // open actions fit with, so a script lands on a framed view.
    constexpr int kOpenDesignFitPaddingPx = 10;
}

const char *last_error_cmd()
{
    return last_error().c_str();
}

int read_lef(const char *path, const char *library_name)
{
    return status(write_view().read_lef(path ? path : "", library_name ? library_name : ""));
}

int read_def(const char *path, const char *library_name)
{
    return status(write_view().read_def(path ? path : "", library_name ? library_name : ""));
}

// paths is a space-separated list of one or more filenames - same
// plain-word-split convention write_lef_cmd's own abstract_tokens uses
// above (no filename this project's own test fixtures/workflow uses
// contains whitespace); read_verilog (le_tcl_procs.tcl) builds this via
// [join $positional] on the way in. Passing every file through one
// read_verilog call (rather than one call per file) matters for
// SVReader::read_netlist specifically - see verilog_stub_writer.hpp's
// own top-of-file comment - only files elaborated together in the same
// call share one slang::ast::Compilation, which is what lets a
// generated stub file (write_verilog_stubs) actually resolve a real
// netlist's own leaf-cell instantiations.
int read_verilog_cmd(const char *paths, int is_netlist, const char *library_name)
{
    std::vector<std::string> files;
    std::istringstream stream(paths ? paths : "");
    std::string token;
    while (stream >> token)
        files.push_back(token);
    return status(write_view().read_verilog(files, is_netlist != 0, library_name ? library_name : ""));
}

int write_verilog_stubs_cmd(const char *path, const char *library_token)
{
    le::WriteView view = write_view();
    const le::LibraryId library = resolve_library_id(lookup(view), library_token);
    return status(view.write_verilog_stubs(path ? path : "", library));
}

int link_unresolved_instances_cmd()
{
    const std::expected<size_t, std::string> resolved = write_view().link_unresolved_instances();
    return resolved ? static_cast<int>(*resolved) : fail(resolved.error(), -1);
}

// Hierarchical path searches: the count found (-1 for a bad -filter),
// each read back by get_<type>_at like a flat search.

int get_instances_by_path_cmd(const char *of_schematic, const char *path, const char *filter_expression)
{
    const le::ReadView view = read_view();
    const Lookup lk = lookup(view);
    const auto found = view.get_instances_by_path(resolve_schematic_id(lk, of_schematic), path ? path : "", filter_expression ? filter_expression : "");
    if (!found)
        return fail("get_instances: invalid -filter expression: " + found.error(), -1);
    return keep_search_results(*found, [&](le::InstanceId id) { return format_instance_id(lk, id); });
}

int get_nets_by_path_cmd(const char *of_schematic, const char *path, const char *filter_expression)
{
    const le::ReadView view = read_view();
    const Lookup lk = lookup(view);
    const auto found = view.get_nets_by_path(resolve_schematic_id(lk, of_schematic), path ? path : "", filter_expression ? filter_expression : "");
    if (!found)
        return fail("get_nets: invalid -filter expression: " + found.error(), -1);
    return keep_search_results(*found, [&](le::NetId id) { return format_net_id(lk, id); });
}

int get_ports_by_path_cmd(const char *of_schematic, const char *path, const char *filter_expression)
{
    const le::ReadView view = read_view();
    const Lookup lk = lookup(view);
    const auto found = view.get_ports_by_path(resolve_schematic_id(lk, of_schematic), path ? path : "", filter_expression ? filter_expression : "");
    if (!found)
        return fail("get_ports: invalid -filter expression: " + found.error(), -1);
    return keep_search_results(*found, [&](le::PortId id) { return format_port_id(lk, id); });
}

// Netlist edits that keep the Layout linked: le_tcl_procs.tcl's
// delete_net, and update_net/update_instance when they rename, call these
// instead of the generated commands.

int delete_net_cascade_cmd(const char *id)
{
    le::WriteView view = write_view();
    const std::expected<void, std::string> deleted = view.delete_net_cascade(resolve_net_id(lookup(view), id));
    return deleted ? 0 : fail("delete_net: " + deleted.error());
}

const char *rename_net_cmd(const char *id, const char *new_name)
{
    le::WriteView view = write_view();
    const le::NetId net = resolve_net_id(lookup(view), id);
    const std::expected<void, std::string> renamed = view.rename_net(net, new_name ? new_name : "");
    if (!renamed)
        return fail_text("update_net: " + renamed.error());
    return return_string(format_net_id(lookup(view), net));
}

const char *rename_instance_cmd(const char *id, const char *new_name)
{
    le::WriteView view = write_view();
    const le::InstanceId instance = resolve_instance_id(lookup(view), id);
    const std::expected<void, std::string> renamed = view.rename_instance(instance, new_name ? new_name : "");
    if (!renamed)
        return fail_text("update_instance: " + renamed.error());
    return return_string(format_instance_id(lookup(view), instance));
}

int design_count()
{
    return static_cast<int>(read_view().root().get_design_size());
}

const char *design_name(int index)
{
    const le::ReadView view = read_view();
    const auto ids = view.root().get_design_ids();
    if (index < 0 || static_cast<size_t>(index) >= ids.size())
        return nullptr;
    const le::DesignData *design = view.root().get_design(ids[static_cast<size_t>(index)]);
    return design ? return_string(design->name) : nullptr;
}

int property_path_failed()
{
    return property_path_failed_flag() ? 1 : 0;
}

void set_viewport_size_cmd(int width_px, int height_px)
{
    write_view().set_viewport_size(width_px, height_px);
}

int viewport_width()
{
    return read_view().view().width_px;
}

int viewport_height()
{
    return read_view().view().height_px;
}

long long design_abstract_id(int design_index)
{
    const le::ReadView view = read_view();
    const auto libraries = view.root().get_library_ids();
    if (libraries.empty() || design_index < 0)
        return pack(le::AbstractId{});
    const auto &designs = view.root().get_library_designs(libraries.front());
    if (static_cast<size_t>(design_index) >= designs.size())
        return pack(le::AbstractId{});
    return pack(view.root().get_design_abstract(designs[static_cast<size_t>(design_index)]));
}

long long design_by_name(const char *name)
{
    const le::ReadView view = read_view();
    return pack(le::resolve_design_reference(view.root(), name ? name : ""));
}

const char *technology_id()
{
    const le::ReadView view = read_view();
    const auto technologies = view.root().get_technology_ids();
    if (technologies.empty())
        return return_string("");
    return return_string(format_technology_id(lookup(view), technologies.front()));
}

int set_current_design_abstract_cmd(long long design_id)
{
    le::WriteView view = write_view();
    const int result = status(view.open_abstract(unpack<le::DesignId>(design_id)));
    // A script-driven open lands framed, as the GUI's does.
    if (result == 0)
        view.fit_view(kOpenDesignFitPaddingPx);
    return result;
}

int set_current_design_layout_cmd(long long design_id)
{
    le::WriteView view = write_view();
    const int result = status(view.open_layout(unpack<le::DesignId>(design_id)));
    if (result == 0)
        view.fit_view(kOpenDesignFitPaddingPx);
    return result;
}

void zoom_cmd(double factor)
{
    le::WriteView view = write_view();
    const le::ViewState state = view.view();
    view.zoom(factor, state.width_px / 2, state.height_px / 2);
}

void zoom_area_cmd(double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um, int padding_px)
{
    le::WriteView view = write_view();
    const double scale = dbu_per_um(view);
    view.fit_rect(le::Rect{.ll = {to_dbu(ll_x_um, scale), to_dbu(ll_y_um, scale)}, .ur = {to_dbu(ur_x_um, scale), to_dbu(ur_y_um, scale)}}, padding_px);
}

void set_layer_visible_cmd(const char *layer_name, bool visible)
{
    if (layer_name)
        write_view().set_layer_visible(layer_name, visible);
}

int dump_png_cmd(const char *path)
{
    const le::RenderedFrame frame = le::Renderer(session()).render();
    if (frame.empty() || frame.width <= 0 || frame.height <= 0)
        return 1;

    BLImage image(frame.width, frame.height, BL_FORMAT_PRGB32);
    if (image.is_empty())
        return 1;

    BLImageData image_data;
    if (image.get_data(&image_data) != BL_SUCCESS)
        return 1;

    // The frame is RGBA in byte order; BL_FORMAT_PRGB32 is premultiplied
    // BGRA in memory on this little-endian target, so R and B swap back.
    auto *dst_base = static_cast<uint8_t *>(image_data.pixel_data);
    for (int y = 0; y < frame.height; ++y)
    {
        const uint8_t *src_row = frame.data + static_cast<std::ptrdiff_t>(y) * static_cast<std::ptrdiff_t>(frame.row_bytes);
        uint8_t *dst_row = dst_base + static_cast<std::ptrdiff_t>(y) * image_data.stride;
        for (int x = 0; x < frame.width; ++x)
        {
            const uint8_t *src_px = src_row + static_cast<std::ptrdiff_t>(x) * 4;
            uint8_t *dst_px = dst_row + static_cast<std::ptrdiff_t>(x) * 4;
            dst_px[0] = src_px[2]; // B <- R
            dst_px[1] = src_px[1]; // G <- G
            dst_px[2] = src_px[0]; // R <- B
            dst_px[3] = src_px[3]; // A <- A
        }
    }

    return image.write_to_file(path) == BL_SUCCESS ? 0 : 1;
}

bool get_layer_visible_cmd(const char *layer_name)
{
    return !layer_name || read_view().layer_visible(layer_name);
}

// abstract_token/layout_token empty means "use write_lef/write_def's
// own current-Abstract/current-Layout fallback" - resolve_abstract_id/
// resolve_layout_id (generated, this file's own anonymous namespace)
// would otherwise happily "resolve" an empty string to the same invalid
// sentinel anyway, but skipping the call entirely when there's nothing to
// resolve keeps this from looking like it's doing real lookup work for a
// deliberately-omitted flag.
// abstract_tokens is a space-separated list of zero or more friendly
// Abstract ids (e.g. "abstract:1 abstract:5 abstract:9") - a plain word
// split, not a real Tcl list parse, is safe here since no friendly id
// this codebase generates ever contains whitespace (see le_tcl_shim.hpp's
// own "IDs" comment); write_lef (le_tcl_procs.tcl) builds this from its
// own -abstracts flag's Tcl list value via [join ...] on the way in.
int write_lef_cmd(const char *path, const char *abstract_tokens, const char *library_token, int32_t layer_write_mode)
{
    le::WriteView view = write_view();
    const Lookup lk = lookup(view);
    std::vector<le::AbstractId> abstracts;
    std::istringstream stream(abstract_tokens ? abstract_tokens : "");
    std::string token;
    while (stream >> token)
        abstracts.push_back(resolve_abstract_id(lk, token.c_str()));
    const le::LibraryId library = resolve_library_id(lk, library_token);
    const le::LefLayerWriteMode layers = in_range(layer_write_mode, le::LefLayerWriteMode::NONE, le::LefLayerWriteMode::TECHNOLOGY_ONLY)
                                             ? static_cast<le::LefLayerWriteMode>(layer_write_mode)
                                             : le::LefLayerWriteMode::NONE;
    return status(view.write_lef(path ? path : "", abstracts, library, layers));
}

int write_def_cmd(const char *path, const char *layout_token)
{
    le::WriteView view = write_view();
    const le::LayoutId layout = resolve_layout_id(lookup(view), layout_token);
    return status(view.write_def(path ? path : "", layout));
}

int write_db_cmd(const char *path, int with_session) { return status(write_view().write_db(path ? path : "", with_session != 0)); }

int read_db_cmd(const char *path, int with_session) { return status(write_view().read_db(path ? path : "", with_session != 0)); }

const char *db_info_cmd(const char *path)
{
    const std::expected<std::string, std::string> info = le::db_info(path ? path : "");
    return return_string(info ? *info : "error: " + info.error());
}
const char *migrate_db_cmd(const char *in_path, const char *out_path)
{
    const std::expected<std::string, std::string> report = le::migrate_db(in_path ? in_path : "", out_path ? out_path : "");
    return return_string(report ? *report : "error: " + report.error());
}

// get_selection/select. Only Shape/Wire/Row/Placement/Region friendly ids are
// meaningful here (the kinds WriteView::select takes); literal prefix strings rather than the
// generated kShapePrefix/etc constants, since those live in the generated
// file's own scope and duplicating a plain "shape:"/"wire:"/"row:"/... literal
// here is simpler than reaching for them.
int selection_count_cmd()
{
    return static_cast<int>(read_view().selection().size());
}

const char *get_selection_at_cmd(int index)
{
    const le::ReadView view = read_view();
    const std::span<const le::SelectedObject> selection = view.selection();
    if (index < 0 || static_cast<size_t>(index) >= selection.size())
        return return_string(std::string{});
    // A piece names its Shape or Wire.
    const Lookup lk = lookup(view);
    return return_string(std::visit([&](const auto &entry) -> std::string
                                    {
                                        using T = std::decay_t<decltype(entry)>;
                                        if constexpr (std::is_same_v<T, le::ShapePiece>)
                                            return entry.wire_id.valid() ? format_wire_id(lk, entry.wire_id) : format_shape_id(lk, entry.shape_id);
                                        else if constexpr (std::is_same_v<T, le::RowId>)
                                            return format_row_id(lk, entry);
                                        else if constexpr (std::is_same_v<T, le::PlacementId>)
                                            return format_placement_id(lk, entry);
                                        else
                                            return format_region_id(lk, entry); },
                                    selection[static_cast<size_t>(index)]));
}

int select_cmd(const char *token)
{
    if (!token)
        return fail("no token");
    const std::string_view sv(token);
    le::WriteView view = write_view();
    const Lookup lk = lookup(view);
    le::SelectableObject object;
    if (sv.starts_with("shape:"))
        object = resolve_shape_id(lk, token);
    else if (sv.starts_with("wire:"))
        object = resolve_wire_id(lk, token);
    else if (sv.starts_with("row:"))
        object = resolve_row_id(lk, token);
    else if (sv.starts_with("placement:"))
        object = resolve_placement_id(lk, token);
    else if (sv.starts_with("region:"))
        object = resolve_region_id(lk, token);
    else
        return 2; // an unrecognized prefix: select names the token itself
    return status(view.select(object));
}

void set_layer_selectable_cmd(const char *layer_name, bool selectable)
{
    if (layer_name)
        write_view().set_layer_selectable(layer_name, selectable);
}

bool get_layer_selectable_cmd(const char *layer_name)
{
    return !layer_name || read_view().layer_selectable(layer_name);
}

void set_purpose_visible_cmd(int32_t purpose, bool visible)
{
    write_view().set_purpose_visible(static_cast<le::ViewLayerPurpose>(purpose), visible);
}

bool get_purpose_visible_cmd(int32_t purpose)
{
    return read_view().purpose_visible(static_cast<le::ViewLayerPurpose>(purpose));
}

void set_purpose_selectable_cmd(int32_t purpose, bool selectable)
{
    write_view().set_purpose_selectable(static_cast<le::ViewLayerPurpose>(purpose), selectable);
}

bool get_purpose_selectable_cmd(int32_t purpose)
{
    return read_view().purpose_selectable(static_cast<le::ViewLayerPurpose>(purpose));
}

int purpose_kind_count_cmd()
{
    return static_cast<int>(le::kViewLayerPurposeCount);
}

const char *purpose_name_cmd(int32_t purpose)
{
    if (purpose < 0 || static_cast<size_t>(purpose) >= le::kViewLayerPurposeCount)
        return "";
    return le::kViewLayerPurposes[static_cast<size_t>(purpose)].label.data(); // a literal: null-terminated
}

bool purpose_visible_by_default_cmd(int32_t purpose)
{
    return purpose >= 0 && static_cast<size_t>(purpose) < le::kViewLayerPurposeCount && le::kViewLayerPurposes[static_cast<size_t>(purpose)].visible_by_default;
}

void set_object_filter_visible_cmd(int32_t filter, const char *value, bool visible)
{
    if (value)
        write_view().set_object_filter_visible(static_cast<le::ObjectFilter>(filter), value, visible);
}

bool get_object_filter_visible_cmd(int32_t filter, const char *value)
{
    return !value || read_view().object_filter_visible(static_cast<le::ObjectFilter>(filter), value);
}

void set_object_filter_selectable_cmd(int32_t filter, const char *value, bool selectable)
{
    if (value)
        write_view().set_object_filter_selectable(static_cast<le::ObjectFilter>(filter), value, selectable);
}

bool get_object_filter_selectable_cmd(int32_t filter, const char *value)
{
    return !value || read_view().object_filter_selectable(static_cast<le::ObjectFilter>(filter), value);
}

void set_mode_cmd(int32_t mode)
{
    if (in_range(mode, le::InteractionMode::SELECT, le::InteractionMode::RULER))
        write_view().set_mode(static_cast<le::InteractionMode>(mode));
}

int32_t get_mode_cmd()
{
    return static_cast<int32_t>(read_view().mode());
}

void clear_rulers_cmd()
{
    write_view().clear_rulers();
}

void deselect_all_cmd()
{
    write_view().clear_selection();
}

void arm_move_cmd()
{
    write_view().arm_move();
}

int selected_locked_placement_count_cmd()
{
    return static_cast<int>(read_view().selected_locked_placement_count());
}

bool is_move_anchored_cmd()
{
    return read_view().move_anchored();
}

int delete_selected_pieces_cmd()
{
    return static_cast<int>(write_view().delete_selected_pieces());
}

void set_placement_snap_mode_cmd(int mode)
{
    if (in_range(mode, le::PlacementSnapMode::NONE, le::PlacementSnapMode::MANUFACTURING_GRID))
        write_view().set_placement_snap_mode(static_cast<le::PlacementSnapMode>(mode));
}

int get_placement_snap_mode_cmd()
{
    return static_cast<int>(read_view().placement_snap_mode());
}

int set_view_flip_cmd(int flip)
{
    if (!in_range(flip, le::ViewFlip::NONE, le::ViewFlip::VERTICAL))
        return 1;
    write_view().set_view_flip(static_cast<le::ViewFlip>(flip));
    return 0;
}

int get_view_flip_cmd()
{
    return static_cast<int>(read_view().view().flip);
}

bool is_placement_snap_mode_available_cmd(int mode)
{
    return in_range(mode, le::PlacementSnapMode::NONE, le::PlacementSnapMode::MANUFACTURING_GRID) &&
           read_view().placement_snap_mode_available(static_cast<le::PlacementSnapMode>(mode));
}

int apply_placement_orientation_op_cmd(int op)
{
    if (!in_range(op, le::OrientationOp::ROTATE_CCW, le::OrientationOp::FLIP_VERTICAL))
        return -1;
    const std::expected<void, le::OrientationOpError> applied = write_view().apply_placement_orientation_op(static_cast<le::OrientationOp>(op));
    if (applied)
        return 0;
    switch (applied.error())
    {
    case le::OrientationOpError::NOTHING_SELECTED:
        return 1;
    case le::OrientationOpError::NOT_AVAILABLE:
        return 2;
    case le::OrientationOpError::LOCKED:
        return 3;
    }
    return -1;
}

void arm_resize_cmd()
{
    write_view().arm_resize();
}

void set_shape_snap_mode_cmd(int kind, int mode)
{
    if (valid_piece_kind(kind) && valid_shape_snap_mode(mode))
        write_view().set_shape_snap_mode(static_cast<le::PieceKind>(kind), static_cast<le::ShapeSnapMode>(mode));
}

int get_shape_snap_mode_cmd(int kind)
{
    if (!valid_piece_kind(kind))
        return static_cast<int>(le::ShapeSnapMode::USER_GRID);
    return static_cast<int>(read_view().shape_snap_mode(static_cast<le::PieceKind>(kind)));
}

bool is_shape_snap_mode_available_cmd(int kind, int mode)
{
    return valid_piece_kind(kind) && valid_shape_snap_mode(mode) &&
           read_view().shape_snap_mode_available(static_cast<le::PieceKind>(kind), static_cast<le::ShapeSnapMode>(mode));
}

void request_show_gui_cmd()
{
    le::SessionRequests(session()).request_show_gui();
}

void request_close_gui_cmd()
{
    le::SessionRequests(session()).request_close_gui();
}

int has_unsaved_database_changes_cmd()
{
    return read_view().has_unsaved_database() ? 1 : 0;
}

int has_unsaved_settings_cmd()
{
    return read_view().has_unsaved_settings() ? 1 : 0;
}

void set_session_handle(long long handle_address)
{
    injected_handle() = reinterpret_cast<LeHandle *>(static_cast<uintptr_t>(handle_address));
}

// --- Terminal/TerminalPort/Obstruction CRUD is fully generated now
// (create_X_cmd/update_X_cmd/delete_X_cmd - le_tcl_shim_generated.inc) ---

// --- Shape (create_shape_cmd/update_shape_cmd/delete_shape_cmd are
// generated - create_shape_cmd unifies the former
// create_terminal_port_shape_cmd/create_obstruction_shape_cmd split into
// one function taking both parent tokens, exactly one of which must
// resolve) ---

namespace
{
    // The geometry of the Shape `id` names, read under one shared lock,
    // and its scale in microns (nullopt before a Technology is read).
    struct ShapeRead
    {
        le::ReadView view = read_view();
        const le::ShapeData *shape = nullptr;
        std::optional<double> scale;

        explicit ShapeRead(const char *id)
            : shape(view.root().get_shape(resolve_shape_id(lookup(view), id))), scale(units_dbu_per_um(view)) {}

        // `points[index]` in microns, "x y"; "0 0" if out of range or unscaled.
        std::string point_at(const std::vector<le::Point> &points, int index) const
        {
            double x = 0;
            double y = 0;
            if (scale && index >= 0 && static_cast<size_t>(index) < points.size())
            {
                x = static_cast<double>(points[static_cast<size_t>(index)].x) / *scale;
                y = static_cast<double>(points[static_cast<size_t>(index)].y) / *scale;
            }
            std::ostringstream out;
            out << x << ' ' << y;
            return out.str();
        }

        const le::Polygon *polygon(int index) const
        {
            return shape && index >= 0 && static_cast<size_t>(index) < shape->polygons.size() ? &shape->polygons[static_cast<size_t>(index)] : nullptr;
        }

        const le::Path *path(int index) const
        {
            return shape && index >= 0 && static_cast<size_t>(index) < shape->paths.size() ? &shape->paths[static_cast<size_t>(index)] : nullptr;
        }
    };

    // The Shape or Wire a "shape:"/"wire:" token names.
    le::GeometryId resolve_geometry_id(const Lookup &lk, const char *id)
    {
        if (std::string_view(id ? id : "").starts_with("wire:"))
            return le::GeometryId(resolve_wire_id(lk, id));
        return le::GeometryId(resolve_shape_id(lk, id));
    }

    int remove_piece(const char *id, le::PieceKind kind, int index, const char *what)
    {
        if (index < 0)
            return fail(std::string("a ") + what + " index can't be negative");
        le::WriteView view = write_view();
        return status(view.remove_shape_piece(resolve_geometry_id(lookup(view), id), kind, static_cast<size_t>(index)));
    }
}

const char *shape_layer_name(const char *id)
{
    const ShapeRead read(id);
    const le::LayerData *layer = read.shape ? read.view.root().get_layer(read.shape->layer) : nullptr;
    return layer ? return_string(layer->name) : nullptr;
}

int shape_rect_count(const char *id)
{
    const ShapeRead read(id);
    return read.shape ? static_cast<int>(read.shape->rects.size()) : 0;
}

const char *shape_rect_at(const char *id, int index)
{
    // Brace-nested {{ll_x ll_y} {ur_x ur_y}}, as create_shape's -rects
    // flag and get_properties' rects take and show them.
    const ShapeRead read(id);
    std::vector<le::Point> corners;
    if (read.shape && index >= 0 && static_cast<size_t>(index) < read.shape->rects.size())
        corners = {read.shape->rects[static_cast<size_t>(index)].ll, read.shape->rects[static_cast<size_t>(index)].ur};
    return return_string('{' + read.point_at(corners, 0) + "} {" + read.point_at(corners, 1) + '}');
}

int remove_shape_rect(const char *id, int index)
{
    return remove_piece(id, le::PieceKind::RECT, index, "rect");
}

int shape_polygon_count(const char *id)
{
    const ShapeRead read(id);
    return read.shape ? static_cast<int>(read.shape->polygons.size()) : 0;
}

int shape_polygon_point_count(const char *id, int polygon_index)
{
    const ShapeRead read(id);
    const le::Polygon *polygon = read.polygon(polygon_index);
    return polygon ? static_cast<int>(polygon->points.size()) : 0;
}

const char *shape_polygon_point_at(const char *id, int polygon_index, int point_index)
{
    const ShapeRead read(id);
    const le::Polygon *polygon = read.polygon(polygon_index);
    return return_string(read.point_at(polygon ? polygon->points : std::vector<le::Point>{}, point_index));
}

int remove_shape_polygon(const char *id, int polygon_index)
{
    return remove_piece(id, le::PieceKind::POLYGON, polygon_index, "polygon");
}

int shape_path_count(const char *id)
{
    const ShapeRead read(id);
    return read.shape ? static_cast<int>(read.shape->paths.size()) : 0;
}

double shape_path_width_um(const char *id, int path_index)
{
    const ShapeRead read(id);
    const le::Path *path = read.path(path_index);
    return path && read.scale ? static_cast<double>(path->width) / *read.scale : 0.0;
}

int shape_path_point_count(const char *id, int path_index)
{
    const ShapeRead read(id);
    const le::Path *path = read.path(path_index);
    return path ? static_cast<int>(path->polygon.points.size()) : 0;
}

const char *shape_path_point_at(const char *id, int path_index, int point_index)
{
    const ShapeRead read(id);
    const le::Path *path = read.path(path_index);
    return return_string(read.point_at(path ? path->polygon.points : std::vector<le::Point>{}, point_index));
}

int remove_shape_path(const char *id, int path_index)
{
    return remove_piece(id, le::PieceKind::PATH, path_index, "path");
}

// --- shape_* operations ---
//
// Shape token lists cross as one space-separated string (le_tcl_procs.tcl
// builds it with [join ...]) - the same plain word split write_lef_cmd's
// own abstract_tokens uses. An unknown shape token resolves to the invalid
// id, which the shape op then reports as unknown; an unknown -layer/
// -parent token can't be told apart from an omitted one that way, so it's
// caught here instead and returned as its own status (kShapeOpBadLayer/
// kShapeOpBadParent) for the Tcl proc to report. Any other failure is -1,
// its reason kept for last_error_cmd.

namespace
{
    constexpr int kShapeOpFailed = -1;
    constexpr int kShapeOpBadLayer = -2;
    constexpr int kShapeOpBadParent = -3;

    // shape: and wire: tokens.
    std::vector<le::GeometryId> resolve_shape_tokens(const Lookup &lk, const char *tokens)
    {
        std::vector<le::GeometryId> shapes;
        std::istringstream stream(tokens ? tokens : "");
        std::string token;
        while (stream >> token)
            shapes.push_back(resolve_geometry_id(lk, token.c_str()));
        return shapes;
    }

    bool token_given(const char *token)
    {
        return token && token[0];
    }

    bool is_debug_layer(const char *token)
    {
        std::string lower(token);
        for (char &c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return lower == "debug";
    }

    // A -layer value: a real layer token, or `debug` (any case) for the
    // layer-less DEBUG purpose; nothing for no token. nullopt = a layer
    // token was given but doesn't resolve.
    std::optional<std::optional<le::shape_ops::LayerOrPurpose>> resolve_optional_layer(const Lookup &lk, const char *token)
    {
        if (!token_given(token))
            return std::optional<le::shape_ops::LayerOrPurpose>{};
        if (is_debug_layer(token))
            return std::optional(le::shape_ops::LayerOrPurpose{.purpose = le::ShapePurpose::DEBUG});
        const le::LayerId id = resolve_layer_id(lk, token);
        if (!id.valid())
            return std::nullopt;
        return std::optional(le::shape_ops::LayerOrPurpose{.layer = id});
    }

    // nothing for no token; nullopt = a parent token was given but isn't
    // a supported, existing kind.
    std::optional<std::optional<le::shape_ops::ShapeParent>> resolve_optional_parent(const Lookup &lk, const char *token)
    {
        if (!token_given(token))
            return std::optional<le::shape_ops::ShapeParent>{};
        const std::string_view sv(token);
        const auto valid = [](auto id) -> std::optional<std::optional<le::shape_ops::ShapeParent>>
        {
            if (!id.valid())
                return std::nullopt;
            return std::optional<le::shape_ops::ShapeParent>(id);
        };
        if (sv.starts_with("abstract:"))
            return valid(resolve_abstract_id(lk, token));
        if (sv.starts_with("layout:"))
            return valid(resolve_layout_id(lk, token));
        if (sv.starts_with("obstruction:"))
            return valid(resolve_obstruction_id(lk, token));
        if (sv.starts_with("terminal_port:"))
            return valid(resolve_terminal_port_id(lk, token));
        if (sv.starts_with("route:"))
            return valid(resolve_route_id(lk, token));
        if (sv.starts_with("blockage:"))
            return valid(resolve_blockage_id(lk, token));
        if (sv.starts_with("physical_port_segment:"))
            return valid(resolve_physical_port_segment_id(lk, token));
        return std::nullopt;
    }

    // The last shape op's new Shapes, for shape_op_results_cmd.
    std::vector<le::ShapeId> &shape_op_results()
    {
        static thread_local std::vector<le::ShapeId> results;
        return results;
    }

    // Shared by every creating shape_*_cmd: resolves -layer/-parent, then
    // runs `call(view, lk, layer, parent)` - the matching WriteView shape
    // op. How many Shapes it made, or a negative status.
    template <typename Call>
    int run_shape_op_cmd(const char *layer_token, const char *parent_token, Call &&call)
    {
        le::WriteView view = write_view();
        const Lookup lk = lookup(view);
        const auto layer = resolve_optional_layer(lk, layer_token);
        if (!layer)
            return kShapeOpBadLayer;
        const auto parent = resolve_optional_parent(lk, parent_token);
        if (!parent)
            return kShapeOpBadParent;
        const le::ShapeOpResult result = call(view, lk, *layer, *parent);
        if (!result)
        {
            fail(result.error());
            return kShapeOpFailed;
        }
        shape_op_results() = *result;
        return static_cast<int>(result->size());
    }
}

int shape_copy_cmd(const char *shape_tokens, const char *layer_token, const char *parent_token)
{
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const Lookup &lk, const auto &layer, const auto &parent)
                            {
                                const std::vector<le::GeometryId> shapes = resolve_shape_tokens(lk, shape_tokens);
                                return layer ? view.shape_copy(shapes, *layer, parent) : le::ShapeOpResult(std::unexpected(std::string("-layer is required"))); });
}

int shape_boolean_cmd(const char *shape_tokens_a, const char *shape_tokens_b, int op, const char *layer_token, const char *parent_token)
{
    const le::BooleanOp boolean_op = in_range(op, le::BooleanOp::Or, le::BooleanOp::Not) ? static_cast<le::BooleanOp>(op) : le::BooleanOp::Or;
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const Lookup &lk, const auto &layer, const auto &parent)
                            {
                                const std::vector<le::GeometryId> a = resolve_shape_tokens(lk, shape_tokens_a);
                                const std::vector<le::GeometryId> b = resolve_shape_tokens(lk, shape_tokens_b);
                                return view.shape_boolean(a, b, boolean_op, layer, parent); });
}

int shape_to_polygon_cmd(const char *shape_tokens, const char *layer_token, const char *parent_token)
{
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const Lookup &lk, const auto &layer, const auto &parent)
                            {
                                const std::vector<le::GeometryId> shapes = resolve_shape_tokens(lk, shape_tokens);
                                return view.shape_to_polygons(shapes, layer, parent); });
}

int shape_to_rects_cmd(const char *shape_tokens, int vertical, const char *layer_token, const char *parent_token)
{
    const le::FractureDirection direction = vertical ? le::FractureDirection::Vertical : le::FractureDirection::Horizontal;
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const Lookup &lk, const auto &layer, const auto &parent)
                            {
                                const std::vector<le::GeometryId> shapes = resolve_shape_tokens(lk, shape_tokens);
                                return view.shape_to_rects(shapes, direction, layer, parent); });
}

int shape_size_cmd(const char *shape_tokens, double dx_um, double dy_um, const char *layer_token, const char *parent_token)
{
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const Lookup &lk, const auto &layer, const auto &parent)
                            {
                                const std::vector<le::GeometryId> shapes = resolve_shape_tokens(lk, shape_tokens);
                                const double scale = dbu_per_um(view);
                                return view.shape_size(shapes, to_dbu(dx_um, scale), to_dbu(dy_um, scale), layer, parent); });
}

int shape_path_cmd(const char *shape_tokens, double width_um, const char *layer_token, const char *parent_token)
{
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const Lookup &lk, const auto &layer, const auto &parent)
                            {
                                const std::vector<le::GeometryId> shapes = resolve_shape_tokens(lk, shape_tokens);
                                return view.shape_outline_paths(shapes, to_dbu(width_um, dbu_per_um(view)), layer, parent); });
}

int shape_change_layer_cmd(const char *shape_tokens, const char *layer_token)
{
    le::WriteView view = write_view();
    const Lookup lk = lookup(view);
    const auto layer = resolve_optional_layer(lk, layer_token);
    if (!layer || !*layer)
        return kShapeOpBadLayer;
    const std::vector<le::GeometryId> shapes = resolve_shape_tokens(lk, shape_tokens);
    if (const std::expected<void, std::string> changed = view.shape_change_layer(shapes, **layer); !changed)
    {
        fail(changed.error());
        return kShapeOpFailed;
    }
    return static_cast<int>(shapes.size());
}

const char *shape_op_results_cmd(int count)
{
    const le::ReadView view = read_view();
    const Lookup lk = lookup(view);
    const std::vector<le::ShapeId> &results = shape_op_results();
    std::ostringstream out;
    for (int i = 0; i < count && static_cast<size_t>(i) < results.size(); ++i)
    {
        if (i > 0)
            out << ' ';
        out << format_shape_id(lk, results[static_cast<size_t>(i)]);
    }
    return return_string(out.str());
}

const char *shape_bbox_cmd(const char *shape_tokens)
{
    const le::ReadView view = read_view();
    const std::vector<le::GeometryId> shapes = resolve_shape_tokens(lookup(view), shape_tokens);
    const std::expected<le::Rect, std::string> box = view.shape_bbox(shapes);
    if (!box)
    {
        fail(box.error());
        return return_string("");
    }
    const double scale = dbu_per_um(view);
    // The {{llx lly} {urx ury}} Rect form every -bbox/-rects flag and
    // zoom_area take. std::to_chars gives the shortest exact round-trip
    // form (ostream's default 6 significant digits would truncate a large
    // micron coordinate).
    auto number = [&](int64_t dbu)
    {
        char buf[32];
        const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), static_cast<double>(dbu) / scale);
        return std::string(buf, end);
    };
    return return_string("{" + number(box->ll.x) + " " + number(box->ll.y) + "} {" + number(box->ur.x) + " " + number(box->ur.y) + "}");
}

// --- Editing / undo-redo ---

void begin_command(const char *label)
{
    if (label)
        write_view().begin_command(label);
}

int end_command(int succeeded)
{
    write_view().end_command(succeeded != 0);
    return 0;
}

int undo_command()
{
    const std::expected<bool, std::string> undone = write_view().undo();
    return undone ? (*undone ? 1 : 0) : -1;
}

int redo_command()
{
    const std::expected<bool, std::string> redone = write_view().redo();
    return redone ? (*redone ? 1 : 0) : -1;
}

int command_history_count()
{
    return static_cast<int>(read_view().command_history_size());
}

const char *command_history_at(int index)
{
    const le::ReadView view = read_view();
    if (index < 0 || static_cast<size_t>(index) >= view.command_history_size())
        return "";
    return return_string(view.command_history_at(static_cast<size_t>(index)));
}

int get_hierarchy_depth_command()
{
    return read_view().hierarchy_depth();
}

void set_hierarchy_depth_command(int depth)
{
    write_view().set_hierarchy_depth(depth);
}

int get_flightline_max_fanout_command()
{
    return read_view().flightline_max_fanout();
}

void set_flightline_max_fanout_command(int max_fanout)
{
    write_view().set_flightline_max_fanout(max_fanout);
}

int get_undo_limit_command()
{
    return static_cast<int>(std::min<size_t>(read_view().undo_limit(), INT32_MAX));
}

void set_undo_limit_command(int limit)
{
    if (limit >= 0)
        write_view().set_undo_limit(static_cast<size_t>(limit));
}

double get_grid_spacing_um_command(int major)
{
    return read_view().grid_spacing_um(major != 0);
}

void set_grid_spacing_um_command(double minor_um, double major_um)
{
    write_view().set_grid_spacing_um(minor_um, major_um);
}

int get_confirm_overwrite_command() { return read_view().confirm_overwrite() ? 1 : 0; }
void set_confirm_overwrite_command(int confirm) { write_view().set_confirm_overwrite(confirm != 0); }
const char *get_theme_command() { return le::kThemes[read_view().theme()].name.data(); }
int set_theme_command(const char *name) { return name && write_view().set_theme(name) ? 0 : -1; }
int theme_count_command() { return static_cast<int>(le::kThemes.size()); }
const char *theme_name_command(int index)
{
    return index >= 0 && static_cast<size_t>(index) < le::kThemes.size() ? le::kThemes[static_cast<size_t>(index)].name.data() : "";
}

double get_ruler_label_size_command()
{
    return read_view().ruler_label_size();
}

void set_ruler_label_size_command(double px)
{
    write_view().set_ruler_label_size(px);
}

double get_label_min_size_command()
{
    return read_view().label_min_size();
}

void set_label_min_size_command(double px)
{
    write_view().set_label_min_size(px);
}

double get_label_max_size_command()
{
    return read_view().label_max_size();
}

void set_label_max_size_command(double px)
{
    write_view().set_label_max_size(px);
}

// Layer colors as "#rrggbb" (the "#" is
// optional). 1 if `color` isn't one.
int set_layer_color_command(const char *layer, const char *color)
{
    std::string_view text = color ? color : "";
    if (!text.empty() && text.front() == '#')
        text.remove_prefix(1);
    if (text.size() != 6 || !std::all_of(text.begin(), text.end(), [](char c)
                                         { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
        return 1;
    const unsigned long value = std::stoul(std::string(text), nullptr, 16);
    if (!layer)
        return 1;
    write_view().set_layer_color(layer, le::Color{static_cast<uint8_t>((value >> 16) & 0xff), static_cast<uint8_t>((value >> 8) & 0xff), static_cast<uint8_t>(value & 0xff)});
    return 0;
}

void reset_layer_color_command(const char *layer)
{
    if (layer)
        write_view().reset_layer_color(layer);
}

// "#rrggbb", or "" for a layer with no row.
const char *get_layer_color_command(const char *layer)
{
    const std::optional<le::Color> color = layer ? read_view().layer_color(layer) : std::nullopt;
    if (!color)
        return return_string("");
    char text[8];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", color->r, color->g, color->b);
    return return_string(text);
}

int save_settings_command(const char *path)
{
    return status(write_view().save_settings(path ? path : ""));
}

int load_settings_command(const char *path)
{
    return status(write_view().load_settings(path ? path : ""));
}

const char *default_settings_path_command()
{
    return return_string(le::default_settings_path());
}

const char *generated_procs_default_path_command()
{
#ifdef LE_TCL_GENERATED_PROCS_PATH
    return LE_TCL_GENERATED_PROCS_PATH;
#else
    return "";
#endif
}

int get_max_concurrency_command()
{
    return read_view().max_concurrency();
}

void set_max_concurrency_command(int max_concurrency)
{
    write_view().set_max_concurrency(max_concurrency);
}
