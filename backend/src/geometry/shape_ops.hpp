#pragma once

// Business logic for the shape_* TCL commands (NEW_FEATURES_SEPT_2026.md
// item 1): resolves input Shapes, runs the matching Geometry operation,
// and persists each result as a new Shape. Takes a plain Root& and no
// locks - api.cpp's le_shape_* functions call this from inside their own
// HandleWriteLock and handle mutation-version bumping and undo recording,
// the same split generate_verilog_stubs (src/sv/verilog_stub_writer.hpp)
// already uses.

#include "geometry.hpp"

#include <expected>
#include <string>
#include <variant>
#include <vector>

namespace le::shape_ops
{
    /// @brief Where a shape_* command's new Shapes go. An Abstract/Layout
    /// means that object's own free-standing shapes (Shape.in_abstract/
    /// in_layout - never written by write_lef/write_def); every other kind
    /// is that object's own regular shapes list.
    using ShapeParent = std::variant<AbstractId, LayoutId, ObstructionId, TerminalPortId, RouteId, BlockageId, PhysicalPortSegmentId>;

    /// @brief The new Shapes' own ids, or a user-facing error message.
    using Result = std::expected<std::vector<ShapeId>, std::string>;

    /// @brief A Shape's layer-or-purpose - exactly one is ever set (see
    /// Shape.layer's own schema.py comment).
    struct LayerOrPurpose
    {
        LayerId layer;
        std::optional<ShapePurpose> purpose;
    };

    namespace detail
    {
        inline bool parent_exists(const Root &root, const ShapeParent &parent)
        {
            return std::visit([&](auto id) -> bool
                              {
                using IdT = decltype(id);
                if constexpr (std::is_same_v<IdT, AbstractId>) return root.get_abstract(id) != nullptr;
                else if constexpr (std::is_same_v<IdT, LayoutId>) return root.get_layout(id) != nullptr;
                else if constexpr (std::is_same_v<IdT, ObstructionId>) return root.get_obstruction(id) != nullptr;
                else if constexpr (std::is_same_v<IdT, TerminalPortId>) return root.get_terminal_port(id) != nullptr;
                else if constexpr (std::is_same_v<IdT, RouteId>) return root.get_route(id) != nullptr;
                else if constexpr (std::is_same_v<IdT, BlockageId>) return root.get_blockage(id) != nullptr;
                else return root.get_physical_port_segment(id) != nullptr; },
                              parent);
        }

        inline void set_parent(ShapeData &data, const ShapeParent &parent)
        {
            std::visit([&](auto id)
                       {
                using IdT = decltype(id);
                if constexpr (std::is_same_v<IdT, AbstractId>) data.in_abstract = id;
                else if constexpr (std::is_same_v<IdT, LayoutId>) data.in_layout = id;
                else if constexpr (std::is_same_v<IdT, ObstructionId>) data.obstruction = id;
                else if constexpr (std::is_same_v<IdT, TerminalPortId>) data.terminal_port = id;
                else if constexpr (std::is_same_v<IdT, RouteId>) data.route = id;
                else if constexpr (std::is_same_v<IdT, BlockageId>) data.blockage = id;
                else data.physical_port_segment = id; },
                       parent);
        }

        // Every input id resolved and its iterates expanded (Geometry's
        // operations only read rects/polygons/paths), or the first bad id's
        // error.
        inline std::expected<std::vector<Shape>, std::string> resolve_inputs(const Root &root, const std::vector<ShapeId> &ids, const char *what)
        {
            if (ids.empty())
                return std::unexpected(std::string("no ") + what + " given");
            std::vector<Shape> shapes;
            shapes.reserve(ids.size());
            for (ShapeId id : ids)
            {
                const Shape *shape = root.get_shape(id);
                if (!shape)
                    return std::unexpected(std::string("unknown shape in ") + what);
                shapes.push_back(Geometry::expand_iterates(*shape));
            }
            return shapes;
        }

        inline std::vector<const Shape *> pointers(const std::vector<Shape> &shapes)
        {
            std::vector<const Shape *> out;
            out.reserve(shapes.size());
            for (const Shape &shape : shapes)
                out.push_back(&shape);
            return out;
        }

        inline ShapeId create(Root &root, const ShapeParent &parent, const LayerOrPurpose &layer, ShapeData data)
        {
            set_parent(data, parent);
            data.layer = layer.layer;
            data.purpose = layer.layer.valid() ? std::nullopt : layer.purpose;
            return root.create_shape(std::move(data));
        }

