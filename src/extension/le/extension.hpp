#pragma once
// The Layout Engine extension SDK: everything an extension's C++ may use.
// Anything reachable from here is public API, versioned by
// LE_EXTENSION_API_VERSION (set by the le::extension_sdk CMake target); an
// extension states the version it targets with a static_assert. Design:
// docs/EXTENSION_MECHANISM_RESEARCH.md §3, §5.

#include "api.hpp"
#include "database.hpp"
#include "le/register_all.hpp"

#include <json.hpp>

#include <map>
#include <memory>
#include <mutex>
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

        /// @brief Records where an extension's files are (le_shell does,
        /// from extensions.json).
        void set_directory(const std::string &extension, std::string directory);
        /// @brief `relative` under the extension's directory ("" if the
        /// directory isn't known); an absolute path is returned unchanged.
        std::string resource(const std::string &extension, const std::string &relative) const;

    private:
        std::vector<ExtensionInfo> extensions_;
        std::map<std::string, SettingsSection> settings_;
    };

    /// @brief This process's registry. Filled once by register_all() at
    /// startup and read-only afterwards.
    Registry &registry();

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

    private:
        std::shared_lock<std::shared_mutex> lock_;
        const Root *root_;
    };

    /// @brief An exclusively locked, writable view of the handle's database.
    /// When it ends it bumps the database's mutation version and wakes the
    /// renderer. Edits made through it are NOT undoable - for undoable edits,
    /// call the C API (le_create_<type>/le_update_<type>/le_delete_<type>)
    /// inside a Transaction instead. Don't call le_* functions while holding one.
    class WriteView
    {
    public:
        explicit WriteView(LeHandle *handle);
        ~WriteView();
        WriteView(const WriteView &) = delete;
        WriteView &operator=(const WriteView &) = delete;

        Root &root() { return *root_; }

    private:
        LeHandle *handle_;
        std::unique_lock<std::shared_mutex> lock_;
        Root *root_;
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
        Transaction transaction(const std::string &label) { return Transaction(handle_, label); }

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
