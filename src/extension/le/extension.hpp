#pragma once
// The Layout Engine extension SDK: everything an extension's C++ may use.
// Anything reachable from here is public API, versioned by
// LE_EXTENSION_API_VERSION (set by the le::extension_sdk CMake target); an
// extension states the version it targets with a static_assert.

#include "api.hpp"
#include "core/session_types.hpp"
#include "database.hpp"
#include "database/wire_helpers.hpp"
#include "generated/api/edit_types.hpp"
#include "generated/api/id_conversions.hpp"
#include "geometry/shape_op_types.hpp"
#include "le/register_all.hpp"

#include <json.hpp>

#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

#ifndef LE_EXTENSION_API_VERSION
#error "LE_EXTENSION_API_VERSION is set by the le::extension_sdk CMake target - link it"
#endif

namespace le::ext
{
    class ExtensionContext;
    class OverlayContext;

    /// @brief One extension built into this process.
    struct ExtensionInfo
    {
        std::string name;
        std::string version;
        /// @brief Its directory - procs and resources live under it. Set by
        /// le_shell from extensions.json; empty elsewhere.
        std::string directory;
    };

    /// @brief An extension's section of settings.json:
    /// `"extensions": {"<name>": {"version": N, ...}}`, saved and loaded with
    /// the core settings (nlohmann::json, which the SDK provides). Both
    /// callbacks run with the session locked, so they may only use
    /// ctx.data<T>() - no read()/write() or le_* calls.
    struct SettingsSection
    {
        /// @brief The section's format version, written beside its keys.
        int version = 1;
        /// @brief The section's keys (a JSON object).
        nlohmann::json (*save)(ExtensionContext &ctx) = nullptr;
        /// @brief Applies a saved section written at `version`: whatever
        /// keys it has, so missing or unexpected ones must be tolerated;
        /// migrating an older version is the extension's to do.
        void (*load)(ExtensionContext &ctx, const nlohmann::json &section, int version) = nullptr;
    };

    /// @brief What changed in the database since mutation listeners were
    /// last called (Registry::add_mutation_listener).
    struct MutationEvent
    {
        /// @brief Too much changed to list - a file read, `link`, or more
        /// edits than Root's change log holds: treat everything as changed.
        bool everything = false;
        /// @brief Every create, update and delete, in order (Root's change
        /// log entries: the object, what happened, and its owner). Empty
        /// when `everything`. Valid only during the call.
        std::span<const ChangeLogEntry> changes;
    };

    /// @brief What the extensions built into this process registered.
    /// register_all() adds each extension's info, then calls its
    /// `le_ext_<name>_register(Registry &)` - in dependency order, once.
    class Registry
    {
    public:
        void add(ExtensionInfo info);
        const std::vector<ExtensionInfo> &extensions() const { return extensions_; }

        /// @brief Gives the extension being registered (the one added last)
        /// a settings.json section.
        void add_settings(SettingsSection section);
        /// @brief Every settings section, by extension name.
        const std::map<std::string, SettingsSection> &settings() const { return settings_; }

        /// @brief Draws on the design view, after Layout Engine's own
        /// overlays (le/extension_overlay.hpp). Redrawn whenever the view
        /// is, and on ExtensionContext::request_redraw.
        void add_overlay(void (*draw)(OverlayContext &ctx));
        /// @brief Every overlay: (extension name, draw), in registration order.
        const std::vector<std::pair<std::string, void (*)(OverlayContext &)>> &overlays() const { return overlays_; }

        /// @brief Calls `on_mutation` after the database changes: once per
        /// typed Tcl command, GUI edit or other C API call, after its lock
        /// is released - so it may read(), write() and call le_*. It runs on
        /// the thread that made the change, one call at a time per session.
        /// Changes it (or another listener) makes during the call aren't
        /// reported back to listeners; ones another thread makes meanwhile
        /// are, in a following call. Edits made through a mutable Root
        /// pointer are reported only if their maker calls
        /// note_<type>_changed().
        void add_mutation_listener(void (*on_mutation)(ExtensionContext &ctx, const MutationEvent &event));
        /// @brief Every mutation listener: (extension name, callback), in
        /// registration order.
        const std::vector<std::pair<std::string, void (*)(ExtensionContext &, const MutationEvent &)>> &mutation_listeners() const
        {
            return mutation_listeners_;
        }

