#include "api_internal.hpp"
#include <json.hpp>
#include <spdlog/spdlog.h>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace le::api_detail;

namespace
{
    // --- The session saved beside the database (a .led file's SESS chunk) ---
    // Per-design view state that settings.json (global) doesn't hold: which
    // view is open, the viewport, the generated "current" objects, and
    // layer/purpose/filter visibility and selectability. Loose JSON:
    // unknown keys are ignored and missing ones keep their defaults, so it
    // needs no migrations. Purposes are keyed by label - ordinals change
    // with the extensions built in. Object references use the native
    // format's {"$ref", "index", "generation"} form, stored as file rows.
    constexpr int kSessionFormat = 1;

    template <class Id>
    nlohmann::json session_ref(const char *klass, Id id)
    {
        return {{"$ref", klass}, {"index", id.index}, {"generation", id.generation}};
    }

    // `obj[key]` if it's an unsigned number that fits a uint32_t, else
    // `fallback`. json::value would throw on a mistyped value.
    uint32_t session_uint32(const nlohmann::json &obj, const char *key, uint32_t fallback)
    {
        if (!obj.is_object() || !obj.contains(key) || !obj[key].is_number_unsigned())
            return fallback;
        const uint64_t value = obj[key].get<uint64_t>();
        return value <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(value) : fallback;
    }

    template <class Id>
    Id session_id(const nlohmann::json &ref)
    {
        if (!ref.is_object() || !ref.contains("index") || !ref["index"].is_number_unsigned())
            return Id{};
        return Id{session_uint32(ref, "index", Id{}.index), session_uint32(ref, "generation", 0)};
    }

    nlohmann::json filter_values_json(const std::set<std::string> &values) { return nlohmann::json(std::vector<std::string>(values.begin(), values.end())); }

    // The session's names for le::ViewFlip, in enum order.
    constexpr std::array<const char *, 3> kViewFlipNames = {"none", "horizontal", "vertical"};
}

namespace le::api_detail
{
    std::string session_to_json(const LeHandle *handle)
    {
        const le::Root &root = handle->root;
        nlohmann::json j;
        j["format"] = kSessionFormat;
        if (root.get_layout(handle->current_layout()))
            j["view"] = {{"layout", session_ref("Layout", handle->current_layout())}};
        else if (root.get_abstract(handle->current_abstract()))
            j["view"] = {{"abstract", session_ref("Abstract", handle->current_abstract())}};
        j["viewport"] = {{"pan", {handle->pan().x, handle->pan().y}},
                         {"scale", handle->scale()},
                         {"flip", kViewFlipNames[static_cast<std::size_t>(handle->view_flip())]}};

        nlohmann::json current = nlohmann::json::object();
        if (root.get_technology(handle->current_technology_id))
            current["technology"] = session_ref("Technology", handle->current_technology_id);
        if (root.get_abstract(handle->current_abstract_id))
            current["abstract"] = session_ref("Abstract", handle->current_abstract_id);
        if (root.get_schematic(handle->current_schematic_id))
            current["schematic"] = session_ref("Schematic", handle->current_schematic_id);
        if (root.get_layout(handle->current_layout_id))
            current["layout"] = session_ref("Layout", handle->current_layout_id);
        j["current"] = current;

        nlohmann::json layers_visible = nlohmann::json::object(), layers_selectable = nlohmann::json::object();
        for (const auto &[name, visible] : handle->layer_name_visibility())
            layers_visible[name] = visible;
        for (const auto &[name, selectable] : handle->layer_name_selectability())
            layers_selectable[name] = selectable;
        j["layers"] = {{"visible", layers_visible}, {"selectable", layers_selectable}};

        nlohmann::json purposes_visible = nlohmann::json::object(), purposes_selectable = nlohmann::json::object();
        for (const le::ViewLayerPurposeInfo &info : le::kViewLayerPurposes)
        {
            purposes_visible[std::string(info.label)] = handle->is_purpose_visible(info.purpose);
            purposes_selectable[std::string(info.label)] = handle->purpose_selectable_toggle(info.purpose);
        }
        j["purposes"] = {{"visible", purposes_visible}, {"selectable", purposes_selectable}};

        const auto filters = [](const le::ObjectFilterSets &sets) {
            return nlohmann::json{{"placement_types", filter_values_json(sets.placement_types)},
                                  {"route_uses", filter_values_json(sets.route_uses)},
                                  {"placement_statuses", filter_values_json(sets.placement_statuses)},
                                  {"routing_statuses", filter_values_json(sets.routing_statuses)},
                                  {"route_shapes", filter_values_json(sets.route_shapes)}};
        };
        j["filters"] = {{"hidden", filters(handle->hidden_objects())}, {"unselectable", filters(handle->unselectable_objects())}};
        return j.dump();
    }

