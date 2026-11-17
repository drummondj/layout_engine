// The C++ API (views.hpp): sessions, their locked views and the renderer.

#include "views.hpp"
#include "api_internal.hpp"
#include "edit_ops.hpp"
#include "le_handle.hpp"
#include "lock_check.hpp"
#include "shape_ops.hpp"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#ifndef NDEBUG
namespace le::lock_check
{
    namespace
    {
        struct Held
        {
            const LeHandle *handle;
            const char *what;
        };

        std::vector<Held> &held()
        {
            thread_local std::vector<Held> locks;
            return locks;
        }
    }

    void enter(const LeHandle *handle, const char *what)
    {
        for (const Held &lock : held())
            if (lock.handle == handle)
            {
                std::fprintf(stderr,
                             "layout_engine: opening %s on a session while this thread already holds %s on it; that deadlocks. "
                             "Close the first before opening the second, or use the open view's own members.\n",
                             what, lock.what);
                std::abort();
            }
        held().push_back(Held{handle, what});
    }

    void leave(const LeHandle *handle)
    {
        std::vector<Held> &locks = held();
        for (auto it = locks.rbegin(); it != locks.rend(); ++it)
            if (it->handle == handle)
            {
                locks.erase(std::next(it).base());
                return;
            }
    }
}
#endif

namespace le
{
    namespace
    {
        // Tracy pairs a FrameMarkStart with its FrameMarkEnd by the name's
        // pointer, so both must pass this one constant.
        constexpr const char *kRenderFrameName = "render";
    }

    // Each view checks with lock_check before it locks (the comma
    // expressions), and lets go of the check as it unlocks.
    ReadView::ReadView(LeHandle *handle)
        : handle_(handle), lock_((lock_check::enter(handle, "a ReadView"), handle->mutex_)), root_(&handle->root)
    {
    }

    ReadView::ReadView(LeHandle *handle, std::try_to_lock_t)
        : handle_(handle), lock_((lock_check::enter(handle, "a ReadView"), handle->mutex_), std::try_to_lock), root_(&handle->root)
    {
        if (!lock_.owns_lock())
            lock_check::leave(handle_);
    }

    ReadView::~ReadView()
    {
        if (lock_.owns_lock())
            lock_check::leave(handle_);
    }

    WriteView::WriteView(LeHandle *handle)
        : handle_(handle), lock_((lock_check::enter(handle, "a WriteView"), handle->mutex_)), root_(&handle->root),
          begin_(handle->root.change_log().end_sequence())
    {
    }

    // Opens the step under this view's lock, as begin_command does
    // under its own.
    WriteView::WriteView(LeHandle *handle, const std::string &label) : WriteView(handle)
    {
        if (!handle_->command_history.is_recording())
        {
            handle_->command_history.begin(label);
            handle_->hold_renders();
            owns_step_ = true;
        }
    }

    WriteView::~WriteView()
    {
        if (owns_step_)
        {
            handle_->command_history.end(succeeded_);
            handle_->release_renders();
        }
        if (root_lent_)
            root_->bump_mutation_version();
        handle_->note_write_finished(begin_);
        const bool deliver = !handle_->renders_held();
        lock_check::leave(handle_);
        lock_.unlock();
        handle_->notify_render_needed();
        if (deliver)
            dispatch_mutation_listeners(handle_);
    }

    namespace
    {
        // A Transaction's exclusive lock while it opens or closes its step,
        // waking the renderer and telling listeners once it's released.
        class HandleWriteLock
        {
        public:
            HandleWriteLock(LeHandle *handle, const char *what)
                : handle_(handle), lock_((lock_check::enter(handle, what), handle->mutex_)), begin_(handle->root.change_log().end_sequence())
            {
            }

            ~HandleWriteLock()
            {
                handle_->note_write_finished(begin_);
                const bool deliver = !handle_->renders_held();
                lock_check::leave(handle_);
                lock_.unlock();
                handle_->notify_render_needed();
                if (deliver)
                    dispatch_mutation_listeners(handle_);
            }

            HandleWriteLock(const HandleWriteLock &) = delete;
            HandleWriteLock &operator=(const HandleWriteLock &) = delete;

