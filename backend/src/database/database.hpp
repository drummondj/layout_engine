#pragma once
#include "generated/property.hpp"
#include "generated/root.hpp"
#include "piece_kind.hpp"

namespace le
{
    // Shape used to be a plain (non-pooled) value type - Geometry/Pipeline
    // still construct/copy/mutate "a shape" as an ordinary value (Pipeline
    // synthesizes many ephemeral ones per render call via RECT/PATH/POLYGON
    // ITERATE expansion, never persisted to Root), even though Shape is now
    // pooled/addressable (TCL_EXPLORATION.md Phase 3) for TerminalPort/
    // Obstruction ownership and stable-id shape CRUD. This alias keeps
    // every existing Shape-typed signature (Geometry::bbox,
    // RenderedShape::shape, ...) compiling unchanged - ShapeData and Shape
    // are the exact same type, just two names for two different purposes
    // (Root-addressed storage vs. a plain in-memory value with no
    // database identity).
    using Shape = ShapeData;
}
