#include "le_tcl_shim.hpp"

#include "api.hpp"

#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
    // Set by set_session_handle() (see le_tcl_shim.hpp) - an externally-
    // owned handle wins over the lazy self-create below whenever one has
    // been injected, e.g. by a Flutter-embedded Tcl console sharing the
    // Dart-owned LeHandle* (see TCL_EXPLORATION.md's show_gui section).
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
        static LeHandle *handle = le_create();
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

// Generated TCL property-reading surface - friendly-id resolve/format,
// property-table accessors, and is_child-field enumeration, for every
// TCL-readable class not already covered by hand-written code above.
// Placed here (after the anonymous namespace above, before every bare
// function below) so session()/pack/unpack/return_string/
// format_property_value/resolve_numeric_friendly_id/
// format_numeric_friendly_id are all already in scope, and so
// technology_id() below can use the generated format_technology_id().
// Never edit generated/le_tcl_shim_generated.inc directly - regenerate
// via the regen-tcl skill instead.
#include "generated/le_tcl_shim_generated.inc"

int read_lef(const char *path)
{
    return le_read_lef(session(), path);
}

int read_def(const char *path)
{
    return le_read_def(session(), path);
}

int design_count()
{
    return le_design_count(session());
}

const char *design_name(int index)
{
    return le_design_name(session(), index);
}

int message_count()
{
    return le_message_count(session());
}

const char *message_at(int index)
{
    return le_message_at(session(), index);
}

void set_viewport_size_cmd(int width_px, int height_px)
{
    le_set_viewport_size(session(), width_px, height_px);
}

int viewport_width()
{
    return le_render_pixel_buffer(session()).width;
}

int viewport_height()
{
    return le_render_pixel_buffer(session()).height;
}

long long design_abstract_id(int design_index)
{
    return pack(le_library_design_at(session(), 0, design_index).abstract_id);
}

long long design_by_name(const char *name)
{
    return pack(le_design_by_name(session(), name));
}

const char *technology_id()
{
    LeTechnologyId id = le_technology_id(session());
    if (id.index == UINT32_MAX)
        return return_string("");
    return return_string(format_technology_id(id));
}

int set_current_design_abstract_cmd(long long design_id)
{
    return le_set_current_design_abstract_by_id(session(), unpack<LeDesignId>(design_id));
}

int set_current_design_layout_cmd(long long design_id)
{
    return le_set_current_design_layout_by_id(session(), unpack<LeDesignId>(design_id));
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
    LeRectUm rect = le_shape_rect_at(session(), resolve_shape_id(id), index);
    std::ostringstream out;
    out << rect.ll_x_um << ' ' << rect.ll_y_um << ' ' << rect.ur_x_um << ' ' << rect.ur_y_um;
    return return_string(out.str());
}

int remove_shape_rect(const char *id, int index)
{
    return le_remove_shape_rect(session(), resolve_shape_id(id), index);
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
    return le_remove_shape_polygon(session(), resolve_shape_id(id), polygon_index);
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
    return le_remove_shape_path(session(), resolve_shape_id(id), path_index);
}

// --- Editing / undo-redo (UPDATES.md item 21) ---

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
    return le_undo(session());
}

int redo_command()
{
    return le_redo(session());
}

int command_history_count()
{
    return le_command_history_count(session());
}

const char *command_history_at(int index)
{
    const char *value = le_command_history_at(session(), index);
    return value ? value : "";
}