        private:
            LeHandle *handle_;
            std::unique_lock<std::shared_mutex> lock_;
            uint64_t begin_;
        };

        std::expected<Units, std::string> units_of(const Root &root)
        {
            const std::optional<double> scale = edit::dbu_per_um(root);
            if (!scale)
                return std::unexpected("no Technology with a DATABASE MICRONS scale has been read yet");
            return Units(*scale);
        }
    }

    std::expected<Units, std::string> ReadView::units() const { return units_of(*root_); }

    std::expected<Units, std::string> WriteView::units() const { return units_of(*root_); }

    ShapeBuilder WriteView::build_shape(ShapeOwner owner) { return ShapeBuilder(*this, owner); }

    ShapeBuilder::ShapeBuilder(WriteView &view, ShapeOwner owner) : view_(&view), units_(view.units()), data_{.owner = owner} {}

    void ShapeBuilder::fail(std::string error)
    {
        if (error_.empty())
            error_ = std::move(error);
    }

    ShapeBuilder &ShapeBuilder::layer(LayerId layer)
    {
        data_.layer = layer;
        data_.purpose.reset();
        return *this;
    }

    ShapeBuilder &ShapeBuilder::purpose(ShapePurpose purpose)
    {
        data_.layer = LayerId{};
        data_.purpose = purpose;
        return *this;
    }

    ShapeBuilder &ShapeBuilder::rect(double llx, double lly, double urx, double ury) { return rect(RectUm{{llx, lly}, {urx, ury}}); }

    ShapeBuilder &ShapeBuilder::rect(RectUm rect)
    {
        if (units_)
            data_.rects.push_back(units_->to_dbu(rect));
        return *this;
    }

    ShapeBuilder &ShapeBuilder::polygon(const std::vector<PointUm> &points)
    {
        if (points.size() < 3)
            fail("a polygon needs at least 3 points");
        else if (units_)
        {
            Polygon polygon;
            polygon.points.reserve(points.size());
            for (const PointUm p : points)
                polygon.points.push_back(units_->to_dbu(p));
            data_.polygons.push_back(std::move(polygon));
        }
        return *this;
    }

    ShapeBuilder &ShapeBuilder::path(double width, const std::vector<PointUm> &points)
    {
        if (points.size() < 2)
            fail("a path needs at least 2 points");
        else if (width <= 0.0)
            fail("a path's width must be positive");
        else if (units_)
        {
            Path path{.width = units_->to_dbu(width)};
            path.polygon.points.reserve(points.size());
            for (const PointUm p : points)
                path.polygon.points.push_back(units_->to_dbu(p));
            data_.paths.push_back(std::move(path));
        }
        return *this;
    }

    std::expected<ShapeId, std::string> ShapeBuilder::create()
    {
        if (!units_)
            return std::unexpected(units_.error());
        if (!error_.empty())
            return std::unexpected(error_);
        return view_->create_shape(data_);
    }

    std::expected<Rect, std::string> ReadView::shape_bbox(const std::vector<GeometryId> &shapes) const { return shape_ops::bbox(*root_, shapes); }

    std::expected<Rect, std::string> WriteView::shape_bbox(const std::vector<GeometryId> &shapes) const { return shape_ops::bbox(*root_, shapes); }

    ShapeOpResult WriteView::shape_copy(const std::vector<GeometryId> &shapes, const shape_ops::LayerOrPurpose &layer,
                                        const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_copy(*handle_, shapes, layer, parent);
    }

    ShapeOpResult WriteView::shape_boolean(const std::vector<GeometryId> &a, const std::vector<GeometryId> &b, BooleanOp op,
                                           const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_boolean(*handle_, a, b, op, layer, parent);
    }

    ShapeOpResult WriteView::shape_to_polygons(const std::vector<GeometryId> &shapes, const std::optional<shape_ops::LayerOrPurpose> &layer,
                                               const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_to_polygons(*handle_, shapes, layer, parent);
    }

    ShapeOpResult WriteView::shape_to_rects(const std::vector<GeometryId> &shapes, FractureDirection direction,
                                            const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_to_rects(*handle_, shapes, direction, layer, parent);
    }

