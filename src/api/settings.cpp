#include "api_internal.hpp"
#include "le/extension.hpp"
#include <json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>

using namespace le::api_detail;

namespace
{
    // --- Settings ---

    // le_set_max_concurrency's body - also applied by a loaded settings
    // file. Clamped to at least 2.
    void set_max_concurrency_unlocked(LeHandle *handle, int32_t max_concurrency)
    {
        const int32_t clamped = std::max(2, max_concurrency);
        if (clamped == handle->max_concurrency_)
            return;
        handle->max_concurrency_ = clamped;
        // No setter on global_control itself - destroy the old limit
        // before constructing the new one (a live global_control's own
        // limit is the min across every currently-constructed instance,
        // so leaving the old one alive while constructing a new one could
        // never raise the effective limit, only lower it).
        handle->concurrency_control_.reset();
        handle->concurrency_control_.emplace(oneapi::tbb::global_control::max_allowed_parallelism, static_cast<size_t>(clamped));
        // Blend2D's render threads are its own, outside TBB's arena.
        handle->view_render_pipeline.set_render_thread_count(static_cast<uint32_t>(clamped));
    }

    // Sets grid spacing given in um (a value <= 0 leaves that one alone).
    // With no Technology yet there's no dbu scale to convert through, so
    // the um value waits on the handle until apply_pending_grid_um_unlocked
    // (le_read_lef/le_read_def) can.
    void set_grid_spacing_um_unlocked(LeHandle *handle, double minor_um, double major_um)
    {
        const double dbu_per_um = technology_dbu_per_um(handle->root);
        const auto apply = [&](double um, std::optional<double> &pending, auto set_dbu)
        {
            if (um <= 0.0)
                return;
            if (dbu_per_um <= 0.0)
            {
                pending = um;
                return;
            }
            pending.reset();
            set_dbu(std::max<int64_t>(1, std::llround(um * dbu_per_um)));
        };
        apply(minor_um, handle->pending_minor_grid_um, [&](int64_t dbu)
              { handle->set_minor_grid_spacing(dbu); });
        apply(major_um, handle->pending_major_grid_um, [&](int64_t dbu)
              { handle->set_major_grid_spacing(dbu); });
    }
}

namespace le::api_detail
{
    void apply_pending_grid_um_unlocked(LeHandle *handle)
    {
        if (handle->pending_minor_grid_um || handle->pending_major_grid_um)
            set_grid_spacing_um_unlocked(handle, handle->pending_minor_grid_um.value_or(0.0), handle->pending_major_grid_um.value_or(0.0));
    }
}

namespace
{
    // Grid spacing in um - the pending value if there's no Technology to
    // convert through yet, else the dbu value converted; -1 if neither.
    double grid_spacing_um_unlocked(const LeHandle *handle, bool major)
    {
        const std::optional<double> &pending = major ? handle->pending_major_grid_um : handle->pending_minor_grid_um;
        if (pending)
            return *pending;
        const double dbu_per_um = technology_dbu_per_um(handle->root);
        if (dbu_per_um <= 0.0)
            return -1.0;
        return static_cast<double>(major ? handle->major_grid_spacing() : handle->minor_grid_spacing()) / dbu_per_um;
    }

    constexpr const char *kPlacementSnapNames[] = {"none", "site", "fin", "manufacturing"}; // le::PlacementSnapMode order
    constexpr const char *kShapeSnapNames[] = {"none", "user", "manufacturing", "fin", "tracks"}; // le::ShapeSnapMode order
    // "path" covers vias and via arrays too (le::shape_snap_slot) - they
    // share one mode, so there's no separate "via" entry to disagree with it.
    constexpr std::pair<const char *, le::PieceKind> kShapeSnapKinds[] = {
        {"rect", le::PieceKind::RECT}, {"polygon", le::PieceKind::POLYGON}, {"path", le::PieceKind::PATH}};

    // The settings file's JSON - everything the Settings panel edits, plus
    // the snap modes the secondary toolbar sets. Grid spacing is stored in
    // um (portable across technologies with different dbu scales) and left
    // out if it can't be expressed in um yet (no Technology, nothing pending).
    // "#rrggbb" <-> Color, for the settings file's layer_colors. parse
    // accepts either case, with
    // or without the "#".
    std::string hex_color(le::Color color)
    {
        char text[8];
        std::snprintf(text, sizeof(text), "#%02x%02x%02x", color.r, color.g, color.b);
        return text;
    }

