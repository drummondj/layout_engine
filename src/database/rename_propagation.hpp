#pragma once

// Rename-propagation side effects for the Schematic<->Layout link
// (LINKING_STRATEGY_RESEARCH.md section 5c): when an Instance renames,
// every DEF-style hierarchical name that embeds its own path segment -
// its own linked Placement (the renamed segment is the trailing
// component) and every descendant Instance/Net's linked
// Placement/Route/PhysicalPort (the renamed segment is a leading
// component of theirs) - needs to be rewritten to match. Header-only,
// like hierarchical_resolver.hpp/schematic_layout_linker.hpp (which this
// builds on) - no new CMake compiled target for one relatively small
// feature.
//
// Deliberately real id-based graph traversal (walking Instance/Net
// objects and looking up their *linked* Placement/Route/PhysicalPort via
// a request-scoped reverse map built once), never string-prefix matching
// against existing names - LINKING_STRATEGY_RESEARCH.md section 6 flags
// prefix matching as a real bug risk (e.g. renaming "top/a/b" incorrectly
// also touching a sibling "top/a/b2").
//
// Every mutation callback below is a template parameter, not a direct
// dependency on src/editing/ (Transaction) - src/editing/transaction.hpp
// already depends on src/database/database.hpp, so a direct dependency
// the other way would be circular. A caller that wants each rename
// recorded into an undo transaction (api.cpp's own
// le_rename_instance_propagate) supplies callbacks that do so; a test
// (or any caller that doesn't care about undo) can pass no-ops.

