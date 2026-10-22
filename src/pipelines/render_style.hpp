#pragma once
// A ViewLayer's colors and fill pattern, apart from view_style.hpp so the
// generated renderable_classes.hpp can declare a class's default style.
#include <cstdint>

namespace le
{
    struct Color
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 255;

        bool operator==(const Color &) const = default;
    };

    /// @brief The fill treatment RasterizeBlend2DStage draws a ViewLayer's
    /// shapes with, on top of/instead of a flat color wash (a tiled
    /// pattern - see pattern_blend2d - or, for CROSS, drawn directly
    /// per-shape). Always drawn with a transparent
    /// background so ViewLayers below remain visible through the gaps.
    enum class FillPattern
    {
        NONE,                // flat translucent fill (BOUNDARY today)
        DIAGONAL_STRIPES_NE, // ROUTING, horizontal direction: "/" hatch
        DIAGONAL_STRIPES_NW, // ROUTING, vertical direction: "\" hatch
        CROSS,               // CUT: an X spanning each shape's own bounds
        BRICK,               // any OBSTRUCTION, regardless of layer type
        DOTS,                // every other layer type
    };
}
