#pragma once
#include "area_ops.hpp"
#include "geometry_core.hpp"
#include "iterate_expansion.hpp"
#include "label_placement.hpp"
#include "piece_hit_test.hpp"
#include "transforms.hpp"

namespace le
{
    /// @brief Boost.Geometry-backed operations over the database's
    /// Point/Rect/Polygon/Path/Shape types, one header per concern; every
    /// static member is reachable as Geometry::name.
    class Geometry : public LabelPlacement, public IterateExpansion
    {
    };
}
