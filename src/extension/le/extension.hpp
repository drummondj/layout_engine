#pragma once
// The Layout Engine extension SDK: everything an extension's C++ may use.
// Anything reachable from here is public API, versioned by
// LE_EXTENSION_API_VERSION (set by the le::extension_sdk CMake target); an
// extension states the version it targets with a static_assert.

#include "views.hpp"
#include "core/session_types.hpp"
#include "database.hpp"
#include "database/wire_helpers.hpp"
#include "generated/api/edit_types.hpp"
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
    /// ctx.data<T>() - no read() or write().
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
        /// typed Tcl command, GUI edit or WriteView, after its lock is
        /// released - so it may read() and write(). It runs on
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

    // The core's C++ API (views.hpp), under the names extensions have
    // always used.
    using le::PointUm;
    using le::ReadView;
    using le::RectUm;
    using le::ShapeBuilder;
    using le::ShapeOpResult;
    using le::Transaction;
    using le::Units;
    using le::WriteView;

    /// @brief An extension's access to one session (Session::handle()).
    class ExtensionContext
    {
    public:
        ExtensionContext(LeHandle *handle, std::string_view extension_name);

        ReadView read() const { return ReadView(handle_); }
        WriteView write() { return WriteView(handle_); }
        /// @brief A WriteView whose edits are one undo step labelled `label`.
        WriteView write(const std::string &label) { return WriteView(handle_, label); }
        Transaction transaction(const std::string &label) { return Transaction(handle_, label); }
        // These take the session's lock for the read: inside a read() or
        // write(), use the view's own accessors instead.
#include "generated/api/view_current_decls.inc"

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
