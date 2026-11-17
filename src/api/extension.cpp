// The extension SDK's implementation (src/extension/le/extension.hpp) - part of
// `api` because it needs LeHandle's internals, which extensions never see.

#include "le/extension.hpp"
#include "le/extension_overlay.hpp"
#include "le_handle.hpp"
#include "lock_check.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
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

    void Registry::add_mutation_listener(void (*on_mutation)(ExtensionContext &ctx, const MutationEvent &event))
    {
        if (!extensions_.empty() && on_mutation)
            mutation_listeners_.emplace_back(extensions_.back().name, on_mutation);
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

    ExtensionContext::ExtensionContext(LeHandle *handle, std::string_view extension_name) : handle_(handle), extension_name_(extension_name) {}

#include "generated/api/extension_current_defs.inc"

    void *ExtensionContext::data_slot(const char *type_name, std::shared_ptr<void> (*make)())
    {
        std::mutex *unused = nullptr;
        return data_slot(type_name, make, &unused);
    }

    void *ExtensionContext::data_slot(const char *type_name, std::shared_ptr<void> (*make)(), std::mutex **mutex)
    {
        const std::string key = extension_name_ + "/" + type_name;
        std::lock_guard<std::mutex> lock(handle_->extension_data_mutex);
        LeHandle::ExtensionDataSlot &slot = handle_->extension_data[key];
        if (!slot.value)
            slot.value = make();
        *mutex = &slot.mutex;
        return slot.value.get();
    }
}

void dispatch_mutation_listeners(LeHandle *handle)
{
    const auto &listeners = le::ext::registry().mutation_listeners();
    if (listeners.empty())
        return;
    LeHandle::MutationDispatch &dispatch = handle->mutation_dispatch;
    if (dispatch.thread.load() == std::this_thread::get_id())
        return; // a listener's own change, left out when it returns
    dispatch.pending.store(true);
    // A thread that finds another dispatching leaves its change to it: the
    // dispatcher loops until nothing is pending, then checks once more
    // after letting go of the mutex.
    while (dispatch.pending.load() && dispatch.mutex.try_lock())
    {
        std::lock_guard<std::mutex> guard(dispatch.mutex, std::adopt_lock);
        while (dispatch.pending.exchange(false))
        {
            le::ext::MutationEvent event;
            std::vector<le::ChangeLogEntry> changes;
            {
                const le::lock_check::Scope check(handle, "mutation listener dispatch");
                std::shared_lock<std::shared_mutex> lock(handle->mutex_);
                const le::ChangeLog &log = handle->root.change_log();
                const uint64_t end = log.end_sequence();
                if (end == dispatch.delivered)
                    continue;
                const std::vector<std::pair<uint64_t, uint64_t>> own = std::exchange(dispatch.own_changes, {});
                if (!log.covers(dispatch.delivered))
                    event.everything = true;
                else
                {
                    uint64_t sequence = dispatch.delivered;
                    log.for_each_since(dispatch.delivered, [&](const le::ChangeLogEntry &entry)
                                       {
                                           if (std::ranges::none_of(own, [&](const auto &range)
                                                                    { return sequence >= range.first && sequence < range.second; }))
                                               changes.push_back(entry);
                                           ++sequence; });
                }
                dispatch.delivered = end;
            }
            if (!event.everything && changes.empty())
                continue;
            event.changes = changes;
            dispatch.thread.store(std::this_thread::get_id());
            for (const auto &[name, on_mutation] : listeners)
            {
                le::ext::ExtensionContext ctx(handle, name);
                try
                {
                    on_mutation(ctx, event);
                }
                catch (const std::exception &error)
                {
                    spdlog::error("{}: mutation listener failed: {}", name, error.what());
                }
                catch (...)
                {
                    spdlog::error("{}: mutation listener failed", name);
                }
            }
            dispatch.thread.store(std::thread::id{});
        }
    }
}