    ShapeOpResult WriteView::shape_size(const std::vector<GeometryId> &shapes, int64_t dx, int64_t dy,
                                        const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_size(*handle_, shapes, dx, dy, layer, parent);
    }

    ShapeOpResult WriteView::shape_outline_paths(const std::vector<GeometryId> &shapes, int64_t width,
                                                 const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_outline_paths(*handle_, shapes, width, layer, parent);
    }

    std::expected<void, std::string> WriteView::shape_change_layer(const std::vector<GeometryId> &shapes, const shape_ops::LayerOrPurpose &layer)
    {
        return edit::shape_change_layer(*handle_, shapes, layer);
    }

    std::expected<void, std::string> WriteView::remove_shape_piece(GeometryId shape, PieceKind kind, size_t index)
    {
        return edit::remove_shape_piece(*handle_, shape, kind, index);
    }

    std::span<const SelectedObject> ReadView::selection() const { return handle_->selection(); }
    uint64_t ReadView::selection_version() const { return handle_->selection_version(); }
    std::span<const Ruler> ReadView::rulers() const { return handle_->rulers(); }
    ViewState ReadView::view() const { return edit::view_state(*handle_); }

    std::span<const SelectedObject> WriteView::selection() const { return handle_->selection(); }
    uint64_t WriteView::selection_version() const { return handle_->selection_version(); }
    std::expected<void, std::string> WriteView::select(SelectableObject object) { return edit::select(*handle_, object); }
    std::expected<void, std::string> WriteView::select_entry(const SelectedObject &entry) { return edit::select_entry(*handle_, entry); }
    void WriteView::deselect(const SelectedObject &entry) { edit::deselect_entry(*handle_, entry); }
    void WriteView::clear_selection() { edit::clear_selection(*handle_); }

    ViewState WriteView::view() const { return edit::view_state(*handle_); }
    std::expected<void, std::string> WriteView::open_abstract(DesignId design) { return edit::open_abstract(*handle_, design); }
    std::expected<void, std::string> WriteView::open_layout(DesignId design) { return edit::open_layout(*handle_, design); }
    void WriteView::fit_view(int padding_px) { edit::fit_view(*handle_, padding_px); }
    void WriteView::fit_rect(Rect area, int padding_px) { edit::fit_rect(*handle_, area, padding_px); }
    void WriteView::zoom(double factor, int x_px, int y_px) { edit::zoom(*handle_, factor, x_px, y_px); }
    void WriteView::pan(double x_factor, double y_factor) { edit::pan(*handle_, x_factor, y_factor); }
    void WriteView::set_view_flip(ViewFlip flip) { edit::set_view_flip(*handle_, flip); }

    std::span<const Ruler> WriteView::rulers() const { return handle_->rulers(); }
    std::expected<void, std::string> WriteView::add_ruler(std::span<const Point> points) { return edit::add_ruler(*handle_, points); }
    void WriteView::finish_ruler() { edit::finish_ruler(*handle_); }
    void WriteView::clear_rulers() { edit::clear_rulers(*handle_); }

