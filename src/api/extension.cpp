// The extension SDK's implementation (src/extension/le/extension.hpp) - part of
// `api` because it needs LeHandle's internals, which extensions never see.

#include "le/extension.hpp"
#include "le/extension_overlay.hpp"
#include "edit_ops.hpp"
#include "../geometry/shape_ops.hpp"
#include "le_handle.hpp"

#include <filesystem>

namespace le::ext
{
    void Registry::add(ExtensionInfo info)
    {
        extensions_.push_back(std::move(info));
    }

    void Registry::add_settings(SettingsSection section)
    {
        if (!extensions_.empty())
            settings_[extensions_.back().name] = section;
    }

    void Registry::add_overlay(void (*draw)(OverlayContext &ctx))
    {
        if (!extensions_.empty())
            overlays_.emplace_back(extensions_.back().name, draw);
    }

    void ExtensionContext::request_redraw()
    {
        handle_->extension_overlay_version.fetch_add(1, std::memory_order_relaxed);
        handle_->notify_render_needed();
    }

    OverlayContext::OverlayContext(BLContext &canvas, const OverlayFrame &frame, LeHandle *handle, std::string_view extension_name)
        : canvas_(canvas), frame_(frame), handle_(handle), extension_(handle, extension_name)
    {
    }

    BLPoint OverlayContext::to_pixel(Point dbu) const
    {
        const auto [x, y] = frame_.pixel(dbu);
        return BLPoint(x, y);
    }

    double OverlayContext::scale() const { return frame_.scale; }
    Rect OverlayContext::visible_area() const { return frame_.viewport; }
    int OverlayContext::width() const { return frame_.pixel_width; }
    int OverlayContext::height() const { return frame_.pixel_height; }
    const Root &OverlayContext::root() const { return handle_->root; }

    void Registry::set_directory(const std::string &extension, std::string directory)
    {
        for (ExtensionInfo &info : extensions_)
            if (info.name == extension)
                info.directory = std::move(directory);
    }

    std::string Registry::resource(const std::string &extension, const std::string &relative) const
    {
        if (std::filesystem::path(relative).is_absolute())
            return relative;
        for (const ExtensionInfo &info : extensions_)
            if (info.name == extension && !info.directory.empty())
                return (std::filesystem::path(info.directory) / relative).string();
        return {};
    }

    Registry &registry()
    {
        static Registry instance;
        return instance;
    }

    ReadView::ReadView(LeHandle *handle) : handle_(handle), lock_(handle->mutex_), root_(&handle->root) {}

    ReadView::ReadView(LeHandle *handle, std::try_to_lock_t) : handle_(handle), lock_(handle->mutex_, std::try_to_lock), root_(&handle->root) {}

    WriteView::WriteView(LeHandle *handle) : handle_(handle), lock_(handle->mutex_), root_(&handle->root) {}

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
        root_->bump_mutation_version();
        lock_.unlock();
        handle_->notify_render_needed();
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

    std::expected<Rect, std::string> ReadView::shape_bbox(const std::vector<ShapeId> &shapes) const { return shape_ops::bbox(*root_, shapes); }

    std::expected<Rect, std::string> WriteView::shape_bbox(const std::vector<ShapeId> &shapes) const { return shape_ops::bbox(*root_, shapes); }

    ShapeOpResult WriteView::shape_copy(const std::vector<ShapeId> &shapes, const shape_ops::LayerOrPurpose &layer,
                                        const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_copy(*handle_, shapes, layer, parent);
    }

    ShapeOpResult WriteView::shape_boolean(const std::vector<ShapeId> &a, const std::vector<ShapeId> &b, BooleanOp op,
                                           const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_boolean(*handle_, a, b, op, layer, parent);
    }

    ShapeOpResult WriteView::shape_to_polygons(const std::vector<ShapeId> &shapes, const std::optional<shape_ops::LayerOrPurpose> &layer,
                                               const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_to_polygons(*handle_, shapes, layer, parent);
    }

    ShapeOpResult WriteView::shape_to_rects(const std::vector<ShapeId> &shapes, FractureDirection direction,
                                            const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_to_rects(*handle_, shapes, direction, layer, parent);
    }

    ShapeOpResult WriteView::shape_size(const std::vector<ShapeId> &shapes, int64_t dx, int64_t dy,
                                        const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_size(*handle_, shapes, dx, dy, layer, parent);
    }

    ShapeOpResult WriteView::shape_outline_paths(const std::vector<ShapeId> &shapes, int64_t width,
                                                 const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return edit::shape_outline_paths(*handle_, shapes, width, layer, parent);
    }

    std::expected<void, std::string> WriteView::shape_change_layer(const std::vector<ShapeId> &shapes, const shape_ops::LayerOrPurpose &layer)
    {
        return edit::shape_change_layer(*handle_, shapes, layer);
    }

    std::expected<void, std::string> WriteView::remove_shape_piece(ShapeId shape, PieceKind kind, size_t index)
    {
        return edit::remove_shape_piece(*handle_, shape, kind, index);
    }

    Transaction::Transaction(LeHandle *handle, const std::string &label) : handle_(handle)
    {
        if (le_is_command_running(handle) == 0)
        {
            le_begin_command(handle, label.c_str());
            owns_ = true;
        }
    }

    Transaction::~Transaction()
    {
        if (owns_)
            le_end_command(handle_, succeeded_ ? 1 : 0);
    }

    ExtensionContext::ExtensionContext(LeHandle *handle, std::string_view extension_name) : handle_(handle), extension_name_(extension_name) {}

#include "generated/api/extension_current_defs.inc"
#include "generated/api/extension_edit_defs.inc"

    void *ExtensionContext::data_slot(const char *type_name, std::shared_ptr<void> (*make)())
    {
        const std::string key = extension_name_ + "/" + type_name;
        std::lock_guard<std::mutex> lock(handle_->extension_data_mutex);
        auto &slot = handle_->extension_data[key];
        if (!slot)
            slot = make();
        return slot.get();
    }
}

extern "C"
{
    int32_t le_extension_count(void)
    {
        return static_cast<int32_t>(le::ext::registry().extensions().size());
    }

    const char *le_extension_name(int32_t index)
    {
        const auto &extensions = le::ext::registry().extensions();
        return index >= 0 && static_cast<size_t>(index) < extensions.size() ? extensions[static_cast<size_t>(index)].name.c_str() : nullptr;
    }

    const char *le_extension_version(int32_t index)
    {
        const auto &extensions = le::ext::registry().extensions();
        return index >= 0 && static_cast<size_t>(index) < extensions.size() ? extensions[static_cast<size_t>(index)].version.c_str() : nullptr;
    }
}
