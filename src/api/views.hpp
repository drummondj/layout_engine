#pragma once
// Layout Engine's C++ API: a Session (one editor instance) and the locked
// views whose members are its operations. A ReadView holds the session's
// lock shared, a WriteView exclusively, for as long as it lives, so no
// operation runs unlocked. The render thread uses a Renderer, which takes
// the locks it needs itself and never opens a view. Every front end - the
// GUI, the Tcl shim, extensions (le/extension.hpp) - calls this.

#include "../core/session_types.hpp"
#include "../database/database.hpp"
#include "../database/wire_helpers.hpp"
#include "../geometry/shape_op_types.hpp"
#include "generated/api/edit_types.hpp"
#include "interaction_types.hpp"
#include "generated/pipelines/view_layer_purpose.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @brief One editor instance's state; opaque outside the api module
/// (le_handle.hpp).
struct LeHandle;

namespace le
{
    /// @brief A point in microns.
    struct PointUm
    {
        double x = 0.0;
        double y = 0.0;
    };

    /// @brief A rect in microns: lower-left and upper-right corners.
    struct RectUm
    {
        PointUm ll;
        PointUm ur;
    };

    /// @brief Converts between microns and dbu at the session Technology's
    /// scale (DATABASE MICRONS), from ReadView/WriteView::units(). Microns
    /// round to the nearest dbu, as Tcl does.
    class Units
    {
    public:
        explicit Units(double dbu_per_um) : dbu_per_um_(dbu_per_um) {}

        double dbu_per_um() const { return dbu_per_um_; }

        int64_t to_dbu(double um) const { return static_cast<int64_t>(std::llround(um * dbu_per_um_)); }
        Point to_dbu(PointUm p) const { return Point{to_dbu(p.x), to_dbu(p.y)}; }
        Rect to_dbu(RectUm r) const { return Rect{to_dbu(r.ll), to_dbu(r.ur)}; }

        double to_um(int64_t dbu) const { return static_cast<double>(dbu) / dbu_per_um_; }
        PointUm to_um(Point p) const { return PointUm{to_um(p.x), to_um(p.y)}; }
        RectUm to_um(Rect r) const { return RectUm{to_um(r.ll), to_um(r.ur)}; }
        std::vector<PointUm> to_um(const Polygon &polygon) const
        {
            std::vector<PointUm> points;
            points.reserve(polygon.points.size());
            for (const Point p : polygon.points)
                points.push_back(to_um(p));
            return points;
        }

    private:
        double dbu_per_um_;
    };

    class ShapeBuilder;

    /// @brief New Shapes' ids, or a user-facing error.
    using ShapeOpResult = std::expected<std::vector<ShapeId>, std::string>;

    /// @brief A shared-locked, read-only view of the session for as long as
    /// it lives. Don't open a WriteView (or call a C API function that
    /// takes the session's lock) on the same thread while holding one.
    class ReadView
    {
    public:
        explicit ReadView(LeHandle *handle);
        /// @brief Doesn't wait: invalid if the lock isn't free right now.
        ReadView(LeHandle *handle, std::try_to_lock_t);
        ReadView(const ReadView &) = delete;
        ReadView &operator=(const ReadView &) = delete;

        bool valid() const { return lock_.owns_lock(); }
        /// @brief Only when valid().
        const Root &root() const { return *root_; }
        /// @brief Only when valid(): the bbox of `shapes` together, in dbu.
        std::expected<Rect, std::string> shape_bbox(const std::vector<GeometryId> &shapes) const;
        /// @brief Only when valid(): micron conversion; an error if no Technology has been read.
        std::expected<Units, std::string> units() const;
        /// @brief Only when valid(): the selection, in the order it was made.
        std::span<const SelectedObject> selection() const;
        /// @brief Only when valid(): bumped on every selection change.
        uint64_t selection_version() const;
        /// @brief Only when valid(): every ruler, the one being drawn (if any) last.
        std::span<const Ruler> rulers() const;
        /// @brief Only when valid(): what the design view shows and where.
        ViewState view() const;

        // Interaction state, only when valid(). Pixels have a top-left
        // origin, as the rendered frame does.