    InteractionMode ReadView::mode() const { return edit::mode(*handle_); }
    std::string_view ReadView::tooltip() const { return edit::tooltip(*handle_); }
    bool ReadView::move_armed() const { return edit::move_armed(*handle_); }
    bool ReadView::move_anchored() const { return edit::move_anchored(*handle_); }
    PlacementSnapMode ReadView::placement_snap_mode() const { return edit::placement_snap_mode(*handle_); }
    bool ReadView::placement_snap_mode_available(PlacementSnapMode mode) const { return edit::placement_snap_mode_available(*handle_, mode); }
    size_t ReadView::selected_locked_placement_count() const { return edit::selected_locked_placement_count(*handle_); }
    bool ReadView::selected_placements_all_fixed() const { return edit::selected_placements_all_fixed(*handle_); }
    uint32_t ReadView::placement_orientation_ops_enabled() const { return edit::placement_orientation_ops_enabled(*handle_); }
    bool ReadView::resize_armed() const { return edit::resize_armed(*handle_); }
    std::optional<ResizeAxis> ReadView::resize_hover_axis() const { return edit::resize_hover_axis(*handle_); }
    ShapeSnapMode ReadView::shape_snap_mode(PieceKind kind) const { return edit::shape_snap_mode(*handle_, kind); }
    bool ReadView::shape_snap_mode_available(PieceKind kind, ShapeSnapMode mode) const { return edit::shape_snap_mode_available(*handle_, kind, mode); }
    uint32_t ReadView::selected_piece_kinds() const { return edit::selected_piece_kinds(*handle_); }
    uint32_t ReadView::selected_move_snap_piece_kinds() const { return edit::selected_move_snap_piece_kinds(*handle_); }
    std::optional<Point> ReadView::snapped_mouse_position() const { return edit::snapped_mouse_position(*handle_); }
    bool ReadView::key_held(Key key) const { return edit::key_held(*handle_, key); }

    InteractionMode WriteView::mode() const { return edit::mode(*handle_); }
    void WriteView::set_mode(InteractionMode mode) { edit::set_mode(*handle_, mode); }
    void WriteView::arm_move() { edit::arm_move(*handle_); }
    void WriteView::cancel_move() { edit::cancel_move(*handle_); }
    void WriteView::set_placement_snap_mode(PlacementSnapMode mode) { edit::set_placement_snap_mode(*handle_, mode); }
    size_t WriteView::delete_selected_pieces() { return edit::delete_selected_pieces(*handle_); }
    std::expected<void, OrientationOpError> WriteView::apply_placement_orientation_op(OrientationOp op)
    {
        return edit::apply_placement_orientation_op(*handle_, op);
    }
    void WriteView::arm_resize() { edit::arm_resize(*handle_); }
    bool WriteView::set_shape_snap_mode(PieceKind kind, ShapeSnapMode mode) { return edit::set_shape_snap_mode(*handle_, kind, mode); }
    void WriteView::set_mouse_position(int x_px, int y_px) { edit::set_mouse_position(*handle_, x_px, y_px); }
    void WriteView::clear_mouse_position() { edit::clear_mouse_position(*handle_); }
    void WriteView::key_down(Key key) { edit::key_down(*handle_, key); }
    void WriteView::key_up(Key key) { edit::key_up(*handle_, key); }
    void WriteView::clear_keys() { edit::clear_keys(*handle_); }
    void WriteView::mouse_down(int x_px, int y_px) { edit::mouse_down(*handle_, x_px, y_px); }
    void WriteView::zoom_drag_down(int x_px, int y_px) { edit::zoom_drag_down(*handle_, x_px, y_px); }
    void WriteView::cancel_drag() { edit::cancel_drag(*handle_); }
    void WriteView::mouse_up(int x_px, int y_px) { edit::mouse_up(*handle_, x_px, y_px); }

