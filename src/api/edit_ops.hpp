#pragma once
// The C++ half of the C API's edits: validate, change the Root, bump its
// mutation version and record undo, in native types (database ids, dbu,
// std::expected errors). The le_* functions convert their C arguments and
// call these under the handle's lock; extensions reach them through
// le::WriteView (views.hpp). Every function here expects the caller to hold the
// handle's exclusive lock.

#include "api.hpp"
#include "database.hpp"
#include "generated/api/edit_types.hpp"
#include "../database/wire_helpers.hpp"
#include "../core/session_types.hpp"
#include "../geometry/shape_op_types.hpp"
#include "interaction_types.hpp"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace le::edit
{
    /// @brief update_placement's check (Klass.update_check): a FIXED or
    /// COVER placement's location and orientation can't change unless the
    /// same update unlocks it.
    std::optional<std::string> check_placement_update(const Root &root, const PlacementData &before, const PlacementChanges &changes);

    /// @brief create_instance's check (Klass.create_check): an Instance
    /// can't make its Design instantiate itself, directly or through
    /// its descendants.
    std::optional<std::string> check_instance_create(const Root &root, const InstanceData &data);

    /// @brief update_instance's check (Klass.update_check): the same rule
    /// as check_instance_create, for a new reference_design or schematic.
    std::optional<std::string> check_instance_update(const Root &root, const InstanceData &before, const InstanceChanges &changes);

#include "generated/api/edit_ops_decls.inc"

    /// @brief The Technology's dbu per micron (DATABASE MICRONS), if one
    /// with a positive scale has been read. The session has one Technology.
    std::optional<double> dbu_per_um(const Root &root);

    /// @brief New Shapes' ids, or a user-facing error.
    using ShapeOpResult = std::expected<std::vector<ShapeId>, std::string>;

    // The shape_* operations. Each new Shape goes to `parent`, else the
    // open view's Abstract/Layout (the GUI's, then the current one);
    // an unset `layer` keeps each input's own.

    /// @brief One new Shape per input, same geometry, on `layer`.
    ShapeOpResult shape_copy(LeHandle &handle, const std::vector<GeometryId> &shapes, const shape_ops::LayerOrPurpose &layer,
                             const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new Shape holding `op` of `a` against `b`, on a[0]'s layer unless `layer` is set.
    ShapeOpResult shape_boolean(LeHandle &handle, const std::vector<GeometryId> &a, const std::vector<GeometryId> &b, BooleanOp op,
                                const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new polygon-only Shape per input.
    ShapeOpResult shape_to_polygons(LeHandle &handle, const std::vector<GeometryId> &shapes, const std::optional<shape_ops::LayerOrPurpose> &layer,
                                    const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new rect-only Shape per input, cut in `direction`.
    ShapeOpResult shape_to_rects(LeHandle &handle, const std::vector<GeometryId> &shapes, FractureDirection direction,
                                 const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new Shape per input, grown (positive) or shrunk (negative) by dx/dy dbu.
    ShapeOpResult shape_size(LeHandle &handle, const std::vector<GeometryId> &shapes, int64_t dx, int64_t dy,
                             const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new path-only Shape per input along its outline, `width` dbu wide.
    ShapeOpResult shape_outline_paths(LeHandle &handle, const std::vector<GeometryId> &shapes, int64_t width,
                                      const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief Moves each shape in place onto `layer`; all or nothing.
    std::expected<void, std::string> shape_change_layer(LeHandle &handle, const std::vector<GeometryId> &shapes, const shape_ops::LayerOrPurpose &layer);

    /// @brief Removes the rect, polygon or path at `index` from `shape` (a
    /// Wire has paths only), with its mask if it has one; later pieces'
    /// indexes shift down by one.
    std::expected<void, std::string> remove_shape_piece(LeHandle &handle, GeometryId shape, PieceKind kind, size_t index);

    // --- Selection ---
    // Neither undoable nor a database change: selection is session state.

    /// @brief Selects every piece of a Shape, Wire, Route or PhysicalPort,
    /// or a whole Row, Placement or Region, adding to the selection. An
    /// error if it doesn't exist or has no geometry.
    std::expected<void, std::string> select(LeHandle &handle, SelectableObject object);
    /// @brief Adds one entry - a single piece, or a whole Row, Placement or
    /// Region - to the selection. An error if it doesn't exist.
    std::expected<void, std::string> select_entry(LeHandle &handle, const SelectedObject &entry);
    /// @brief Removes one entry from the selection, if it's there.
    void deselect_entry(LeHandle &handle, const SelectedObject &entry);
    void clear_selection(LeHandle &handle);

    // --- The design view ---

    /// @brief Opens `design`'s Abstract in the design view, closing any
    /// Layout, and makes it the current Abstract; the selection and rulers
    /// clear when the view changes. A Design with no Abstract opens nothing.
    std::expected<void, std::string> open_abstract(LeHandle &handle, DesignId design);
    /// @brief The same for `design`'s Layout.
    std::expected<void, std::string> open_layout(LeHandle &handle, DesignId design);
    /// @brief What the design view shows and where.
    ViewState view_state(const LeHandle &handle);
    /// @brief Fits the open view's boundary (a Layout's DIEAREA, an
    /// Abstract's declared size) into the viewport with `padding_px` around it.
    void fit_view(LeHandle &handle, int padding_px);
    /// @brief Fits `area` (dbu) into the viewport with `padding_px` around it.
    void fit_rect(LeHandle &handle, Rect area, int padding_px);
    /// @brief Zooms by `factor` (new scale = scale * (1 + factor)) about
    /// pixel (x, y), top-left origin; a factor <= -1 does nothing.
    void zoom(LeHandle &handle, double factor, int x_px, int y_px);
    /// @brief Pans by a fraction of the viewport's size, toward increasing
    /// dbu x/y.
    void pan(LeHandle &handle, double x_factor, double y_factor);
    void set_view_flip(LeHandle &handle, ViewFlip flip);

    // --- Rulers ---

    /// @brief Adds a finished ruler through `points` (dbu, at least 2).
    std::expected<void, std::string> add_ruler(LeHandle &handle, std::span<const Point> points);
    /// @brief Finishes the ruler being drawn, if any.
    void finish_ruler(LeHandle &handle);
    void clear_rulers(LeHandle &handle);

    // --- Interaction: modes, Move, Resize, Delete, the mouse and keys ---
    // Pixel coordinates have a top-left origin, as the rendered frame does.

    InteractionMode mode(const LeHandle &handle);
    /// @brief Switches mode; leaving Ruler finishes the ruler being drawn,
    /// leaving Edit disarms Move and Resize, and choosing Ruler again
    /// starts a new ruler.
    void set_mode(LeHandle &handle, InteractionMode mode);
    /// @brief How to use the current mode, for the status bar; static storage.
    std::string_view tooltip(const LeHandle &handle);

    /// @brief Arms Move for the selection: Edit mode only, and not for a
    /// mix of placements and shapes or a FIXED/COVER placement.
    void arm_move(LeHandle &handle);
    void cancel_move(LeHandle &handle);
    bool move_armed(const LeHandle &handle);
    /// @brief Whether an armed Move has its start point (its first click).
    bool move_anchored(const LeHandle &handle);
    void set_placement_snap_mode(LeHandle &handle, PlacementSnapMode mode);
    PlacementSnapMode placement_snap_mode(const LeHandle &handle);
    /// @brief Whether the open Layout has what `mode` snaps to.
    bool placement_snap_mode_available(const LeHandle &handle, PlacementSnapMode mode);

    /// @brief Deletes the selected shape pieces as one undo step; a Shape or
    /// Wire left with no geometry is deleted too. How many pieces it deleted.
    size_t delete_selected_pieces(LeHandle &handle);

    /// @brief How many selected placements are FIXED or COVER.
    size_t selected_locked_placement_count(const LeHandle &handle);
    /// @brief Whether placements are selected and every one is FIXED.
    bool selected_placements_all_fixed(const LeHandle &handle);
    /// @brief A bit (1 << OrientationOp) per op every selected placement
    /// allows right now.
    uint32_t placement_orientation_ops_enabled(const LeHandle &handle);
    /// @brief Rotates or flips every selected placement in place, as one undo step.
    std::expected<void, OrientationOpError> apply_placement_orientation_op(LeHandle &handle, OrientationOp op);
    /// @brief A user-facing reason for `error`.
    std::string_view describe(OrientationOpError error);

    /// @brief Arms Resize: Edit mode with selected pieces that have edges.
    void arm_resize(LeHandle &handle);
    bool resize_armed(const LeHandle &handle);
    /// @brief Which way the edge under the mouse would move; nullopt if
    /// nothing grabbable is there.
    std::optional<ResizeAxis> resize_hover_axis(const LeHandle &handle);
    /// @brief Sets how `kind` pieces snap; false (changing nothing) if
    /// `mode` doesn't apply to that kind.
    bool set_shape_snap_mode(LeHandle &handle, PieceKind kind, ShapeSnapMode mode);
    ShapeSnapMode shape_snap_mode(const LeHandle &handle, PieceKind kind);
    /// @brief Whether `mode` applies to `kind` and has something to snap to
    /// (TRACKS: a selected piece of that kind on a layer with tracks).
    bool shape_snap_mode_available(const LeHandle &handle, PieceKind kind, ShapeSnapMode mode);
    /// @brief A bit (1 << PieceKind) per kind of selected piece Resize can change.
    uint32_t selected_piece_kinds(const LeHandle &handle);
    /// @brief A bit (1 << snap slot's PieceKind) per kind of selected piece
    /// that a Move snaps on its own (paths, vias and via arrays).
    uint32_t selected_move_snap_piece_kinds(const LeHandle &handle);

    /// @brief Moves the mouse to pixel (x, y), updating the Move ghost,
    /// Resize hover and the ruler being drawn.
    void set_mouse_position(LeHandle &handle, int32_t x, int32_t y);
    /// @brief The mouse left the design view.
    void clear_mouse_position(LeHandle &handle);
    /// @brief The mouse position snapped to the grid, in dbu; nullopt if
    /// the mouse isn't over the design view.
    std::optional<Point> snapped_mouse_position(const LeHandle &handle);

    void key_down(LeHandle &handle, Key key);
    void key_up(LeHandle &handle, Key key);
    bool key_held(const LeHandle &handle, Key key);
    /// @brief Releases every key, e.g. when the window loses focus.
    void clear_keys(LeHandle &handle);

    /// @brief Starts a click or drag at pixel (x, y).
    void mouse_down(LeHandle &handle, int32_t x, int32_t y);
    /// @brief Starts a rectangle-zoom drag at pixel (x, y).
    void zoom_drag_down(LeHandle &handle, int32_t x, int32_t y);
    void cancel_drag(LeHandle &handle);
    /// @brief Ends the click or drag at pixel (x, y): a rectangle zoom, or
    /// by mode a click- or drag-select, a ruler point, or a Resize or Move
    /// click. Does nothing if no drag was started.
    void mouse_up(LeHandle &handle, int32_t x, int32_t y);

    // --- Files ---
    // Each logs what it reports, as the C API always has (the readers and
    // writers log as they go), and returns the error text too: the reader's
    // or writer's ERROR lines, or its own reason. A read records into the
    // open undo step, if any.

    /// @brief Reads a LEF file, its MACROs into the Library named `library`
    /// (created if missing).
    std::expected<void, std::string> read_lef(LeHandle &handle, const std::string &path, const std::string &library);
    /// @brief Reads a DEF file, its DESIGN into the Library named `library`.
    std::expected<void, std::string> read_def(LeHandle &handle, const std::string &path, const std::string &library);
    /// @brief Reads SystemVerilog/Verilog files into the Library named
    /// `library`: elaborated as a netlist, or syntax-only RTL.
    std::expected<void, std::string> read_verilog(LeHandle &handle, std::span<const std::string> files, bool netlist,
                                                  const std::string &library);
    /// @brief Loads a native database file into an empty database.
    std::expected<void, std::string> read_db(LeHandle &handle, const std::string &path, bool with_session);
    /// @brief Writes a LEF file: `abstracts` if any, else every Abstract in
    /// `library` if valid, else the current Abstract (none of them for
    /// TECHNOLOGY_ONLY).
    std::expected<void, std::string> write_lef(LeHandle &handle, const std::string &path, std::span<const AbstractId> abstracts,
                                               LibraryId library, LefLayerWriteMode layers);
    /// @brief Writes a DEF file for `layout`, or the current Layout if invalid.
    std::expected<void, std::string> write_def(LeHandle &handle, const std::string &path, LayoutId layout);
    /// @brief Saves the whole database (and, with `with_session`, the
    /// session) to a native database file.
    std::expected<void, std::string> write_db(LeHandle &handle, const std::string &path, bool with_session);
    /// @brief Writes stub Verilog modules for `library`'s Abstract-only
    /// Designs; an invalid `library` means the only Library.
    std::expected<void, std::string> write_verilog_stubs(LeHandle &handle, const std::string &path, LibraryId library);
}
