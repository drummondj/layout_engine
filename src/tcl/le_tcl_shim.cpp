#include "le_tcl_shim.hpp"
#include "le/extension_tcl.hpp"

#include "api.hpp"
#include "views.hpp"
#include "../core/themes.hpp"

#include <blend2d/blend2d.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <expected>
#include <span>
#include <variant>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
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
    // custom SWIG struct typemap. Generic over every LeXxxId (all
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

    // --- Friendly id formatting/parsing (see le_tcl_shim.hpp's own "IDs"
    // comment for the full contract) ---

    // design:NAME, or design:LIB/NAME when several Libraries have a Design
    // of that name - le_design_by_name accepts either.
    constexpr std::string_view kDesignPrefix = "design:";

    std::string format_design_id(LeDesignId id)
    {
        const char *name = le_design_name_by_id(session(), id);
        if (!name)
            return std::string(kDesignPrefix);
        const LeDesignId by_name = le_design_by_name(session(), name);
        if (by_name.index == id.index && by_name.generation == id.generation)
            return std::string(kDesignPrefix) + name;
        const char *library = le_library_name_by_id(session(), le_design_library_by_id(session(), id));
        return std::string(kDesignPrefix) + (library ? library : "") + "/" + name;
    }

    LeDesignId resolve_design_id(const char *s)
    {
        const LeDesignId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kDesignPrefix.size()) != kDesignPrefix)
            return invalid;
        return le_design_by_name(session(), std::string(sv.substr(kDesignPrefix.size())).c_str());
    }

    constexpr std::string_view kTerminalPrefix = "terminal:";

    std::string format_terminal_id(const char *name)
    {
        return std::string(kTerminalPrefix) + (name ? name : "");
    }

    // Overload taking the Id directly - needed by generated is_child
    // enumeration (e.g. Abstract.terminals), same reasoning as every
    // generated format_X_id(Id) overload (le_tcl_shim_generated.inc's own
    // comment), hand-written here since Terminal's own resolve/format
    // pair isn't generated (see that file's own comment on why).
    std::string format_terminal_id(LeTerminalId id)
    {
        return format_terminal_id(le_terminal_name(session(), id));
    }

    // Fixed-prefix compare (not "find first colon") so a LEF-legal name
    // containing ':' itself can't misparse - the prefix always matches
    // the whole leading literal, everything after is the raw name
    // verbatim. An empty/null `s` (no -of token given at all) fails the
    // prefix check the same way a malformed one does, resolving to the
    // same invalid sentinel - exactly what "use the default scope" needs
    // (see le_tcl_shim.hpp's own "IDs" comment).
    //
    // le_terminal_by_name is itself already scoped to the current view,
    // same as le_get_terminals - see resolve_library_id's own comment for
    // the general fixed-prefix/empty-means-default-scope reasoning.
    LeTerminalId resolve_terminal_id(const char *s)
    {
        const LeTerminalId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kTerminalPrefix.size()) != kTerminalPrefix)
            return invalid;
        return le_terminal_by_name(session(), std::string(sv.substr(kTerminalPrefix.size())).c_str());
    }

    // Row/Placement/PhysicalPort/Route/Region/LayoutVia - same unique_per_parent
    // shape as Terminal above (each name is scoped to its own Layout,
    // not global), so each gets the same hand-written prefix/format/
    // resolve triple, scoped through le_X_by_name/le_X_name
    // (handle->current_layout_id-scoped, see api.cpp's own comment).

    constexpr std::string_view kRowPrefix = "row:";

    std::string format_row_id(const char *name)
    {
        return std::string(kRowPrefix) + (name ? name : "");
    }

    std::string format_row_id(LeRowId id)
    {
        return format_row_id(le_row_name(session(), id));
    }

    LeRowId resolve_row_id(const char *s)
    {
        const LeRowId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kRowPrefix.size()) != kRowPrefix)
            return invalid;
        return le_row_by_name(session(), std::string(sv.substr(kRowPrefix.size())).c_str());
    }

    constexpr std::string_view kPlacementPrefix = "placement:";

    std::string format_placement_id(const char *name)
    {
        return std::string(kPlacementPrefix) + (name ? name : "");
    }

    std::string format_placement_id(LePlacementId id)
    {
        return format_placement_id(le_placement_name(session(), id));
    }

    LePlacementId resolve_placement_id(const char *s)
    {
        const LePlacementId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kPlacementPrefix.size()) != kPlacementPrefix)
            return invalid;
        return le_placement_by_name(session(), std::string(sv.substr(kPlacementPrefix.size())).c_str());
    }

    constexpr std::string_view kPhysicalPortPrefix = "physical_port:";

    std::string format_physical_port_id(const char *name)
    {
        return std::string(kPhysicalPortPrefix) + (name ? name : "");
    }

    std::string format_physical_port_id(LePhysicalPortId id)
    {
        return format_physical_port_id(le_physical_port_name(session(), id));
    }

    LePhysicalPortId resolve_physical_port_id(const char *s)
    {
        const LePhysicalPortId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kPhysicalPortPrefix.size()) != kPhysicalPortPrefix)
            return invalid;
        return le_physical_port_by_name(session(), std::string(sv.substr(kPhysicalPortPrefix.size())).c_str());
    }

    constexpr std::string_view kRoutePrefix = "route:";

    std::string format_route_id(const char *name)
    {
        return std::string(kRoutePrefix) + (name ? name : "");
    }

    std::string format_route_id(LeRouteId id)
    {
        return format_route_id(le_route_name(session(), id));
    }

    LeRouteId resolve_route_id(const char *s)
    {
        const LeRouteId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kRoutePrefix.size()) != kRoutePrefix)
            return invalid;
        return le_route_by_name(session(), std::string(sv.substr(kRoutePrefix.size())).c_str());
    }

    constexpr std::string_view kRegionPrefix = "region:";

    std::string format_region_id(const char *name)
    {
        return std::string(kRegionPrefix) + (name ? name : "");
    }

    std::string format_region_id(LeRegionId id)
    {
        return format_region_id(le_region_name(session(), id));
    }

    LeRegionId resolve_region_id(const char *s)
    {
        const LeRegionId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kRegionPrefix.size()) != kRegionPrefix)
            return invalid;
        return le_region_by_name(session(), std::string(sv.substr(kRegionPrefix.size())).c_str());
    }

    constexpr std::string_view kLayoutViaPrefix = "layout_via:";

    std::string format_layout_via_id(const char *name)
    {
        return std::string(kLayoutViaPrefix) + (name ? name : "");
    }

    std::string format_layout_via_id(LeLayoutViaId id)
    {
        return format_layout_via_id(le_layout_via_name(session(), id));
    }

    LeLayoutViaId resolve_layout_via_id(const char *s)
    {
        const LeLayoutViaId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kLayoutViaPrefix.size()) != kLayoutViaPrefix)
            return invalid;
        return le_layout_via_by_name(session(), std::string(sv.substr(kLayoutViaPrefix.size())).c_str());
    }

    // Port/Net/Instance/PortBus/NetBus - same unique_per_parent shape as
    // Row/Placement/etc. above (each name is scoped to its own Schematic,
    // not global), so the same hand-written prefix/format/resolve triple,
    // relative to the current Schematic: an object below it carries its
    // Instance path (`instance:u_mid/u_leaf`, le_<type>_token_name).

    constexpr std::string_view kPortPrefix = "port:";

    std::string format_port_id(const char *name)
    {
        return std::string(kPortPrefix) + (name ? name : "");
    }

    std::string format_port_id(LePortId id)
    {
        return format_port_id(le_port_token_name(session(), id));
    }

    LePortId resolve_port_id(const char *s)
    {
        const LePortId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kPortPrefix.size()) != kPortPrefix)
            return invalid;
        return le_port_by_name(session(), std::string(sv.substr(kPortPrefix.size())).c_str());
    }

    constexpr std::string_view kNetPrefix = "net:";

    std::string format_net_id(const char *name)
    {
        return std::string(kNetPrefix) + (name ? name : "");
    }

    std::string format_net_id(LeNetId id)
    {
        return format_net_id(le_net_token_name(session(), id));
    }

    LeNetId resolve_net_id(const char *s)
    {
        const LeNetId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kNetPrefix.size()) != kNetPrefix)
            return invalid;
        return le_net_by_name(session(), std::string(sv.substr(kNetPrefix.size())).c_str());
    }

    constexpr std::string_view kInstancePrefix = "instance:";

    std::string format_instance_id(const char *name)
    {
        return std::string(kInstancePrefix) + (name ? name : "");
    }

    std::string format_instance_id(LeInstanceId id)
    {
        return format_instance_id(le_instance_token_name(session(), id));
    }

    LeInstanceId resolve_instance_id(const char *s)
    {
        const LeInstanceId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kInstancePrefix.size()) != kInstancePrefix)
            return invalid;
        return le_instance_by_name(session(), std::string(sv.substr(kInstancePrefix.size())).c_str());
    }

    constexpr std::string_view kPortBusPrefix = "port_bus:";

    std::string format_port_bus_id(const char *name)
    {
        return std::string(kPortBusPrefix) + (name ? name : "");
    }

    std::string format_port_bus_id(LePortBusId id)
    {
        return format_port_bus_id(le_port_bus_token_name(session(), id));
    }

    LePortBusId resolve_port_bus_id(const char *s)
    {
        const LePortBusId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kPortBusPrefix.size()) != kPortBusPrefix)
            return invalid;
        return le_port_bus_by_name(session(), std::string(sv.substr(kPortBusPrefix.size())).c_str());
    }

    constexpr std::string_view kNetBusPrefix = "net_bus:";

    std::string format_net_bus_id(const char *name)
    {
        return std::string(kNetBusPrefix) + (name ? name : "");
    }

    std::string format_net_bus_id(LeNetBusId id)
    {
        return format_net_bus_id(le_net_bus_token_name(session(), id));
    }

    LeNetBusId resolve_net_bus_id(const char *s)
    {
        const LeNetBusId invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, kNetBusPrefix.size()) != kNetBusPrefix)
            return invalid;
        return le_net_bus_by_name(session(), std::string(sv.substr(kNetBusPrefix.size())).c_str());
    }

    // Obstruction/TerminalPort/Shape have no name field - their friendly
    // id is just their existing packed integer, type-prefixed for
    // self-description. A malformed string or wrong-type prefix (e.g. a
    // "terminal_port:..." id passed where "shape:..." is expected) parses to the
    // same invalid sentinel as an unknown id - api.hpp's own not-found
    // paths already degrade gracefully for that, so no separate error
    // path is needed here.
    template <typename IdT>
    IdT resolve_numeric_friendly_id(const char *s, std::string_view prefix)
    {
        const IdT invalid{.index = UINT32_MAX, .generation = 0};
        if (!s)
            return invalid;
        std::string_view sv(s);
        if (sv.substr(0, prefix.size()) != prefix)
            return invalid;
        std::string_view digits = sv.substr(prefix.size());
        if (digits.empty())
            return invalid;
        int64_t packed = 0;
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), packed);
        if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size())
            return invalid;
        return unpack<IdT>(packed);
    }

    template <typename IdT>
    std::string format_numeric_friendly_id(IdT id, std::string_view prefix)
    {
        return std::string(prefix) + std::to_string(pack(id));
    }

    // Tcl is "everything is a string" by design (`expr {$v + 1}` works on
    // a numeric string exactly like a native int) - see le_tcl_shim.hpp's
    // "property tables and search results" comment for why every
    // property value crossing this shim is pre-stringified rather than
    // exposed with its LePropertyType tag.
    std::string format_property_value(const LeProperty &prop)
    {
        switch (prop.type)
        {
        case LE_PROPERTY_TYPE_STRING:
            return prop.string_value ? prop.string_value : "";
        case LE_PROPERTY_TYPE_INT:
            return std::to_string(prop.int_value);
        case LE_PROPERTY_TYPE_DOUBLE:
            return std::to_string(prop.double_value);
        default:
            return "";
        }
    }

    // Friendly-numeric-id-list-as-space-separated-string, shared by
    // get_terminal_ports/get_obstructions/terminal_port_shapes/
    // obstruction_shapes below - every token is purely numeric
    // ("terminal_port:N"/"obstruction:N"/"shape:N"), never LEF-authored text, so
    // this is provably a well-formed Tcl list with no escaping needed -
    // see le_tcl_shim.hpp's own comment on why get_terminals_cmd/_at is
    // shaped differently.
    template <typename IdT>
    std::string join_friendly_ids(int32_t count, IdT (*at)(LeHandle *, int32_t), std::string_view prefix)
    {
        std::ostringstream out;
        for (int32_t i = 0; i < count; ++i)
        {
            if (i > 0)
            {
                out << ' ';
            }
            out << format_numeric_friendly_id(at(session(), i), prefix);
        }
        return out.str();
    }
}

