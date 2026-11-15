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
#include "generated/pipelines/view_layer_purpose.hpp"

#include <array>
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

    // --- The Layers panel, view options and settings ---
    // Settings are what settings.json saves (save_settings/load_settings).

    int hierarchy_depth(const LeHandle &handle);
    /// @brief How many Placement levels the design view descends (0 draws
    /// each placement as its outline only).
    void set_hierarchy_depth(LeHandle &handle, int depth);
    /// @brief The Layers panel's rows, in drawing order, as of the database
    /// now (built for this call if an edit has outdated the handle's).
    std::vector<LayerRow> layer_rows(const LeHandle &handle);
    /// @brief Every purpose some row has a column for, in drawing order.
    std::vector<ViewLayerPurpose> purposes(const LeHandle &handle);
    /// @brief A row's outline color, with any the user picked; nullopt for an unknown row.
    std::optional<Color> layer_color(const LeHandle &handle, const std::string &layer);
    bool layer_visible(const LeHandle &handle, const std::string &layer);
    void set_layer_visible(LeHandle &handle, const std::string &layer, bool visible);
    bool layer_selectable(const LeHandle &handle, const std::string &layer);
    void set_layer_selectable(LeHandle &handle, const std::string &layer, bool selectable);
    bool purpose_visible(const LeHandle &handle, ViewLayerPurpose purpose);
    void set_purpose_visible(LeHandle &handle, ViewLayerPurpose purpose, bool visible);
    bool purpose_selectable(const LeHandle &handle, ViewLayerPurpose purpose);
    void set_purpose_selectable(LeHandle &handle, ViewLayerPurpose purpose, bool selectable);
    /// @brief The values `filter` can hide, as named in the Layers panel;
    /// valid while the lock is held.
    std::span<const std::string> object_filter_values(const LeHandle &handle, ObjectFilter filter);
    bool object_filter_visible(const LeHandle &handle, ObjectFilter filter, const std::string &value);
    void set_object_filter_visible(LeHandle &handle, ObjectFilter filter, const std::string &value, bool visible);
    bool object_filter_selectable(const LeHandle &handle, ObjectFilter filter, const std::string &value);
    void set_object_filter_selectable(LeHandle &handle, ObjectFilter filter, const std::string &value, bool selectable);
    /// @brief Sizes the design view, each side clamped to 0..8192 pixels.
    void set_viewport_size(LeHandle &handle, int width_px, int height_px);
    /// @brief Where dbu point `p` is shown to the user: relative to the
    /// open view's origin and mirrored as the view is, in dbu.
    std::array<double, 2> display_point(const LeHandle &handle, Point p);

    int max_concurrency(const LeHandle &handle);
    /// @brief Caps the threads rendering uses (at least 2).
    void set_max_concurrency(LeHandle &handle, int max_concurrency);
    int flightline_max_fanout(const LeHandle &handle);
    /// @brief Nets with more pins than this get no flightlines; negative is ignored.
    void set_flightline_max_fanout(LeHandle &handle, int max_fanout);
    bool confirm_overwrite(const LeHandle &handle);
    void set_confirm_overwrite(LeHandle &handle, bool confirm);
    /// @brief The GUI theme, an index into kThemes (core/themes.hpp).
    size_t theme(const LeHandle &handle);
    /// @brief Picks a theme by name; false (changing nothing) if there's none by that name.
    bool set_theme(LeHandle &handle, std::string_view name);
    int64_t minor_grid_spacing(const LeHandle &handle);
    int64_t major_grid_spacing(const LeHandle &handle);
    void set_minor_grid_spacing(LeHandle &handle, int64_t dbu);
    void set_major_grid_spacing(LeHandle &handle, int64_t dbu);
    /// @brief A grid spacing in microns, including one set before any
    /// Technology could convert it; <= 0 if unset.
    double grid_spacing_um(const LeHandle &handle, bool major);
    /// @brief Sets the grid in microns (a value <= 0 leaves that one
    /// alone); before a Technology is read it waits to be converted.
    void set_grid_spacing_um(LeHandle &handle, double minor_um, double major_um);
    /// @brief The Technology's MANUFACTURINGGRID in dbu, if it has one.
    std::optional<int64_t> manufacturing_grid(const LeHandle &handle);
    double ruler_label_size(const LeHandle &handle);
    void set_ruler_label_size(LeHandle &handle, double px);
    double label_min_size(const LeHandle &handle);
    void set_label_min_size(LeHandle &handle, double px);
    double label_max_size(const LeHandle &handle);
    void set_label_max_size(LeHandle &handle, double px);
    /// @brief Draws row `layer` in `color` (opaque).
    void set_layer_color(LeHandle &handle, const std::string &layer, Color color);
    /// @brief Back to the palette's color.
    void reset_layer_color(LeHandle &handle, const std::string &layer);
    size_t undo_limit(const LeHandle &handle);
    /// @brief Keeps at most `limit` undo steps, forgetting the oldest.
    void set_undo_limit(LeHandle &handle, size_t limit);
    /// @brief Whether any setting changed since the settings were last saved or loaded.
    bool has_unsaved_settings(const LeHandle &handle);
    /// @brief Writes settings.json to `path`, or default_settings_path() if empty.
    std::expected<void, std::string> save_settings(LeHandle &handle, const std::string &path);
    /// @brief Applies a settings.json (any format version) from `path`, or
    /// default_settings_path() if empty; keys this version doesn't know are
    /// kept for the next save.
    std::expected<void, std::string> load_settings(LeHandle &handle, const std::string &path);

    // --- Undo and the command history ---

    /// @brief Opens an undo step labelled `label` and holds renders until
    /// end_command; a no-op for the undo step if one is already open.
    void begin_command(LeHandle &handle, const std::string &label);
    /// @brief Closes the step begin_command opened, pushing it if it
    /// recorded anything, adds its label to the command history, and
    /// releases renders.
    void end_command(LeHandle &handle, bool succeeded);
    /// @brief Undoes the newest step: whether there was one, or (logged)
    /// how many of its steps couldn't be applied.
    std::expected<bool, std::string> undo(LeHandle &handle);
    std::expected<bool, std::string> redo(LeHandle &handle);
    bool can_undo(const LeHandle &handle);
    bool can_redo(const LeHandle &handle);
    /// @brief How many commands the history holds, oldest first.
    size_t command_history_size(const LeHandle &handle);
    /// @brief Command `index` (< command_history_size), oldest first.
    const std::string &command_history_at(const LeHandle &handle, size_t index);

    // --- The database file ---

    /// @brief Whether the database changed since it was last read or written as a .led file.
    bool has_unsaved_database(const LeHandle &handle);
    /// @brief The .led file the database was last read from or written to; empty if none.
    const std::string &db_path(const LeHandle &handle);
    /// @brief Whether the database holds no objects (read_db needs an empty one).
    bool database_is_empty(const LeHandle &handle);

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
