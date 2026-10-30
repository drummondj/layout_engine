#include "edit_ops.hpp"

#include "../database/hierarchical_resolver.hpp"
#include "../editing/editing.hpp"
#include "../geometry/shape_ops.hpp"
#include "generated/api/snapshot_appliers.hpp"
#include "le_handle.hpp"
#include "placement_move.hpp"

#include <fmt/format.h>

namespace le::edit
{
    std::optional<std::string> check_placement_update(const Root &, const PlacementData &before, const PlacementChanges &changes)
    {
        if (!placement_is_locked(changes.placement_status.value_or(before.placement_status)))
            return std::nullopt;
        const bool moves = changes.location &&
                           (!before.location || changes.location->x != before.location->x || changes.location->y != before.location->y);
        const bool turns = changes.orientation && *changes.orientation != before.orientation;
        if (!moves && !turns)
            return std::nullopt;
        return fmt::format("placement '{}' is {} - unlock it (placement_status PLACED) before changing its {}", before.name,
                           to_string(changes.placement_status.value_or(before.placement_status)), moves ? "location" : "orientation");
    }

namespace
    {
        std::optional<std::string> check_instantiation(const Root &root, SchematicId parent, DesignId reference_design)
        {
            if (!hierarchy::instantiation_would_cycle(root, parent, reference_design))
                return std::nullopt;
            const SchematicData *schematic = root.get_schematic(parent);
            const DesignData *owner = schematic ? root.get_design(schematic->design) : nullptr;
            const DesignData *referenced = root.get_design(reference_design);
            return fmt::format("instantiating design '{}' in design '{}' would make the hierarchy recursive",
                               referenced ? referenced->name : "", owner ? owner->name : "");
        }
    }

    std::optional<std::string> check_instance_create(const Root &root, const InstanceData &data)
    {
        return check_instantiation(root, data.schematic, data.reference_design);
    }

    std::optional<std::string> check_instance_update(const Root &root, const InstanceData &before, const InstanceChanges &changes)
    {
        if (!changes.schematic && !changes.reference_design)
            return std::nullopt;
        return check_instantiation(root, changes.schematic.value_or(before.schematic), changes.reference_design.value_or(before.reference_design));
    }

    #include "generated/api/edit_ops_defs.inc"

    std::optional<double> dbu_per_um(const Root &root)
    {
        const auto &technology_ids = root.get_technology_ids();
        if (technology_ids.empty())
            return std::nullopt;
        const TechnologyData *technology = root.get_technology(technology_ids.front());
        if (!technology || technology->database_units_microns <= 0.0)
            return std::nullopt;
        return technology->database_units_microns;
    }

    namespace
    {
        // An explicit parent, else the current view's own Abstract/Layout
        // free-shapes list - the GUI's open view first, then the Tcl-level
        // current_abstract/current_layout for a script that never opened one.
        std::expected<shape_ops::ShapeParent, std::string> resolve_parent(const LeHandle &handle, const std::optional<shape_ops::ShapeParent> &parent)
        {
            if (parent)
                return *parent;
            if (handle.root.get_abstract(handle.current_abstract()))
                return handle.current_abstract();
            if (handle.root.get_layout(handle.current_layout()))
                return handle.current_layout();
            if (handle.root.get_abstract(handle.current_abstract_id))
                return handle.current_abstract_id;
            if (handle.root.get_layout(handle.current_layout_id))
                return handle.current_layout_id;
            return std::unexpected("no -parent given and no current Abstract or Layout is open");
        }

        // Resolves the parent, runs `op` on it, then bumps the mutation
        // version and records each new Shape for undo.
        template <typename Op>
        ShapeOpResult run_creating(LeHandle &handle, const std::optional<shape_ops::ShapeParent> &parent, Op &&op)
        {
            const std::expected<shape_ops::ShapeParent, std::string> resolved = resolve_parent(handle, parent);
            if (!resolved)
                return std::unexpected(resolved.error());
            ShapeOpResult result = op(*resolved);
            if (!result || result->empty())
                return result;
            handle.root.bump_mutation_version();
            if (editing::Transaction *txn = handle.command_history.current())
                for (ShapeId id : *result)
                    txn->record_create(id);
            return result;
        }
    }

