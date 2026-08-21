#pragma once
#include "../scene/scene.hpp"
#include "include/core/SkSurface.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace le
{
    struct PixelPoint
    {
        double x = 0.0;
        double y = 0.0;
    };

    struct PixelRect
    {
        PixelPoint ll;
        PixelPoint ur;
    };

    struct PixelPolygon
    {
        std::vector<PixelPoint> points;
    };

    struct PixelPath
    {
        PixelPolygon polygon;
        double width = 0.0;

        /// Transformed from RenderedShape::path_outlines (core/rendered_shape.hpp) -
        /// the path's buffered outline (flat ends, miter joins), computed
        /// once at Pipeline::generate_shapes time, not here - see that
        /// field's own comment for why. draw_group fills/outlines this the
        /// same way it already does a real PixelPolygon, instead of
        /// stroking `polygon` at full `width` (which used to read as a
        /// solid block regardless of the layer's fill pattern).
        std::vector<PixelPolygon> buffered_outline;
    };

    struct PixelText
    {
        std::string label;
        PixelPoint location;
        double size = 0.0;
    };

    /// @brief A Shape transformed from dbu-space to pixel-space (Scene's
    /// `pixel = (dbu - pan) * scale`), mirroring Shape's own
    /// rects/polygons/paths/texts structure with double coordinates instead
    /// of Rect/Polygon/Path/Text's integer dbu ones. Still no Y-axis flip
    /// applied here - dbu-space y increases upward (physical layout
    /// convention) and this maps it straight through, so pixel-space y does
    /// too, which doesn't match Skia's own y-down canvas convention until
    /// corrected. That correction happens at `Renderer::rasterize()` (a
    /// canvas transform applied once per frame, not a per-shape one here) -
    /// see its own doc comment. No ViewLayerId field - callers get that
    /// from the grouping map key (see Renderer) instead of carrying a
    /// redundant copy per shape.
    struct PixelShape
    {
        std::vector<PixelRect> rects;
        std::vector<PixelPolygon> polygons;
        std::vector<PixelPath> paths;
        std::vector<PixelText> texts;

        /// Mirrors RenderedShape::origin (core/rendered_shape.hpp) - nullopt
        /// for the BOUNDARY shape. Carried through transform_to_pixels
        /// unchanged (no coordinate transform needed) so build_picture can
        /// draw a selection outline (UPDATES.md 7) without needing the
        /// dbu-space RenderedShape map too.
        std::optional<SelectionRef> origin;
    };

    /// @brief Raw RGBA8888 pixel data for one rasterized frame, produced by
    /// `Renderer::rasterize()`. Explicitly `kRGBA_8888_SkColorType` (not
    /// Skia's platform-native `kN32_SkColorType`, which is BGRA on some
    /// platforms and RGBA on others - see `SkColorType.h`) so the byte
    /// layout is identical between this project's macOS dev machine and
    /// its Linux deployment target; the raw output isn't visually
    /// inspectable, so a silent per-platform channel-order mismatch here
    /// would be very easy to miss until it showed up as wrong colors on
    /// Linux specifically. Premultiplied alpha, row-major, top-to-bottom
    /// (row 0 is the top on-screen row - see `rasterize()`'s Y-flip). Exact
    /// row_bytes may exceed `width * 4` (Skia may pad rows for alignment) -
    /// always index by it, never assume a tight stride. `data` points into
    /// memory owned by the `Renderer` instance (the cached raster surface
    /// backing it) - valid only until the next call that invalidates this
    /// cache entry, same lifetime convention as `build_picture`'s returned
    /// `sk_sp<SkPicture>&`. The exact format/orientation a real Flutter
    /// texture needs isn't confirmed against Flutter's own API yet (no
    /// `api`/plugin module exists in this repo yet) - revisit this comment
    /// once that integration happens.
    struct PixelBuffer
    {
        const uint8_t *data = nullptr;
        int width = 0;
        int height = 0;
        size_t row_bytes = 0;
    };

    /// @brief Bundles the PixelBuffer view together with the raster surface
    /// that owns its backing memory, so a RasterizeStage's cache keeps that
    /// memory alive for as long as the cache entry is valid - PixelBuffer
    /// itself holds no ownership, just a view into this. Also what makes
    /// ComposeWithOverlaysStage's cheap image-snapshot reuse possible - the
    /// surface has to still be alive to snapshot it.
    struct RasterizedFrame
    {
        sk_sp<SkSurface> surface;
        PixelBuffer buffer;
    };
}