LeHandle *le::ext::tcl_session()
{
    return session();
}

// Generated TCL property-reading surface - friendly-id resolve/format,
// property-table accessors, and is_child-field enumeration, for every
// TCL-readable class not already covered by hand-written code above.
// Placed here (after the anonymous namespace above, before every bare
// function below) so session()/pack/unpack/return_string/
// format_property_value/resolve_numeric_friendly_id/
// format_numeric_friendly_id are all already in scope, and so
// technology_id() below can use the generated format_technology_id().
// Never edit generated/tcl/le_tcl_shim_generated.inc directly - regenerate
// by rebuilding.
#include "generated/tcl/le_tcl_shim_generated.inc"

namespace
{
    le::ReadView read_view() { return le::ReadView(session()); }
    le::WriteView write_view() { return le::WriteView(session()); }

    // The reason the last failing command gave, for last_error_cmd.
    std::string &last_error()
    {
        static thread_local std::string error;
        return error;
    }

    // 0, or 1 with the reason kept for last_error_cmd.
    int status(const std::expected<void, std::string> &result)
    {
        if (result)
            return 0;
        last_error() = result.error();
        return 1;
    }

    int fail(std::string reason)
    {
        last_error() = std::move(reason);
        return 1;
    }

    // Microns to dbu at the Technology's scale, or 1:1 without one, as the
    // C API's micron arguments always were.
    template <typename View>
    double dbu_per_um(const View &view)
    {
        const std::expected<le::Units, std::string> units = view.units();
        return units ? units->dbu_per_um() : 1.0;
    }