        inline LayerOrPurpose layer_of(const Shape &shape, const std::optional<LayerOrPurpose> &target)
        {
            if (target)
                return *target;
            return LayerOrPurpose{.layer = shape.layer, .purpose = shape.purpose};
        }

        inline std::expected<void, std::string> check_target(const Root &root, const std::optional<LayerOrPurpose> &target)
        {
            if (target && !target->purpose && !root.get_layer(target->layer))
                return std::unexpected("unknown layer");
            return {};
        }

        inline std::expected<void, std::string> check_common(const Root &root, const ShapeParent &parent, const std::optional<LayerOrPurpose> &target)
        {
            if (!parent_exists(root, parent))
                return std::unexpected("unknown parent object");
            return check_target(root, target);
        }

        // Per-input operations share everything but the geometry step: one
        // new Shape per input, on that input's own layer unless overridden,
        // skipping any input whose result has no geometry at all.
        template <typename MakeData>
        Result per_input(Root &root, const std::vector<ShapeId> &inputs, const std::optional<LayerOrPurpose> &layer, const ShapeParent &parent, MakeData &&make_data)
        {
            if (auto ok = check_common(root, parent, layer); !ok)
                return std::unexpected(ok.error());
            auto shapes = resolve_inputs(root, inputs, "shapes");
            if (!shapes)
                return std::unexpected(shapes.error());

            // Compute every result before creating any Shape, so a failing
            // input (e.g. an unsupported size) leaves the database untouched.
            std::vector<std::pair<LayerOrPurpose, ShapeData>> pending;
            for (const Shape &shape : *shapes)
            {
                std::expected<ShapeData, std::string> data = make_data(shape);
                if (!data)
                    return std::unexpected(data.error());
                if (data->rects.empty() && data->polygons.empty() && data->paths.empty())
                    continue;
                pending.emplace_back(layer_of(shape, layer), std::move(*data));
            }

            std::vector<ShapeId> created;
            created.reserve(pending.size());
            for (auto &[target_layer, data] : pending)
                created.push_back(create(root, parent, target_layer, std::move(data)));
            return created;
        }
    }

    /// @brief shape_copy: one new Shape per input, same geometry, on `layer`
    /// (a real Layer, or a layer-less purpose such as DEBUG).
    inline Result copy(Root &root, const std::vector<ShapeId> &inputs, LayerOrPurpose layer, const ShapeParent &parent)
    {
        return detail::per_input(root, inputs, layer, parent, [](const Shape &shape) -> std::expected<ShapeData, std::string>
                                 { return ShapeData{
                                       .paths = shape.paths,
                                       .polygons = shape.polygons,
                                       .rects = shape.rects,
                                       .rect_masks = shape.rect_masks,
                                       .polygon_masks = shape.polygon_masks,
                                       .path_masks = shape.path_masks,
                                   }; });
    }

    /// @brief shape_or/and/not: one new Shape holding the combination of
    /// every shape in `a` with every shape in `b` - on `layer`, else the
    /// first shape in `a`'s own layer. An empty result creates nothing.
    inline Result boolean(Root &root, const std::vector<ShapeId> &a, const std::vector<ShapeId> &b, BooleanOp op,
                          const std::optional<LayerOrPurpose> &layer, const ShapeParent &parent)
    {
        if (auto ok = detail::check_common(root, parent, layer); !ok)
            return std::unexpected(ok.error());
        auto shapes_a = detail::resolve_inputs(root, a, "first shape list");
        if (!shapes_a)
            return std::unexpected(shapes_a.error());
        auto shapes_b = detail::resolve_inputs(root, b, "second shape list (-with)");
        if (!shapes_b)
            return std::unexpected(shapes_b.error());

        AreaGeometry geometry = Geometry::boolean_shapes(detail::pointers(*shapes_a), detail::pointers(*shapes_b), op);
        if (geometry.empty())
            return std::vector<ShapeId>{};
        return std::vector<ShapeId>{detail::create(root, parent, detail::layer_of(shapes_a->front(), layer),
                                                   ShapeData{.polygons = std::move(geometry.polygons), .rects = std::move(geometry.rects)})};
    }

    /// @brief shape_to_polygon: one new polygon-only Shape per input.
    inline Result to_polygons(Root &root, const std::vector<ShapeId> &inputs, const std::optional<LayerOrPurpose> &layer, const ShapeParent &parent)
    {
        return detail::per_input(root, inputs, layer, parent, [](const Shape &shape) -> std::expected<ShapeData, std::string>
                                 { return ShapeData{.polygons = Geometry::shape_to_polygons(shape)}; });
    }