        InteractionMode mode() const;
        /// @brief How to use the current mode, for the status bar.
        std::string_view tooltip() const;
        bool move_armed() const;
        /// @brief Whether an armed Move has its start point (its first click).
        bool move_anchored() const;
        PlacementSnapMode placement_snap_mode() const;
        /// @brief Whether the open Layout has what `mode` snaps to.
        bool placement_snap_mode_available(PlacementSnapMode mode) const;
        /// @brief How many selected placements are FIXED or COVER.
        size_t selected_locked_placement_count() const;
        /// @brief Whether placements are selected and every one is FIXED.
        bool selected_placements_all_fixed() const;
        /// @brief A bit (1 << OrientationOp) per op every selected placement allows right now.
        uint32_t placement_orientation_ops_enabled() const;
        bool resize_armed() const;
        /// @brief Which way the edge under the mouse would move; nullopt if nothing grabbable is there.
        std::optional<ResizeAxis> resize_hover_axis() const;
        ShapeSnapMode shape_snap_mode(PieceKind kind) const;
        /// @brief Whether `mode` applies to `kind` and has something to snap to.
        bool shape_snap_mode_available(PieceKind kind, ShapeSnapMode mode) const;
        /// @brief A bit (1 << PieceKind) per kind of selected piece Resize can change.
        uint32_t selected_piece_kinds() const;
        /// @brief A bit (1 << PieceKind) per routing snap slot of a selected
        /// piece a Move snaps on its own (paths, vias and via arrays share PATH's).
        uint32_t selected_move_snap_piece_kinds() const;
        /// @brief The mouse position snapped to the grid, in dbu; nullopt
        /// if the mouse isn't over the design view.
        std::optional<Point> snapped_mouse_position() const;
        bool key_held(Key key) const;

        // The Layers panel, view options and settings, only when valid().
        // Lengths are dbu, except the micron grid spacing settings.json keeps.

        int hierarchy_depth() const;
        /// @brief The Layers panel's rows, in drawing order, as of the
        /// database now.
        std::vector<LayerRow> layer_rows() const;
        /// @brief Every purpose some row has a column for, in drawing order.
        std::vector<ViewLayerPurpose> purposes() const;
        /// @brief A row's outline color, with any the user picked; nullopt for an unknown row.
        std::optional<Color> layer_color(const std::string &layer) const;
        bool layer_visible(const std::string &layer) const;
        bool layer_selectable(const std::string &layer) const;
        bool purpose_visible(ViewLayerPurpose purpose) const;
        bool purpose_selectable(ViewLayerPurpose purpose) const;
        /// @brief The values `filter` can hide; valid while the view lives.
        std::span<const std::string> object_filter_values(ObjectFilter filter) const;
        bool object_filter_visible(ObjectFilter filter, const std::string &value) const;
        bool object_filter_selectable(ObjectFilter filter, const std::string &value) const;
        /// @brief Where dbu point `p` is shown to the user: relative to the
        /// open view's origin and mirrored as the view is, in dbu.
        std::array<double, 2> display_point(Point p) const;
        int max_concurrency() const;
        int flightline_max_fanout() const;
        bool confirm_overwrite() const;
        /// @brief The GUI theme, an index into kThemes (core/themes.hpp).
        size_t theme() const;
        int64_t minor_grid_spacing() const;
        int64_t major_grid_spacing() const;
        /// @brief A grid spacing in microns, including one set before any
        /// Technology could convert it; <= 0 if unset.
        double grid_spacing_um(bool major) const;
        /// @brief The Technology's MANUFACTURINGGRID in dbu, if it has one.
        std::optional<int64_t> manufacturing_grid() const;
        double ruler_label_size() const;
        double label_min_size() const;
        double label_max_size() const;
        size_t undo_limit() const;
        /// @brief Whether any setting changed since the settings were last saved or loaded.
        bool has_unsaved_settings() const;

        // Undo, the command history and the database file, only when valid().