    int64_t to_dbu(double um, double scale) { return static_cast<int64_t>(std::llround(um * scale)); }

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
// le_read_verilog call (rather than one call per file) matters for
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
    const le::LibraryId library = (library_token && library_token[0]) ? le::ext::from_c(resolve_library_id(library_token)) : le::LibraryId{};
    return status(write_view().write_verilog_stubs(path ? path : "", library));
}

int link_unresolved_instances_cmd()
{
    return le_link_unresolved_instances(session());
}

int get_instances_by_path_cmd(const char *of_schematic, const char *path, const char *filter_expression)
{
    return le_get_instances_by_path(session(), resolve_schematic_id(of_schematic), path, filter_expression);
}

int get_nets_by_path_cmd(const char *of_schematic, const char *path, const char *filter_expression)
{
    return le_get_nets_by_path(session(), resolve_schematic_id(of_schematic), path, filter_expression);
}

int get_ports_by_path_cmd(const char *of_schematic, const char *path, const char *filter_expression)
{
    return le_get_ports_by_path(session(), resolve_schematic_id(of_schematic), path, filter_expression);
}

// --- Schematic<->Layout link mutation side-effects - le_tcl_procs.tcl's
// own delete_net/update_net/
// update_instance overrides route to these instead of the generated
// delete_net_cmd/update_net_cmd/update_instance_cmd (still used as the
// fallback for an update_net/update_instance call that doesn't touch
// -name at all, since only a rename has anything to propagate).