    /// @brief shape_to_rects: one new rect-only Shape per input.
    inline Result to_rects(Root &root, const std::vector<ShapeId> &inputs, FractureDirection direction,
                           const std::optional<LayerOrPurpose> &layer, const ShapeParent &parent)
    {
        return detail::per_input(root, inputs, layer, parent, [direction](const Shape &shape) -> std::expected<ShapeData, std::string>
                                 { return ShapeData{.rects = Geometry::shape_to_rects(shape, direction)}; });
    }

    /// @brief shape_size: one new Shape per input, grown (positive) or
    /// shrunk (negative) by dx/dy dbu. A shape shrunk away entirely
    /// creates nothing.
    inline Result size(Root &root, const std::vector<ShapeId> &inputs, int64_t dx, int64_t dy,
                       const std::optional<LayerOrPurpose> &layer, const ShapeParent &parent)
    {
        return detail::per_input(root, inputs, layer, parent, [dx, dy](const Shape &shape) -> std::expected<ShapeData, std::string>
                                 {
            std::optional<AreaGeometry> geometry = Geometry::size_shape(shape, dx, dy);
            if (!geometry)
                return std::unexpected("different X and Y sizes are only supported for rectilinear (axis-aligned) shapes");
            return ShapeData{.polygons = std::move(geometry->polygons), .rects = std::move(geometry->rects)}; });
    }

    /// @brief shape_path: one new path-only Shape per input, following its
    /// outline (and any holes) at `width` dbu.
    inline Result outline_paths(Root &root, const std::vector<ShapeId> &inputs, int64_t width,
                                const std::optional<LayerOrPurpose> &layer, const ShapeParent &parent)
    {
        if (width <= 0)
            return std::unexpected("width must be positive");
        return detail::per_input(root, inputs, layer, parent, [width](const Shape &shape) -> std::expected<ShapeData, std::string>
                                 { return ShapeData{.paths = Geometry::shape_outline_paths(shape, width)}; });
    }

    /// @brief One changed Shape's layer-or-purpose before and after, for undo.
    struct LayerChange
    {
        ShapeId id;
        LayerOrPurpose before;
        LayerOrPurpose after;
    };

    /// @brief Sets `shape`'s layer and purpose to exactly `target` (clearing
    /// whichever one `target` doesn't set). Written directly rather than via
    /// Root::update_shape: its optional<ShapePurpose> can set a purpose but
    /// never clear one, which moving off the debug layer needs. Both fields
    /// are unindexed, so there's no Root index bookkeeping to bypass.
    inline void set_layer_or_purpose(ShapeData &shape, const LayerOrPurpose &target)
    {
        shape.layer = target.layer;
        shape.purpose = target.layer.valid() ? std::nullopt : target.purpose;
    }

    /// @brief shape_change_layer: puts each input in place onto `target` (a real
    /// Layer, or a layer-less purpose such as DEBUG); geometry and owner
    /// are unchanged. All-or-nothing: an unknown input changes nothing.
    inline std::expected<std::vector<LayerChange>, std::string> change_layer(Root &root, const std::vector<ShapeId> &inputs, const LayerOrPurpose &target)
    {
        if (auto ok = detail::check_target(root, target); !ok)
            return std::unexpected(ok.error());
        if (inputs.empty())
            return std::unexpected("no shapes given");
        for (ShapeId id : inputs)
            if (!root.get_shape(id))
                return std::unexpected("unknown shape in shapes");

        std::vector<LayerChange> changed;
        changed.reserve(inputs.size());
        for (ShapeId id : inputs)
        {
            ShapeData &shape = *root.get_shape(id);
            LayerChange entry{.id = id, .before = LayerOrPurpose{.layer = shape.layer, .purpose = shape.purpose}};
            set_layer_or_purpose(shape, target);
            entry.after = LayerOrPurpose{.layer = shape.layer, .purpose = shape.purpose};
            changed.push_back(entry);
        }
        return changed;
    }

    /// @brief shape_bbox: the bbox of every input shape together. Creates nothing.
    inline std::expected<Rect, std::string> bbox(const Root &root, const std::vector<ShapeId> &inputs)
    {
        auto shapes = detail::resolve_inputs(root, inputs, "shapes");
        if (!shapes)
            return std::unexpected(shapes.error());
        std::optional<Rect> box = Geometry::bbox(detail::pointers(*shapes));
        if (!box)
            return std::unexpected("shapes have no geometry");
        return *box;
    }
}
