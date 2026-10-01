#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "placement_geometry.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace le
{
    /// @brief One flightline (NEW_FEATURES_SEPT_2026.md item 5) - a straight
    /// dbu-space segment between two connected pins.
    struct Flightline
    {
        Point from;
        Point to;
    };

    /// @brief Every placed endpoint of every net in one Layout - built once
    /// per Root mutation (not per selection change), since it walks the
    /// whole Layout. Connectivity comes from `link`: a Placement's own
    /// `instance` Pins (Pin.net), plus top-level PhysicalPorts (PhysicalPort.net).
    /// Neither Pin.net nor PhysicalPort.net has a reverse index in Root,
    /// hence this one.
    class NetEndpointIndex
    {
    public:
        struct Endpoint
        {
            PlacementId placement; // valid for a placed instance pin...
            PinId pin;             // ...naming which of its pins
            PhysicalPortId port;   // valid instead for a top-level pin
        };

        NetEndpointIndex() = default;
        NetEndpointIndex(const Root &root, LayoutId layout_id)
        {
            for (const PlacementId placement_id : root.get_layout_placements(layout_id))
            {
                const PlacementData *placement = root.get_placement(placement_id);
                if (!placement || !placement->instance.valid() || !placement->location)
                    continue;
                for (const PinId pin_id : root.get_instance_pins(placement->instance))
                    if (const PinData *pin = root.get_pin(pin_id); pin && pin->net.valid())
                        by_net_[pin->net].push_back(Endpoint{.placement = placement_id, .pin = pin_id, .port = {}});
            }
            for (const PhysicalPortId port_id : root.get_layout_physical_ports(layout_id))
                if (const PhysicalPortData *port = root.get_physical_port(port_id); port && port->net.valid())
                    by_net_[port->net].push_back(Endpoint{.placement = {}, .pin = {}, .port = port_id});
        }

        std::span<const Endpoint> endpoints(NetId net) const
        {
            const auto it = by_net_.find(net);
            return it == by_net_.end() ? std::span<const Endpoint>{} : std::span<const Endpoint>(it->second);
        }

    private:
        std::unordered_map<NetId, std::vector<Endpoint>> by_net_;
    };

    namespace flightline_detail
    {
        // Resolves an endpoint to its dbu-space location, memoizing each
        // Abstract's pin centers (local space) across one call.
        class EndpointLocator
        {
        public:
            explicit EndpointLocator(const Root &root) : root_(root) {}

            /// A placed pin's location: the center of its Terminal's port
            /// geometry in the placed Abstract, run through the placement's
            /// own transform - the placement's bbox center if the Abstract
            /// has no Terminal of that name (or no geometry for it).
            std::optional<Point> placed_pin(PlacementId placement_id, const std::string &pin_name)
            {
                const PlacementData *placement = root_.get_placement(placement_id);
                if (!placement || !placement->location)
                    return std::nullopt;
                const AbstractId abstract_id = root_.get_design_abstract(placement->reference_design);
                if (!abstract_id.valid())
                    return std::nullopt;

                const Rect local_bbox = abstract_declared_bbox(root_, abstract_id);
                const Geometry::InstanceTransform t =
                    Geometry::instance_transform(placement->orientation.value_or(Orientation::N), local_bbox, *placement->location);
                const Point local = local_pin_center(abstract_id, pin_name).value_or(Point{
                    .x = (local_bbox.ll.x + local_bbox.ur.x) / 2,
                    .y = (local_bbox.ll.y + local_bbox.ur.y) / 2,
                });
                const Point rotated = Geometry::apply_linear(t.linear, local);
                return Point{.x = rotated.x + t.translation.x, .y = rotated.y + t.translation.y};
            }

            /// A top-level pin's location (PhysicalPort.location, else its
            /// first placed segment's).
            std::optional<Point> port(PhysicalPortId port_id) const
            {
                const PhysicalPortData *port = root_.get_physical_port(port_id);
                if (!port)
                    return std::nullopt;
                if (port->location)
                    return port->location;
                for (const PhysicalPortSegmentId segment_id : root_.get_physical_port_segments(port_id))
                    if (const PhysicalPortSegmentData *segment = root_.get_physical_port_segment(segment_id); segment && segment->location)
                        return segment->location;
                return std::nullopt;
            }

            std::optional<Point> locate(const NetEndpointIndex::Endpoint &endpoint)
            {
                if (endpoint.port.valid())
                    return port(endpoint.port);
                const PinData *pin = root_.get_pin(endpoint.pin);
                return pin ? placed_pin(endpoint.placement, pin->name) : std::nullopt;
            }

        private:
            std::optional<Point> local_pin_center(AbstractId abstract_id, const std::string &pin_name)
            {
                auto &centers = pin_centers_[abstract_id];
                if (const auto it = centers.find(pin_name); it != centers.end())
                    return it->second;

                std::optional<Point> center;
                const TerminalId terminal_id = root_.get_terminal_by_name(abstract_id, pin_name);
                if (terminal_id.valid())
                {
                    std::vector<const Shape *> shapes;
                    for (const TerminalPortId port_id : root_.get_terminal_ports(terminal_id))
                        for (const ShapeId shape_id : root_.get_terminal_port_shapes(port_id))
                            if (const Shape *shape = root_.get_shape(shape_id))
                                shapes.push_back(shape);
                    if (const std::optional<Rect> bbox = Geometry::bbox(shapes))
                        center = Point{.x = (bbox->ll.x + bbox->ur.x) / 2, .y = (bbox->ll.y + bbox->ur.y) / 2};
                }
                centers.emplace(pin_name, center);
                return center;
            }

            const Root &root_;
            std::unordered_map<AbstractId, std::unordered_map<std::string, std::optional<Point>>> pin_centers_;
        };
    }

    /// @brief The flightlines of `selected` placements: from each of their
    /// connected pins to every other endpoint on the same net (other
    /// placements' pins and top-level pins, selected or not) - a star per
    /// selected pin. A connection between two selected pins is drawn once.
    /// Placements without a linked Instance (see `link`) have no
    /// connectivity and draw nothing. A net whose fanout - its endpoints
    /// other than the selected pin - exceeds `max_fanout` draws nothing
    /// either (high-fanout clock/reset nets would bury everything else);
    /// `max_fanout` 0 means no limit.
    inline std::vector<Flightline> placement_flightlines(const Root &root, const NetEndpointIndex &index, std::span<const PlacementId> selected,
                                                         int max_fanout = 0)
    {
        flightline_detail::EndpointLocator locator(root);
        std::vector<Flightline> lines;
        // Unordered (pin, pin) pairs already drawn - only matters when two
        // selected pins share a net; a port's id is kept apart from a
        // pin's by its own flag bit.
        std::set<std::pair<uint64_t, uint64_t>> seen;
        const auto key = [](const NetEndpointIndex::Endpoint &e)
        {
            return e.port.valid() ? ((uint64_t{1} << 63) | e.port.index) : e.pin.index;
        };

        for (const PlacementId placement_id : selected)
        {
            const PlacementData *placement = root.get_placement(placement_id);
            if (!placement || !placement->instance.valid())
                continue;
            for (const PinId pin_id : root.get_instance_pins(placement->instance))
            {
                const PinData *pin = root.get_pin(pin_id);
                if (!pin || !pin->net.valid())
                    continue;
                const std::span<const NetEndpointIndex::Endpoint> endpoints = index.endpoints(pin->net);
                if (max_fanout > 0 && endpoints.size() > static_cast<size_t>(max_fanout) + 1)
                    continue;
                const NetEndpointIndex::Endpoint self{.placement = placement_id, .pin = pin_id, .port = {}};
                const std::optional<Point> from = locator.placed_pin(placement_id, pin->name);
                if (!from)
                    continue;

                for (const NetEndpointIndex::Endpoint &other : endpoints)
                {
                    if (!other.port.valid() && other.pin == pin_id)
                        continue;
                    const uint64_t a = key(self);
                    const uint64_t b = key(other);
                    if (!seen.insert(std::minmax(a, b)).second)
                        continue;
                    if (const std::optional<Point> to = locator.locate(other))
                        lines.push_back(Flightline{.from = *from, .to = *to});
                }
            }
        }
        return lines;
    }
}