    int ReadView::hierarchy_depth() const { return edit::hierarchy_depth(*handle_); }
    std::vector<LayerRow> ReadView::layer_rows() const { return edit::layer_rows(*handle_); }
    std::vector<ViewLayerPurpose> ReadView::purposes() const { return edit::purposes(*handle_); }
    std::optional<Color> ReadView::layer_color(const std::string &layer) const { return edit::layer_color(*handle_, layer); }
    bool ReadView::layer_visible(const std::string &layer) const { return edit::layer_visible(*handle_, layer); }
    bool ReadView::layer_selectable(const std::string &layer) const { return edit::layer_selectable(*handle_, layer); }
    bool ReadView::purpose_visible(ViewLayerPurpose purpose) const { return edit::purpose_visible(*handle_, purpose); }
    bool ReadView::purpose_selectable(ViewLayerPurpose purpose) const { return edit::purpose_selectable(*handle_, purpose); }
    std::span<const std::string> ReadView::object_filter_values(ObjectFilter filter) const { return edit::object_filter_values(*handle_, filter); }
    bool ReadView::object_filter_visible(ObjectFilter filter, const std::string &value) const { return edit::object_filter_visible(*handle_, filter, value); }
    bool ReadView::object_filter_selectable(ObjectFilter filter, const std::string &value) const
    {
        return edit::object_filter_selectable(*handle_, filter, value);
    }
    std::array<double, 2> ReadView::display_point(Point p) const { return edit::display_point(*handle_, p); }
    int ReadView::max_concurrency() const { return edit::max_concurrency(*handle_); }
    int ReadView::flightline_max_fanout() const { return edit::flightline_max_fanout(*handle_); }
    bool ReadView::confirm_overwrite() const { return edit::confirm_overwrite(*handle_); }
    size_t ReadView::theme() const { return edit::theme(*handle_); }
    int64_t ReadView::minor_grid_spacing() const { return edit::minor_grid_spacing(*handle_); }
    int64_t ReadView::major_grid_spacing() const { return edit::major_grid_spacing(*handle_); }
    double ReadView::grid_spacing_um(bool major) const { return edit::grid_spacing_um(*handle_, major); }
    std::optional<int64_t> ReadView::manufacturing_grid() const { return edit::manufacturing_grid(*handle_); }
    double ReadView::ruler_label_size() const { return edit::ruler_label_size(*handle_); }
    double ReadView::label_min_size() const { return edit::label_min_size(*handle_); }
    double ReadView::label_max_size() const { return edit::label_max_size(*handle_); }
    size_t ReadView::undo_limit() const { return edit::undo_limit(*handle_); }
    bool ReadView::has_unsaved_settings() const { return edit::has_unsaved_settings(*handle_); }

    void WriteView::set_hierarchy_depth(int depth) { edit::set_hierarchy_depth(*handle_, depth); }
    void WriteView::set_layer_visible(const std::string &layer, bool visible) { edit::set_layer_visible(*handle_, layer, visible); }
    void WriteView::set_layer_selectable(const std::string &layer, bool selectable) { edit::set_layer_selectable(*handle_, layer, selectable); }
    void WriteView::set_purpose_visible(ViewLayerPurpose purpose, bool visible) { edit::set_purpose_visible(*handle_, purpose, visible); }
    void WriteView::set_purpose_selectable(ViewLayerPurpose purpose, bool selectable) { edit::set_purpose_selectable(*handle_, purpose, selectable); }
    void WriteView::set_object_filter_visible(ObjectFilter filter, const std::string &value, bool visible)
    {
        edit::set_object_filter_visible(*handle_, filter, value, visible);
    }
    void WriteView::set_object_filter_selectable(ObjectFilter filter, const std::string &value, bool selectable)
    {
        edit::set_object_filter_selectable(*handle_, filter, value, selectable);
    }
    void WriteView::set_viewport_size(int width_px, int height_px) { edit::set_viewport_size(*handle_, width_px, height_px); }
    void WriteView::set_max_concurrency(int max_concurrency) { edit::set_max_concurrency(*handle_, max_concurrency); }
    void WriteView::set_flightline_max_fanout(int max_fanout) { edit::set_flightline_max_fanout(*handle_, max_fanout); }
    void WriteView::set_confirm_overwrite(bool confirm) { edit::set_confirm_overwrite(*handle_, confirm); }
    bool WriteView::set_theme(std::string_view name) { return edit::set_theme(*handle_, name); }
    void WriteView::set_minor_grid_spacing(int64_t dbu) { edit::set_minor_grid_spacing(*handle_, dbu); }
    void WriteView::set_major_grid_spacing(int64_t dbu) { edit::set_major_grid_spacing(*handle_, dbu); }
    void WriteView::set_grid_spacing_um(double minor_um, double major_um) { edit::set_grid_spacing_um(*handle_, minor_um, major_um); }
    void WriteView::set_ruler_label_size(double px) { edit::set_ruler_label_size(*handle_, px); }
    void WriteView::set_label_min_size(double px) { edit::set_label_min_size(*handle_, px); }
    void WriteView::set_label_max_size(double px) { edit::set_label_max_size(*handle_, px); }
    void WriteView::set_layer_color(const std::string &layer, Color color) { edit::set_layer_color(*handle_, layer, color); }
    void WriteView::reset_layer_color(const std::string &layer) { edit::reset_layer_color(*handle_, layer); }
    void WriteView::set_undo_limit(size_t limit) { edit::set_undo_limit(*handle_, limit); }
    std::expected<void, std::string> WriteView::save_settings(const std::string &path) { return edit::save_settings(*handle_, path); }
    std::expected<void, std::string> WriteView::load_settings(const std::string &path) { return edit::load_settings(*handle_, path); }