    std::optional<le::Color> parse_hex_color(std::string_view text)
    {
        if (!text.empty() && text.front() == '#')
            text.remove_prefix(1);
        if (text.size() != 6 || !std::ranges::all_of(text, [](char c)
                                                     { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
            return std::nullopt;
        const unsigned long value = std::stoul(std::string(text), nullptr, 16);
        return le::Color{static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value), 255};
    }

    // The settings file's format version. Bump it when a key is renamed,
    // moved or changes meaning, add the step to migrate_settings_json, and
    // add the new format's golden file under src/api/tests/fixtures/
    // settings/. Adding a key needs neither - an older file just lacks it.
    constexpr int64_t kSettingsVersion = 3;

    nlohmann::json settings_to_json(const LeHandle *handle)
    {
        nlohmann::json j;
        j["version"] = kSettingsVersion;
        nlohmann::json grid = nlohmann::json::object();
        if (const double minor = grid_spacing_um_unlocked(handle, false); minor > 0.0)
            grid["minor_um"] = minor;
        if (const double major = grid_spacing_um_unlocked(handle, true); major > 0.0)
            grid["major_um"] = major;
        j["grid"] = grid;
        j["ruler_label_size_px"] = handle->ruler_label_size_px();
        j["label_min_size_px"] = handle->label_min_size_px();
        j["label_max_size_px"] = handle->label_max_size_px();
        j["flightline_max_fanout"] = handle->flightline_max_fanout();
        j["undo_limit"] = handle->command_history.undo_limit();
        j["confirm_overwrite"] = handle->confirm_overwrite;
        j["max_concurrency"] = handle->max_concurrency_;
        j["placement_snap_mode"] = kPlacementSnapNames[static_cast<size_t>(handle->placement_snap_mode())];
        nlohmann::json shape_snap = nlohmann::json::object();
        for (const auto &[name, kind] : kShapeSnapKinds)
            shape_snap[name] = kShapeSnapNames[static_cast<size_t>(handle->shape_snap_mode(kind))];
        j["shape_snap_modes"] = shape_snap;
        nlohmann::json layer_colors = nlohmann::json::object();
        for (const auto &[row_name, color] : handle->layer_color_overrides())
            layer_colors[row_name] = hex_color(color);
        j["layer_colors"] = layer_colors;

        // Each extension's section, with its own format version; sections of
        // extensions this build lacks are written back as they were read.
        nlohmann::json extensions =
            handle->unknown_extension_settings_json.empty() ? nlohmann::json::object() : nlohmann::json::parse(handle->unknown_extension_settings_json);
        for (const auto &[name, section] : le::ext::registry().settings())
        {
            if (!section.save)
                continue;
            le::ext::ExtensionContext ctx(const_cast<LeHandle *>(handle), name);
            nlohmann::json body = section.save(ctx);
            if (!body.is_object())
            {
                spdlog::warn("settings: extension {}'s section isn't a JSON object; not saved", name);
                continue;
            }
            body["version"] = section.version;
            extensions[name] = std::move(body);
        }
        if (!extensions.empty())
            j["extensions"] = std::move(extensions);

        if (!handle->unknown_settings_json.empty())
        {
            const nlohmann::json unknown = nlohmann::json::parse(handle->unknown_settings_json);
            for (const auto &[key, value] : unknown.items())
                if (!j.contains(key))
                    j[key] = value;
        }
        return j;
    }

    // Brings a settings file written at format `version` up to
    // kSettingsVersion, one step per version. A step never overwrites a key
    // that's already present: an older layout_engine saving a newer file
    // keeps the keys it doesn't know, so a newer-format key can sit beside
    // its older-format one, and the newer one wins.
    void migrate_settings_json(nlohmann::json &j, int64_t version)
    {
        // 1 -> 2: the single label size became the max of a min/max pair,
        // and vias share the path snap mode instead of having their own.
        if (version < 2)
        {
            if (j.contains("label_size_px") && !j.contains("label_max_size_px"))
                j["label_max_size_px"] = j["label_size_px"];
            j.erase("label_size_px");
            if (j.contains("shape_snap_modes") && j["shape_snap_modes"].is_object())
                j["shape_snap_modes"].erase("via");
        }
        // 2 -> 3: the hierarchy depth is a per-session view choice, not a
        // setting; every session starts at 0.
        if (version < 3)
            j.erase("hierarchy_depth");
    }

    // Applies whatever `j` holds - a missing key keeps its current value,
    // and a key of the wrong type or with an unknown/invalid value is
    // skipped with a warning rather than failing the whole load, so an
    // older or hand-edited file still loads what it can.
    void apply_settings_json(LeHandle *handle, const nlohmann::json &j, const std::string &source)
    {
        const auto warn = [&](const char *key)
        { spdlog::warn("load_settings: {}: ignoring invalid \"{}\"", source, key); };
        const auto number = [&](const nlohmann::json &parent, const char *key) -> std::optional<double>
        {
            if (!parent.is_object() || !parent.contains(key))
                return std::nullopt;
            if (!parent[key].is_number())
            {
                warn(key);
                return std::nullopt;
            }
            return parent[key].get<double>();
        };
        const auto index_of = [](const auto &names, const std::string &value) -> std::optional<size_t>
        {
            for (size_t i = 0; i < std::size(names); ++i)
                if (value == names[i])
                    return i;
            return std::nullopt;
        };

        if (j.contains("grid"))
            set_grid_spacing_um_unlocked(handle, number(j["grid"], "minor_um").value_or(0.0), number(j["grid"], "major_um").value_or(0.0));
        if (const auto v = number(j, "ruler_label_size_px"))
            handle->set_ruler_label_size_px(*v);
        if (const auto v = number(j, "label_min_size_px"))
            handle->set_label_min_size_px(*v);
        if (const auto v = number(j, "label_max_size_px"))
            handle->set_label_max_size_px(*v);
        if (const auto v = number(j, "flightline_max_fanout"); v && *v >= 0)
            handle->set_flightline_max_fanout(static_cast<int>(*v));
        if (const auto v = number(j, "undo_limit"); v && *v >= 0 && *v <= INT32_MAX)
            handle->command_history.set_undo_limit(static_cast<size_t>(*v));
        if (const auto v = number(j, "max_concurrency"))
            set_max_concurrency_unlocked(handle, static_cast<int32_t>(*v));
        handle->unknown_extension_settings_json.clear();
        if (j.contains("extensions"))
        {
            if (!j["extensions"].is_object())
                warn("extensions");
            else
            {
                nlohmann::json unknown = nlohmann::json::object();
                for (const auto &[name, body] : j["extensions"].items())
                {
                    const auto &sections = le::ext::registry().settings();
                    const auto section = sections.find(name);
                    if (section == sections.end() || !section->second.load)
                    {
                        unknown[name] = body;
                        continue;
                    }
                    if (!body.is_object())
                    {
                        spdlog::warn("load_settings: {}: ignoring extension {}'s section, which isn't an object", source, name);
                        continue;
                    }
                    const int version = body.contains("version") && body["version"].is_number_integer() ? body["version"].get<int>() : 1;
                    nlohmann::json keys = body;
                    keys.erase("version");
                    le::ext::ExtensionContext ctx(handle, name);
                    section->second.load(ctx, keys, version);
                }
                if (!unknown.empty())
                    handle->unknown_extension_settings_json = unknown.dump();
            }
        }
        if (j.contains("confirm_overwrite"))
        {
            if (j["confirm_overwrite"].is_boolean())
                handle->confirm_overwrite = j["confirm_overwrite"].get<bool>();
            else
                warn("confirm_overwrite");
        }

        if (j.contains("placement_snap_mode"))
        {
            const auto mode = j["placement_snap_mode"].is_string() ? index_of(kPlacementSnapNames, j["placement_snap_mode"].get<std::string>()) : std::nullopt;
            if (mode)
                handle->set_placement_snap_mode(static_cast<le::PlacementSnapMode>(*mode));
            else
                warn("placement_snap_mode");
        }
        if (j.contains("shape_snap_modes") && j["shape_snap_modes"].is_object())
            for (const auto &[name, kind] : kShapeSnapKinds)
            {
                if (!j["shape_snap_modes"].contains(name))
                    continue;
                const nlohmann::json &value = j["shape_snap_modes"][name];
                const auto mode = value.is_string() ? index_of(kShapeSnapNames, value.get<std::string>()) : std::nullopt;
                if (mode && le::shape_snap_mode_applies(kind, static_cast<le::ShapeSnapMode>(*mode)))
                    handle->set_shape_snap_mode(kind, static_cast<le::ShapeSnapMode>(*mode));
                else
                    warn(name);
            }
        // The file's colors replace every current one - it's the whole
        // saved state.
        if (j.contains("layer_colors") && j["layer_colors"].is_object())
        {
            std::map<std::string, le::Color> overrides;
            for (const auto &[row_name, value] : j["layer_colors"].items())
            {
                const auto color = value.is_string() ? parse_hex_color(value.get<std::string>()) : std::nullopt;
                if (color)
                    overrides[row_name] = *color;
                else
                    warn(("layer_colors." + row_name).c_str());
            }
            handle->set_layer_color_overrides(std::move(overrides));
            if (handle->current_technology_id.valid())
                rebuild_view_layers(handle, handle->current_technology_id);
        }
    }
}

namespace le::api_detail
{
    // Unsaved-changes checks - see LeHandle::saved_mutation_version.
    std::string settings_snapshot(const LeHandle *handle) { return settings_to_json(handle).dump(); }
    bool has_unsaved_database_unlocked(const LeHandle *handle) { return handle->root.mutation_version() != handle->saved_mutation_version; }
    bool has_unsaved_settings_unlocked(const LeHandle *handle) { return settings_snapshot(handle) != handle->saved_settings_json; }
}

namespace
{
    // $HOME/.layout_engine/settings.json - "" if HOME isn't set.
    std::string default_settings_path()
    {
        const char *home = std::getenv("HOME");
        if (!home || !home[0])
            return {};
        return (std::filesystem::path(home) / ".layout_engine" / "settings.json").string();
    }
}

extern "C"
{
    int32_t le_max_concurrency(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_); // a read - polled every GUI frame
        return handle->max_concurrency_;
    }

    void le_set_max_concurrency(LeHandle *handle, int32_t max_concurrency)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        set_max_concurrency_unlocked(handle, max_concurrency);
    }

