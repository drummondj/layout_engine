#pragma once

// Works out the use of schematic Nets that have none from what their Pins
// connect to: the cell Terminals (LEF PIN USE) and child-schematic Ports
// they reach.

#include "database.hpp"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace le
{
    namespace signal_use_detail
    {
        /// @brief What `pin` connects to inside its instance's design: the
        /// Terminal's use, else the child schematic Port's (unset is
        /// SIGNAL, the default), or nullopt when the pin reaches neither.
        inline std::optional<SignalUse> pin_use(const Root &root, const InstanceData &instance, const PinData &pin)
        {
            if (!instance.reference_design.valid())
                return std::nullopt;
            const AbstractId abstract = root.get_design_abstract(instance.reference_design);
            if (abstract.valid())
                if (const TerminalData *terminal = root.get_terminal(root.get_terminal_by_name(abstract, pin.name)))
                    return terminal->use.value_or(SignalUse::SIGNAL);
            const SchematicId child = root.get_design_schematic(instance.reference_design);
            if (child.valid())
                if (const PortData *port = root.get_port(root.get_port_by_name(child, pin.name)))
                    return port->use.value_or(SignalUse::SIGNAL);
            return std::nullopt;
        }

        struct Reached
        {
            std::optional<SignalUse> use; ///< the one use every pin reached, so far
            bool mixed = false;           ///< pins reach different uses, or an unknown one
        };

        inline void infer_schematic(Root &root, SchematicId schematic, std::unordered_set<SchematicId> &done, size_t &inferred)
        {
            if (!done.insert(schematic).second)
                return;
            // Children first, so their Ports have uses for this level's Pins.
            for (const InstanceId instance_id : root.get_schematic_instances(schematic))
                if (const InstanceData *instance = root.get_instance(instance_id); instance && instance->reference_design.valid())
                    if (const SchematicId child = root.get_design_schematic(instance->reference_design); child.valid())
                        infer_schematic(root, child, done, inferred);

            std::unordered_map<NetId, Reached> reached;
            for (const InstanceId instance_id : root.get_schematic_instances(schematic))
            {
                const InstanceData *instance = root.get_instance(instance_id);
                if (!instance)
                    continue;
                for (const PinId pin_id : root.get_instance_pins(instance_id))
                {
                    const PinData *pin = root.get_pin(pin_id);
                    if (!pin || !pin->net.valid())
                        continue;
                    Reached &net = reached[pin->net];
                    const std::optional<SignalUse> use = pin_use(root, *instance, *pin);
                    if (!use || (net.use && *net.use != *use))
                        net.mixed = true;
                    else
                        net.use = use;
                }
            }
            for (const auto &[net_id, net_reached] : reached)
            {
                if (net_reached.mixed || !net_reached.use || *net_reached.use == SignalUse::SIGNAL)
                    continue;
                NetData *net = root.get_net(net_id);
                if (!net || net->use)
                    continue;
                net->use = net_reached.use;
                root.note_net_changed(net_id);
                ++inferred;
            }
            for (const PortId port_id : root.get_schematic_ports(schematic))
            {
                PortData *port = root.get_port(port_id);
                const NetData *net = port ? root.get_net(port->net) : nullptr;
                if (!port || port->use || !net || !net->use)
                    continue;
                port->use = net->use;
                root.note_port_changed(port_id);
                ++inferred;
            }
        }
    }

    /// @brief Gives every Net with no use the one use all its Pins reach,
    /// when that is not SIGNAL and every Pin reaches a Terminal or child
    /// Port (e.g. a net reaching only POWER terminals is POWER), and then
    /// every Port with no use its Net's. Child schematics go first. A use
    /// already set is kept. Returns the number of Nets and Ports set.
    inline size_t infer_signal_uses(Root &root)
    {
        std::vector<SchematicId> schematics;
        root.for_each_schematic_id([&](SchematicId id) { schematics.push_back(id); });
        std::unordered_set<SchematicId> done;
        size_t inferred = 0;
        for (const SchematicId schematic : schematics)
            signal_use_detail::infer_schematic(root, schematic, done, inferred);
        return inferred;
    }
}