    bool ReadView::can_undo() const { return edit::can_undo(*handle_); }
    bool ReadView::can_redo() const { return edit::can_redo(*handle_); }
    size_t ReadView::command_history_size() const { return edit::command_history_size(*handle_); }
    const std::string &ReadView::command_history_at(size_t index) const { return edit::command_history_at(*handle_, index); }
    bool ReadView::has_unsaved_database() const { return edit::has_unsaved_database(*handle_); }
    const std::string &ReadView::db_path() const { return edit::db_path(*handle_); }
    bool ReadView::database_is_empty() const { return edit::database_is_empty(*handle_); }
    std::expected<bool, std::string> WriteView::undo() { return edit::undo(*handle_); }
    void WriteView::begin_command(const std::string &label) { edit::begin_command(*handle_, label); }
    void WriteView::end_command(bool succeeded) { edit::end_command(*handle_, succeeded); }
    std::expected<void, std::string> WriteView::delete_net_cascade(NetId net) { return edit::delete_net_cascade(*handle_, net); }
    std::expected<void, std::string> WriteView::rename_net(NetId net, const std::string &name) { return edit::rename_net(*handle_, net, name); }
    std::expected<void, std::string> WriteView::rename_instance(InstanceId instance, const std::string &name)
    {
        return edit::rename_instance(*handle_, instance, name);
    }
    std::expected<size_t, std::string> WriteView::link_unresolved_instances() { return edit::link_unresolved_instances(*handle_); }
    std::expected<bool, std::string> WriteView::redo() { return edit::redo(*handle_); }

#include "generated/api/view_search_defs.inc"
    std::vector<PropertyValue> ReadView::properties(const ObjectId &id) const { return edit::object_properties(*handle_, id); }
    std::optional<ObjectId> ReadView::parent(const ObjectId &id) const { return edit::object_parent(*handle_, id); }
    std::expected<std::optional<PropertyValue>, std::string> ReadView::property(const ObjectId &id, const std::string &path) const
    {
        return edit::object_property_path(*handle_, id, path);
    }
    std::expected<std::vector<InstanceId>, std::string> ReadView::get_instances_by_path(SchematicId of, const std::string &path,
                                                                                        const std::string &filter) const
    {
        return edit::get_instances_by_path(*handle_, of, path, filter);
    }
    std::expected<std::vector<NetId>, std::string> ReadView::get_nets_by_path(SchematicId of, const std::string &path, const std::string &filter) const
    {
        return edit::get_nets_by_path(*handle_, of, path, filter);
    }
    std::expected<std::vector<PortId>, std::string> ReadView::get_ports_by_path(SchematicId of, const std::string &path, const std::string &filter) const
    {
        return edit::get_ports_by_path(*handle_, of, path, filter);
    }

    std::expected<void, std::string> WriteView::read_lef(const std::string &path, const std::string &library) { return edit::read_lef(*handle_, path, library); }
    std::expected<void, std::string> WriteView::read_def(const std::string &path, const std::string &library) { return edit::read_def(*handle_, path, library); }
    std::expected<void, std::string> WriteView::read_verilog(std::span<const std::string> files, bool netlist, const std::string &library)
    {
        return edit::read_verilog(*handle_, files, netlist, library);
    }
    std::expected<void, std::string> WriteView::read_db(const std::string &path, bool with_session) { return edit::read_db(*handle_, path, with_session); }
    std::expected<void, std::string> WriteView::write_lef(const std::string &path, std::span<const AbstractId> abstracts, LibraryId library,
                                                          LefLayerWriteMode layers)
    {
        return edit::write_lef(*handle_, path, abstracts, library, layers);
    }
    std::expected<void, std::string> WriteView::write_def(const std::string &path, LayoutId layout) { return edit::write_def(*handle_, path, layout); }
    std::expected<void, std::string> WriteView::write_db(const std::string &path, bool with_session) { return edit::write_db(*handle_, path, with_session); }
    std::expected<void, std::string> WriteView::write_verilog_stubs(const std::string &path, LibraryId library)
    {
        return edit::write_verilog_stubs(*handle_, path, library);
    }