#include "database.hpp"
#include "hierarchical_resolver.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace le
{
    struct RenamePropagationResult
    {
        size_t placements_renamed = 0;
        size_t routes_renamed = 0;
        size_t physical_ports_renamed = 0;
    };

    namespace rename_detail
    {
        struct PhysicalReverseMaps
        {
            std::unordered_map<InstanceId, PlacementId> instance_to_placement;
            std::unordered_map<NetId, RouteId> net_to_route;
            std::unordered_map<NetId, PhysicalPortId> net_to_physical_port;
        };

        /// @brief Built once per propagate_instance_rename() call, across
        /// every Layout in `root` (not just one "the" Layout - matches
        /// link_physical's own for_each_layout_id scope) - O(total
        /// physical design size) once, turning every per-descendant
        /// lookup below into O(1) rather than a fresh linear scan per
        /// descendant.
        inline PhysicalReverseMaps build_reverse_maps(Root &root)
        {
            PhysicalReverseMaps maps;
            root.for_each_layout_id(
                [&](LayoutId layout_id)
                {
                    for (const PlacementId placement_id : root.get_layout_placements(layout_id))
                    {
                        const PlacementData *placement = root.get_placement(placement_id);
                        if (placement && placement->instance.valid())
                            maps.instance_to_placement[placement->instance] = placement_id;
                    }
                    for (const RouteId route_id : root.get_layout_routes(layout_id))
                    {
                        const RouteData *route = root.get_route(route_id);
                        if (route && route->net.valid())
                            maps.net_to_route[route->net] = route_id;
                    }
                    for (const PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
                    {
                        const PhysicalPortData *port = root.get_physical_port(port_id);
                        if (port && port->net.valid())
                            maps.net_to_physical_port[port->net] = port_id;
                    }
                });
            return maps;
        }

        template <typename OnPlacementRenamed, typename OnRouteRenamed, typename OnPhysicalPortRenamed>
        inline void rename_descendants(Root &root, SchematicId schematic, const std::string &new_prefix,
                                        const PhysicalReverseMaps &maps, RenamePropagationResult &result,
                                        OnPlacementRenamed &on_placement_renamed, OnRouteRenamed &on_route_renamed,
                                        OnPhysicalPortRenamed &on_physical_port_renamed)
        {
            for (const InstanceId child_id : root.get_schematic_instances(schematic))
            {
                const InstanceData *child = root.get_instance(child_id);
                if (!child)
                    continue;
                const std::string child_path = new_prefix + "/" + child->name;

                const auto it = maps.instance_to_placement.find(child_id);
                if (it != maps.instance_to_placement.end())
                {
                    const PlacementData before = *root.get_placement(it->second);
                    root.update_placement(it->second, LayoutId{}, std::nullopt, std::nullopt,
                                           std::optional<std::string>(child_path), std::nullopt, std::nullopt,
                                           std::nullopt, std::nullopt, std::nullopt, std::nullopt);
                    on_placement_renamed(it->second, before, *root.get_placement(it->second));
                    ++result.placements_renamed;
                }

                const SchematicId grandchild_schematic = root.get_design_schematic(child->reference_design);
                if (grandchild_schematic.valid())
                    rename_descendants(root, grandchild_schematic, child_path, maps, result, on_placement_renamed,
                                        on_route_renamed, on_physical_port_renamed);
            }

            for (const NetId net_id : root.get_schematic_nets(schematic))
            {
                const NetData *net = root.get_net(net_id);
                if (!net)
                    continue;
                const std::string net_path = new_prefix + "/" + net->name;

                const auto route_it = maps.net_to_route.find(net_id);
                if (route_it != maps.net_to_route.end())
                {
                    const RouteData before = *root.get_route(route_it->second);
                    root.update_route(route_it->second, LayoutId{}, std::nullopt, std::optional<std::string>(net_path),
                                       std::nullopt, std::nullopt, std::nullopt, std::nullopt);
                    on_route_renamed(route_it->second, before, *root.get_route(route_it->second));
                    ++result.routes_renamed;
                }

                const auto port_it = maps.net_to_physical_port.find(net_id);
                if (port_it != maps.net_to_physical_port.end())
                {
                    const PhysicalPortData before = *root.get_physical_port(port_it->second);
                    root.update_physical_port(port_it->second, LayoutId{}, std::nullopt,
                                               std::optional<std::string>(net_path), std::nullopt, std::nullopt,
                                               std::nullopt, std::nullopt, std::nullopt, std::nullopt);
                    on_physical_port_renamed(port_it->second, before, *root.get_physical_port(port_it->second));
                    ++result.physical_ports_renamed;
                }
            }
        }
    }

    /// @brief Propagates an already-applied Instance rename
    /// (`instance_id`'s own `.name` must already be the *new* name by
    /// the time this runs) to every Placement/Route/PhysicalPort whose
    /// own DEF-style hierarchical name embeds the renamed Instance's own
    /// path segment.
    ///
    /// The renamed Instance's own parent-path prefix (the part of its
    /// own linked Placement's name *above* its own segment, which the
    /// rename doesn't change) is recovered from that Placement's
    /// existing name via hierarchy::split_path (escape-aware) rather
    /// than by string-stripping the old Instance name - works
    /// regardless of what the old name was, and tolerates DEF's own
    /// backslash-escaping of special characters in ancestor segments.
    ///
    /// Known limitation: if the renamed Instance itself has no linked
    /// Placement (e.g. `link` hasn't resolved it, or it's physical_only
    /// under a different name), its own correct path prefix can't be
    /// recovered this way, and descendant propagation is skipped
    /// entirely (returns with everything at 0) rather than guessing a
    /// wrong prefix - a real, documented scope limit, not an oversight.
    template <typename OnPlacementRenamed, typename OnRouteRenamed, typename OnPhysicalPortRenamed>
    inline RenamePropagationResult propagate_instance_rename(Root &root, InstanceId instance_id,
                                                               OnPlacementRenamed on_placement_renamed,
                                                               OnRouteRenamed on_route_renamed,
                                                               OnPhysicalPortRenamed on_physical_port_renamed)
    {
        RenamePropagationResult result;
        const InstanceData *instance = root.get_instance(instance_id);
        if (!instance)
            return result;

        const rename_detail::PhysicalReverseMaps maps = rename_detail::build_reverse_maps(root);

        const auto self_it = maps.instance_to_placement.find(instance_id);
        if (self_it == maps.instance_to_placement.end())
            return result; // no linked Placement - see this function's own "known limitation" comment

        const PlacementData *own_placement = root.get_placement(self_it->second);
        if (!own_placement)
            return result;

        std::vector<std::string> ancestor_segments = hierarchy::split_path(own_placement->name);
        if (!ancestor_segments.empty())
            ancestor_segments.pop_back(); // drop the (old) trailing segment for this Instance itself

        std::string parent_prefix;
        for (size_t i = 0; i < ancestor_segments.size(); ++i)
        {
            if (i)
                parent_prefix += "/";
            parent_prefix += ancestor_segments[i];
        }
        const std::string new_own_path = parent_prefix.empty() ? instance->name : parent_prefix + "/" + instance->name;

        const PlacementData before = *own_placement;
        root.update_placement(self_it->second, LayoutId{}, std::nullopt, std::nullopt,
                               std::optional<std::string>(new_own_path), std::nullopt, std::nullopt, std::nullopt,
                               std::nullopt, std::nullopt, std::nullopt);
        on_placement_renamed(self_it->second, before, *root.get_placement(self_it->second));
        ++result.placements_renamed;

        const SchematicId child_schematic = root.get_design_schematic(instance->reference_design);
        if (child_schematic.valid())
            rename_detail::rename_descendants(root, child_schematic, new_own_path, maps, result, on_placement_renamed,
                                               on_route_renamed, on_physical_port_renamed);

        return result;
    }
}