        bool can_undo() const;
        bool can_redo() const;
        /// @brief How many commands the history holds, oldest first.
        size_t command_history_size() const;
        /// @brief Command `index` (< command_history_size), oldest first.
        const std::string &command_history_at(size_t index) const;
        /// @brief Whether the database changed since it was last read or
        /// written as a .led file.
        bool has_unsaved_database() const;
        /// @brief The .led file the database was last read from or written
        /// to; empty if none.
        const std::string &db_path() const;
        /// @brief Whether the database holds no objects (read_db needs an empty one).
        bool database_is_empty() const;

        // Reading the database by class, only when valid().

#include "generated/api/view_search_decls.inc"
        /// @brief `id`'s property table, as the Property Viewer and Tcl
        /// show it (lengths in microns); empty if it doesn't exist.
        std::vector<PropertyValue> properties(const ObjectId &id) const;
        /// @brief `id`'s owner; nullopt for a class with none or an id that doesn't exist.
        std::optional<ObjectId> parent(const ObjectId &id) const;
        /// @brief `path` (`.name`, or chained like `.terminal.name`, as in
        /// a -filter) read off `id`, in microns: nullopt if `id` doesn't
        /// exist or the path leads nowhere; an error if it doesn't parse or
        /// names no field.
        std::expected<std::optional<PropertyValue>, std::string> property(const ObjectId &id, const std::string &path) const;
        /// @brief The Instances a hierarchical `path` (`u1/u2/*`, `**` for
        /// any depth) names under `of`, or the current Schematic if `of`
        /// isn't valid, narrowed by a -filter expression.
        std::expected<std::vector<InstanceId>, std::string> get_instances_by_path(SchematicId of, const std::string &path,
                                                                                  const std::string &filter = {}) const;
        /// @brief get_instances_by_path for Nets.
        std::expected<std::vector<NetId>, std::string> get_nets_by_path(SchematicId of, const std::string &path, const std::string &filter = {}) const;
        /// @brief get_instances_by_path for Ports.
        std::expected<std::vector<PortId>, std::string> get_ports_by_path(SchematicId of, const std::string &path, const std::string &filter = {}) const;
        // Only when valid().
#include "generated/api/view_current_decls.inc"

    private:
        LeHandle *handle_;
        std::shared_lock<std::shared_mutex> lock_;
        const Root *root_;
    };

    /// @brief An exclusively locked, writable view of the session. Its
    /// create_<type>/update_<type>/delete_<type> and shape operations
    /// validate and are undoable: they record into the open undo step,
    /// which write(label) opens. Edits made through unrecorded_root() are
    /// neither - use it for bulk work such as importing. When the view ends
    /// it wakes the renderer, bumps the database's mutation version if
    /// unrecorded_root() was used (the members bump it as they edit), and
    /// then tells extensions' mutation listeners what changed.
    class WriteView
    {
    public:
        explicit WriteView(LeHandle *handle);
        /// @brief Also opens an undo step labelled `label`, closed when the
        /// view ends. Joins the step already open, if any (e.g. inside a
        /// typed Tcl command).
        WriteView(LeHandle *handle, const std::string &label);
        ~WriteView();
        WriteView(const WriteView &) = delete;
        WriteView &operator=(const WriteView &) = delete;

        /// @brief Records the undo step this view opened as failed.
        void fail() { succeeded_ = false; }

        /// @brief The database, read-only; edit it through the members below.
        const Root &root() const { return *root_; }
        /// @brief The database, writable directly: not validated, not
        /// recorded for undo, and a change made through a get_<type>()
        /// pointer reaches mutation listeners and incremental rendering only
        /// after note_<type>_changed().
        Root &unrecorded_root()
        {
            root_lent_ = true;
            return *root_;
        }
#include "generated/api/view_current_decls.inc"
#include "generated/api/view_edit_decls.inc"

        /// @brief Micron conversion; an error if no Technology has been read.
        std::expected<Units, std::string> units() const;
        /// @brief Builds a Shape owned by `owner` from geometry in microns;
        /// its create() calls create_shape.
        ShapeBuilder build_shape(ShapeOwner owner);

        // The shape operations, in dbu, on Shapes or Wires (a Wire read as
        // the Shape it converts to). New Shapes go to `parent`, else the
        // open view's Abstract/Layout; an unset `layer` keeps each input's own.

