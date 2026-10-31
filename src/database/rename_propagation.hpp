#pragma once

// Renaming an Instance or Net renames the physical objects named after it.
// A Placement, Route or PhysicalPort names its logical counterpart by a
// DEF-style hierarchical path. One linked to the renamed object keeps its
// path's prefix byte for byte and takes the escaped new name as its last
// segment; one named below such a path (a descendant's Placement, Route
// or PhysicalPort) has that leading part replaced, whole segments only,
// so "top/a/b" never touches "top/a/b2" and every other segment keeps its
// escaping. A module instantiated twice has one Instance per child but a
// linked Placement per instantiation, and each is renamed by its own path.
//
// plan_*_rename computes every change and every clash before anything
// changes, so a caller can refuse a rename that would collide.
// apply_rename_plan's callbacks are template parameters, not a dependency
// on src/editing/ (which depends on this module): a caller recording undo
// passes callbacks that record each update.

#include "database.hpp"
#include "hierarchical_resolver.hpp"

#include <fmt/format.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace le
{
    /// @brief Every rename a planned Instance or Net rename makes, and
    /// every name it would collide with.
    struct RenamePlan
    {
        std::vector<std::pair<PlacementId, std::string>> placements;
        std::vector<std::pair<RouteId, std::string>> routes;
        std::vector<std::pair<PhysicalPortId, std::string>> physical_ports;
        /// @brief One message per new name already taken in its Layout.
        std::vector<std::string> clashes;
    };

    struct RenamePropagationResult
    {
        size_t placements_renamed = 0;
        size_t routes_renamed = 0;
        size_t physical_ports_renamed = 0;
    };

    namespace rename_detail
    {
        /// Old path -> new path, for one Layout.
        using PathMap = std::unordered_map<std::string, std::string>;

        /// @brief `name` rewritten if it is a path in `paths` or lies
        /// below one (the longest such prefix, ending at a separator).
        inline std::optional<std::string> rewritten(const std::string &name, const PathMap &paths)
        {
            if (const auto it = paths.find(name); it != paths.end())
                return it->second;
            const std::vector<size_t> separators = hierarchy::separator_positions(name);
            for (auto at = separators.rbegin(); at != separators.rend(); ++at)
                if (const auto it = paths.find(name.substr(0, *at)); it != paths.end())
                    return it->second + name.substr(*at);
            return std::nullopt;
        }

        /// @brief `path` with its last segment replaced by `new_name`, escaped.
        inline std::string with_last_segment(const std::string &path, std::string_view new_name)
        {
            const std::vector<size_t> separators = hierarchy::separator_positions(path);
            const std::string prefix = separators.empty() ? std::string{} : path.substr(0, separators.back() + 1);
            return prefix + hierarchy::escape_segment(new_name);
        }

        /// @brief Plans renaming every object of one kind in `ids` whose
        /// name `paths` rewrites, noting clashes with names not being
        /// renamed away.
        template <typename Id, typename GetName, typename FindByName>
        void plan_kind(const std::vector<Id> &ids, const PathMap &paths, GetName &&name_of, FindByName &&find_by_name, const char *kind,
                       std::vector<std::pair<Id, std::string>> &out, std::vector<std::string> &clashes)
        {
            const size_t first = out.size();
            std::unordered_set<Id> renamed;
            for (const Id id : ids)
                if (const std::string *name = name_of(id))
                    if (std::optional<std::string> next = rewritten(*name, paths); next && *next != *name)
                    {
                        out.emplace_back(id, std::move(*next));
                        renamed.insert(id);
                    }
            for (size_t i = first; i < out.size(); ++i)
                if (const Id taken = find_by_name(out[i].second); taken.valid() && !renamed.contains(taken))
                    clashes.push_back(fmt::format("a {} named '{}' already exists", kind, out[i].second));
        }

        /// @brief Plans the renames in every Layout `paths_by_layout` names.
        inline RenamePlan plan(const Root &root, const std::unordered_map<LayoutId, PathMap> &paths_by_layout)
        {
            RenamePlan plan;
            for (const auto &[layout, paths] : paths_by_layout)
            {
                plan_kind(
                    root.get_layout_placements(layout), paths,
                    [&](PlacementId id) { const PlacementData *d = root.get_placement(id); return d ? &d->name : nullptr; },
                    [&](const std::string &name) { return root.get_placement_by_name(layout, name); }, "Placement", plan.placements,
                    plan.clashes);
                plan_kind(
                    root.get_layout_routes(layout), paths,
                    [&](RouteId id) { const RouteData *d = root.get_route(id); return d ? &d->name : nullptr; },
                    [&](const std::string &name) { return root.get_route_by_name(layout, name); }, "Route", plan.routes, plan.clashes);
                plan_kind(
                    root.get_layout_physical_ports(layout), paths,
                    [&](PhysicalPortId id) { const PhysicalPortData *d = root.get_physical_port(id); return d ? &d->name : nullptr; },
                    [&](const std::string &name) { return root.get_physical_port_by_name(layout, name); }, "PhysicalPort",
                    plan.physical_ports, plan.clashes);
            }
            return plan;
        }
    }

    /// @brief What renaming `instance_id` to `new_name` does to the
    /// Layouts: its linked Placements, and everything named below them.
    inline RenamePlan plan_instance_rename(const Root &root, InstanceId instance_id, std::string_view new_name)
    {
        std::unordered_map<LayoutId, rename_detail::PathMap> paths_by_layout;
        root.for_each_layout_id([&](LayoutId layout)
                                {
            for (const PlacementId id : root.get_layout_placements(layout))
                if (const PlacementData *placement = root.get_placement(id); placement && placement->instance == instance_id)
                    paths_by_layout[layout][placement->name] = rename_detail::with_last_segment(placement->name, new_name); });
        return rename_detail::plan(root, paths_by_layout);
    }

    /// @brief What renaming `net_id` to `new_name` does to the Layouts: its
    /// linked Routes and PhysicalPorts.
    inline RenamePlan plan_net_rename(const Root &root, NetId net_id, std::string_view new_name)
    {
        std::unordered_map<LayoutId, rename_detail::PathMap> paths_by_layout;
        root.for_each_layout_id([&](LayoutId layout)
                                {
            for (const RouteId id : root.get_layout_routes(layout))
                if (const RouteData *route = root.get_route(id); route && route->net == net_id)
                    paths_by_layout[layout][route->name] = rename_detail::with_last_segment(route->name, new_name);
            for (const PhysicalPortId id : root.get_layout_physical_ports(layout))
                if (const PhysicalPortData *port = root.get_physical_port(id); port && port->net == net_id)
                    paths_by_layout[layout][port->name] = rename_detail::with_last_segment(port->name, new_name); });
        return rename_detail::plan(root, paths_by_layout);
    }

    /// @brief Applies a clash-free `plan`, calling on_<kind>_renamed(id,
    /// before, after) after each update.
    template <typename OnPlacementRenamed, typename OnRouteRenamed, typename OnPhysicalPortRenamed>
    inline RenamePropagationResult apply_rename_plan(Root &root, const RenamePlan &plan, OnPlacementRenamed on_placement_renamed,
                                                     OnRouteRenamed on_route_renamed, OnPhysicalPortRenamed on_physical_port_renamed)
    {
        RenamePropagationResult result;
        for (const auto &[id, name] : plan.placements)
        {
            const PlacementData *existing = root.get_placement(id);
            if (!existing)
                continue;
            const PlacementData before = *existing;
            if (root.update_placement(id, LayoutId{}, std::nullopt, std::nullopt, name, std::nullopt, std::nullopt, std::nullopt,
                                      std::nullopt, std::nullopt, std::nullopt))
            {
                on_placement_renamed(id, before, *root.get_placement(id));
                ++result.placements_renamed;
            }
        }
        for (const auto &[id, name] : plan.routes)
        {
            const RouteData *existing = root.get_route(id);
            if (!existing)
                continue;
            const RouteData before = *existing;
            if (root.update_route(id, LayoutId{}, std::nullopt, name, std::nullopt, std::nullopt, std::nullopt))
            {
                on_route_renamed(id, before, *root.get_route(id));
                ++result.routes_renamed;
            }
        }
        for (const auto &[id, name] : plan.physical_ports)
        {
            const PhysicalPortData *existing = root.get_physical_port(id);
            if (!existing)
                continue;
            const PhysicalPortData before = *existing;
            if (root.update_physical_port(id, LayoutId{}, std::nullopt, name, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                          std::nullopt, std::nullopt))
            {
                on_physical_port_renamed(id, before, *root.get_physical_port(id));
                ++result.physical_ports_renamed;
            }
        }
        return result;
    }
}
