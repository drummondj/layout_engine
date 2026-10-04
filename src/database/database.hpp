#pragma once
#include "generated/database/property.hpp"
#include "generated/database/root.hpp"
#include "piece_kind.hpp"

namespace le
{
    // Geometry and the pipelines construct/copy/mutate "a shape" as an
    // ordinary value (e.g. the ephemeral shapes RECT/PATH/POLYGON ITERATE
    // expansion produces, never persisted to Root), while Root stores
    // Shapes in a pool for stable-id ownership and CRUD. ShapeData and
    // Shape are the exact same type, just two names for two different purposes
    // (Root-addressed storage vs. a plain in-memory value with no
    // database identity).
    using Shape = ShapeData;
}
