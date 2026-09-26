#include "le_tcl_shim.hpp"

#include "api.hpp"

#include <blend2d/blend2d.h>

#include <algorithm>
#include <cctype>
#include <charconv>
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

    // Port/Net/Instance - same unique_per_parent shape as Row/Placement/etc.
    // above (each name is scoped to its own Schematic, not global), so
    // the same hand-written prefix/format/resolve triple, scoped through
    // le_port_by_name/le_port_name (handle->current_schematic_id-scoped,
    // see api.cpp's own comment).

    constexpr std::string_view kPortPrefix = "port:";

    std::string format_port_id(const char *name)
    {
        return std::string(kPortPrefix) + (name ? name : "");
    }

    std::string format_port_id(LePortId id)
    {
        return format_port_id(le_port_name(session(), id));
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
        return format_net_id(le_net_name(session(), id));
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
        return format_instance_id(le_instance_name(session(), id));
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
        return format_port_bus_id(le_port_bus_name(session(), id));
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
        return format_net_bus_id(le_net_bus_name(session(), id));
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

int read_lef(const char *path, const char *library_name)
{
    return le_read_lef(session(), path, library_name);
}

int read_def(const char *path, const char *library_name)
{
    return le_read_def(session(), path, library_name);
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
    std::vector<std::string> path_strings;
    std::istringstream stream(paths ? paths : "");
    std::string token;
    while (stream >> token)
        path_strings.push_back(token);

    std::vector<const char *> path_ptrs;
    path_ptrs.reserve(path_strings.size());
    for (const std::string &s : path_strings)
        path_ptrs.push_back(s.c_str());

    return le_read_verilog(session(), path_ptrs.empty() ? nullptr : path_ptrs.data(), static_cast<int32_t>(path_ptrs.size()), is_netlist, library_name);
}

int write_verilog_stubs_cmd(const char *path, const char *library_token)
{
    const LeLibraryId library_id = (library_token && library_token[0])
                                        ? resolve_library_id(library_token)
                                        : LeLibraryId{.index = UINT32_MAX, .generation = 0};
    return le_write_verilog_stubs(session(), path, library_id);
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

// --- Phase 5 mutation side-effects (LINKING_STRATEGY_RESEARCH.md
// section 5) - le_tcl_procs.tcl's own delete_net/update_net/
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
    return le_design_count(session());
}

const char *design_name(int index)
{
    return le_design_name(session(), index);
}

int property_path_failed()
{
    return le_property_path_failed(session());
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

namespace
{
    // Matches api.cpp's own kKeyFitPaddingPx / the Dart-side LeProvider.
    // openDesign's own `_editor.fitScene(10)` call - the same padding
    // convention every "just opened a view" fit already uses, duplicated
    // here since le_tcl_shim.cpp is a separate TU from both.
    constexpr int32_t kOpenDesignFitPaddingPx = 10;
}

int set_current_design_abstract_cmd(long long design_id)
{
    const int result = le_set_current_design_abstract_by_id(session(), unpack<LeDesignId>(design_id));
    // Frames the newly-opened Abstract's own content the same way the
    // GUI's own Library Browser open action does (LeProvider.openDesign)
    // - a script-driven open_design should land on a sensible view, not
    // whatever scale/pan happened to be left over from a previous one.
    // Safe even with no viewport set yet (e.g. a headless le_shell run) -
    // Scene::fit_to_content degrades to scale=1/pan={0,0} in that case.
    if (result == 0)
        le_fit_scene(session(), kOpenDesignFitPaddingPx);
    return result;
}

int set_current_design_layout_cmd(long long design_id)
{
    const int result = le_set_current_design_layout_by_id(session(), unpack<LeDesignId>(design_id));
    // Same reasoning as set_current_design_abstract_cmd above - a Layout
    // view didn't get this at all before (the actual bug report this
    // fixes: opening a layout view left the scene at its previous/default
    // scale and pan instead of framing the Layout's own diearea, unlike
    // an Abstract view opened through the GUI).
    if (result == 0)
        le_fit_scene(session(), kOpenDesignFitPaddingPx);
    return result;
}

void zoom_cmd(double factor)
{
    le_zoom(session(), factor, viewport_width() / 2, viewport_height() / 2);
}

void zoom_area_cmd(double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um, int padding_px)
{
    le_fit_rect(session(), ll_x_um, ll_y_um, ur_x_um, ur_y_um, padding_px);
}

void set_layer_visible_cmd(const char *layer_name, bool visible)
{
    le_set_layer_name_visible(session(), layer_name, visible);
}

int dump_png_cmd(const char *path)
{
    const LePixelBuffer buffer = le_render_pixel_buffer(session());
    if (!buffer.data || buffer.width <= 0 || buffer.height <= 0)
        return 1;

    BLImage image(buffer.width, buffer.height, BL_FORMAT_PRGB32);
    if (image.is_empty())
        return 1;

    BLImageData image_data;
    if (image.get_data(&image_data) != BL_SUCCESS)
        return 1;

    // LePixelBuffer is literal RGBA byte order (api.hpp's own contract -
    // ComposeStage's own BGRA-to-RGBA swap, compose_stage.hpp, exists
    // specifically to produce this for consumers like this one), while
    // BL_FORMAT_PRGB32 is premultiplied BGRA in memory on this little-
    // endian target (same finding that swap's own comment documents) -
    // this is the mirror-image conversion, swapping back on the way in.
    auto *dst_base = static_cast<uint8_t *>(image_data.pixel_data);
    for (int y = 0; y < buffer.height; ++y)
    {
        const uint8_t *src_row = buffer.data + static_cast<std::ptrdiff_t>(y) * buffer.row_bytes;
        uint8_t *dst_row = dst_base + static_cast<std::ptrdiff_t>(y) * image_data.stride;
        for (int x = 0; x < buffer.width; ++x)
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
    return le_is_layer_name_visible(session(), layer_name);
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
    std::vector<LeAbstractId> abstract_ids;
    if (abstract_tokens)
    {
        std::istringstream stream(abstract_tokens);
        std::string token;
        while (stream >> token)
            abstract_ids.push_back(resolve_abstract_id(token.c_str()));
    }
    const LeLibraryId library_id = (library_token && library_token[0])
                                        ? resolve_library_id(library_token)
                                        : LeLibraryId{.index = UINT32_MAX, .generation = 0};
    return le_write_lef(session(), path, abstract_ids.empty() ? nullptr : abstract_ids.data(), static_cast<int32_t>(abstract_ids.size()), library_id, layer_write_mode);
}

int write_def_cmd(const char *path, const char *layout_token)
{
    const LeLayoutId layout_id = (layout_token && layout_token[0])
                                      ? resolve_layout_id(layout_token)
                                      : LeLayoutId{.index = UINT32_MAX, .generation = 0};
    return le_write_def(session(), path, layout_id);
}

// BUGS_AND_ENHANCEMENTS.md E30 - get_selection/select. Only Shape/Row/
// Placement/Region friendly ids are meaningful here (the same four kinds
// Scene::SelectedObject's own variant covers - see le_select_object_ref's
// own api.hpp doc comment); literal prefix strings rather than the
// generated kShapePrefix/etc constants, since those live in the generated
// file's own scope and duplicating a plain "shape:"/"row:"/... literal
// here is simpler than reaching for them.
int selection_count_cmd()
{
    return le_selection_count(session());
}

const char *get_selection_at_cmd(int index)
{
    const LeObjectRef ref = le_selected_object_ref(session(), index);
    switch (ref.kind)
    {
    case LE_OBJECT_KIND_SHAPE:
        return return_string(format_shape_id(LeShapeId{.index = ref.index, .generation = ref.generation}));
    case LE_OBJECT_KIND_ROW:
        return return_string(format_row_id(LeRowId{.index = ref.index, .generation = ref.generation}));
    case LE_OBJECT_KIND_PLACEMENT:
        return return_string(format_placement_id(LePlacementId{.index = ref.index, .generation = ref.generation}));
    case LE_OBJECT_KIND_REGION:
        return return_string(format_region_id(LeRegionId{.index = ref.index, .generation = ref.generation}));
    default:
        return return_string(std::string{});
    }
}

int select_cmd(const char *token)
{
    if (!token)
        return 1;
    const std::string_view sv(token);
    LeObjectRef ref{};
    if (sv.substr(0, 6) == "shape:")
    {
        const LeShapeId id = resolve_shape_id(token);
        ref = LeObjectRef{.kind = LE_OBJECT_KIND_SHAPE, .index = id.index, .generation = id.generation};
    }
    else if (sv.substr(0, 4) == "row:")
    {
        const LeRowId id = resolve_row_id(token);
        ref = LeObjectRef{.kind = LE_OBJECT_KIND_ROW, .index = id.index, .generation = id.generation};
    }
    else if (sv.substr(0, 10) == "placement:")
    {
        const LePlacementId id = resolve_placement_id(token);
        ref = LeObjectRef{.kind = LE_OBJECT_KIND_PLACEMENT, .index = id.index, .generation = id.generation};
    }
    else if (sv.substr(0, 7) == "region:")
    {
        const LeRegionId id = resolve_region_id(token);
        ref = LeObjectRef{.kind = LE_OBJECT_KIND_REGION, .index = id.index, .generation = id.generation};
    }
    else
    {
        // Unlike le_select_object_ref's own ERROR messages below (logged
        // via spdlog::error, api.cpp has real access to spdlog there) -
        // this shim has no compelling reason to log here too, so an
        // unrecognized prefix is surfaced by the caller instead: select
        // (le_tcl_procs.tcl) checks this return value and raises its own
        // clear Tcl error with the token text it already has.
        return 2; // distinct from 1 (a real, resolved-but-invalid/unsupported ref)
    }
    return le_select_object_ref(session(), ref);
}

void set_layer_selectable_cmd(const char *layer_name, bool selectable)
{
    le_set_layer_name_selectable(session(), layer_name, selectable);
}

bool get_layer_selectable_cmd(const char *layer_name)
{
    return le_is_layer_name_selectable(session(), layer_name);
}

void set_purpose_visible_cmd(int32_t purpose, bool visible)
{
    le_set_purpose_visible(session(), purpose, visible);
}

bool get_purpose_visible_cmd(int32_t purpose)
{
    return le_is_purpose_visible(session(), purpose);
}

void set_purpose_selectable_cmd(int32_t purpose, bool selectable)
{
    le_set_purpose_selectable(session(), purpose, selectable);
}

bool get_purpose_selectable_cmd(int32_t purpose)
{
    return le_is_purpose_selectable(session(), purpose);
}

void set_mode_cmd(int32_t mode)
{
    le_set_mode(session(), mode);
}

int32_t get_mode_cmd()
{
    return le_get_mode(session());
}

void clear_rulers_cmd()
{
    le_clear_rulers(session());
}

void select_all_cmd()
{
    le_select_all(session());
}

void deselect_all_cmd()
{
    le_deselect_all(session());
}

void arm_move_cmd()
{
    le_arm_move(session());
}

void set_placement_snap_mode_cmd(int mode)
{
    le_set_placement_snap_mode(session(), mode);
}

int get_placement_snap_mode_cmd()
{
    return le_get_placement_snap_mode(session());
}

bool is_placement_snap_mode_available_cmd(int mode)
{
    return le_is_placement_snap_mode_available(session(), mode) != 0;
}

int apply_placement_orientation_op_cmd(int op)
{
    return le_apply_placement_orientation_op(session(), op);
}

void arm_resize_cmd()
{
    le_arm_resize(session());
}

void set_shape_snap_mode_cmd(int kind, int mode)
{
    le_set_shape_snap_mode(session(), kind, mode);
}

int get_shape_snap_mode_cmd(int kind)
{
    return le_get_shape_snap_mode(session(), kind);
}

bool is_shape_snap_mode_available_cmd(int kind, int mode)
{
    return le_is_shape_snap_mode_available(session(), kind, mode) != 0;
}

void request_show_gui_cmd()
{
    le_request_show_gui(session());
}

void request_close_gui_cmd()
{
    le_request_close_gui(session());
}

int has_unsaved_database_changes_cmd()
{
    return le_has_unsaved_database_changes(session());
}

int has_unsaved_settings_cmd()
{
    return le_has_unsaved_settings(session());
}

void set_antialiasing_enabled_cmd(bool enabled)
{
    le_set_antialiasing_enabled(session(), enabled);
}

bool get_antialiasing_enabled_cmd()
{
    return le_is_antialiasing_enabled(session());
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
    // BUGS_AND_ENHANCEMENTS.md E21 - brace-nested {{ll_x ll_y} {ur_x
    // ur_y}}, matching every other Rect-shaped value's own convention
    // (create_shape's own -rects flag, get_properties' rects display) -
    // not the flat 4-number string this returned before.
    LeRectUm rect = le_shape_rect_at(session(), resolve_shape_id(id), index);
    std::ostringstream out;
    out << '{' << rect.ll_x_um << ' ' << rect.ll_y_um << "} {" << rect.ur_x_um << ' ' << rect.ur_y_um << '}';
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

// --- shape_* operations (NEW_FEATURES_SEPT_2026.md item 1) ---
//
// Shape token lists cross as one space-separated string (le_tcl_procs.tcl
// builds it with [join ...]) - the same plain word split write_lef_cmd's
// own abstract_tokens uses. An unknown shape token resolves to the invalid
// id, which le_shape_* then reports as an unknown shape; an unknown
// -layer/-parent token can't be told apart from an omitted one that way,
// so it's caught here instead and returned as its own status
// (kShapeOpBadLayer/kShapeOpBadParent) for the Tcl proc to report.

namespace
{
    constexpr int kShapeOpBadLayer = -2;
    constexpr int kShapeOpBadParent = -3;

    std::vector<LeShapeId> resolve_shape_tokens(const char *tokens)
    {
        std::vector<LeShapeId> ids;
        std::istringstream stream(tokens ? tokens : "");
        std::string token;
        while (stream >> token)
            ids.push_back(resolve_shape_id(token.c_str()));
        return ids;
    }

    bool token_given(const char *token)
    {
        return token && token[0];
    }

    // A -layer value: a real layer token, or `debug` (any case) for the
    // layer-less DEBUG purpose.
    struct LayerTarget
    {
        LeLayerId layer{.index = UINT32_MAX, .generation = 0};
        const char *purpose = nullptr;
    };

    bool is_debug_layer(const char *token)
    {
        std::string lower(token);
        for (char &c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return lower == "debug";
    }

    // nullopt = a layer token was given but doesn't resolve.
    std::optional<LayerTarget> resolve_optional_layer(const char *token)
    {
        if (!token_given(token))
            return LayerTarget{};
        if (is_debug_layer(token))
            return LayerTarget{.purpose = "DEBUG"};
        const LeLayerId id = resolve_layer_id(token);
        if (id.index == UINT32_MAX)
            return std::nullopt;
        return LayerTarget{.layer = id};
    }

    template <typename IdT>
    LeObjectRef object_ref(int32_t kind, IdT id)
    {
        return LeObjectRef{.kind = kind, .index = id.index, .generation = id.generation};
    }

    // nullopt = a parent token was given but isn't a supported, existing kind.
    std::optional<LeObjectRef> resolve_optional_parent(const char *token)
    {
        if (!token_given(token))
            return le_object_invalid_ref();
        const std::string_view sv(token);
        LeObjectRef ref = le_object_invalid_ref();
        if (sv.starts_with("abstract:"))
            ref = object_ref(LE_OBJECT_KIND_ABSTRACT, resolve_abstract_id(token));
        else if (sv.starts_with("layout:"))
            ref = object_ref(LE_OBJECT_KIND_LAYOUT, resolve_layout_id(token));
        else if (sv.starts_with("obstruction:"))
            ref = object_ref(LE_OBJECT_KIND_OBSTRUCTION, resolve_obstruction_id(token));
        else if (sv.starts_with("terminal_port:"))
            ref = object_ref(LE_OBJECT_KIND_TERMINAL_PORT, resolve_terminal_port_id(token));
        else if (sv.starts_with("route:"))
            ref = object_ref(LE_OBJECT_KIND_ROUTE, resolve_route_id(token));
        else if (sv.starts_with("blockage:"))
            ref = object_ref(LE_OBJECT_KIND_BLOCKAGE, resolve_blockage_id(token));
        else if (sv.starts_with("physical_port_segment:"))
            ref = object_ref(LE_OBJECT_KIND_PHYSICAL_PORT_SEGMENT, resolve_physical_port_segment_id(token));
        if (ref.index == UINT32_MAX)
            return std::nullopt;
        return ref;
    }

    // Shared by every creating shape_*_cmd: resolves -layer/-parent, then
    // hands the resolved shape ids to `call` (the matching le_shape_*).
    template <typename Call>
    int run_shape_op_cmd(const char *layer_token, const char *parent_token, Call &&call)
    {
        const std::optional<LayerTarget> target = resolve_optional_layer(layer_token);
        if (!target)
            return kShapeOpBadLayer;
        const std::optional<LeObjectRef> parent = resolve_optional_parent(parent_token);
        if (!parent)
            return kShapeOpBadParent;
        return call(target->layer, target->purpose, *parent);
    }

    const LeShapeId *data_or_null(const std::vector<LeShapeId> &ids)
    {
        return ids.empty() ? nullptr : ids.data();
    }
}

int shape_copy_cmd(const char *shape_tokens, const char *layer_token, const char *parent_token)
{
    const std::vector<LeShapeId> shapes = resolve_shape_tokens(shape_tokens);
    return run_shape_op_cmd(layer_token, parent_token, [&](LeLayerId layer, const char *purpose, LeObjectRef parent)
                            { return le_shape_copy(session(), data_or_null(shapes), static_cast<int32_t>(shapes.size()), layer, purpose, parent); });
}

int shape_boolean_cmd(const char *shape_tokens_a, const char *shape_tokens_b, int op, const char *layer_token, const char *parent_token)
{
    const std::vector<LeShapeId> a = resolve_shape_tokens(shape_tokens_a);
    const std::vector<LeShapeId> b = resolve_shape_tokens(shape_tokens_b);
    return run_shape_op_cmd(layer_token, parent_token, [&](LeLayerId layer, const char *purpose, LeObjectRef parent)
                            { return le_shape_boolean(session(), data_or_null(a), static_cast<int32_t>(a.size()), data_or_null(b),
                                                      static_cast<int32_t>(b.size()), op, layer, purpose, parent); });
}

int shape_to_polygon_cmd(const char *shape_tokens, const char *layer_token, const char *parent_token)
{
    const std::vector<LeShapeId> shapes = resolve_shape_tokens(shape_tokens);
    return run_shape_op_cmd(layer_token, parent_token, [&](LeLayerId layer, const char *purpose, LeObjectRef parent)
                            { return le_shape_to_polygon(session(), data_or_null(shapes), static_cast<int32_t>(shapes.size()), layer, purpose, parent); });
}

int shape_to_rects_cmd(const char *shape_tokens, int vertical, const char *layer_token, const char *parent_token)
{
    const std::vector<LeShapeId> shapes = resolve_shape_tokens(shape_tokens);
    return run_shape_op_cmd(layer_token, parent_token, [&](LeLayerId layer, const char *purpose, LeObjectRef parent)
                            { return le_shape_to_rects(session(), data_or_null(shapes), static_cast<int32_t>(shapes.size()), vertical, layer, purpose, parent); });
}

int shape_size_cmd(const char *shape_tokens, double dx_um, double dy_um, const char *layer_token, const char *parent_token)
{
    const std::vector<LeShapeId> shapes = resolve_shape_tokens(shape_tokens);
    return run_shape_op_cmd(layer_token, parent_token, [&](LeLayerId layer, const char *purpose, LeObjectRef parent)
                            { return le_shape_size(session(), data_or_null(shapes), static_cast<int32_t>(shapes.size()), dx_um, dy_um, layer, purpose, parent); });
}

int shape_path_cmd(const char *shape_tokens, double width_um, const char *layer_token, const char *parent_token)
{
    const std::vector<LeShapeId> shapes = resolve_shape_tokens(shape_tokens);
    return run_shape_op_cmd(layer_token, parent_token, [&](LeLayerId layer, const char *purpose, LeObjectRef parent)
                            { return le_shape_path(session(), data_or_null(shapes), static_cast<int32_t>(shapes.size()), width_um, layer, purpose, parent); });
}

int shape_change_layer_cmd(const char *shape_tokens, const char *layer_token)
{
    const std::optional<LayerTarget> target = resolve_optional_layer(layer_token);
    if (!target || (target->layer.index == UINT32_MAX && !target->purpose))
        return kShapeOpBadLayer;
    const std::vector<LeShapeId> shapes = resolve_shape_tokens(shape_tokens);
    return le_shape_change_layer(session(), data_or_null(shapes), static_cast<int32_t>(shapes.size()), target->layer, target->purpose);
}

const char *shape_op_results_cmd(int count)
{
    std::ostringstream out;
    for (int i = 0; i < count; ++i)
    {
        if (i > 0)
            out << ' ';
        out << format_shape_id(le_shape_op_result_at(session(), i));
    }
    return return_string(out.str());
}

const char *shape_bbox_cmd(const char *shape_tokens)
{
    const std::vector<LeShapeId> shapes = resolve_shape_tokens(shape_tokens);
    const LeShapeBbox box = le_shape_bbox(session(), data_or_null(shapes), static_cast<int32_t>(shapes.size()));
    if (!box.valid)
        return return_string("");
    // The {{llx lly} {urx ury}} Rect form every -bbox/-rects flag and
    // zoom_area take. std::to_chars gives the shortest exact round-trip
    // form (ostream's default 6 significant digits would truncate a large
    // micron coordinate).
    auto number = [](double value)
    {
        char buf[32];
        const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), value);
        return std::string(buf, end);
    };
    return return_string("{" + number(box.ll_x_um) + " " + number(box.ll_y_um) + "} {" + number(box.ur_x_um) + " " + number(box.ur_y_um) + "}");
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

int get_hierarchy_depth_command()
{
    return le_hierarchy_depth(session());
}

void set_hierarchy_depth_command(int depth)
{
    le_set_hierarchy_depth(session(), depth);
}

int get_flightline_max_fanout_command()
{
    return le_flightline_max_fanout(session());
}

void set_flightline_max_fanout_command(int max_fanout)
{
    le_set_flightline_max_fanout(session(), max_fanout);
}

double get_grid_spacing_um_command(int major)
{
    return le_grid_spacing_um(session(), major);
}

void set_grid_spacing_um_command(double minor_um, double major_um)
{
    le_set_grid_spacing_um(session(), minor_um, major_um);
}

double get_ruler_label_size_command()
{
    return le_ruler_label_size(session());
}

void set_ruler_label_size_command(double px)
{
    le_set_ruler_label_size(session(), px);
}

double get_label_min_size_command()
{
    return le_label_min_size(session());
}

void set_label_min_size_command(double px)
{
    le_set_label_min_size(session(), px);
}

double get_label_max_size_command()
{
    return le_label_max_size(session());
}

void set_label_max_size_command(double px)
{
    le_set_label_max_size(session(), px);
}

// NEW_FEATURES_SEPT_2026.md item 17 - layer colors as "#rrggbb" (the "#" is
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
    return le_set_layer_color(session(), layer, static_cast<int32_t>((value >> 16) & 0xff), static_cast<int32_t>((value >> 8) & 0xff),
                              static_cast<int32_t>(value & 0xff));
}

void reset_layer_color_command(const char *layer)
{
    le_reset_layer_color(session(), layer);
}

// "#rrggbb", or "" for a layer with no row.
const char *get_layer_color_command(const char *layer)
{
    const int32_t rgb = le_layer_color_rgb(session(), layer);
    if (rgb < 0)
        return return_string("");
    char text[8];
    std::snprintf(text, sizeof(text), "#%06x", static_cast<unsigned>(rgb));
    return return_string(text);
}

int save_settings_command(const char *path)
{
    return le_save_settings(session(), path);
}

int load_settings_command(const char *path)
{
    return le_load_settings(session(), path);
}

const char *default_settings_path_command()
{
    return le_default_settings_path();
}

int get_max_concurrency_command()
{
    return le_max_concurrency(session());
}

void set_max_concurrency_command(int max_concurrency)
{
    le_set_max_concurrency(session(), max_concurrency);
}
