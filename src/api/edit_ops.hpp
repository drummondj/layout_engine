#pragma once
// The C++ half of the C API's edits: validate, change the Root, bump its
// mutation version and record undo, in native types (database ids, dbu,
// std::expected errors). The le_* functions convert their C arguments and
// call these under the handle's lock; extensions reach them through
// le::ext::WriteView. Every function here expects the caller to hold the
// handle's exclusive lock.

#include "api.hpp"
#include "database.hpp"
#include "generated/api/edit_types.hpp"
#include "../geometry/shape_op_types.hpp"

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace le::edit
{
#include "generated/api/edit_ops_decls.inc"

    /// @brief The Technology's dbu per micron (DATABASE MICRONS), if one
    /// with a positive scale has been read. The session has one Technology.
    std::optional<double> dbu_per_um(const Root &root);

    /// @brief New Shapes' ids, or a user-facing error.
    using ShapeOpResult = std::expected<std::vector<ShapeId>, std::string>;

    // The shape_* operations. Each new Shape goes to `parent`, else the
    // open view's Abstract/Layout (the GUI's, then the current one);
    // an unset `layer` keeps each input's own.

    /// @brief One new Shape per input, same geometry, on `layer`.
    ShapeOpResult shape_copy(LeHandle &handle, const std::vector<ShapeId> &shapes, const shape_ops::LayerOrPurpose &layer,
                             const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new Shape holding `op` of `a` against `b`, on a[0]'s layer unless `layer` is set.
    ShapeOpResult shape_boolean(LeHandle &handle, const std::vector<ShapeId> &a, const std::vector<ShapeId> &b, BooleanOp op,
                                const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new polygon-only Shape per input.
    ShapeOpResult shape_to_polygons(LeHandle &handle, const std::vector<ShapeId> &shapes, const std::optional<shape_ops::LayerOrPurpose> &layer,
                                    const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new rect-only Shape per input, cut in `direction`.
    ShapeOpResult shape_to_rects(LeHandle &handle, const std::vector<ShapeId> &shapes, FractureDirection direction,
                                 const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new Shape per input, grown (positive) or shrunk (negative) by dx/dy dbu.
    ShapeOpResult shape_size(LeHandle &handle, const std::vector<ShapeId> &shapes, int64_t dx, int64_t dy,
                             const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief One new path-only Shape per input along its outline, `width` dbu wide.
    ShapeOpResult shape_outline_paths(LeHandle &handle, const std::vector<ShapeId> &shapes, int64_t width,
                                      const std::optional<shape_ops::LayerOrPurpose> &layer, const std::optional<shape_ops::ShapeParent> &parent);
    /// @brief Moves each shape in place onto `layer`; all or nothing.
    std::expected<void, std::string> shape_change_layer(LeHandle &handle, const std::vector<ShapeId> &shapes, const shape_ops::LayerOrPurpose &layer);
}