int delete_net_cascade_cmd(const char *id)
{
    return le_delete_net_cascade(session(), resolve_net_id(id));
}

const char *rename_net_cmd(const char *id, const char *new_name)
{
    const LeNetId typed_id = resolve_net_id(id);
    if (le_rename_net_propagate(session(), typed_id, new_name) != 0)
        return return_string("");
    return return_string(format_net_id(typed_id));
}

const char *rename_instance_cmd(const char *id, const char *new_name)
{
    const LeInstanceId typed_id = resolve_instance_id(id);
    if (le_rename_instance_propagate(session(), typed_id, new_name) != 0)
        return return_string("");
    return return_string(format_instance_id(typed_id));
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
    return le_property_path_failed(session());
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
        return pack(LeAbstractId{.index = UINT32_MAX, .generation = 0});
    const auto &designs = view.root().get_library_designs(libraries.front());
    if (static_cast<size_t>(design_index) >= designs.size())
        return pack(LeAbstractId{.index = UINT32_MAX, .generation = 0});
    return pack(le::ext::to_c(view.root().get_design_abstract(designs[static_cast<size_t>(design_index)])));
}

long long design_by_name(const char *name)
{
    return pack(le_design_by_name(session(), name));
}

const char *technology_id()
{
    const auto technologies = read_view().root().get_technology_ids();
    if (technologies.empty())
        return return_string("");
    return return_string(format_technology_id(le::ext::to_c(technologies.front())));
}

