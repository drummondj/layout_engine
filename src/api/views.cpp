// The C++ API (views.hpp): sessions, their locked views and the renderer.

#include "views.hpp"
#include "api_internal.hpp"
#include "edit_ops.hpp"
#include "le_handle.hpp"
#include "shape_ops.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace le
{
    namespace
    {
        // Tracy pairs a FrameMarkStart with its FrameMarkEnd by the name's
        // pointer, so both must pass this one constant.
        constexpr const char *kRenderFrameName = "render";
    }

    ReadView::ReadView(LeHandle *handle) : handle_(handle), lock_(handle->mutex_), root_(&handle->root) {}

    ReadView::ReadView(LeHandle *handle, std::try_to_lock_t) : handle_(handle), lock_(handle->mutex_, std::try_to_lock), root_(&handle->root) {}

    WriteView::WriteView(LeHandle *handle)
        : handle_(handle), lock_(handle->mutex_), root_(&handle->root), begin_(handle->root.change_log().end_sequence())
    {
    }

    // Opens the step under this view's lock, as le_begin_command does
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
        lock_.unlock();
        handle_->notify_render_needed();
        if (deliver)
            dispatch_mutation_listeners(handle_);
    }

    namespace
    {
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
        HandleWriteLock lock(handle_);
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
        HandleWriteLock lock(handle_);
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
