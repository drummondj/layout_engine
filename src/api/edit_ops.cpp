#include "edit_ops.hpp"

#include "../editing/editing.hpp"
#include "../geometry/shape_ops.hpp"
#include "generated/api/snapshot_appliers.hpp"
#include "le_handle.hpp"

#include <fmt/format.h>

namespace le::edit
{
#include "generated/api/edit_ops_defs.inc"

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
            if (handle.command_history.is_recording())
            {
                for (ShapeId id : *result)
                    handle.command_history.current()->record_create<ShapeId, ShapeData>(
                        id, *handle.root.get_shape(id),
                        [](Root &r, const ShapeData &d) { return r.create_shape(d); },
                        [](Root &r, ShapeId i) { return r.delete_shape(i); });
            }
            return result;
        }
    }

    ShapeOpResult shape_copy(LeHandle &handle, const std::vector<ShapeId> &shapes, const shape_ops::LayerOrPurpose &layer,
                             const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::copy(handle.root, shapes, layer, owner); });
    }

    ShapeOpResult shape_boolean(LeHandle &handle, const std::vector<ShapeId> &a, const std::vector<ShapeId> &b, BooleanOp op,
                                const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::boolean(handle.root, a, b, op, layer, owner); });
    }

    ShapeOpResult shape_to_polygons(LeHandle &handle, const std::vector<ShapeId> &shapes, const std::optional<shape_ops::LayerOrPurpose> &layer,
                                    const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::to_polygons(handle.root, shapes, layer, owner); });
    }

    ShapeOpResult shape_to_rects(LeHandle &handle, const std::vector<ShapeId> &shapes, FractureDirection direction,
                                 const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::to_rects(handle.root, shapes, direction, layer, owner); });
    }

    ShapeOpResult shape_size(LeHandle &handle, const std::vector<ShapeId> &shapes, int64_t dx, int64_t dy,
                             const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::size(handle.root, shapes, dx, dy, layer, owner); });
    }

    ShapeOpResult shape_outline_paths(LeHandle &handle, const std::vector<ShapeId> &shapes, int64_t width,
                                      const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent)
    {
        return run_creating(handle, parent, [&](const shape_ops::ShapeParent &owner)
                            { return shape_ops::outline_paths(handle.root, shapes, width, layer, owner); });
    }

    std::expected<void, std::string> shape_change_layer(LeHandle &handle, const std::vector<ShapeId> &shapes, const shape_ops::LayerOrPurpose &layer)
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
            for (const shape_ops::LayerChange &entry : *changed)
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