int set_current_design_abstract_cmd(long long design_id)
{
    le::WriteView view = write_view();
    const int result = status(view.open_abstract(le::ext::from_c(unpack<LeDesignId>(design_id))));
    // A script-driven open lands framed, as the GUI's does.
    if (result == 0)
        view.fit_view(kOpenDesignFitPaddingPx);
    return result;
}

int set_current_design_layout_cmd(long long design_id)
{
    le::WriteView view = write_view();
    const int result = status(view.open_layout(le::ext::from_c(unpack<LeDesignId>(design_id))));
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

// abstract_token/layout_token empty means "use le_write_lef/le_write_def's
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
    std::vector<le::AbstractId> abstracts;
    std::istringstream stream(abstract_tokens ? abstract_tokens : "");
    std::string token;
    while (stream >> token)
        abstracts.push_back(le::ext::from_c(resolve_abstract_id(token.c_str())));
    const le::LibraryId library = (library_token && library_token[0]) ? le::ext::from_c(resolve_library_id(library_token)) : le::LibraryId{};
    le::LefLayerWriteMode layers = le::LefLayerWriteMode::NONE;
    if (layer_write_mode == LE_LEF_LAYER_WRITE_MODE_INCLUDE_WITH_ABSTRACT)
        layers = le::LefLayerWriteMode::INCLUDE_WITH_ABSTRACT;
    else if (layer_write_mode == LE_LEF_LAYER_WRITE_MODE_TECHNOLOGY_ONLY)
        layers = le::LefLayerWriteMode::TECHNOLOGY_ONLY;
    return status(write_view().write_lef(path ? path : "", abstracts, library, layers));
}

int write_def_cmd(const char *path, const char *layout_token)
{
    const le::LayoutId layout = (layout_token && layout_token[0]) ? le::ext::from_c(resolve_layout_id(layout_token)) : le::LayoutId{};
    return status(write_view().write_def(path ? path : "", layout));
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
// meaningful here (the kinds LeHandle::SelectedObject covers - see le_select_object_ref's
// own api.hpp doc comment); literal prefix strings rather than the
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
    return return_string(std::visit([](const auto &entry) -> std::string
                                    {
                                        using T = std::decay_t<decltype(entry)>;
                                        if constexpr (std::is_same_v<T, le::ShapePiece>)
                                            return entry.wire_id.valid() ? format_wire_id(le::ext::to_c(entry.wire_id)) : format_shape_id(le::ext::to_c(entry.shape_id));
                                        else if constexpr (std::is_same_v<T, le::RowId>)
                                            return format_row_id(le::ext::to_c(entry));
                                        else if constexpr (std::is_same_v<T, le::PlacementId>)
                                            return format_placement_id(le::ext::to_c(entry));
                                        else
                                            return format_region_id(le::ext::to_c(entry)); },
                                    selection[static_cast<size_t>(index)]));
}

