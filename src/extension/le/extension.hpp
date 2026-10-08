#pragma once
// The Layout Engine extension SDK: everything an extension's C++ may use.
// Anything reachable from here is public API, versioned by
// LE_EXTENSION_API_VERSION (set by the le::extension_sdk CMake target); an
// extension states the version it targets with a static_assert. Design:
// docs/EXTENSION_MECHANISM_RESEARCH.md §3, §5.

#include "api.hpp"
#include "database.hpp"
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
#include <string>
#include <string_view>
#include <typeinfo>
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
        std::expected<Rect, std::string> shape_bbox(const std::vector<ShapeId> &shapes) const;
        /// @brief Only when valid(): micron conversion; an error if no Technology has been read.
        std::expected<Units, std::string> units() const;
        // Only when valid().
#include "generated/api/extension_current_decls.inc"

    private:
        LeHandle *handle_;
        std::shared_lock<std::shared_mutex> lock_;
        const Root *root_;
    };

    /// @brief An exclusively locked, writable view of the handle's database.
    /// Its create_<type>/update_<type>/delete_<type> are undoable: they
    /// record into the open undo step, which write(label) opens. Edits made
    /// straight through root() are NOT undoable - use it for bulk work such
    /// as importing. When the view ends it bumps the database's mutation
    /// version and wakes the renderer. Don't call le_* functions while
    /// holding one.
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

        Root &root() { return *root_; }
#include "generated/api/extension_current_decls.inc"
#include "generated/api/extension_edit_decls.inc"

        /// @brief Micron conversion; an error if no Technology has been read.
        std::expected<Units, std::string> units() const;
        /// @brief Builds a Shape owned by `owner` from geometry in microns;
        /// its create() calls create_shape.
        ShapeBuilder build_shape(ShapeOwner owner);

        // The shape operations, in dbu. New Shapes go to `parent`, else the
        // open view's Abstract/Layout; an unset `layer` keeps each input's own.

        /// @brief The bbox of `shapes` together.
        std::expected<Rect, std::string> shape_bbox(const std::vector<ShapeId> &shapes) const;
        /// @brief One new Shape per input, same geometry, on `layer`.
        ShapeOpResult shape_copy(const std::vector<ShapeId> &shapes, const shape_ops::LayerOrPurpose &layer,
                                 const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief One new Shape holding `op` of `a` against `b` (Not is a minus b), on a[0]'s layer unless `layer` is set.
        ShapeOpResult shape_boolean(const std::vector<ShapeId> &a, const std::vector<ShapeId> &b, BooleanOp op,
                                    const std::optional<shape_ops::LayerOrPurpose> &layer = std::nullopt,
                                    const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief One new polygon-only Shape per input.
        ShapeOpResult shape_to_polygons(const std::vector<ShapeId> &shapes, const std::optional<shape_ops::LayerOrPurpose> &layer = std::nullopt,
                                        const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief One new rect-only Shape per input, cut in `direction`.
        ShapeOpResult shape_to_rects(const std::vector<ShapeId> &shapes, FractureDirection direction,
                                     const std::optional<shape_ops::LayerOrPurpose> &layer = std::nullopt,
                                     const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief One new Shape per input, grown (positive) or shrunk (negative) by dx/dy.
        ShapeOpResult shape_size(const std::vector<ShapeId> &shapes, int64_t dx, int64_t dy,
                                 const std::optional<shape_ops::LayerOrPurpose> &layer = std::nullopt,
                                 const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief One new path-only Shape per input along its outline, `width` wide.
        ShapeOpResult shape_outline_paths(const std::vector<ShapeId> &shapes, int64_t width,
                                          const std::optional<shape_ops::LayerOrPurpose> &layer = std::nullopt,
                                          const std::optional<shape_ops::ShapeParent> &parent = std::nullopt);
        /// @brief Moves each shape in place onto `layer`; all or nothing.
        std::expected<void, std::string> shape_change_layer(const std::vector<ShapeId> &shapes, const shape_ops::LayerOrPurpose &layer);
        /// @brief Removes the rect, polygon or path (`kind`) at `index` from
        /// `shape`, with its mask; later pieces' indexes shift down by one.
        std::expected<void, std::string> remove_shape_piece(ShapeId shape, PieceKind kind, size_t index);

    private:
        LeHandle *handle_;
        std::unique_lock<std::shared_mutex> lock_;
        Root *root_;
        bool owns_step_ = false;
        bool succeeded_ = true;
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
        template <class T>
        T &data()
        {
            return *static_cast<T *>(data_slot(typeid(T).name(), []() -> std::shared_ptr<void> { return std::make_shared<T>(); }));
        }

    private:
        void *data_slot(const char *type_name, std::shared_ptr<void> (*make)());

        LeHandle *handle_;
        std::string extension_name_;
    };
}
