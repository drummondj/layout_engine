// The extension SDK's implementation (src/extension/le/extension.hpp) - part of
// `api` because it needs LeHandle's internals, which extensions never see.

#include "le/extension.hpp"
#include "le/extension_overlay.hpp"
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
        return BLPoint(static_cast<double>(dbu.x - frame_.viewport.ll.x) * frame_.scale,
                       static_cast<double>(frame_.pixel_height) - static_cast<double>(dbu.y - frame_.viewport.ll.y) * frame_.scale);
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

    WriteView::~WriteView()
    {
        root_->bump_mutation_version();
        lock_.unlock();
        handle_->notify_render_needed();
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