    void le_set_flightline_max_fanout(LeHandle *handle, int32_t max_fanout)
    {
        if (!handle || max_fanout < 0)
            return;
        HandleWriteLock lock(handle);
        handle->set_flightline_max_fanout(max_fanout);
    }

    void le_set_confirm_overwrite(LeHandle *handle, int32_t confirm)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->confirm_overwrite = confirm != 0;
    }

    int32_t le_confirm_overwrite(LeHandle *handle)
    {
        if (!handle)
            return 1;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->confirm_overwrite ? 1 : 0;
    }

    int32_t le_flightline_max_fanout(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->flightline_max_fanout();
    }

    int64_t le_minor_grid_spacing(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->minor_grid_spacing();
    }

    void le_set_minor_grid_spacing(LeHandle *handle, int64_t dbu)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_minor_grid_spacing(dbu);
    }

    int64_t le_major_grid_spacing(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->major_grid_spacing();
    }

    void le_set_major_grid_spacing(LeHandle *handle, int64_t dbu)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_major_grid_spacing(dbu);
    }

    double le_ruler_label_size(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_); // a read - polled every GUI frame
        return handle->ruler_label_size_px();
    }

    void le_set_ruler_label_size(LeHandle *handle, double px)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_ruler_label_size_px(px);
    }

    int32_t le_set_layer_color(LeHandle *handle, const char *layer_name, int32_t r, int32_t g, int32_t b)
    {
        const auto in_range = [](int32_t c)
        { return c >= 0 && c <= 255; };
        if (!handle || !layer_name || !in_range(r) || !in_range(g) || !in_range(b))
            return 1;
        HandleWriteLock lock(handle);
        handle->set_layer_color(layer_name, le::Color{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), 255});
        return 0;
    }

    void le_reset_layer_color(LeHandle *handle, const char *layer_name)
    {
        if (!handle || !layer_name)
            return;
        HandleWriteLock lock(handle);
        if (handle->reset_layer_color(layer_name) && handle->current_technology_id.valid())
            rebuild_view_layers(handle, handle->current_technology_id);
    }

    int32_t le_layer_color_rgb(LeHandle *handle, const char *layer_name)
    {
        if (!handle || !layer_name)
            return -1;
        HandleWriteLock lock(handle); // ensure_view_layers_current may rebuild
        ensure_view_layers_current(handle);
        for (const le::ViewLayerRow &row : handle->view_layers.rows())
            if (row.name == layer_name && !row.columns.empty())
                if (const le::ViewLayerData *data = handle->view_layers.get(row.columns.front().id))
                    return (data->style.outline_color.r << 16) | (data->style.outline_color.g << 8) | data->style.outline_color.b;
        return -1;
    }

    double le_label_min_size(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->label_min_size_px();
    }

    void le_set_label_min_size(LeHandle *handle, double px)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_label_min_size_px(px);
    }

    double le_label_max_size(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return handle->label_max_size_px();
    }

    void le_set_label_max_size(LeHandle *handle, double px)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        handle->set_label_max_size_px(px);
    }

    double le_grid_spacing_um(LeHandle *handle, int32_t major)
    {
        if (!handle)
            return -1.0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return grid_spacing_um_unlocked(handle, major != 0);
    }

    void le_set_grid_spacing_um(LeHandle *handle, double minor_um, double major_um)
    {
        if (!handle)
            return;
        HandleWriteLock lock(handle);
        set_grid_spacing_um_unlocked(handle, minor_um, major_um);
    }

    double le_manufacturing_grid_um(LeHandle *handle)
    {
        if (!handle)
            return 0.0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        // technology_manufacturing_grid's own validity checks (a positive
        // grid, a known dbu scale), then the LEF's own micron value.
        if (!le::technology_manufacturing_grid(handle->root))
            return 0.0;
        const le::TechnologyData *technology = handle->root.get_technology(handle->root.get_technology_ids().front());
        return *technology->manufacturing_grid;
    }

    const char *le_default_settings_path(void)
    {
        static const std::string path = default_settings_path();
        return path.c_str();
    }

    int32_t le_save_settings(LeHandle *handle, const char *path)
    try
    {
        if (!handle)
            return 1;
        const std::string target = path && path[0] ? std::string(path) : default_settings_path();
        if (target.empty())
        {
            spdlog::error("save_settings: no path given and HOME isn't set");
            return 1;
        }
        HandleWriteLock lock(handle); // updates saved_settings_json below
        const nlohmann::json j = settings_to_json(handle);
        const std::string text = j.dump(2) + "\n";
        std::error_code ec;
        if (const std::filesystem::path parent = std::filesystem::path(target).parent_path(); !parent.empty())
            std::filesystem::create_directories(parent, ec);
        std::ofstream out(target, std::ios::trunc);
        if (!out || !(out << text) || !out.flush())
        {
            spdlog::error("save_settings: couldn't write '{}'", target);
            return 1;
        }
        handle->saved_settings_json = j.dump(); // the settings are saved
        spdlog::info("save_settings: wrote '{}'", target);
        return 0;
    }
    catch (...)
    {
        log_exception("save_settings");
        return 1;
    }

    int32_t le_has_unsaved_database_changes(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return has_unsaved_database_unlocked(handle) ? 1 : 0;
    }

    int32_t le_has_unsaved_settings(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::shared_lock<std::shared_mutex> lock(handle->mutex_);
        return has_unsaved_settings_unlocked(handle) ? 1 : 0;
    }

    int32_t le_load_settings(LeHandle *handle, const char *path)
    try
    {
        if (!handle)
            return 1;
        const std::string source = path && path[0] ? std::string(path) : default_settings_path();
        std::ifstream in(source);
        if (!in)
        {
            spdlog::error("load_settings: couldn't open '{}'", source);
            return 1;
        }
        nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object())
        {
            spdlog::error("load_settings: '{}' isn't a JSON object", source);
            return 1;
        }
        // Files without a version predate versioning, which started at 1.
        int64_t version = 1;
        if (j.contains("version"))
        {
            if (j["version"].is_number_integer() && j["version"].get<int64_t>() >= 1)
                version = j["version"].get<int64_t>();
            else
                spdlog::warn("load_settings: {}: ignoring invalid \"version\"", source);
        }
        if (version > kSettingsVersion)
            spdlog::warn("load_settings: {}: written by a newer layout_engine (format {}, this one reads up to {}) - "
                         "loading the settings this version knows",
                         source, version, kSettingsVersion);
        migrate_settings_json(j, version);
        HandleWriteLock lock(handle);
        apply_settings_json(handle, j, source);
        handle->unknown_settings_json.clear();
        nlohmann::json unknown = nlohmann::json::object();
        const nlohmann::json known = settings_to_json(handle);
        for (const auto &[key, value] : j.items())
            if (!known.contains(key))
                unknown[key] = value;
        if (!unknown.empty())
            handle->unknown_settings_json = unknown.dump();
        handle->saved_settings_json = settings_snapshot(handle); // the settings are saved
        spdlog::info("load_settings: read '{}'", source);
        return 0;
    }
    catch (...)
    {
        log_exception("load_settings");
        return 1;
    }
}
