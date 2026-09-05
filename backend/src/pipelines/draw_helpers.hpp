#pragma once
#include "../database/database.hpp"
#include "../view_style/view_style.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTileMode.h"

#include <cstdint>
#include <string>

/// @brief Style constants and free Skia drawing helpers ported from
/// pipelines.old/draw_helpers.hpp (git history) for the restarted
/// pipelines module - see that file's own comments for the original
/// rationale behind each constant/algorithm, preserved here only where it
/// still applies to the new dbu-space-drawing, per-node-raster-image
/// design (RasterizeStage's own doc comment) rather than the old
/// pixel-space/SkPicture one.
namespace le
{
    // Fixed screen-pixel tile size for every tiled FillPattern below
    // except the diagonal stripes (which need their own - see
    // kDiagonalStripeTileSize) - deliberately not scaled with the current
    // zoom (the pattern stays a constant visual density at any zoom
    // level, like a hatch fill in a CAD tool, rather than shrinking to
    // nothing zoomed out or ballooning zoomed in) - see
    // pattern_shader_for_scale's own local-matrix compensation below for
    // how that's achieved when drawing happens in dbu space through an
    // ambient scale, unlike the old pixel-space design this was ported
    // from.
    inline constexpr int kPatternTileSize = 12;

    // Spacing between stripes, in screen pixels.
    inline constexpr SkScalar kDiagonalStripePeriod = 8.0f;

    // SkShader's kRepeat tiling only ever translates by exact multiples of
    // the tile's own size - so for the "redundant offset lines, let the
    // canvas clip them" technique below to reconstruct a truly continuous
    // periodic hatch (not a phase-shifted zigzag between tiles), the tile
    // size *must* be an exact multiple of kDiagonalStripePeriod. 3x gives
    // a reasonable amount of visible repetition per tile without an
    // oversized offscreen surface.
    inline constexpr int kDiagonalStripeTileSize = static_cast<int>(kDiagonalStripePeriod) * 3;

    // Minimum on-screen text size in pixels regardless of how thin the
    // labeled geometry is - keeps labels legible at any zoom level
    // instead of shrinking to unreadable specks.
    inline constexpr double kMinLabelPixelSize = 10.0;

    // Fraction of a terminal/route label's own local geometry width
    // (Text::size, Geometry::local_width_at) actually used for text size,
    // so the label doesn't touch/overflow the edges of the shape it's on.
    inline constexpr double kLabelWidthRatio = 0.6;

    // A Placement's own name label - font size is a fraction of the
    // placement's own on-screen *height* (unlike kLabelWidthRatio above,
    // which scales off local width), same "shrinks/grows with the cell,
    // floored at kMinLabelPixelSize" idea, just driven by the other axis.
    inline constexpr double kPlacementLabelHeightRatio = 0.025;

    // Small inset (px) so a placement's own name label doesn't visually
    // touch its left/right/bottom edges - subtracted from both sides of
    // the available width before truncating to fit, and added to the
    // baseline's own position above the bottom edge.
    inline constexpr double kPlacementLabelPaddingPx = 2.0;

    // Stroke width (px) of the "X" FillPattern::CROSS draws through a
    // CUT-purpose TERMINAL shape, instead of a tiled pattern_shader.
    inline constexpr float kViaCrossStrokeWidth = 3.0f;

    inline SkColor to_sk_color(Color c) { return SkColorSetARGB(c.a, c.r, c.g, c.b); }