int select_cmd(const char *token)
{
    if (!token)
        return fail("no token");
    const std::string_view sv(token);
    le::SelectableObject object;
    if (sv.starts_with("shape:"))
        object = le::ext::from_c(resolve_shape_id(token));
    else if (sv.starts_with("wire:"))
        object = le::ext::from_c(resolve_wire_id(token));
    else if (sv.starts_with("row:"))
        object = le::ext::from_c(resolve_row_id(token));
    else if (sv.starts_with("placement:"))
        object = le::ext::from_c(resolve_placement_id(token));
    else if (sv.starts_with("region:"))
        object = le::ext::from_c(resolve_region_id(token));
    else
        return 2; // an unrecognized prefix: select names the token itself
    return status(write_view().select(object));
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
    if (mode >= LE_MODE_SELECT && mode <= LE_MODE_RULER)
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
    if (mode >= LE_PLACEMENT_SNAP_NONE && mode <= LE_PLACEMENT_SNAP_MANUFACTURING_GRID)
        write_view().set_placement_snap_mode(static_cast<le::PlacementSnapMode>(mode));
}

int get_placement_snap_mode_cmd()
{
    return static_cast<int>(read_view().placement_snap_mode());
}

int set_view_flip_cmd(int flip)
{
    if (flip < LE_VIEW_FLIP_NONE || flip > LE_VIEW_FLIP_VERTICAL)
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
    return mode >= LE_PLACEMENT_SNAP_NONE && mode <= LE_PLACEMENT_SNAP_MANUFACTURING_GRID &&
           read_view().placement_snap_mode_available(static_cast<le::PlacementSnapMode>(mode));
}

int apply_placement_orientation_op_cmd(int op)
{
    if (op < LE_ORIENTATION_OP_ROTATE_CCW || op > LE_ORIENTATION_OP_FLIP_VERTICAL)
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
    if (kind >= LE_PIECE_KIND_RECT && kind <= LE_PIECE_KIND_VIA_ITERATE && mode >= LE_SHAPE_SNAP_NONE && mode <= LE_SHAPE_SNAP_TRACKS)
        write_view().set_shape_snap_mode(static_cast<le::PieceKind>(kind), static_cast<le::ShapeSnapMode>(mode));
}

int get_shape_snap_mode_cmd(int kind)
{
    if (kind < LE_PIECE_KIND_RECT || kind > LE_PIECE_KIND_VIA_ITERATE)
        return LE_SHAPE_SNAP_USER_GRID;
    return static_cast<int>(read_view().shape_snap_mode(static_cast<le::PieceKind>(kind)));
}

