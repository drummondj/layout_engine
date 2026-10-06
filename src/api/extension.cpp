// The extension SDK's implementation (src/extension/le/extension.hpp) - part of
// `api` because it needs LeHandle's internals, which extensions never see.

#include "le/extension.hpp"
#include "le_handle.hpp"

namespace le::ext
{
    void Registry::add(ExtensionInfo info)
    {
        extensions_.push_back(std::move(info));
    }

    Registry &registry()
    {
        static Registry instance;
        return instance;
    }

    ReadView::ReadView(LeHandle *handle) : lock_(handle->mutex_), root_(&handle->root) {}

    ReadView::ReadView(LeHandle *handle, std::try_to_lock_t) : lock_(handle->mutex_, std::try_to_lock), root_(&handle->root) {}

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
