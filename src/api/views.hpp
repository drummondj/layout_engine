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

        /// @brief Gives up ownership of the handle; destroy it with destroy().
        LeHandle *release();
        /// @brief Destroys a handle a Session released; a null one is ignored.
        static void destroy(LeHandle *handle);

    private:
        LeHandle *handle_;
    };
}