bool is_shape_snap_mode_available_cmd(int kind, int mode)
{
    return kind >= LE_PIECE_KIND_RECT && kind <= LE_PIECE_KIND_VIA_ITERATE && mode >= LE_SHAPE_SNAP_NONE && mode <= LE_SHAPE_SNAP_TRACKS &&
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

const char *shape_layer_name(const char *id)
{
    return le_shape_layer_name(session(), resolve_shape_id(id));
}

int shape_rect_count(const char *id)
{
    return le_shape_rect_count(session(), resolve_shape_id(id));
}

const char *shape_rect_at(const char *id, int index)
{
    // Brace-nested {{ll_x ll_y} {ur_x ur_y}}, matching every other
    // Rect-shaped value's own convention (create_shape's own -rects flag,
    // get_properties' rects display).
    LeRectUm rect = le_shape_rect_at(session(), resolve_shape_id(id), index);
    std::ostringstream out;
    out << '{' << rect.ll_x_um << ' ' << rect.ll_y_um << "} {" << rect.ur_x_um << ' ' << rect.ur_y_um << '}';
    return return_string(out.str());
}

int remove_shape_rect(const char *id, int index)
{
    if (index < 0)
        return fail("a rect index can't be negative");
    return status(write_view().remove_shape_piece(le::ext::from_c(resolve_shape_id(id)), le::PieceKind::RECT, static_cast<size_t>(index)));
}

int shape_polygon_count(const char *id)
{
    return le_shape_polygon_count(session(), resolve_shape_id(id));
}

int shape_polygon_point_count(const char *id, int polygon_index)
{
    return le_shape_polygon_point_count(session(), resolve_shape_id(id), polygon_index);
}

const char *shape_polygon_point_at(const char *id, int polygon_index, int point_index)
{
    LePointUm pt = le_shape_polygon_point_at(session(), resolve_shape_id(id), polygon_index, point_index);
    std::ostringstream out;
    out << pt.x_um << ' ' << pt.y_um;
    return return_string(out.str());
}

int remove_shape_polygon(const char *id, int polygon_index)
{
    if (polygon_index < 0)
        return fail("a polygon index can't be negative");
    return status(write_view().remove_shape_piece(le::ext::from_c(resolve_shape_id(id)), le::PieceKind::POLYGON, static_cast<size_t>(polygon_index)));
}

int shape_path_count(const char *id)
{
    return le_shape_path_count(session(), resolve_shape_id(id));
}

double shape_path_width_um(const char *id, int path_index)
{
    return le_shape_path_width_um(session(), resolve_shape_id(id), path_index);
}

int shape_path_point_count(const char *id, int path_index)
{
    return le_shape_path_point_count(session(), resolve_shape_id(id), path_index);
}

const char *shape_path_point_at(const char *id, int path_index, int point_index)
{
    LePointUm pt = le_shape_path_point_at(session(), resolve_shape_id(id), path_index, point_index);
    std::ostringstream out;
    out << pt.x_um << ' ' << pt.y_um;
    return return_string(out.str());
}

int remove_shape_path(const char *id, int path_index)
{
    if (path_index < 0)
        return fail("a path index can't be negative");
    const le::GeometryId shape = std::string_view(id ? id : "").starts_with("wire:") ? le::GeometryId(le::ext::from_c(resolve_wire_id(id)))
                                                                                     : le::GeometryId(le::ext::from_c(resolve_shape_id(id)));
    return status(write_view().remove_shape_piece(shape, le::PieceKind::PATH, static_cast<size_t>(path_index)));
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
    std::vector<le::GeometryId> resolve_shape_tokens(const char *tokens)
    {
        std::vector<le::GeometryId> shapes;
        std::istringstream stream(tokens ? tokens : "");
        std::string token;
        while (stream >> token)
        {
            if (token.starts_with("wire:"))
                shapes.emplace_back(le::ext::from_c(resolve_wire_id(token.c_str())));
            else
                shapes.emplace_back(le::ext::from_c(resolve_shape_id(token.c_str())));
        }
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
    std::optional<std::optional<le::shape_ops::LayerOrPurpose>> resolve_optional_layer(const char *token)
    {
        if (!token_given(token))
            return std::optional<le::shape_ops::LayerOrPurpose>{};
        if (is_debug_layer(token))
            return std::optional(le::shape_ops::LayerOrPurpose{.purpose = le::ShapePurpose::DEBUG});
        const le::LayerId id = le::ext::from_c(resolve_layer_id(token));
        if (!id.valid())
            return std::nullopt;
        return std::optional(le::shape_ops::LayerOrPurpose{.layer = id});
    }

    // nothing for no token; nullopt = a parent token was given but isn't
    // a supported, existing kind.
    std::optional<std::optional<le::shape_ops::ShapeParent>> resolve_optional_parent(const char *token)
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
            return valid(le::ext::from_c(resolve_abstract_id(token)));
        if (sv.starts_with("layout:"))
            return valid(le::ext::from_c(resolve_layout_id(token)));
        if (sv.starts_with("obstruction:"))
            return valid(le::ext::from_c(resolve_obstruction_id(token)));
        if (sv.starts_with("terminal_port:"))
            return valid(le::ext::from_c(resolve_terminal_port_id(token)));
        if (sv.starts_with("route:"))
            return valid(le::ext::from_c(resolve_route_id(token)));
        if (sv.starts_with("blockage:"))
            return valid(le::ext::from_c(resolve_blockage_id(token)));
        if (sv.starts_with("physical_port_segment:"))
            return valid(le::ext::from_c(resolve_physical_port_segment_id(token)));
        return std::nullopt;
    }

    // The last shape op's new Shapes, for shape_op_results_cmd.
    std::vector<le::ShapeId> &shape_op_results()
    {
        static thread_local std::vector<le::ShapeId> results;
        return results;
    }

    // Shared by every creating shape_*_cmd: resolves -layer/-parent, then
    // runs `call(view, layer, parent)` - the matching WriteView shape op.
    // How many Shapes it made, or a negative status.
    template <typename Call>
    int run_shape_op_cmd(const char *layer_token, const char *parent_token, Call &&call)
    {
        const auto layer = resolve_optional_layer(layer_token);
        if (!layer)
            return kShapeOpBadLayer;
        const auto parent = resolve_optional_parent(parent_token);
        if (!parent)
            return kShapeOpBadParent;
        le::WriteView view = write_view();
        const le::ShapeOpResult result = call(view, *layer, *parent);
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
    const std::vector<le::GeometryId> shapes = resolve_shape_tokens(shape_tokens);
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const auto &layer, const auto &parent)
                            { return layer ? view.shape_copy(shapes, *layer, parent) : le::ShapeOpResult(std::unexpected(std::string("-layer is required"))); });
}