        /// @brief Records where an extension's files are (le_shell does,
        /// from extensions.json).
        void set_directory(const std::string &extension, std::string directory);
        /// @brief `relative` under the extension's directory ("" if the
        /// directory isn't known); an absolute path is returned unchanged.
        std::string resource(const std::string &extension, const std::string &relative) const;

    private:
        std::vector<ExtensionInfo> extensions_;
        std::map<std::string, SettingsSection> settings_;
        std::vector<std::pair<std::string, void (*)(OverlayContext &)>> overlays_;
        std::vector<std::pair<std::string, void (*)(ExtensionContext &, const MutationEvent &)>> mutation_listeners_;
    };

    /// @brief This process's registry. Filled once by register_all() at
    /// startup and read-only afterwards.
    Registry &registry();

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
    /// round to the nearest dbu, as the C API and Tcl do.
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

    /// @brief A shared-locked, read-only view of the handle's database for
    /// as long as it lives. Don't call le_* functions that take the handle's
    /// lock while holding one.
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
#include "generated/api/extension_current_decls.inc"

    private:
        LeHandle *handle_;
        std::shared_lock<std::shared_mutex> lock_;
        const Root *root_;
    };

    /// @brief An exclusively locked, writable view of the handle's database.
    /// Its create_<type>/update_<type>/delete_<type> and shape operations
    /// validate and are undoable: they record into the open undo step,
    /// which write(label) opens. Edits made through unrecorded_root() are
    /// neither - use it for bulk work such as importing. When the view ends
    /// it wakes the renderer, and bumps the database's mutation version if
    /// unrecorded_root() was used (the members bump it as they edit).
    /// Don't call le_* functions while holding one.
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
#include "generated/api/extension_current_decls.inc"
#include "generated/api/extension_edit_decls.inc"

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

    /// @brief Groups the C API edits made while it lives into one undo step
    /// labelled `label` (le_begin_command/le_end_command). Recorded as
    /// succeeded unless fail() is called. Does nothing if one is already
    /// open, e.g. when the extension runs inside a typed Tcl command.
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

    /// @brief An extension's access to one session (LeHandle).
    class ExtensionContext
    {
    public:
        ExtensionContext(LeHandle *handle, std::string_view extension_name);

        /// @brief For C API calls (le_*), e.g. undoable edits.
        LeHandle *handle() const { return handle_; }
        ReadView read() const { return ReadView(handle_); }
        WriteView write() { return WriteView(handle_); }
        /// @brief A WriteView whose edits are one undo step labelled `label`.
        WriteView write(const std::string &label) { return WriteView(handle_, label); }
        Transaction transaction(const std::string &label) { return Transaction(handle_, label); }
        // These take the session's lock for the read: inside a read() or
        // write(), use the view's own accessors instead.
#include "generated/api/extension_current_decls.inc"

        /// @brief Redraws the design view's overlays - for when what an
        /// overlay draws changed without the database changing (its data<T>()).
        void request_redraw();

        /// @brief This extension's state of type T for this session, created
        /// (default-constructed) on first use and destroyed with the session.
        /// Unsynchronized: when another thread may use the same T at the
        /// same time (an overlay on the render thread, a GUI callback, a Tcl
        /// command on the console thread), use with_data<T>() instead.
        template <class T>
        T &data()
        {
            return *static_cast<T *>(data_slot(typeid(T).name(), &make_data<T>));
        }

        /// @brief Calls `fn(T &)` on data<T>() with it locked against every
        /// other with_data<T>() for this extension and session, on any
        /// thread, and returns what `fn` returns. Don't call with_data<T>()
        /// for the same T inside `fn`.
        template <class T, class Fn>
        decltype(auto) with_data(Fn &&fn)
        {
            std::mutex *mutex = nullptr;
            T &value = *static_cast<T *>(data_slot(typeid(T).name(), &make_data<T>, &mutex));
            std::lock_guard<std::mutex> lock(*mutex);
            return std::forward<Fn>(fn)(value);
        }

    private:
        template <class T>
        static std::shared_ptr<void> make_data()
        {
            return std::make_shared<T>();
        }

        void *data_slot(const char *type_name, std::shared_ptr<void> (*make)());
        void *data_slot(const char *type_name, std::shared_ptr<void> (*make)(), std::mutex **mutex);

        LeHandle *handle_;
        std::string extension_name_;
    };
}
