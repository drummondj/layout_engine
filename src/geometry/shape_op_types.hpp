#pragma once

// The value types the shape operations take, apart from Geometry itself, so
// the extension SDK can name them without pulling in Boost.Geometry.

#include "../database/database.hpp"

#include <optional>
#include <variant>

namespace le
{
    /// @brief Which combination Geometry::boolean_shapes computes between
    /// its two input groups (NOT is "a minus b").
    enum class BooleanOp
    {
        Or,
        And,
        Not,
    };

    /// @brief Cut-line direction for Geometry::shape_to_rects. Horizontal
    /// cuts with horizontal lines, giving horizontal strips (each rect as
    /// wide as the shape allows); Vertical is the transpose.
    enum class FractureDirection
    {
        Horizontal,
        Vertical,
    };
}

namespace le::shape_ops
{
    /// @brief Where a shape operation's new Shapes go. An Abstract/Layout
    /// means that object's own free-standing shapes (Shape.in_abstract/
    /// in_layout - never written by write_lef/write_def); every other kind
    /// is that object's own regular shapes list.
    using ShapeParent = std::variant<AbstractId, LayoutId, ObstructionId, TerminalPortId, RouteId, BlockageId, PhysicalPortSegmentId>;

    /// @brief A Shape's layer-or-purpose - exactly one is ever set (see
    /// Shape.layer's own schema.py comment).
    struct LayerOrPurpose
    {
        LayerId layer;
        std::optional<ShapePurpose> purpose;
    };
}