    ShapeOpResult shape_copy(LeHandle &handle, const std::vector<GeometryId> &shapes, const shape_ops::LayerOrPurpose &layer,
                             const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::copy(handle.root, shapes, layer, owner); });
    }

    ShapeOpResult shape_boolean(LeHandle &handle, const std::vector<GeometryId> &a, const std::vector<GeometryId> &b, BooleanOp op,
                                const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::boolean(handle.root, a, b, op, layer, owner); });
    }

    ShapeOpResult shape_to_polygons(LeHandle &handle, const std::vector<GeometryId> &shapes, const std::optional<shape_ops::LayerOrPurpose> &layer,
                                    const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::to_polygons(handle.root, shapes, layer, owner); });
    }

    ShapeOpResult shape_to_rects(LeHandle &handle, const std::vector<GeometryId> &shapes, FractureDirection direction,
                                 const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::to_rects(handle.root, shapes, direction, layer, owner); });
    }

    ShapeOpResult shape_size(LeHandle &handle, const std::vector<GeometryId> &shapes, int64_t dx, int64_t dy,
                             const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::size(handle.root, shapes, dx, dy, layer, owner); });
    }

    ShapeOpResult shape_outline_paths(LeHandle &handle, const std::vector<GeometryId> &shapes, int64_t width,
                                      const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::outline_paths(handle.root, shapes, width, layer, owner); });
    }

    namespace
    {
        // remove_shape_piece for a Wire: path `index` of the Shape it
        // converts to.
        std::expected<void, std::string> remove_wire_path(LeHandle &handle, WireId id, PieceKind kind, size_t index)
        {
            WireData *wire = handle.root.get_wire(id);
            if (!wire)
                return std::unexpected("unknown shape");
            if (kind != PieceKind::PATH)
                return std::unexpected("a wire has only paths to remove");
            const std::optional<std::pair<size_t, size_t>> range = wire_path_range(*wire, index);
            if (!range)
                return std::unexpected("index out of range");
            const WireData before = *wire;
            wire->segments.erase(wire->segments.begin() + static_cast<std::ptrdiff_t>(range->first),
                                 wire->segments.begin() + static_cast<std::ptrdiff_t>(range->second));
            handle.root.note_wire_changed(id);
            handle.root.bump_mutation_version();
            if (handle.command_history.is_recording())
                handle.command_history.current()->record_update<WireId, WireData>(
                    id, before, *wire,
                    [](Root &r, WireId wire_id, const WireData &snapshot)
                    {
                        WireData *target = r.get_wire(wire_id);
                        if (!target)
                            return false;
                        *target = snapshot;
                        r.note_wire_changed(wire_id);
                        return true;
                    });
            return {};
        }
    }

    std::expected<void, std::string> remove_shape_piece(LeHandle &handle, GeometryId id, PieceKind kind, size_t index)
    {
        if (id.wire.valid())
            return remove_wire_path(handle, id.wire, kind, index);
        const ShapeId shape = id.shape;
        ShapeData *data = handle.root.get_shape(shape);
        if (!data)
            return std::unexpected("unknown shape");
        const bool recording = handle.command_history.is_recording();
        // The masks are parallel to their pieces (and may be shorter), so a
        // piece's mask goes with it. The whole ShapeData is the undo
        // snapshot, since apply_shape_snapshot doesn't restore masks.
        const ShapeData before = recording ? *data : ShapeData{};
        auto erase = [index](auto &pieces, auto &masks)
        {
            if (index >= pieces.size())
                return false;
            pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(index));
            if (index < masks.size())
                masks.erase(masks.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        };
        bool removed = false;
        switch (kind)
        {
        case PieceKind::RECT:
            removed = erase(data->rects, data->rect_masks);
            break;
        case PieceKind::POLYGON:
            removed = erase(data->polygons, data->polygon_masks);
            break;
        case PieceKind::PATH:
            removed = erase(data->paths, data->path_masks);
            break;
        default:
            return std::unexpected("only a rect, polygon or path can be removed");
        }
        if (!removed)
            return std::unexpected("index out of range");
        handle.root.note_shape_changed(shape);
        handle.root.bump_mutation_version();
        if (recording)
            handle.command_history.current()->record_update<ShapeId, ShapeData>(
                shape, before, *data,
                [](Root &r, ShapeId id, const ShapeData &snapshot)
                {
                    ShapeData *target = r.get_shape(id);
                    if (!target)
                        return false;
                    *target = snapshot;
                    r.note_shape_changed(id);
                    return true;
                });
        return {};
    }

    std::expected<void, std::string> shape_change_layer(LeHandle &handle, const std::vector<GeometryId> &shapes, const shape_ops::LayerOrPurpose &layer)
    {
        const auto changed = shape_ops::change_layer(handle.root, shapes, layer);
        if (!changed)
            return std::unexpected(changed.error());
        handle.root.bump_mutation_version();
        if (handle.command_history.is_recording())
        {
            // Undo/redo restores layer and purpose exactly, both ways - the
            // generated apply_shape_snapshot can't clear an unset optional
            // purpose (see shape_ops::set_layer_or_purpose's own comment).
            using Snapshot = shape_ops::LayerOrPurpose;
            for (const shape_ops::WireLayerChange &entry : changed->wires)
                handle.command_history.current()->record_update<WireId, WireData>(
                    entry.id, entry.before, entry.after,
                    [](Root &r, WireId id, const WireData &snapshot)
                    {
                        WireData *wire = r.get_wire(id);
                        if (!wire)
                            return false;
                        *wire = snapshot;
                        r.note_wire_changed(id);
                        return true;
                    });
            for (const shape_ops::LayerChange &entry : changed->shapes)
                handle.command_history.current()->record_update<ShapeId, Snapshot>(
                    entry.id, entry.before, entry.after,
                    [](Root &r, ShapeId id, const Snapshot &snapshot)
                    {
                        ShapeData *shape = r.get_shape(id);
                        if (!shape)
                            return false;
                        shape_ops::set_layer_or_purpose(*shape, snapshot);
                        r.note_shape_changed(id);
                        return true;
                    });
        }
        return {};
    }
}
