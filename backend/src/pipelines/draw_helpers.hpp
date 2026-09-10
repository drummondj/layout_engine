#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../view_style/view_style.hpp"

#include <blend2d/blend2d.h>

#include <algorithm>
#include <cstdint>

/// @brief Style constants and free drawing helpers shared across the
/// restarted pipelines module - originally ported from
/// pipelines.old/draw_helpers.hpp (git history, a Skia/pixel-space/
/// SkPicture design), then narrowed to Blend2D once RasterizeStage
/// (Skia) and the generic Skia/Blend2D-swappable pipeline were removed
/// (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) - RasterizeBlend2DStage is
/// now the only Rasterize backend, and ComposeStage composites natively
/// in Blend2D too, so nothing here needs to stay backend-agnostic
/// anymore. See that entry for what was deleted (`pattern_shader`,
/// `draw_cross`, `truncate_text_to_width`, `to_sk_color` - each had a
/// direct Blend2D-only equivalent already in use, or a `to_bl_color`
/// counterpart below).
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
    inline constexpr double kDiagonalStripePeriod = 8.0;

    // Blend2D's own repeating BLPattern tiles by exact multiples of the
    // tile's own size, same as Skia's kRepeat tiling did (this constant's
    // own reasoning predates the Skia removal, PIPELINE_REFACTOR_BENCHMARK_RESULTS.md,
    // but still applies unchanged) - so for pattern_blend2d's own
    // "redundant offset lines, let the tile edge clip them" technique to
    // reconstruct a truly continuous periodic hatch (not a phase-shifted
    // zigzag between tiles), the tile size *must* be an exact multiple of
    // kDiagonalStripePeriod. 3x gives a reasonable amount of visible
    // repetition per tile without an oversized tile image.
    inline constexpr int kDiagonalStripeTileSize = static_cast<int>(kDiagonalStripePeriod) * 3;

    // Minimum on-screen text size in pixels regardless of how thin the
    // labeled geometry is - keeps labels legible at any zoom level
    // instead of shrinking to unreadable specks.
    inline constexpr double kMinLabelPixelSize = 12.0;

    // Maximum on-screen text size in pixels, regardless of how large the
    // labeled geometry's own on-screen footprint grows (e.g. zoomed in
    // close on a single cell) - a label only needs to stay legible, not
    // grow without bound to match the geometry (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md's
    // own text-rendering-cost investigation: capping this also bounds
    // RasterizeBlend2DStage's own per-(label,size,color) glyph-bitmap
    // cache to a small, fixed handful of distinct sizes instead of a
    // continuously-changing one per zoom tick, and keeps each cached
    // bitmap - and so each blit - small). Tunable; 24px comfortably
    // exceeds what's needed for a short pin/cell name to read clearly.
    inline constexpr double kMaxLabelPixelSize = 24.0;

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

    // Stroke width (on-screen px, constant regardless of zoom) of the "X"
    // FillPattern::CROSS draws through a CUT-purpose TERMINAL shape,
    // instead of a tiled pattern. A BLContext draws in dbu-space through
    // an ambient dbu-to-pixel scale (RasterizeBlend2DStage's own
    // translate+scale+flip setup), so a caller must divide this by that
    // same `scale` before handing it to set_stroke_width - the same
    // "1.0 / scale" pattern this project's own hairline-stroke convention
    // already uses - or the resulting on-screen width scales with zoom
    // instead of staying
    // fixed (a real, found and fixed bug: both backends passed this
    // constant straight through unscaled for a while).
    inline constexpr float kViaCrossStrokeWidth = 3.0f;

    /// @brief True when a dbu-space bbox is under 1 on-screen pixel in
    /// BOTH dimensions at the given scale - the exact "invisible dot"
    /// test pipelines.old/stages/viewport_filter_stage.hpp used to drop a
    /// shape entirely (not just one dimension, so a long thin wire
    /// survives even if its width alone is sub-pixel; only a true
    /// dot-sized shape is culled). Used by RasterizeBlend2DStage's own
    /// draw_view_shapes_blend2d - a Rect/Polygon this returns true for is
    /// skipped before any fill/outline/pattern/cross work is done for it
    /// at all, not merely left undrawn after the fact, since the whole
    /// point is avoiding that work's own real cost (BLPath construction,
    /// pattern lookup, draw-call dispatch) for geometry that ultimately
    /// paints zero visible pixels either way - reintroduced
    /// (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) after a real zoom-fit
    /// investigation found Rasterize walking every shape in a design with
    /// no equivalent size-based skip, unlike the pre-restart pipeline's
    /// own ViewportFilterStage. Text (Shape.texts) is gated indirectly,
    /// not by this function directly: draw_view_shapes_blend2d's own
    /// draw_one_shape skips a whole shape's own text entirely once none
    /// of its rects/polygons/paths survive this check (own doc comment,
    /// PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) - a real, live shape
    /// rendering an unrelated label floored at kMinLabelPixelSize with
    /// nothing visible to anchor it to reads as a rendering bug, not a
    /// feature.
    inline bool bbox_is_sub_pixel(int64_t width_dbu, int64_t height_dbu, double scale)
    {
        return static_cast<double>(width_dbu) * scale < 1.0 && static_cast<double>(height_dbu) * scale < 1.0;
    }

    /// @brief bbox_is_sub_pixel for a Polygon - computes its own bbox
    /// in-line (a Polygon carries no cached bbox of its own) rather than
    /// building a full BLPath/SkPath first just to measure it; an empty
    /// point list (shouldn't occur for a real Shape's own polygon, but
    /// not this function's job to assume) is treated as sub-pixel, since
    /// there is nothing to draw either way.
    inline bool polygon_is_sub_pixel(const Polygon &poly, double scale)
    {
        if (poly.points.empty())
            return true;
        int64_t min_x = poly.points.front().x;
        int64_t max_x = min_x;
        int64_t min_y = poly.points.front().y;
        int64_t max_y = min_y;
        for (const Point &p : poly.points)
        {
            min_x = std::min(min_x, p.x);
            max_x = std::max(max_x, p.x);
            min_y = std::min(min_y, p.y);
            max_y = std::max(max_y, p.y);
        }
        return bbox_is_sub_pixel(max_x - min_x, max_y - min_y, scale);
    }

    // Converts this codebase's own plain, backend-agnostic Color into a
    // BLRgba32 - shared by RasterizeBlend2DStage (per-shape fill/stroke
    // colors) and ComposeStage (drag-rect overlay) now that Blend2D is
    // the only backend either ever draws with. BLRgba32's own constructor
    // takes (r, g, b, a) in that order (core/rgba.h) - direct field-for-
    // field mapping, no reordering needed.
    inline BLRgba32 to_bl_color(Color c) { return BLRgba32(c.r, c.g, c.b, c.a); }

    // Rubber-band drag-rectangle colors (ComposeStage's own doc comment) -
    // ported from pipelines.old/draw_helpers.hpp verbatim, same RGBA
    // values, so a user familiar with the pre-restart tool sees the exact
    // same colors: blue for a plain select-drag, green for a
    // drag-to-zoom gesture (ViewRenderOptions::drag_is_zoom picks which
    // pair). Translucent fill + a more opaque stroke, same convention as
    // every other overlay color in this codebase.
    inline constexpr Color kDragRectFillColor = {80, 160, 255, 60};
    inline constexpr Color kDragRectStrokeColor = {80, 160, 255, 220};
    inline constexpr Color kZoomDragRectFillColor = {80, 255, 160, 60};
    inline constexpr Color kZoomDragRectStrokeColor = {80, 255, 160, 220};
    inline constexpr float kDragRectStrokeWidth = 2.0f;

    // White selection outline (UPDATES.md item 7) - ported verbatim from
    // pipelines.old/draw_helpers.hpp's own kSelectionOutlineColor/
    // kSelectionOutlineStrokeWidth, same RGBA/width, so a user familiar
    // with the pre-restart tool sees the exact same highlight.
    inline constexpr Color kSelectionOutlineColor = {255, 255, 255, 255};
    inline constexpr double kSelectionOutlineStrokeWidth = 2.0;

    // Red grid-snap cursor box (UPDATES.md 7.1 item 1) and yellow hover
    // outline (UPDATES.md 7.1) - both ported verbatim from
    // pipelines.old/draw_helpers.hpp's own kCursorBoxColor/
    // kCursorBoxStrokeWidth/kCursorBoxSizePx/kHoverOutlineColor/
    // kHoverOutlineStrokeWidth, same RGBA/width/size.
    inline constexpr Color kCursorBoxColor = {255, 0, 0, 255};
    inline constexpr double kCursorBoxStrokeWidth = 1.0;
    inline constexpr double kCursorBoxSizePx = 7.0;
    inline constexpr Color kHoverOutlineColor = {255, 255, 0, 255};
    inline constexpr double kHoverOutlineStrokeWidth = 2.0;

    // Dashed, translucent-white Move ghost preview (UPDATES.md item 21) -
    // ported verbatim from pipelines.old/draw_helpers.hpp's own
    // kMoveGhostColor/kMoveGhostStrokeWidth/kMoveGhostDashOnPx/
    // kMoveGhostDashOffPx, same RGBA/width/dash pattern.
    inline constexpr Color kMoveGhostColor = {255, 255, 255, 160};
    inline constexpr double kMoveGhostStrokeWidth = 2.0;
    inline constexpr double kMoveGhostDashOnPx = 6.0;
    inline constexpr double kMoveGhostDashOffPx = 4.0;

    /// @brief Strokes `piece`'s own geometry (already mapped to device-
    /// pixel space by the caller's own `to_pixel` - see `ComposeStage`'s
    /// own doc comment for why its top-level composite canvas has no
    /// ambient transform to draw through directly, unlike
    /// `RasterizeBlend2DStage`'s own per-node canvas) with `color`/
    /// `stroke_width` - shared by every ComposeStage overlay that traces
    /// one piece's own outline (selection, hover, Move-ghost): each is
    /// just a different color/width/dash on the exact same "trace each
    /// rect/polygon/path's own buffered outline" technique, ported from
    /// pipelines.old/draw_helpers.hpp's own draw_selected_piece_outline/
    /// draw_hover_outline/draw_move_ghost (near-identical bodies there
    /// too, just duplicated three ways under Skia - unified into one
    /// helper here since Blend2D's own `BLContext` state (fill/stroke
    /// style, dash array) needs setting by the caller anyway, right next
    /// to whichever color/width/dash *this* overlay uses, not hidden
    /// inside a helper that would otherwise need a color/width/dash
    /// parameter per overlay kind). A Path traces its own *buffered
    /// outline polygon* (`Geometry::path_to_polygons`), not a stroke
    /// along its centerline - matching exactly what
    /// `Geometry::find_hit_piece`/`fully_enclosed_pieces` themselves test
    /// against, so a highlight traces what's actually
    /// clickable/selected/hit, not an invisible centerline that would
    /// collapse into a solid-looking blob for a path whose width is
    /// comparable to its own length (a real reported bug against
    /// synthetic stress-test geometry shaped exactly like that).
    ///
    /// Caller's own responsibility: `ctx.set_stroke_style`/
    /// `set_stroke_width`/dash state, called once before this (per-layer-
    /// style, not per-piece) if drawing several pieces with the same
    /// style in a row - this function only ever calls `ctx.stroke_path`.
    template <typename ToPixel>
    inline void stroke_piece_outline(BLContext &ctx, const Shape &piece, ToPixel &&to_pixel)
    {
        auto stroke_polygon = [&](const Polygon &polygon)
        {
            if (polygon.points.empty())
                return;

            BLPath path;
            const BLPoint first = to_pixel(polygon.points.front());
            path.move_to(first);
            for (size_t i = 1; i < polygon.points.size(); ++i)
                path.line_to(to_pixel(polygon.points[i]));
            path.close();
            ctx.stroke_path(path);
        };

        for (const Rect &rect : piece.rects)
            stroke_polygon(Geometry::rect_to_polygon(rect));

        for (const Polygon &polygon : piece.polygons)
            stroke_polygon(polygon);

        for (const Path &path : piece.paths)
            for (const Polygon &buffered : Geometry::path_to_polygons(path))
                stroke_polygon(buffered);
    }
}
