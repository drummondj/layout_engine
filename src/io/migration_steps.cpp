#include "migration_steps.hpp"

namespace le::persistence::migration_steps
{
    void copy_route_is_special_to_wiring(StepData &data)
    {
        const std::vector<uint8_t> special = data.dropped_bools("Route", "is_special");
        auto is_special = [&](RouteId route) { return route.valid() && route.index < special.size() && special[route.index] != 0; };
        Root &root = data.root;
        root.for_each_wire_id([&](WireId id) {
            WireData *wire = root.get_wire(id);
            wire->is_special = is_special(wire->route);
        });
        root.for_each_shape_id([&](ShapeId id) {
            ShapeData *shape = root.get_shape(id);
            shape->is_special = is_special(shape->route());
        });
    }
}