        /// @brief The bbox of `shapes` together.
        std::expected<Rect, std::string> shape_bbox(const std::vector<GeometryId> &shapes) const;
        /// @brief One new Shape per input, same geometry, on `layer`.
        ShapeOpResult shape_copy(const std::vector<GeometryId> &shapes, const shape_ops::LayerOrPurpose &layer,
                                 const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief One new Shape holding `op` of `a` against `b` (Not is a minus b), on a[0]'s layer unless `layer` is set.
        ShapeOpResult shape_boolean(const std::vector<GeometryId> &a, const std::vector<GeometryId> &b, BooleanOp op,
                                    const std::optional<shape_ops::LayerOrPurpose> &layer = std::nullopt,
                                    const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief One new polygon-only Shape per input.
        ShapeOpResult shape_to_polygons(const std::vector<GeometryId> &shapes, const std::optional<shape_ops::LayerOrPurpose> &layer = std::nullopt,
                                        const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief One new rect-only Shape per input, cut in `direction`.
        ShapeOpResult shape_to_rects(const std::vector<GeometryId> &shapes, FractureDirection direction,
                                     const std::optional<shape_ops::LayerOrPurpose> &layer = std::nullopt,
                                     const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief One new Shape per input, grown (positive) or shrunk (negative) by dx/dy.
        ShapeOpResult shape_size(const std::vector<GeometryId> &shapes, int64_t dx, int64_t dy,
                                 const std::optional<shape_ops::LayerOrPurpose> &layer = std::nullopt,
                                 const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief One new path-only Shape per input along its outline, `width` wide.
        ShapeOpResult shape_outline_paths(const std::vector<GeometryId> &shapes, int64_t width,
                                          const std::optional<shape_ops::LayerOrPurpose> &layer = std::nullopt,
                                          const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief Moves each shape in place onto `layer`; all or nothing.
        std::expected<void, std::string> shape_change_layer(const std::vector<GeometryId> &shapes, const shape_ops::LayerOrPurpose &layer);
        /// @brief Removes the rect, polygon or path (`kind`) at `index` from
        /// `shape` (a Wire has paths only), with its mask; later pieces'
        /// indexes shift down by one.
        std::expected<void, std::string> remove_shape_piece(GeometryId shape, PieceKind kind, size_t index);

        // Session state beyond the database: the selection, the design view
        // and rulers. Not undoable, and not a database change.

        /// @brief The selection, in the order it was made.
        std::span<const SelectedObject> selection() const;
        /// @brief Bumped on every selection change.
        uint64_t selection_version() const;
        /// @brief Selects every piece of a Shape, Wire, Route or
        /// PhysicalPort, or a whole Row, Placement or Region, adding to the
        /// selection; an error if it doesn't exist or has no geometry.
        std::expected<void, std::string> select(SelectableObject object);
        /// @brief Adds one entry - a single piece, or a whole Row, Placement
        /// or Region - to the selection; an error if it doesn't exist.
        std::expected<void, std::string> select_entry(const SelectedObject &entry);
        /// @brief Removes one entry from the selection, if it's there.
        void deselect(const SelectedObject &entry);
        void clear_selection();

        /// @brief What the design view shows and where.
        ViewState view() const;
        /// @brief Opens `design`'s Abstract in the design view, closing any
        /// Layout, and makes it the current Abstract; changing the view
        /// clears the selection and rulers.
        std::expected<void, std::string> open_abstract(DesignId design);
        /// @brief Opens `design`'s Layout, closing any Abstract, and makes
        /// it the current Layout.
        std::expected<void, std::string> open_layout(DesignId design);
        /// @brief Fits the open view's boundary (a Layout's DIEAREA, an
        /// Abstract's declared size) into the viewport.
        void fit_view(int padding_px = 0);
        /// @brief Fits `area` into the viewport.
        void fit_rect(Rect area, int padding_px = 0);
        /// @brief Zooms by `factor` (new scale = scale * (1 + factor), so
        /// 0.1 zooms in 10%) keeping pixel (x, y), top-left origin, fixed.
        void zoom(double factor, int x_px, int y_px);
        /// @brief Pans by a fraction of the viewport's size, toward
        /// increasing x/y (1.0 moves one viewport width).
        void pan(double x_factor, double y_factor);
        void set_view_flip(ViewFlip flip);

        /// @brief Every ruler, the one being drawn (if any) last.
        std::span<const Ruler> rulers() const;
        /// @brief Adds a finished ruler through `points` (at least 2).
        std::expected<void, std::string> add_ruler(std::span<const Point> points);
        /// @brief Finishes the ruler being drawn, if any.
        void finish_ruler();
        void clear_rulers();

        // Interaction: what the design view's mouse and keys do. Pixels
        // have a top-left origin, as the rendered frame does. Edits these
        // make (Move, Resize, Delete, rotate/flip) are undo steps of their
        // own, or join the open one.

        InteractionMode mode() const;
        /// @brief Switches mode; leaving Ruler finishes the ruler being
        /// drawn, leaving Edit disarms Move and Resize, and choosing Ruler
        /// again starts a new ruler.
        void set_mode(InteractionMode mode);
        /// @brief Arms Move for the selection: Edit mode only, and not for
        /// a mix of placements and shapes or a FIXED/COVER placement.
        void arm_move();
        void cancel_move();
        void set_placement_snap_mode(PlacementSnapMode mode);
        /// @brief Deletes the selected shape pieces; a Shape or Wire left
        /// with no geometry is deleted too. How many pieces it deleted.
        size_t delete_selected_pieces();
        /// @brief Rotates or flips every selected placement in place.
        std::expected<void, OrientationOpError> apply_placement_orientation_op(OrientationOp op);
        /// @brief Arms Resize: Edit mode, with selected pieces that have edges.
        void arm_resize();
        /// @brief Sets how `kind` pieces snap; false (changing nothing) if
        /// `mode` doesn't apply to that kind.
        bool set_shape_snap_mode(PieceKind kind, ShapeSnapMode mode);
        /// @brief Moves the mouse to pixel (x, y): the Move ghost, Resize
        /// hover and the ruler being drawn follow it.
        void set_mouse_position(int x_px, int y_px);
        /// @brief The mouse left the design view.
        void clear_mouse_position();
        /// @brief A key press: Ctrl and Shift are held until key_up; the
        /// others act now (interaction_types.hpp's Key).
        void key_down(Key key);
        void key_up(Key key);
        /// @brief Releases every key, e.g. when the window loses focus.
        void clear_keys();
        /// @brief Starts a click or drag at pixel (x, y).
        void mouse_down(int x_px, int y_px);
        /// @brief Starts a rectangle-zoom drag at pixel (x, y).
        void zoom_drag_down(int x_px, int y_px);
        void cancel_drag();
        /// @brief Ends the click or drag at pixel (x, y): a rectangle zoom,
        /// or by mode a click- or drag-select (Shift adds to the
        /// selection), a ruler point, or a Resize or Move click. Does
        /// nothing if no drag was started.
        void mouse_up(int x_px, int y_px);

        // The Layers panel, view options and settings (settings.json).
        // Lengths are dbu, except the micron grid spacing settings.json keeps.

        /// @brief How many Placement levels the design view descends (0
        /// draws each placement as its outline only).
        void set_hierarchy_depth(int depth);
        void set_layer_visible(const std::string &layer, bool visible);
        void set_layer_selectable(const std::string &layer, bool selectable);
        void set_purpose_visible(ViewLayerPurpose purpose, bool visible);
        void set_purpose_selectable(ViewLayerPurpose purpose, bool selectable);
        void set_object_filter_visible(ObjectFilter filter, const std::string &value, bool visible);
        void set_object_filter_selectable(ObjectFilter filter, const std::string &value, bool selectable);
        /// @brief Sizes the design view, each side clamped to 0..8192 pixels.
        void set_viewport_size(int width_px, int height_px);
        /// @brief Caps the threads rendering uses (at least 2).
        void set_max_concurrency(int max_concurrency);
        /// @brief Nets with more pins than this get no flightlines; negative is ignored.
        void set_flightline_max_fanout(int max_fanout);
        void set_confirm_overwrite(bool confirm);
        /// @brief Picks a theme by name; false (changing nothing) if there's none by that name.
        bool set_theme(std::string_view name);
        void set_minor_grid_spacing(int64_t dbu);
        void set_major_grid_spacing(int64_t dbu);
        /// @brief Sets the grid in microns (a value <= 0 leaves that one
        /// alone); before a Technology is read it waits to be converted.
        void set_grid_spacing_um(double minor_um, double major_um);
        void set_ruler_label_size(double px);
        void set_label_min_size(double px);
        void set_label_max_size(double px);
        /// @brief Draws row `layer` in `color` (opaque).
        void set_layer_color(const std::string &layer, Color color);
        /// @brief Back to the palette's color.
        void reset_layer_color(const std::string &layer);
        /// @brief Keeps at most `limit` undo steps, forgetting the oldest.
        void set_undo_limit(size_t limit);
        /// @brief Writes settings.json to `path`, or default_settings_path() if empty.
        std::expected<void, std::string> save_settings(const std::string &path = {});
        /// @brief Applies a settings.json (any format version) from `path`,
        /// or default_settings_path() if empty; keys this version doesn't
        /// know are kept for the next save.
        std::expected<void, std::string> load_settings(const std::string &path = {});

        /// @brief Undoes the newest undo step: whether there was one to
        /// undo, or (also logged) how many of its steps couldn't be applied.
        std::expected<bool, std::string> undo();
        /// @brief Redoes the newest undone step, as undo() reports.
        std::expected<bool, std::string> redo();
        /// @brief Opens an undo step labelled `label` that outlives this
        /// view, until end_command, and holds renders meanwhile: for a front
        /// end bracketing one command that spans several views.
        void begin_command(const std::string &label);
        /// @brief Closes the step begin_command opened (kept if it recorded
        /// anything), adds its label to the command history and releases
        /// renders.
        void end_command(bool succeeded);

        // Netlist edits that keep the Layout linked. Each is one undo step,
        // or part of the open one.

        /// @brief Deletes `net`, clearing its Pins', Ports' and
        /// PhysicalPorts' references to it and deleting its Routes.
        std::expected<void, std::string> delete_net_cascade(NetId net);
        /// @brief Renames `net` and the Routes and PhysicalPorts linked to
        /// it; an error, changing nothing, if any would take a name in use.
        std::expected<void, std::string> rename_net(NetId net, const std::string &name);
        /// @brief Renames `instance` and the Placements, Routes and
        /// PhysicalPorts linked to it, as rename_net does.
        std::expected<void, std::string> rename_instance(InstanceId instance, const std::string &name);
        /// @brief Resolves Instances whose design wasn't read yet (e.g. by a
        /// later LEF read) and links Layouts to their Schematics: how many
        /// Instances resolved. Not undoable, like the reads.
        std::expected<size_t, std::string> link_unresolved_instances();

        // Reading and writing files. Each also logs what it reports, as
        // read_lef and friends do in Tcl. A read is part of the open undo
        // step, if any, and saturates the change log (mutation listeners
        // get `everything`).

        /// @brief Reads a LEF file, its MACROs into the Library named
        /// `library` (created if missing).
        std::expected<void, std::string> read_lef(const std::string &path, const std::string &library);
        /// @brief Reads a DEF file, its DESIGN into the Library named `library`.
        std::expected<void, std::string> read_def(const std::string &path, const std::string &library);
        /// @brief Reads SystemVerilog/Verilog files into the Library named
        /// `library`: elaborated as a netlist, or syntax-only RTL.
        std::expected<void, std::string> read_verilog(std::span<const std::string> files, bool netlist, const std::string &library);
        /// @brief Loads a native database (.led) file; only into an empty database.
        std::expected<void, std::string> read_db(const std::string &path, bool with_session = false);
        /// @brief Writes a LEF file: `abstracts` if any, else every Abstract
        /// in `library` if valid, else the current Abstract (none for
        /// TECHNOLOGY_ONLY).
        std::expected<void, std::string> write_lef(const std::string &path, std::span<const AbstractId> abstracts, LibraryId library = {},
                                                   LefLayerWriteMode layers = LefLayerWriteMode::NONE);
        /// @brief Writes a DEF file for `layout`, or the current Layout if invalid.
        std::expected<void, std::string> write_def(const std::string &path, LayoutId layout = {});
        /// @brief Saves the whole database, and with `with_session` the
        /// session, to a native database (.led) file.
        std::expected<void, std::string> write_db(const std::string &path, bool with_session = false);
        /// @brief Writes stub Verilog modules for `library`'s Abstract-only
        /// Designs; an invalid `library` means the only one.
        std::expected<void, std::string> write_verilog_stubs(const std::string &path, LibraryId library = {});

    private:
        LeHandle *handle_;
        std::unique_lock<std::shared_mutex> lock_;
        Root *root_;
        uint64_t begin_; // the change log's end when the view opened
        bool owns_step_ = false;
        bool succeeded_ = true;
        bool root_lent_ = false; // unrecorded_root() was called
    };

    /// @brief Builds one Shape from geometry in microns, converted to dbu at
    /// the Technology's scale, then creates it through the WriteView it came
    /// from (WriteView::build_shape), undoably. Use it while that view is open.
    class ShapeBuilder
    {
    public:
        /// @brief Puts the shape on `layer` (clearing any purpose).
        ShapeBuilder &layer(LayerId layer);
        /// @brief Makes the shape layer-less, drawn on `purpose`'s row (e.g. DEBUG).
        ShapeBuilder &purpose(ShapePurpose purpose);
        /// @brief Adds a rect from (llx, lly) to (urx, ury).
        ShapeBuilder &rect(double llx, double lly, double urx, double ury);
        ShapeBuilder &rect(RectUm rect);
        /// @brief Adds a polygon (at least 3 points).
        ShapeBuilder &polygon(const std::vector<PointUm> &points);
        /// @brief Adds a path `width` wide through `points` (at least 2).
        ShapeBuilder &path(double width, const std::vector<PointUm> &points);

        /// @brief The Shape so far, in dbu.
        const ShapeData &data() const { return data_; }
        /// @brief Creates the Shape; the first error from building it, if
        /// any, or create_shape's.
        std::expected<ShapeId, std::string> create();

    private:
        friend class WriteView;
        ShapeBuilder(WriteView &view, ShapeOwner owner);
        // Records only the first error.
        void fail(std::string error);

        WriteView *view_;
        std::expected<Units, std::string> units_;
        ShapeData data_;
        std::string error_;
    };

    /// @brief Groups every edit made while it lives - through WriteViews
    /// and the C API alike - into one undo step labelled `label`, and holds
    /// renders until it ends. Recorded as succeeded unless fail() is
    /// called. Does nothing if a step is already open, e.g. inside a typed
    /// Tcl command. Takes the session's lock to open and close the step, so
    /// don't construct or destroy one while holding a view.
    class Transaction
    {
    public:
        Transaction(LeHandle *handle, const std::string &label);
        ~Transaction();
        Transaction(const Transaction &) = delete;
        Transaction &operator=(const Transaction &) = delete;

        void fail() { succeeded_ = false; }

    private:
        LeHandle *handle_;
        bool owns_ = false;
        bool succeeded_ = true;
    };

    /// @brief $HOME/.layout_engine/settings.json; empty if HOME isn't set.
    std::string default_settings_path();

    /// @brief The snake_case name of `id`'s class ("terminal_port"), as Tcl's friendly ids use it.
    std::string_view object_kind_name(const ObjectId &id);
    /// @brief Whether Tcl's friendly ids name `id`'s class by its name
    /// (`terminal:IN0`) rather than a number (`shape:12`).
    bool object_kind_is_named(const ObjectId &id);

    /// @brief Describes a native database (.led) file without loading it:
    /// its versions, extensions, the migrations it needs and its object
    /// counts; an error if it can't be read.
    std::expected<std::string, std::string> db_info(const std::string &path);
    /// @brief Rewrites the .led file `in_path` with this build's schema to
    /// `out_path`, describing what it did (with any warnings).
    std::expected<std::string, std::string> migrate_db(const std::string &in_path, const std::string &out_path);

    /// @brief Requests passed between a session's threads without its
    /// lock: Tcl's show_gui/close_gui to the GUI thread, and the GUI's Tcl
    /// commands to the console thread (so they enter the command history).
    class SessionRequests
    {
    public:
        explicit SessionRequests(LeHandle *handle) : handle_(handle) {}

        /// @brief Asks the GUI thread to open the design view window.
        void request_show_gui() const;
        /// @brief Whether a show request is pending, clearing it.
        bool take_show_gui_request() const;
        /// @brief Asks the GUI thread to close the window.
        void request_close_gui() const;
        bool take_close_gui_request() const;
        /// @brief Queues a Tcl command for the console thread to run.
        void enqueue_tcl_command(std::string command) const;
        /// @brief The oldest queued Tcl command, removed from the queue.
        std::optional<std::string> take_next_tcl_command() const;

    private:
        LeHandle *handle_;
    };

    /// @brief One rendered frame of the design view: premultiplied RGBA8888,
    /// row-major, top to bottom. `data` stays valid for as long as this
    /// frame (or a copy) lives, whatever is rendered meanwhile; row_bytes
    /// may exceed width * 4.
    struct RenderedFrame
    {
        const uint8_t *data = nullptr;
        int width = 0;
        int height = 0;
        size_t row_bytes = 0;
        /// @brief Owns the pixels `data` points into.
        std::shared_ptr<const void> pixels;

        bool empty() const { return data == nullptr; }
    };

    /// @brief The render thread's access to a session. Never opens a view:
    /// render() takes the session's lock shared for the render alone, so
    /// panels stay live, and the waits take no session lock at all.
    class Renderer
    {
    public:
        explicit Renderer(LeHandle *handle) : handle_(handle) {}

        /// @brief Blocks until something has changed since this last
        /// returned (any WriteView or other exclusive lock on the session),
        /// or cancel_wait() is called. While a Transaction or typed command
        /// holds renders, only cancel_wait() wakes it.
        void wait_until_needed() const;
        /// @brief Wakes wait_until_needed() without a change, so the render
        /// thread can check whether to stop. A level, not an edge: the next
        /// wait returns at once if nothing is waiting now.
        void cancel_wait() const;
        /// @brief Renders the design view at its viewport size; an empty
        /// frame before the viewport is sized. Recomputes only what changed
        /// since the last render.
        RenderedFrame render() const;
        /// @brief Whether a render is recomputing right now; lock-free.
        bool is_rendering() const;
        /// @brief Whether a Transaction or typed command is holding renders; lock-free.
        bool renders_held() const;

    private:
        LeHandle *handle_;
    };

    /// @brief One editor instance (database, view and interaction state),
    /// owned for as long as this lives. Its views and Renderer refer to it,
    /// so destroy it only once no other thread uses it.
    class Session
    {
    public:
        /// @brief A new, empty session: no database, nothing open, a 0x0 viewport.
        Session();
        ~Session();
        Session(Session &&other) noexcept;
        Session &operator=(Session &&other) noexcept;
        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        /// @brief The session's handle, for ExtensionContext and the C API;
        /// null once moved from or released.
        LeHandle *handle() const { return handle_; }

        ReadView read() const { return ReadView(handle_); }
        /// @brief Doesn't wait: invalid if the lock isn't free right now.
        ReadView try_read() const { return ReadView(handle_, std::try_to_lock); }
        WriteView write() { return WriteView(handle_); }
        /// @brief A WriteView whose edits are one undo step labelled `label`.
        WriteView write(const std::string &label) { return WriteView(handle_, label); }
        Transaction transaction(const std::string &label) { return Transaction(handle_, label); }
        Renderer renderer() const { return Renderer(handle_); }
        SessionRequests requests() const { return SessionRequests(handle_); }

        /// @brief Gives up ownership of the handle; destroy it with destroy().
        LeHandle *release();
        /// @brief Destroys a handle a Session released; a null one is ignored.
        static void destroy(LeHandle *handle);

    private:
        LeHandle *handle_;
    };
}