    // Applies what a session holds, skipping anything malformed or naming
    // objects that no longer exist. Call with the handle locked.
    void apply_session_json(LeHandle *handle, const std::string &text, const std::string &path)
    {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object())
        {
            spdlog::warn("read_db: {}'s session isn't valid JSON; ignored", path);
            return;
        }
        if (j.contains("format") && j["format"].is_number_integer() && j["format"].get<int64_t>() > kSessionFormat)
            spdlog::warn("read_db: {}'s session was written by a newer Layout Engine; restoring what this one knows", path);
        le::Root &root = handle->root;
        const auto object = [&](const char *key) -> const nlohmann::json & {
            static const nlohmann::json empty = nlohmann::json::object();
            return j.contains(key) && j[key].is_object() ? j[key] : empty;
        };
        const auto bools = [](const nlohmann::json &table, const char *key, auto &&apply) {
            if (table.contains(key) && table[key].is_object())
                for (const auto &[name, value] : table[key].items())
                    if (value.is_boolean())
                        apply(name, value.get<bool>());
        };

        const nlohmann::json &layers = object("layers");
        bools(layers, "visible", [&](const std::string &name, bool v) { handle->set_layer_name_visible(name, v); });
        bools(layers, "selectable", [&](const std::string &name, bool v) { handle->set_layer_name_selectable(name, v); });
        const nlohmann::json &purposes = object("purposes");
        bools(purposes, "visible", [&](const std::string &label, bool v) {
            if (const auto purpose = le::purpose_from_label(label))
                handle->set_purpose_visible(*purpose, v);
        });
        bools(purposes, "selectable", [&](const std::string &label, bool v) {
            if (const auto purpose = le::purpose_from_label(label))
                handle->set_purpose_selectable(*purpose, v);
        });
        const nlohmann::json &filters = object("filters");
        for (const auto &[key, visible_axis] : {std::pair{"hidden", true}, std::pair{"unselectable", false}})
        {
            if (!filters.contains(key) || !filters[key].is_object())
                continue;
            for (const auto &[list, filter] : {std::pair{"placement_types", LE_OBJECT_FILTER_PLACEMENT_TYPE}, std::pair{"route_uses", LE_OBJECT_FILTER_ROUTE_USE},
                                               std::pair{"placement_statuses", LE_OBJECT_FILTER_PLACEMENT_STATUS},
                                               std::pair{"routing_statuses", LE_OBJECT_FILTER_ROUTING_STATUS},
                                               std::pair{"route_shapes", LE_OBJECT_FILTER_ROUTE_SHAPE}})
                if (filters[key].contains(list) && filters[key][list].is_array())
                    for (const auto &value : filters[key][list])
                        if (value.is_string())
                        {
                            if (visible_axis)
                                handle->set_object_filter_visible(filter, value.get<std::string>(), false);
                            else
                                handle->set_object_filter_selectable(filter, value.get<std::string>(), false);
                        }
        }

        const nlohmann::json &current = object("current");
        if (const auto id = session_id<le::TechnologyId>(current.value("technology", nlohmann::json())); root.get_technology(id))
            handle->current_technology_id = id;
        if (const auto id = session_id<le::AbstractId>(current.value("abstract", nlohmann::json())); root.get_abstract(id))
            handle->current_abstract_id = id;
        if (const auto id = session_id<le::SchematicId>(current.value("schematic", nlohmann::json())); root.get_schematic(id))
            handle->current_schematic_id = id;
        if (const auto id = session_id<le::LayoutId>(current.value("layout", nlohmann::json())); root.get_layout(id))
            handle->current_layout_id = id;

        // Only one view is open at a time (le_set_current_design_layout).
        const nlohmann::json &view = object("view");
        if (const auto layout = session_id<le::LayoutId>(view.value("layout", nlohmann::json())); root.get_layout(layout))
        {
            handle->set_current_layout(layout);
            handle->set_current_abstract(le::AbstractId{});
        }
        else if (const auto abstract = session_id<le::AbstractId>(view.value("abstract", nlohmann::json())); root.get_abstract(abstract))
        {
            handle->set_current_abstract(abstract);
            handle->set_current_layout(le::LayoutId{});
        }
        const nlohmann::json &viewport = object("viewport");
        if (viewport.contains("pan") && viewport["pan"].is_array() && viewport["pan"].size() == 2 && viewport["pan"][0].is_number_integer() &&
            viewport["pan"][1].is_number_integer())
            handle->set_pan(le::Point{viewport["pan"][0].get<int64_t>(), viewport["pan"][1].get<int64_t>()});
        if (viewport.contains("scale") && viewport["scale"].is_number())
            handle->set_scale(viewport["scale"].get<double>());
        // Older sessions have no flip: unmirrored.
        handle->set_view_flip(le::ViewFlip::NONE);
        if (viewport.contains("flip") && viewport["flip"].is_string())
            for (std::size_t i = 0; i < kViewFlipNames.size(); ++i)
                if (viewport["flip"].get<std::string>() == kViewFlipNames[i])
                    handle->set_view_flip(static_cast<le::ViewFlip>(i));
    }
}
