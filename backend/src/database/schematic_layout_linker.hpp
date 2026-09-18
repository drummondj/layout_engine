#pragma once

// Cross-view physical<->logical linking - the physical half of the `link`
// step (LINKING_STRATEGY_RESEARCH.md sections 1, 2), run after
// SVReader::link_unresolved_instances has already resolved every
// Instance.reference_design it can. Header-only, like
// hierarchical_resolver.hpp (which this builds on) - avoids a whole new
// CMake compiled target for one relatively small feature, matching this
// project's own minimal-abstraction bias.

#include "database.hpp"
#include "hierarchical_resolver.hpp"

#include <fmt/format.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace le
{
    struct PhysicalLinkResult
    {
        size_t placements_linked = 0;
        size_t placements_marked_physical_only = 0;
        size_t default_placements_created = 0;
        size_t routes_linked = 0;
        size_t physical_ports_linked = 0;
        /// @brief WARNING:/ERROR: prefixed messages, same convention as
        /// LEFReader/DEFReader/SVReader's own messages(): a Placement
        /// with no matching Instance is a WARNING (LINKING_STRATEGY.md
        /// scenario 1 - still a legitimate physical-only cell); a Route
        /// with no resolvable Net is an ERROR (scenario 2); a
        /// PhysicalPort with no resolvable Net is a WARNING (boundary
        /// power/ground pins legitimately lack one far more often - see
        /// LINKING_STRATEGY_RESEARCH.md section 2).
        std::vector<std::string> messages;
    };

    namespace physical_link_detail
    {
        /// @brief Matches every un-linked Placement in `layout_id`
        /// against `top_schematic` by name (LINKING_STRATEGY_RESEARCH.md
        /// section 1). Populates `linked_instances` with every Instance
        /// that has *any* linked Placement, old or new - not just ones
        /// resolved in this call - since create_default_placements()
        /// below needs that full picture to stay idempotent across
        /// repeated `link` calls (an Instance already placed on a prior
        /// call must not get a second default Placement here).
        ///
        /// A Placement already successfully linked (`.instance.valid()`)
        /// is skipped - that resolution is permanent, matching
        /// link_unresolved_instances' own "already resolved, skip"
        /// convention. A Placement still marked `physical_only`, though,
        /// is *always* re-attempted, never permanently skipped - same
        /// convention link_unresolved_instances itself already uses for
        /// Instance.reference_design (keyed purely on "is it resolved
        /// yet", never on "did we already try and fail once"), since the
        /// whole point of `link` being re-runnable is picking up a fix
        /// made between calls (e.g. the user adds the missing Instance
        /// this Placement was named after). Re-marking/re-warning an
        /// unresolved Placement key `link` re-run is therefore correct,
        /// not a duplicate-work bug - it reports a real, still-current
        /// problem, the same way a linter re-reports an unfixed issue
        /// every run.
        inline void link_placements(Root &root, LayoutId layout_id, SchematicId top_schematic,
                                     PhysicalLinkResult &result, std::unordered_set<InstanceId> &linked_instances)
        {
            for (const PlacementId placement_id : root.get_layout_placements(layout_id))
            {
                const PlacementData *placement = root.get_placement(placement_id);
                if (!placement)
                    continue;
                if (placement->instance.valid())
                {
                    linked_instances.insert(placement->instance);
                    continue; // already linked - permanent, re-running `link` doesn't redo this
                }

                const std::vector<std::string> segments = hierarchy::split_path(placement->name);
                const std::vector<InstanceId> matches = hierarchy::resolve_instances(root, top_schematic, segments);
                if (matches.size() == 1)
                {
                    // physical_only explicitly cleared (not left nullopt/
                    // unchanged) - a Placement resolving successfully on
                    // this call may have been marked physical_only=true
                    // by an earlier call that missed, and that stale
                    // marker must not survive a real resolution.
                    root.update_placement(placement_id, layout_id, std::nullopt, std::optional<InstanceId>(matches[0]),
                                           std::nullopt, std::optional<bool>(false), std::nullopt, std::nullopt,
                                           std::nullopt, std::nullopt, std::nullopt);
                    linked_instances.insert(matches[0]);
                    ++result.placements_linked;
                }
                else
                {
                    root.update_placement(placement_id, layout_id, std::nullopt, std::nullopt, std::nullopt,
                                           std::optional<bool>(true), std::nullopt, std::nullopt, std::nullopt,
                                           std::nullopt, std::nullopt);
                    ++result.placements_marked_physical_only;
                    result.messages.push_back(fmt::format(
                        "WARNING: link: Placement '{}' has no matching Instance in the Schematic - marked physical_only.",
                        placement->name));
                }
            }
        }

        /// @brief Recursively walks every Instance reachable from
        /// `schematic`, creating a default Placement
        /// (LINKING_STRATEGY.md scenario 3) for any whose id isn't in
        /// `linked_instances`. `path_prefix` accumulates the same
        /// "/"-joined full path a real DEF Placement.name would use
        /// (LINKING_STRATEGY_RESEARCH.md section 1's confirmed naming
        /// rule), so a later `link` re-run - or a real DEF later placing
        /// this same instance - naturally lines up by name too, not just
        /// by id.
        inline void create_default_placements(Root &root, LayoutId layout_id, SchematicId schematic,
                                                const std::string &path_prefix,
                                                const std::unordered_set<InstanceId> &linked_instances,
                                                PhysicalLinkResult &result)
        {
            for (const InstanceId inst_id : root.get_schematic_instances(schematic))
            {
                const InstanceData *inst = root.get_instance(inst_id);
                if (!inst || !inst->reference_design.valid())
                    continue; // reference_design still unresolved - link_unresolved_instances hasn't placed it yet

                const std::string full_path = path_prefix.empty() ? inst->name : path_prefix + "/" + inst->name;

                if (!linked_instances.count(inst_id))
                {
                    root.create_placement(PlacementData{
                        .layout = layout_id,
                        .name = full_path,
                        .reference_design = inst->reference_design,
                        .instance = inst_id,
                        .placement_status = PlacementStatus::UNPLACED,
                        .location = Point{.x = 0, .y = 0},
                        .orientation = Orientation::N,
                    });
                    ++result.default_placements_created;
                }

                const SchematicId child = root.get_design_schematic(inst->reference_design);
                if (child.valid())
                    create_default_placements(root, layout_id, child, full_path, linked_instances, result);
            }
        }

        /// @brief Matches every un-linked Route/PhysicalPort in
        /// `layout_id` against `top_schematic` by name
        /// (LINKING_STRATEGY_RESEARCH.md section 2).
        inline void link_routes_and_physical_ports(Root &root, LayoutId layout_id, SchematicId top_schematic,
                                                     PhysicalLinkResult &result)
        {
            for (const RouteId route_id : root.get_layout_routes(layout_id))
            {
                const RouteData *route = root.get_route(route_id);
                if (!route || route->net.valid())
                    continue;

                const std::vector<std::string> segments = hierarchy::split_path(route->name);
                const std::vector<NetId> matches = hierarchy::resolve_nets(root, top_schematic, segments);
                if (matches.size() == 1)
                {
                    root.update_route(route_id, layout_id, std::optional<NetId>(matches[0]), std::nullopt,
                                       std::nullopt, std::nullopt, std::nullopt, std::nullopt);
                    ++result.routes_linked;
                }
                else
                {
                    result.messages.push_back(
                        fmt::format("ERROR: link: Route '{}' has no matching Net in the Schematic.", route->name));
                }
            }

            for (const PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
            {
                const PhysicalPortData *port = root.get_physical_port(port_id);
                if (!port || port->net.valid() || !port->net_name.has_value())
                    continue;

                const std::vector<std::string> segments = hierarchy::split_path(*port->net_name);
                const std::vector<NetId> matches = hierarchy::resolve_nets(root, top_schematic, segments);
                if (matches.size() == 1)
                {
                    root.update_physical_port(port_id, layout_id, std::optional<NetId>(matches[0]), std::nullopt,
                                               std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                               std::nullopt);
                    ++result.physical_ports_linked;
                }
                else
                {
                    result.messages.push_back(fmt::format(
                        "WARNING: link: PhysicalPort '{}' net '{}' has no matching Net in the Schematic.", port->name,
                        *port->net_name));
                }
            }
        }
    }

    /// @brief The physical-side half of the `link` step
    /// (LINKING_STRATEGY_RESEARCH.md sections 1/2): for every Layout,
    /// matches its Placements/Routes/PhysicalPorts against the sibling
    /// Schematic reached via `Layout.design -> Design.schematic`, using
    /// the shared hierarchical resolver (hierarchical_resolver.hpp) in
    /// exact-match mode (a real DEF-derived name never contains glob
    /// metacharacters, so this always takes the resolver's O(1)-per-
    /// segment fast path). MUST run after
    /// SVReader::link_unresolved_instances has already resolved
    /// Instance.reference_design to a fixed point - an Instance whose own
    /// reference_design is still unresolved can't be reached by the
    /// resolver's own descent (LINKING_STRATEGY_RESEARCH.md section 6's
    /// "ordering dependency"). Re-runnable, same convention as
    /// link_unresolved_instances: a row that's genuinely, permanently
    /// resolved (Placement.instance/Route.net/PhysicalPort.net already
    /// valid) is never re-processed or re-mutated, and
    /// create_default_placements() never creates a second default
    /// Placement for an Instance that already has one. A row still
    /// unresolved, though, is deliberately re-attempted (and, if still
    /// unresolved, re-reported) on every call - re-running `link` is how
    /// a user re-checks current state after fixing something, and an
    /// unresolved row is a real, still-current condition to keep
    /// surfacing until it's actually fixed, not a one-time notice to
    /// suppress after the first sighting.
    inline PhysicalLinkResult link_physical(Root &root)
    {
        PhysicalLinkResult result;
        root.for_each_layout_id(
            [&](LayoutId layout_id)
            {
                const LayoutData *layout = root.get_layout(layout_id);
                if (!layout)
                    return;
                const SchematicId top_schematic = root.get_design_schematic(layout->design);
                if (!top_schematic.valid())
                    return; // no Schematic for this Layout's own Design at all - nothing to link against

                std::unordered_set<InstanceId> linked_instances;
                physical_link_detail::link_placements(root, layout_id, top_schematic, result, linked_instances);
                physical_link_detail::create_default_placements(root, layout_id, top_schematic, "", linked_instances,
                                                                  result);
                physical_link_detail::link_routes_and_physical_ports(root, layout_id, top_schematic, result);
            });
        return result;
    }
}