    // Renders `pattern` into a small transparent tile and wraps it as a
    // repeating SkShader, in the tile's own local (untransformed)
    // coordinate space - the caller is responsible for countering
    // whatever ambient scale is active (see draw_view_shapes' own call
    // site) so the tile reads as a fixed on-screen pixel density rather
    // than scaling with zoom, the same visual goal the old pixel-space
    // pipeline got "for free" by never needing this compensation at all.
    // NONE/CROSS return nullptr (flat fill / drawn specially via
    // draw_cross below) - ported verbatim from pipelines.old/draw_helpers.hpp.
    inline sk_sp<SkShader> pattern_shader(FillPattern pattern, SkColor color)
    {
        if (pattern == FillPattern::NONE || pattern == FillPattern::CROSS)
            return nullptr;

        const bool diagonal = pattern == FillPattern::DIAGONAL_STRIPES_NE || pattern == FillPattern::DIAGONAL_STRIPES_NW;
        const int tile_size = diagonal ? kDiagonalStripeTileSize : kPatternTileSize;

        const SkImageInfo info = SkImageInfo::MakeN32Premul(tile_size, tile_size);
        sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
        SkCanvas *canvas = surface->getCanvas();
        canvas->clear(SK_ColorTRANSPARENT);

        SkPaint paint;
        // Anti-aliasing off, deliberately - a hairline at an exact
        // pixel-grid coordinate still gets split into two ~50%-coverage
        // rows by Skia's AA, which repeated across every tile turns crisp
        // brick/stripe edges into a hazy, low-alpha wash instead of a
        // legible pattern.
        paint.setAntiAlias(false);
        paint.setColor(color);

        const auto s = static_cast<SkScalar>(tile_size);

        switch (pattern)
        {
        case FillPattern::DIAGONAL_STRIPES_NE:
        case FillPattern::DIAGONAL_STRIPES_NW:
        {
            paint.setStyle(SkPaint::kStroke_Style);
            const bool ne = pattern == FillPattern::DIAGONAL_STRIPES_NE;
            for (SkScalar offset = -s; offset <= 2 * s; offset += kDiagonalStripePeriod)
            {
                if (ne)
                    canvas->drawLine(offset, 0, offset + s, s, paint);
                else
                    canvas->drawLine(offset + s, 0, offset, s, paint);
            }
            break;
        }
        case FillPattern::BRICK:
        {
            paint.setStyle(SkPaint::kStroke_Style);
            canvas->drawLine(0, 0, s, 0, paint);             // horizontal joint at the tile's top edge
            canvas->drawLine(0, s / 2, s, s / 2, paint);     // horizontal joint between the two rows
            canvas->drawLine(s / 2, 0, s / 2, s / 2, paint); // top row's interior vertical joint
            canvas->drawLine(0, s / 2, 0, s, paint);         // bottom row's interior vertical joint (staggered to the tile edge)
            break;
        }
        case FillPattern::DOTS:
        {
            paint.setStyle(SkPaint::kFill_Style);
            canvas->drawCircle(s / 2, s / 2, s * 0.15f, paint);
            break;
        }
        default:
            break;
        }

        sk_sp<SkImage> image = surface->makeImageSnapshot();
        return image->makeShader(SkTileMode::kRepeat, SkTileMode::kRepeat, SkSamplingOptions());
    }

    // Draws an "X" spanning `bounds` - FillPattern::CROSS's own drawn-
    // specially case (a CUT-purpose TERMINAL layer), not a tiled
    // pattern_shader.
    inline void draw_cross(SkCanvas &canvas, const SkRect &bounds, const SkPaint &paint)
    {
        canvas.drawLine(bounds.left(), bounds.top(), bounds.right(), bounds.bottom(), paint);
        canvas.drawLine(bounds.left(), bounds.bottom(), bounds.right(), bounds.top(), paint);
    }

    // Binary-searches how many leading characters of `text` to drop
    // (prefixing "...") so it fits within `max_width_px` - keeps the
    // distinguishing suffix of a hierarchical instance name visible
    // rather than the shared prefix. Returns "" if even the ellipsis
    // alone doesn't fit.
    inline std::string truncate_text_to_width(const std::string &text, const SkFont &font, SkScalar max_width_px)
    {
        if (font.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8) <= max_width_px)
            return text;

        static const std::string kEllipsis = "...";
        if (font.measureText(kEllipsis.c_str(), kEllipsis.size(), SkTextEncoding::kUTF8) > max_width_px)
            return "";

        std::size_t lo = 0, hi = text.size();
        while (lo < hi)
        {
            const std::size_t mid = lo + (hi - lo) / 2;
            const std::string candidate = kEllipsis + text.substr(mid);
            if (font.measureText(candidate.c_str(), candidate.size(), SkTextEncoding::kUTF8) <= max_width_px)
                hi = mid;
            else
                lo = mid + 1;
        }
        return kEllipsis + text.substr(lo);
    }
}