int shape_boolean_cmd(const char *shape_tokens_a, const char *shape_tokens_b, int op, const char *layer_token, const char *parent_token)
{
    const le::BooleanOp boolean_op = op == LE_SHAPE_BOOLEAN_AND ? le::BooleanOp::And : op == LE_SHAPE_BOOLEAN_NOT ? le::BooleanOp::Not : le::BooleanOp::Or;
    const std::vector<le::GeometryId> a = resolve_shape_tokens(shape_tokens_a);
    const std::vector<le::GeometryId> b = resolve_shape_tokens(shape_tokens_b);
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const auto &layer, const auto &parent)
                            { return view.shape_boolean(a, b, boolean_op, layer, parent); });
}

int shape_to_polygon_cmd(const char *shape_tokens, const char *layer_token, const char *parent_token)
{
    const std::vector<le::GeometryId> shapes = resolve_shape_tokens(shape_tokens);
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const auto &layer, const auto &parent)
                            { return view.shape_to_polygons(shapes, layer, parent); });
}

int shape_to_rects_cmd(const char *shape_tokens, int vertical, const char *layer_token, const char *parent_token)
{
    const std::vector<le::GeometryId> shapes = resolve_shape_tokens(shape_tokens);
    const le::FractureDirection direction = vertical ? le::FractureDirection::Vertical : le::FractureDirection::Horizontal;
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const auto &layer, const auto &parent)
                            { return view.shape_to_rects(shapes, direction, layer, parent); });
}

int shape_size_cmd(const char *shape_tokens, double dx_um, double dy_um, const char *layer_token, const char *parent_token)
{
    const std::vector<le::GeometryId> shapes = resolve_shape_tokens(shape_tokens);
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const auto &layer, const auto &parent)
                            {
                                const double scale = dbu_per_um(view);
                                return view.shape_size(shapes, to_dbu(dx_um, scale), to_dbu(dy_um, scale), layer, parent); });
}

int shape_path_cmd(const char *shape_tokens, double width_um, const char *layer_token, const char *parent_token)
{
    const std::vector<le::GeometryId> shapes = resolve_shape_tokens(shape_tokens);
    return run_shape_op_cmd(layer_token, parent_token, [&](le::WriteView &view, const auto &layer, const auto &parent)
                            { return view.shape_outline_paths(shapes, to_dbu(width_um, dbu_per_um(view)), layer, parent); });
}

int shape_change_layer_cmd(const char *shape_tokens, const char *layer_token)
{
    const auto layer = resolve_optional_layer(layer_token);
    if (!layer || !*layer)
        return kShapeOpBadLayer;
    const std::vector<le::GeometryId> shapes = resolve_shape_tokens(shape_tokens);
    if (const std::expected<void, std::string> changed = write_view().shape_change_layer(shapes, **layer); !changed)
    {
        fail(changed.error());
        return kShapeOpFailed;
    }
    return static_cast<int>(shapes.size());
}

const char *shape_op_results_cmd(int count)
{
    const std::vector<le::ShapeId> &results = shape_op_results();
    std::ostringstream out;
    for (int i = 0; i < count && static_cast<size_t>(i) < results.size(); ++i)
    {
        if (i > 0)
            out << ' ';
        out << format_shape_id(le::ext::to_c(results[static_cast<size_t>(i)]));
    }
    return return_string(out.str());
}

const char *shape_bbox_cmd(const char *shape_tokens)
{
    const std::vector<le::GeometryId> shapes = resolve_shape_tokens(shape_tokens);
    const le::ReadView view = read_view();
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
    le_begin_command(session(), label);
}

int end_command(int succeeded)
{
    le_end_command(session(), succeeded);
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