    Transaction::Transaction(LeHandle *handle, const std::string &label) : handle_(handle)
    {
        HandleWriteLock lock(handle_, "a Transaction");
        if (!handle_->command_history.is_recording())
        {
            handle_->command_history.begin(label);
            handle_->hold_renders();
            owns_ = true;
        }
    }

    Transaction::~Transaction()
    {
        if (!owns_)
            return;
        HandleWriteLock lock(handle_, "the end of a Transaction");
        handle_->command_history.end(succeeded_);
        handle_->release_renders();
    }

#include "generated/api/view_current_defs.inc"
#include "generated/api/view_edit_defs.inc"

    void Renderer::wait_until_needed() const { handle_->wait_for_render_needed(); }

    void Renderer::cancel_wait() const { handle_->cancel_render_wait(); }

    bool Renderer::is_rendering() const { return handle_->is_rendering_.load(std::memory_order_relaxed); }

    bool Renderer::renders_held() const { return handle_->renders_held(); }

    RenderedFrame Renderer::render() const
    {
        // The GUI render thread and the Tcl thread (dump_png) both render;
        // render_mutex_ keeps their pipeline runs apart. mutex_ is only
        // shared, so panel reads stay live during a render: the render
        // mutates nothing but view_render_pipeline (and the flightline
        // cache, which has its own mutex).
        const lock_check::Scope check(handle_, "a render");
        std::lock_guard<std::mutex> render_lock(handle_->render_mutex_);
        std::shared_lock<std::shared_mutex> lock(handle_->mutex_);

        // An unsized viewport has nothing to render, and Blend2D can't
        // make a zero-sized image.
        if (handle_->viewport_width_px() <= 0 || handle_->viewport_height_px() <= 0)
            return {};

        // Named start/end marks: frames happen on demand, not once per
        // vsync, so each gets its own duration in Tracy's timeline.
        FrameMarkStart(kRenderFrameName);
        const ViewRenderOptions options = api_detail::view_render_options_for(handle_);

        // is_rendering_ covers only a real recompute, so a poller's
        // spinner doesn't flicker for no-op calls.
        const bool will_recompute = handle_->view_render_pipeline.would_recompute(options);
        if (will_recompute)
            handle_->is_rendering_.store(true, std::memory_order_relaxed);
        struct RenderingGuard
        {
            LeHandle *handle;
            bool active;
            ~RenderingGuard()
            {
                if (active)
                    handle->is_rendering_.store(false, std::memory_order_relaxed);
            }
        } rendering_guard{handle_, will_recompute};

        ViewRenderPipeline::WarmOutput output = handle_->view_render_pipeline.run(&handle_->root, options);
        FrameMarkEnd(kRenderFrameName);

        if (!output.frame || output.frame->empty)
            return {};
        // Frames are immutable, so sharing ownership of this one keeps
        // `data` valid however many renders follow.
        const auto &buffer = output.frame->buffer;
        return RenderedFrame{
            .data = buffer.data,
            .width = buffer.width,
            .height = buffer.height,
            .row_bytes = buffer.row_bytes,
            .pixels = std::move(output.frame),
        };
    }

    Session::Session() : handle_(new LeHandle())
    {
        handle_->saved_mutation_version = handle_->root.mutation_version();
        handle_->saved_settings_json = api_detail::settings_snapshot(handle_);
    }

    Session::~Session() { destroy(handle_); }

    Session::Session(Session &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Session &Session::operator=(Session &&other) noexcept
    {
        if (this != &other)
        {
            destroy(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    LeHandle *Session::release() { return std::exchange(handle_, nullptr); }

    void Session::destroy(LeHandle *handle) { delete handle; }
}
