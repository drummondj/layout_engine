#pragma once
#include "database.hpp"
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace le
{
    /// @brief A holder of selectable geometry: a Shape or a Wire (exactly
    /// one id valid).
    struct GeometryId
    {
        ShapeId shape;
        WireId wire;

        constexpr GeometryId() = default;
        constexpr GeometryId(ShapeId id) : shape(id) {}
        constexpr GeometryId(WireId id) : wire(id) {}

        static constexpr GeometryId of(ShapeId id) { return GeometryId(id); }
        static constexpr GeometryId of(WireId id) { return GeometryId(id); }
        constexpr bool valid() const noexcept { return shape.valid() || wire.valid(); }
        friend auto operator<=>(const GeometryId &, const GeometryId &) = default;
    };

    /// @brief The width of a Wire segment or via with `width_index`: the
    /// layer's default width for 0, else `wire.widths[width_index - 1]`.
    inline int64_t wire_width(const Root &root, const WireData &wire, unsigned width_index)
    {
        if (width_index == 0)
        {
            const LayerData *layer = root.get_layer(wire.layer);
            return layer ? layer->width.value_or(0) : 0;
        }
        return width_index <= wire.widths.size() ? wire.widths[width_index - 1] : 0;
    }

    inline Point wire_segment_start(const WireSegment &segment) { return Point{.x = segment.x, .y = segment.y}; }

    inline Point wire_segment_end(const WireSegment &segment)
    {
        return segment.vertical ? Point{.x = segment.x, .y = int64_t{segment.y} + segment.length}
                                : Point{.x = int64_t{segment.x} + segment.length, .y = segment.y};
    }

    /// @brief Calls `visit(first, last)` for each path of `wire` - the
    /// segment range [first, last) of one run of segments that continue
    /// each other - in order.
    template <typename Visit>
    void for_each_wire_path(const WireData &wire, Visit &&visit)
    {
        std::size_t first = 0;
        for (std::size_t i = 1; i <= wire.segments.size(); ++i)
            if (i == wire.segments.size() || !wire.segments[i].continues)
            {
                visit(first, i);
                first = i;
            }
    }

    /// @brief The segment range [first, last) of `wire`'s path
    /// `path_index`, if it has that many paths.
    inline std::optional<std::pair<std::size_t, std::size_t>> wire_path_range(const WireData &wire, std::size_t path_index)
    {
        std::optional<std::pair<std::size_t, std::size_t>> found;
        std::size_t path = 0;
        for_each_wire_path(wire, [&](std::size_t first, std::size_t last)
                           {
            if (path++ == path_index)
                found = {first, last}; });
        return found;
    }

    /// @brief One path of `wire` (segments [first, last)) as a general Path.
    inline Path wire_path(const Root &root, const WireData &wire, std::size_t first, std::size_t last)
    {
        Path path{.width = wire_width(root, wire, wire.segments[first].width_index)};
        path.polygon.points.reserve(last - first + 1);
        path.polygon.points.push_back(wire_segment_start(wire.segments[first]));
        for (std::size_t i = first; i < last; ++i)
            path.polygon.points.push_back(wire_segment_end(wire.segments[i]));
        return path;
    }

    /// @brief The name of the via a WireVia places - its LayoutVia's, else
    /// its Via's; empty if neither exists any more.
    inline std::string wire_via_name(const Root &root, const WireVia &via)
    {
        if (const LayoutViaData *layout_via = root.get_layout_via(via.layout_via))
            return layout_via->name;
        if (const ViaData *tech_via = root.get_via(via.via))
            return tech_via->name;
        return {};
    }

    /// @brief `via` as a general ShapeVia, as DEF writes it.
    inline ShapeVia wire_via_to_shape_via(const Root &root, const WireData &wire, const WireVia &via)
    {
        ShapeVia out{
            .via_name = wire_via_name(root, via),
            .origin = Point{.x = via.x, .y = via.y},
            .width = wire_width(root, wire, via.width_index),
        };
        if (via.orientation != Orientation::N)
            out.orientation = via.orientation;
        if (via.mask != 0)
            out.mask = via.mask;
        return out;
    }

    /// @brief `wire` as a general route Shape on its layer - its paths, in
    /// order, and its vias - for code that reads any Shape's geometry
    /// (hit-tests, shape ops, bounding boxes, DEF output). Path i of the
    /// Shape is path i of the Wire, via i is via i.
    inline ShapeData wire_to_shape(const Root &root, const WireData &wire)
    {
        ShapeData shape{.layer = wire.layer};
        bool any_mask = false;
        std::vector<int> masks;
        for_each_wire_path(wire, [&](std::size_t first, std::size_t last)
                           {
            shape.paths.push_back(wire_path(root, wire, first, last));
            masks.push_back(wire.segments[first].mask);
            any_mask = any_mask || wire.segments[first].mask != 0; });
        if (any_mask)
            shape.path_masks = std::move(masks);
        shape.vias.reserve(wire.vias.size());
        for (const WireVia &via : wire.vias)
            shape.vias.push_back(wire_via_to_shape_via(root, wire, via));
        return shape;
    }

    /// @brief The geometry `id` holds, as one Shape: the Shape itself, or
    /// the Wire converted into `scratch`; null if it no longer exists.
    inline const ShapeData *geometry_shape(const Root &root, GeometryId id, ShapeData &scratch)
    {
        if (const ShapeData *shape = root.get_shape(id.shape))
            return shape;
        if (const WireData *wire = root.get_wire(id.wire))
        {
            scratch = wire_to_shape(root, *wire);
            return &scratch;
        }
        return nullptr;
    }

    /// @brief The Route whose geometry `id` is, if any.
    inline RouteId geometry_route(const Root &root, GeometryId id)
    {
        if (const ShapeData *shape = root.get_shape(id.shape))
            return shape->route();
        if (const WireData *wire = root.get_wire(id.wire))
            return wire->route;
        return RouteId{};
    }

    /// @brief The via a name places in a Layout: its own DEF via of that
    /// name, else the technology's; neither if the name is unknown (or
    /// names a VIARULE).
    struct WireViaTarget
    {
        ViaId via;
        LayoutViaId layout_via;

        bool valid() const noexcept { return via.valid() || layout_via.valid(); }
    };

    inline WireViaTarget resolve_wire_via(const Root &root, LayoutId layout_id, const std::string &name)
    {
        if (layout_id.valid())
            if (const LayoutViaId layout_via = root.get_layout_via_by_name(layout_id, name); layout_via.valid())
                return WireViaTarget{.layout_via = layout_via};
        return WireViaTarget{.via = root.get_via_by_name(name)};
    }

    /// @brief Builds one layer's Wire from paths and vias, refusing (false)
    /// what the compact form can't hold: a non-Manhattan or out-of-range
    /// segment, a 256th distinct width, an out-of-range via origin or mask.
    /// A refused path or via is the caller's to keep in a general Shape.
    class WireBuilder
    {
    public:
        WireBuilder(const Root &root, LayerId layer) : layer_(layer)
        {
            if (const LayerData *data = root.get_layer(layer))
                default_width_ = data->width.value_or(0);
        }

        /// @brief Appends `path` as segments; false (nothing appended) if
        /// it doesn't fit.
        bool add_path(const Path &path, int mask = 0)
        {
            const auto &points = path.polygon.points;
            if (points.size() < 2 || mask < 0 || mask > std::numeric_limits<unsigned char>::max())
                return false;
            for (const Point &point : points)
                if (!fits(point.x) || !fits(point.y))
                    return false;
            for (std::size_t i = 0; i + 1 < points.size(); ++i)
            {
                if (points[i].x != points[i + 1].x && points[i].y != points[i + 1].y)
                    return false;
                const bool vertical = points[i].x == points[i + 1].x;
                if (!fits(vertical ? points[i + 1].y - points[i].y : points[i + 1].x - points[i].x))
                    return false;
            }
            // Last, so a refused path adds no width.
            const std::optional<unsigned char> width = width_index(path.width);
            if (!width)
                return false;
            for (std::size_t i = 0; i + 1 < points.size(); ++i)
            {
                const bool vertical = points[i].x == points[i + 1].x;
                const int64_t length = vertical ? points[i + 1].y - points[i].y : points[i + 1].x - points[i].x;
                segments_.push_back(WireSegment{
                    .x = static_cast<int32_t>(points[i].x),
                    .y = static_cast<int32_t>(points[i].y),
                    .length = static_cast<int32_t>(length),
                    .vertical = vertical,
                    .continues = i > 0,
                    .width_index = *width,
                    .mask = static_cast<unsigned char>(mask),
                });
            }
            return true;
        }

        /// @brief Appends a via placement; false if it doesn't fit (or
        /// `target` names nothing).
        bool add_via(WireViaTarget target, Point origin, Orientation orientation, int mask, int64_t width)
        {
            if (!target.valid() || !fits(origin.x) || !fits(origin.y) || mask < 0 || mask > std::numeric_limits<unsigned short>::max())
                return false;
            const std::optional<unsigned char> index = width_index(width);
            if (!index)
                return false;
            vias_.push_back(WireVia{
                .via = target.via,
                .layout_via = target.layout_via,
                .x = static_cast<int32_t>(origin.x),
                .y = static_cast<int32_t>(origin.y),
                .orientation = orientation,
                .width_index = *index,
                .mask = static_cast<unsigned short>(mask),
            });
            return true;
        }

        bool empty() const noexcept { return segments_.empty() && vias_.empty(); }

        /// @brief The Wire built so far, owned by `route`.
        WireData build(RouteId route) &&
        {
            return WireData{
                .route = route,
                .layer = layer_,
                .segments = std::move(segments_),
                .vias = std::move(vias_),
                .widths = std::move(widths_),
            };
        }

    private:
        static bool fits(int64_t value)
        {
            return value >= std::numeric_limits<int32_t>::min() && value <= std::numeric_limits<int32_t>::max();
        }

        std::optional<unsigned char> width_index(int64_t width)
        {
            if (width == default_width_)
                return 0;
            for (std::size_t i = 0; i < widths_.size(); ++i)
                if (widths_[i] == width)
                    return static_cast<unsigned char>(i + 1);
            if (widths_.size() >= std::numeric_limits<unsigned char>::max())
                return std::nullopt;
            widths_.push_back(width);
            return static_cast<unsigned char>(widths_.size());
        }

        LayerId layer_;
        int64_t default_width_ = 0;
        std::vector<WireSegment> segments_;
        std::vector<WireVia> vias_;
        std::vector<int64_t> widths_;
    };

    /// @brief `shape` (geometry on one layer: paths and plain vias only) as
    /// a Wire of `route`, or nullopt if any of it doesn't fit - so an edit
    /// made through wire_to_shape() can be stored back. Via names resolve
    /// in the route's Layout.
    inline std::optional<WireData> shape_to_wire(const Root &root, const ShapeData &shape, RouteId route)
    {
        if (!shape.layer.valid() || !shape.rects.empty() || !shape.polygons.empty() || !shape.rect_iterates.empty() ||
            !shape.path_iterates.empty() || !shape.polygon_iterates.empty() || !shape.via_iterates.empty() || !shape.texts.empty())
            return std::nullopt;
        const RouteData *route_data = root.get_route(route);
        const LayoutId layout = route_data ? route_data->layout : LayoutId{};
        WireBuilder builder(root, shape.layer);
        for (std::size_t i = 0; i < shape.paths.size(); ++i)
            if (!builder.add_path(shape.paths[i], i < shape.path_masks.size() ? shape.path_masks[i] : 0))
                return std::nullopt;
        for (const ShapeVia &via : shape.vias)
            if (!builder.add_via(resolve_wire_via(root, layout, via.via_name), via.origin, via.orientation.value_or(Orientation::N),
                                 via.mask.value_or(0), via.width.value_or(0)))
                return std::nullopt;
        return std::move(builder).build(route);
    }
}
