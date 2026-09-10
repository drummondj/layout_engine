#pragma once

#include "../../core/placement_geometry.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../blend2d_font.hpp"
#include "../draw_helpers.hpp"
#include "../pipeline_options.hpp"
#include "../rasterize_output.hpp"
#include "../tbb_core.hpp"
#include "hierarchy_resolver_stage.hpp"

#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"

#include <blend2d/blend2d.h>

#include <boost/geometry/index/rtree.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace le
{
    namespace bgi = boost::geometry::index;

    inline BLRgba32 to_bl_color(Color c) { return BLRgba32(c.r, c.g, c.b, c.a); }

    inline BLPath to_bl_path(const Polygon &polygon, bool close)
    {
        BLPath path;
        bool first = true;
        for (const Point &p : polygon.points)
        {
            if (first)
            {
                path.move_to(static_cast<double>(p.x), static_cast<double>(p.y));
                first = false;
            }
            else
            {
                path.line_to(static_cast<double>(p.x), static_cast<double>(p.y));
            }
        }
        if (close)
            path.close();
        return path;
    }

    /// @brief Blend2D analog of draw_helpers.hpp's own `pattern_shader` -
    /// same tile geometry/constants (kPatternTileSize/kDiagonalStripePeriod/
    /// kDiagonalStripeTileSize, reused directly from that header rather
    /// than redefined here), rendered into a small BLImage via a
    /// throwaway BLContext instead of an SkSurface, then wrapped as a
    /// repeating BLPattern. NONE/CROSS return an empty (default-constructed)
    /// BLPattern - the caller checks `!pattern.empty()` (mirroring
    /// pattern_shader's own nullptr-means-flat-fill contract) since
    /// FillPattern::CROSS is drawn specially (draw_cross_blend2d) rather
    /// than tiled.
    inline BLPattern pattern_blend2d(FillPattern pattern, BLRgba32 color)
    {
        if (pattern == FillPattern::NONE || pattern == FillPattern::CROSS)
            return BLPattern();

        const bool diagonal = pattern == FillPattern::DIAGONAL_STRIPES_NE || pattern == FillPattern::DIAGONAL_STRIPES_NW;
        const int tile_size = diagonal ? kDiagonalStripeTileSize : kPatternTileSize;

        BLImage tile(tile_size, tile_size, BL_FORMAT_PRGB32);
        BLContext ctx(tile);
        ctx.clear_all();

        const double s = static_cast<double>(tile_size);

        switch (pattern)
        {
        case FillPattern::DIAGONAL_STRIPES_NE:
        case FillPattern::DIAGONAL_STRIPES_NW:
        {
            ctx.set_stroke_style(color);
            const bool ne = pattern == FillPattern::DIAGONAL_STRIPES_NE;
            for (double offset = -s; offset <= 2 * s; offset += static_cast<double>(kDiagonalStripePeriod))
            {
                if (ne)
                    ctx.stroke_line(BLLine(offset, 0, offset + s, s));
                else
                    ctx.stroke_line(BLLine(offset + s, 0, offset, s));
            }
            break;
        }
        case FillPattern::BRICK:
        {
            // +0.5 on each line's own cross-axis coordinate (the "thin"
            // direction the 1-unit-wide default stroke actually needs to
            // fully cover, not the along-line span) - without it, a
            // hairline centered exactly on an integer coordinate (0,
            // s/2) straddles the boundary between two pixel rows/
            // columns, each getting only partial AA coverage. Skia's own
            // pattern_shader sidesteps this with paint.setAntiAlias(false)
            // (draw_helpers.hpp's own comment - "turns crisp brick/
            // stripe edges into a hazy, low-alpha wash" is literally
            // this same failure mode), but Blend2D has no equivalent -
            // exactly one BLRenderingQuality value, always antialiased -
            // so the fix here is geometric instead: shift the line so
            // its own AA-softened edges land on the row/column's real
            // integer boundaries (e.g. y=0.5 for row [0,1)) rather than
            // straddling them. Found by direct pixel inspection - real,
            // measured brick ink topped out at ~191/255 alpha before
            // this (RasterizeBlend2DStageFixture's own wide color
            // tolerance was working around it, not just AA softness).
            ctx.set_stroke_style(color);
            ctx.stroke_line(BLLine(0, 0.5, s, 0.5));
            ctx.stroke_line(BLLine(0, s / 2 + 0.5, s, s / 2 + 0.5));
            ctx.stroke_line(BLLine(s / 2 + 0.5, 0, s / 2 + 0.5, s / 2));
            ctx.stroke_line(BLLine(0.5, s / 2, 0.5, s));
            break;
        }
        case FillPattern::DOTS:
        {
            ctx.set_fill_style(color);
            ctx.fill_circle(BLCircle(s / 2, s / 2, s * 0.15));
            break;
        }
        default:
            break;
        }
        ctx.end();

        return BLPattern(tile, BL_EXTEND_MODE_REPEAT);
    }

    inline void draw_cross_blend2d(BLContext &ctx, const BLBox &bounds, BLRgba32 color, double stroke_width)
    {
        ctx.set_stroke_style(color);
        ctx.set_stroke_width(stroke_width);
        ctx.stroke_line(BLLine(bounds.x0, bounds.y0, bounds.x1, bounds.y1));
        ctx.stroke_line(BLLine(bounds.x0, bounds.y1, bounds.x1, bounds.y0));
    }

    /// @brief Cache key for `render_text_bitmap` below - a label's own
    /// rendered ink depends only on its text, the BLFont it was shaped
    /// with (captured here by its own already-rounded pixel size, the
    /// same `font_key` draw_view_shapes_blend2d's own font_cache is keyed
    /// by), and its fill color (a ViewLayer's own outline_color, constant
    /// for every shape on that layer - see draw_view_shapes_blend2d's own
    /// per-layer setup).
    struct TextBitmapCacheKey
    {
        std::string label;
        int font_key;
        uint32_t color;

        bool operator==(const TextBitmapCacheKey &other) const noexcept
        {
            return font_key == other.font_key && color == other.color && label == other.label;
        }
    };

    struct TextBitmapCacheKeyHash
    {
        std::size_t operator()(const TextBitmapCacheKey &key) const noexcept
        {
            std::size_t h = std::hash<std::string>()(key.label);
            h = h * 31 + static_cast<std::size_t>(key.font_key);
            h = h * 31 + static_cast<std::size_t>(key.color);
            return h;
        }
    };

    struct CachedTextBitmap
    {
        // Default-constructed (0x0) means "nothing to draw" (an empty
        // label, or a shaped run with zero glyphs) - the caller checks
        // width()/height() rather than treating every cache entry as
        // real ink.
        BLImage image;
        // Device-pixel offset from the label's own baseline origin (the
        // point draw_view_shapes_blend2d's text loop maps text.location
        // through) to this image's own top-left corner.
        BLPoint offset;
    };

    /// @brief Renders `label` in `font` once into a small, tightly-sized
    /// BLImage - the direct analog of Skia's own internal glyph-bitmap
    /// cache, which Blend2D's `fill_utf8_text` has no equivalent of (it
    /// shapes and fills each glyph run as vector paths on every single
    /// call - measured at ~0.84us/call for a 2-character label,
    /// PIPELINE_REFACTOR_BENCHMARK_RESULTS.md). Blitting this bitmap
    /// (draw_view_shapes_blend2d's own text loop, via its own
    /// text_bitmap_cache) is dramatically cheaper than a fresh shape+fill
    /// for every repeated occurrence of the same (label, size, color) -
    /// the common case here, since `Text::label` is a Terminal's own pin
    /// name (`TerminalData::name`, hierarchy_resolver_stage.hpp), shared
    /// verbatim across every instance of the same library cell in a
    /// design built from a small standard-cell library (measured ~8.6x
    /// faster end to end on a synthetic 8-distinct-label workload).
    ///
    /// `BLTextMetrics::bounding_box` (font.h's own `get_text_metrics`) is
    /// deliberately NOT used for sizing - confirmed by reading Blend2D's
    /// own `font.cpp` (`bl_font_get_text_metrics`): its `y0`/`y1` are
    /// unconditionally `0.0`, and its `x0`/`x1` come from only the FIRST
    /// and LAST glyph's own individual bounds, not a true union of every
    /// glyph in the run - both useless for a tight bitmap size (found by
    /// a direct standalone check: a real "VDD" bounding_box came back
    /// 2px tall). The font's own whole-font ascent/descent (`font.metrics()` -
    /// already scaled for this BLFont's own pixel size) bound any
    /// string's vertical extent correctly regardless of which glyphs it
    /// contains; `metrics.advance.x` (the shaped run's own total advance,
    /// not subject to the border-glyph bug above) bounds the horizontal
    /// extent. A small (+-1px) pad on every side covers AA bleed past the
    /// nominal glyph outline.
    inline CachedTextBitmap render_text_bitmap(const BLFont &font, const std::string &label, BLRgba32 color)
    {
        CachedTextBitmap result;
        if (label.empty())
            return result;

        BLGlyphBuffer gb;
        gb.set_utf8_text(label.c_str(), label.size());
        font.shape(gb);
        if (gb.glyph_run().size == 0)
            return result;

        BLTextMetrics metrics;
        font.get_text_metrics(gb, metrics);

        const BLFontMetrics &font_metrics = font.metrics();
        const double x0 = -1.0;
        const double y0 = -static_cast<double>(font_metrics.ascent) - 1.0;
        const double x1 = metrics.advance.x + 1.0;
        const double y1 = static_cast<double>(font_metrics.descent) + 1.0;
        const int width = std::max(1, static_cast<int>(std::ceil(x1 - x0)));
        const int height = std::max(1, static_cast<int>(std::ceil(y1 - y0)));

        result.image = BLImage(width, height, BL_FORMAT_PRGB32);
        result.offset = BLPoint(x0, y0);

        BLContext glyph_ctx(result.image);
        glyph_ctx.clear_all();
        glyph_ctx.set_fill_style(color);
        // Shifted so the bitmap's own (0, 0) lands at (x0, y0) relative
        // to the label's own baseline origin - the caller blits at
        // (device_origin + offset), reconstructing that same relationship
        // in device space.
        glyph_ctx.fill_glyph_run(BLPoint(-x0, -y0), font, gb.glyph_run());
        glyph_ctx.end();

        return result;
    }

    /// @brief Blend2D sibling of rasterize_stage.hpp's own `draw_view_shapes` -
    /// same per-layer/per-shape structure, same shapes_index-or-fallback
    /// viewport-culling dispatch, same path_outline_cache (Geometry::
    /// path_to_polygons' own std::vector<Polygon> result needs no
    /// backend-specific storage) - only the actual draw calls differ
    /// (BLContext instead of SkCanvas).
    ///
    /// Text (Shape.texts) draws the generic per-shape TERMINAL/ROUTE label
    /// case only (kLabelWidthRatio-scaled, clamped to
    /// [kMinLabelPixelSize, kMaxLabelPixelSize], centered at
    /// `text.location`, never truncated), via a per-call
    /// (label, font_key, color) rendered-glyph-bitmap cache
    /// (`render_text_bitmap`/`text_bitmap_cache` below) rather than a
    /// fresh vector shape+fill per occurrence - see that function's own
    /// doc comment for why. The placement-name label case
    /// (rasterize_stage.hpp's own `is_placement_name_layer`
    /// branch: index-paired with shape.rects, bottom-left-anchored,
    /// truncated to fit via truncate_text_to_width) is still a scoped-out
    /// gap here, not yet ported. Unlike Skia's UprightTextCanvas (which
    /// intercepts a *replayed* SkTextBlob's own CTM to discard any
    /// rotation/reflection while keeping the same scale magnitude - see
    /// that class's own doc comment), this backend never records/replays
    /// anything - one BLContext draws directly into one node's own BLImage,
    /// always under the exact same translate+scale+flip transform
    /// (RasterizeBlend2DStage::compute's own setup), no accumulated
    /// instance rotation ever baked in. So instead of decomposing/replacing
    /// the CTM per glyph run, this maps `text.location` through the
    /// context's own current `final_transform()` once to get its real
    /// device-pixel position, draws under a plain identity transform at
    /// that point (BLFont's own `pixel_size` interpreted directly as final
    /// on-screen pixels, no second multiplication by the ambient dbu-to-
    /// pixel scale), then restores - simpler than Skia's own mechanism
    /// precisely because there's no nested-hierarchy replay to defend
    /// against here.
    ///
    /// Blend2D has no Skia-style "stroke width 0 means always exactly 1
    /// device pixel" hairline convention - a sub-pixel-on-screen (or
    /// deliberately zero-width, Track/GCellGrid) Path instead gets an
    /// explicit `1.0 / scale` stroke width (a real, on-screen-1-pixel-ish
    /// line at the current zoom, computed the same way the Skia path's
    /// own dash-length-in-dbu compensation already does) - a deliberate,
    /// documented difference from the Skia backend, not a bug to chase
    /// parity on for a perf side-test.
    ///
    /// No opaque fast-path option (BL_COMP_OP_SRC_COPY instead of the
    /// default BL_COMP_OP_SRC_OVER for a fully-opaque color) - tried and
    /// removed (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md): measured zero
    /// benefit even after hoisting comp_op out of this per-shape loop,
    /// and even with its own color.a == 255 gate removed entirely, which
    /// only bought a real, confirmed correctness cost instead (SRC_COPY
    /// doesn't blend with the destination, it overwrites it outright -
    /// a translucent fill/stroke drawn that way stops showing whatever
    /// was drawn underneath it). Every draw below relies on BLContext's
    /// own documented default (BL_COMP_OP_SRC_OVER) rather than setting
    /// it explicitly.
    inline void draw_view_shapes_blend2d(
        BLContext &ctx, const ViewLayerShapes &shapes_by_layer, const ViewShapesIndexHandle &shapes_index, const Rect &query_bbox,
        const ViewLayerSet &view_layers, double scale,
        const std::unordered_map<std::string, bool> &layer_name_visible, const std::unordered_map<ViewLayerPurpose, bool> &purpose_visible,
        std::unordered_map<const Path *, std::vector<Polygon>> &path_outline_cache,
        std::unordered_map<int, BLFont> &font_cache,
        std::unordered_map<TextBitmapCacheKey, CachedTextBitmap, TextBitmapCacheKeyHash> &text_bitmap_cache)
    {
        // Sized BLFont instances are cheap to build but not free (real
        // per-call work inside Blend2D, not just a struct copy) - cached
        // here, keyed by rounded pixel size (clamped to
        // [kMinLabelPixelSize, kMaxLabelPixelSize], so this map's own key
        // space stays small regardless of zoom).
        //
        // BOTH `font_cache` and `text_bitmap_cache` are owned by the
        // CALLER (RasterizeBlend2DStage - its own `font_cache_`/
        // `text_bitmap_cache_` members) and passed in by reference here,
        // deliberately shared across every node this stage renders (and
        // across frames, for the lifetime of the stage) rather than
        // scoped to one call of this function. An earlier version of this
        // cache WAS per-call-local, on the (wrong) assumption that the
        // dominant repetition was *intra-node* - many placements of the
        // same library cell sharing one pin name within a single render.
        // Direct instrumentation against the real aes_scaling benchmark
        // fixture disproved that: HierarchyResolverStage renders each
        // distinct AbstractId's own content exactly ONCE regardless of
        // placement count (backend/CLAUDE.md's own HierarchyResolver
        // bullet - "a design placed N times is still resolved once"),
        // and every *repeat* placement instead reuses that one already-
        // rendered node's own image via ComposeStage's compositing - so
        // within any single call of THIS function, a given Terminal's own
        // pin name appears exactly once. A per-call-local cache measured
        // a 100% miss rate (83,000 draws, 83,000 misses) and was
        // strictly slower than no cache at all (extra shape/measure/
        // image-alloc/blit work on top of a fill that still only ever
        // happened once). The REAL repetition this backend can actually
        // exploit is *cross-node*: the same pin name (e.g. "A"/"Y"/"VDD")
        // recurring across many DIFFERENT standard-cell types drawn from
        // one small library, and the same font size/color recurring
        // across many different terminals/layers - which only becomes
        // visible once the cache's own lifetime spans more than one node.
        const BLFontFace &font_face = default_blend2d_font_face();

        for (const ViewLayerId &view_layer_id : view_layers.all())
        {
            const auto group_it = shapes_by_layer.find(view_layer_id);
            if (group_it == shapes_by_layer.end() || group_it->second.empty())
                continue;

            const ViewLayerData *layer = view_layers.get(view_layer_id);
            if (!layer)
                continue;
            if (!is_view_layer_visible(layer_name_visible, purpose_visible, layer->layer_name, layer->purpose))
                continue;

            const ViewLayerStyle &style = layer->style;
            const bool has_fill = style.fill_color.a > 0;
            // Every ViewLayerStyle this codebase actually constructs sets
            // a nonzero outline_color (view_style.hpp - layer_style()'s
            // own `base` always comes from a fully-opaque palette entry,
            // and every other hand-written style literal sets one too) -
            // has_outline is unconditionally true for every real style
            // today, unlike has_fill (which genuinely varies - ROW/
            // BOUNDARY/PLACEMENT_NAME/PLACEMENT_BOUNDARY/GCELLGRID/REGION
            // all have no fill at all). draw_one_shape below therefore
            // draws the outline/stroke unconditionally rather than
            // re-checking has_outline on every single shape the way it
            // still does for has_fill - that per-shape check never
            // actually skipped a draw call in practice (has_outline was
            // never false), so it was pure dead-branch overhead, not a
            // real optimization the way has_fill's own check still is.
            // Not enforced by the type system, but safe even if some
            // future style ever broke this: a stroke drawn with a fully
            // transparent color under normal alpha blending still paints
            // nothing, so an unexpected `outline_color.a == 0` would
            // degrade to one wasted (if incorrectly unconditional) draw
            // call per shape, not visibly wrong output.
            const bool has_outline = style.outline_color.a > 0;
            if (!has_fill && !has_outline)
                continue;

            const bool is_cross = style.fill_pattern == FillPattern::CROSS;

            const BLRgba32 fill_color = to_bl_color(style.fill_color);
            const BLRgba32 stroke_color = to_bl_color(style.outline_color);
            const bool has_fill_pattern = style.fill_pattern != FillPattern::NONE && style.fill_pattern != FillPattern::CROSS;
            const BLPattern fill_pattern = has_fill_pattern ? pattern_blend2d(style.fill_pattern, stroke_color) : BLPattern();

            auto set_fill = [&]
            {
                if (has_fill_pattern)
                    // BL_CONTEXT_STYLE_TRANSFORM_MODE_NONE - the pattern's
                    // own (identity, left unset) transform is absolute,
                    // NOT combined with the ambient BLContext transform
                    // (RasterizeBlend2DStage::compute's own translate+
                    // scale+flip setup) - the default USER mode instead
                    // composes a style's own transform WITH the current
                    // user transform, which would scale the tile by
                    // `scale` *again* on top of what the ambient CTM
                    // already does to every draw call, shrinking the
                    // effective tile to a fraction of a pixel at any real
                    // zoom (found by direct pixel inspection - every
                    // sampled pixel showed a uniform partial-alpha
                    // gradient with no fully-opaque or fully-transparent
                    // pixel anywhere, consistent with sampling deep inside
                    // one antialiased tile edge). NONE mode is the direct
                    // Blend2D analog of Skia's own pattern_shader/
                    // makeWithLocalMatrix scale-compensation
                    // (draw_view_shapes' own doc comment, rasterize_stage.hpp) -
                    // achieved here by *not* combining with the CTM at all,
                    // rather than manually inverting it.
                    ctx.set_fill_style(fill_pattern, BL_CONTEXT_STYLE_TRANSFORM_MODE_NONE);
                else
                    ctx.set_fill_style(fill_color);
            };
            auto set_stroke = [&]
            {
                ctx.set_stroke_style(stroke_color);
                // Blend2D has no Skia-style "stroke width 0 means always
                // exactly 1 device pixel" hairline convention (see this
                // function's own top-level doc comment) - a literal 0
                // renders nothing at all here, unlike Skia's SkPaint
                // (rasterize_stage.hpp never sets a stroke width either,
                // relying on that hairline default). 1.0 / scale is the
                // direct analog: a real, on-screen-~1-pixel-wide line at
                // the current zoom, overridden below only where a wider
                // width matters (kViaCrossStrokeWidth, or a Path's own
                // sub-pixel case).
                ctx.set_stroke_width(1.0 / scale);
                if (style.dashed)
                {
                    const double dash_length = 4.0 / scale;
                    BLArray<double> dash_array;
                    dash_array.append(dash_length);
                    dash_array.append(dash_length);
                    ctx.set_stroke_dash_array(dash_array);
                    ctx.set_stroke_dash_offset(0.0);
                }
                else
                {
                    ctx.set_stroke_dash_array(BLArray<double>());
                }
            };

            // set_fill()/set_stroke() are called (at most) once per LAYER
            // here, not once per shape - style/style.dashed/fill_pattern/
            // stroke_color/the comp_op choice are all ViewLayerStyle-level
            // values, identical for every shape a layer holds, so
            // re-deriving the same BLContext fill/stroke state on every
            // single shape (as this function used to do, inside
            // draw_one_shape below) was pure repeated work - real cost at
            // real shape counts (a fresh BLArray<double> heap allocation
            // per shape for the dash array alone, on every dashed layer,
            // even when nothing about the dash pattern ever changes
            // within it). BLContext keeps whatever fill/stroke style was
            // last set until something changes it again, so setting it
            // once up front and leaving it alone for every draw call in
            // this layer is correct, not just faster.
            //
            // The one real per-shape exception is the CROSS pattern (a
            // CUT-purpose TERMINAL's own "X" through the rect/polygon,
            // drawn via draw_cross_blend2d instead of a tiled fill) -
            // that function sets its own stroke_style/width internally
            // (kViaCrossStrokeWidth, not this layer's own 1.0/scale), so
            // draw_one_shape below still explicitly restores this layer's
            // own default stroke_width to 1.0/scale afterward before its
            // own ctx.stroke_rect/stroke_path call - stroke_style itself
            // never actually changes (draw_cross_blend2d's own `color`
            // argument is this same layer's stroke_color), so only width
            // needs restoring.
            //
            // The other exception is a Path whose own on-screen width is
            // sub-pixel (see draw_one_shape's own Path loop below): when
            // has_fill but not has_outline, that hairline fallback stroke
            // borrows the fill color as its own ink (there's no real
            // outline color to draw with) - a real, per-layer-constant
            // decision (has_fill/has_outline never vary by shape), so
            // it's applied once here too, not re-derived per Path.
            if (has_fill)
                set_fill();
            if (has_outline)
            {
                set_stroke();
            }
            else if (has_fill)
            {
                set_stroke();
                ctx.set_stroke_style(fill_color);
            }

            auto draw_one_shape = [&](const Shape &shape)
            {
                // Tracks whether ANY of this shape's own rects/polygons/paths
                // actually survived their own sub-pixel cull below - if none
                // did, this shape's own text (drawn further down) is skipped
                // too, so a fully-culled shape really does render "no dot,
                // no outline, nothing" (ApiFixture.
                // SubPixelShapeIsNotRenderedAndIsNotSelectable, api_test.cpp) -
                // not a label floating at kMinLabelPixelSize with no visible
                // geometry backing it. A deliberate reversal of this
                // function's own earlier "text is unaffected by sub-pixel
                // culling" stance (bbox_is_sub_pixel's own doc comment,
                // draw_helpers.hpp) once text drawing actually existed to
                // expose the conflict - a real, live shape (e.g. a small
                // pin) rendering an unrelated 10px label with nothing to
                // anchor it to reads as a rendering bug, not a feature.
                bool any_geometry_drawn = false;

                for (const Rect &r : shape.rects)
                {
                    if (bbox_is_sub_pixel(r.ur.x - r.ll.x, r.ur.y - r.ll.y, scale))
                        continue;
                    any_geometry_drawn = true;
                    const BLRect rect(static_cast<double>(r.ll.x), static_cast<double>(r.ll.y),
                                      static_cast<double>(r.ur.x - r.ll.x), static_cast<double>(r.ur.y - r.ll.y));
                    if (is_cross)
                    {
                        // kViaCrossStrokeWidth is a fixed on-screen pixel
                        // width - divide by scale to counter the ambient
                        // dbu-to-pixel scale (see that constant's own doc
                        // comment, draw_helpers.hpp).
                        draw_cross_blend2d(ctx, BLBox(rect.x, rect.y, rect.x + rect.w, rect.y + rect.h), stroke_color, kViaCrossStrokeWidth / scale);
                        ctx.set_stroke_width(1.0 / scale);
                        ctx.stroke_rect(rect);
                        continue;
                    }
                    if (has_fill)
                        ctx.fill_rect(rect);
                    ctx.stroke_rect(rect);
                }

                for (const Polygon &poly : shape.polygons)
                {
                    if (polygon_is_sub_pixel(poly, scale))
                        continue;
                    any_geometry_drawn = true;
                    const BLPath path = to_bl_path(poly, /*close=*/true);
                    if (is_cross)
                    {
                        BLBox bounds;
                        path.get_bounding_box(&bounds);
                        draw_cross_blend2d(ctx, bounds, stroke_color, kViaCrossStrokeWidth / scale);
                        ctx.set_stroke_width(1.0 / scale);
                        ctx.stroke_path(path);
                        continue;
                    }
                    if (has_fill)
                        ctx.fill_path(path);
                    ctx.stroke_path(path);
                }

                for (const Path &p : shape.paths)
                {
                    // p.width == 0 (exact - a dbu integer, not a float
                    // comparison to worry about) is a deliberately
                    // zero-width synthetic line (TRACK/GCellGrid's own
                    // convention, this function's own top-level doc
                    // comment) - not a real route that just happens to be
                    // thin at this zoom, and always sub-pixel by
                    // construction regardless of scale, so it must keep
                    // drawing as a hairline unconditionally rather than
                    // ever being dropped, or TRACK/GCellGrid would vanish
                    // at every zoom level, not just when zoomed out.
                    if (p.width == 0)
                    {
                        // Stroke state (including the fill-color-as-ink
                        // substitution when this layer has no real
                        // outline) was already established once above -
                        // see this function's own comment there.
                        any_geometry_drawn = true;
                        ctx.stroke_path(to_bl_path(p.polygon, /*close=*/false));
                        continue;
                    }
                    if (p.width * scale < 1.0)
                    {
                        // A real routed wire, just too thin to draw a
                        // visible pixel of at this zoom - dropped
                        // entirely rather than drawn as a faint hairline,
                        // same "not worth the draw call" reasoning
                        // bbox_is_sub_pixel/polygon_is_sub_pixel already
                        // apply to Rect/Polygon geometry (draw_helpers.hpp).
                        continue;
                    }
                    any_geometry_drawn = true;

                    auto outline_it = path_outline_cache.find(&p);
                    if (outline_it == path_outline_cache.end())
                        outline_it = path_outline_cache.emplace(&p, Geometry::path_to_polygons(p)).first;
                    for (const Polygon &outline : outline_it->second)
                    {
                        const BLPath outline_path = to_bl_path(outline, /*close=*/true);
                        if (has_fill)
                            ctx.fill_path(outline_path);
                        ctx.stroke_path(outline_path);
                    }
                    ctx.stroke_path(to_bl_path(p.polygon, /*close=*/false));
                }

                if (!any_geometry_drawn || !font_face.is_valid())
                    return; // no visible geometry to attach a label to (or no usable font) - draw nothing

                for (const Text &text : shape.texts)
                {
                    // Clamped at both ends: kMinLabelPixelSize keeps a
                    // label legible when its own local geometry is tiny;
                    // kMaxLabelPixelSize keeps it from growing without
                    // bound when zoomed in close (a label only needs to
                    // stay readable, not track the geometry's own on-screen
                    // size 1:1) - and, since this backend renders each
                    // distinct (label, font_key, color) into a cached
                    // bitmap below, also bounds how many distinct sizes
                    // that cache (and font_cache) ever needs to hold and
                    // how large any one cached bitmap - and so any one
                    // blit - ever gets, regardless of how far a user zooms
                    // in (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md).
                    const double pixel_size = std::clamp(text.size * scale * kLabelWidthRatio, kMinLabelPixelSize, kMaxLabelPixelSize);
                    const int font_key = std::max(1, static_cast<int>(std::lround(pixel_size)));
                    auto font_it = font_cache.find(font_key);
                    if (font_it == font_cache.end())
                    {
                        BLFont font;
                        font.create_from_face(font_face, static_cast<float>(font_key));
                        font_it = font_cache.emplace(font_key, std::move(font)).first;
                    }

                    // render_text_bitmap's own doc comment has the full
                    // rationale - shaping+filling text as vector paths on
                    // every call (fill_utf8_text, this loop's own earlier
                    // form) is real, measured per-call cost with no
                    // built-in Blend2D glyph cache to amortize it; a
                    // cached bitmap turns every repeat of the same label/
                    // size/color (the common case - Text::label is a
                    // Terminal's own pin name, shared verbatim across
                    // every instance of one library cell) into a cheap
                    // blit instead.
                    const TextBitmapCacheKey bitmap_key{text.label, font_key, stroke_color.value};
                    auto bitmap_it = text_bitmap_cache.find(bitmap_key);
                    if (bitmap_it == text_bitmap_cache.end())
                        bitmap_it = text_bitmap_cache.emplace(bitmap_key, render_text_bitmap(font_it->second, text.label, stroke_color)).first;

                    const CachedTextBitmap &bitmap = bitmap_it->second;
                    if (bitmap.image.width() == 0 || bitmap.image.height() == 0)
                        continue; // nothing to draw (e.g. an empty label)

                    // Maps this shape's own dbu-space label origin through
                    // the context's current (translate+scale+flip) transform
                    // once, to a real device-pixel point, then blits under a
                    // plain identity transform at that point - see this
                    // function's own top-level doc comment for why that's
                    // sufficient here (no accumulated instance rotation to
                    // defend against, unlike Skia's UprightTextCanvas).
                    const BLPoint device_origin = ctx.final_transform().map_point(text.location.x, text.location.y);
                    ctx.save();
                    ctx.set_transform(BLMatrix2D::make_identity());
                    ctx.blit_image(BLPoint(device_origin.x + bitmap.offset.x, device_origin.y + bitmap.offset.y), bitmap.image);
                    ctx.restore();
                }
            };

            const std::vector<Shape> &shapes = group_it->second;
            const auto layer_index_it = shapes_index ? shapes_index->find(view_layer_id) : ViewLayerShapeIndex::const_iterator{};
            if (shapes_index && layer_index_it != shapes_index->end())
            {
                std::vector<ShapeIndexEntry> hits;
                layer_index_it->second.query(bgi::intersects(query_bbox), std::back_inserter(hits));
                for (const ShapeIndexEntry &hit : hits)
                    draw_one_shape(shapes[hit.second]);
            }
            else
            {
                for (const Shape &shape : shapes)
                    draw_one_shape(shape);
            }
        }
    }

    /// @brief Blend2D-backed sibling of RasterizeStage (rasterize_stage.hpp)
    /// - a side experiment (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) trying
    /// Blend2D's own JIT-compiled rasterization pipeline, multi-threaded
    /// tiled rendering, and opaque fast-path composition operator against
    /// Skia's CPU rasterizer for this project's own "millions of small
    /// shapes" workload. Same MemoizingStage template shape as
    /// RasterizeStage (identical InputData/OutputData/PipelineOptions -
    /// RasterizeOutput, rasterize_output.hpp - so both share one
    /// ViewRenderPipelineImpl<RasterizeStageT>, view_render_pipeline.hpp),
    /// same per-node NodePathOutlineCache pattern (its own, private,
    /// separate cache instance - not shared with RasterizeStage's), same
    /// options_did_change. Generic per-shape text is drawn (see
    /// draw_view_shapes_blend2d's own doc comment); placement-name labels
    /// are not yet - still not a byte-for-byte feature match with
    /// RasterizeStage, a smaller scoped gap than before.
    ///
    /// `thread_count_` is a plain mutable setting (set_thread_count
    /// below), not a constructor parameter - keeps this class's own
    /// constructor signature identical to RasterizeStage's
    /// (`(graph, label)`), which is what lets ViewRenderPipelineImpl's
    /// single `RasterizeStageT rasterize_(graph_, label + ".Rasterize")`
    /// construction site work unchanged for either backend; a benchmark
    /// wanting a specific thread count calls the setter once after
    /// construction instead.
    class RasterizeBlend2DStage : public MemoizingStage<HierarchyResolverStage::OutputHandle, RasterizeOutput, ViewRenderOptions>
    {
    public:
        explicit RasterizeBlend2DStage(oneapi::tbb::flow::graph &g, std::string label = "RasterizeBlend2D")
            : MemoizingStage(g, std::move(label)) {}

        /// @brief 0 (the default) means synchronous, single-threaded
        /// rendering (Blend2D's own BLContextCreateInfo::thread_count
        /// convention - "no defaults or heuristics ... users must
        /// carefully use a value that makes sense for their workloads",
        /// Blend2D's own multithreaded-rendering docs) - 2 to 4 is that
        /// same documentation's own general guidance for most workloads.
        void set_thread_count(uint32_t thread_count) { thread_count_ = thread_count; }

    protected:
        RasterizeOutput compute(const HierarchyResolverStage::OutputHandle &culled, const ViewRenderOptions &options) override
        {
            RasterizeOutput result;
            result.culled = culled;
            if (culled == nullptr || options.root == nullptr)
                return result;

            static const ViewLayerSet kEmptyViewLayers;
            const ViewLayerSet &view_layers = culled->view_layers != nullptr ? *culled->view_layers : kEmptyViewLayers;
            static const ViewLayerShapes kEmptyShapes;

            constexpr int kMaxDimensionPx = 8192;

            for (const auto &[id, data] : culled->view_data)
            {
                const Rect local_bbox = (id == options.top_level) ? options.viewport : node_local_bbox(*options.root, id);

                const double width_dbu = static_cast<double>(local_bbox.ur.x - local_bbox.ll.x);
                const double height_dbu = static_cast<double>(local_bbox.ur.y - local_bbox.ll.y);
                const int pixel_width = std::clamp(static_cast<int>(std::ceil(width_dbu * options.scale)), 1, kMaxDimensionPx);
                const int pixel_height = std::clamp(static_cast<int>(std::ceil(height_dbu * options.scale)), 1, kMaxDimensionPx);

                BLImage image(pixel_width, pixel_height, BL_FORMAT_PRGB32);

                BLContextCreateInfo create_info{};
                create_info.thread_count = thread_count_;
                BLContext ctx(image, create_info);
                ctx.clear_all();

                // Same translate+scale+flip setup as RasterizeStage's own
                // canvas (rasterize_stage.hpp) - dbu y increases upward,
                // pixel y increases downward.
                ctx.translate(0.0, static_cast<double>(pixel_height));
                ctx.scale(options.scale, -options.scale);
                ctx.translate(static_cast<double>(-local_bbox.ll.x), static_cast<double>(-local_bbox.ll.y));

                NodePathOutlineCache &node_outline_cache = path_outline_cache_by_node_[id];
                if (node_outline_cache.source != data.shapes)
                {
                    node_outline_cache.outlines.clear();
                    node_outline_cache.source = data.shapes;
                }

                draw_view_shapes_blend2d(
                    ctx, data.shapes ? *data.shapes : kEmptyShapes, data.shapes_index, local_bbox, view_layers, options.scale,
                    options.layer_name_visible, options.purpose_visible, node_outline_cache.outlines,
                    font_cache_, text_bitmap_cache_);

                ctx.end();

                // Wrap Blend2D's own raw pixel buffer into an sk_sp<SkImage>
                // so ComposeStage (Skia-based) needs zero changes regardless
                // of which backend rasterized a given node - rasterize_output.hpp's
                // own RasterizedImage/RasterizeOutput comment. BL_FORMAT_PRGB32
                // is premultiplied-ARGB32 (Blend2D's own doc comment,
                // format.h: "Format_ARGB32_Premultiplied"/"CAIRO_FORMAT_ARGB32"),
                // which is B,G,R,A byte order in memory on this little-endian
                // target - kBGRA_8888_SkColorType describes that layout
                // directly rather than needing a manual channel-swap copy;
                // Skia's own compositor (ComposeStage's drawImage calls)
                // converts as needed during compositing, for free.
                BLImageData image_data;
                image.get_data(&image_data);
                const SkImageInfo sk_info = SkImageInfo::Make(pixel_width, pixel_height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
                const SkPixmap pixmap(sk_info, image_data.pixel_data, static_cast<size_t>(image_data.stride));
                sk_sp<SkImage> sk_image = SkImages::RasterFromPixmapCopy(pixmap);

                result.images.emplace(id, RasterizedImage{std::move(sk_image), local_bbox.ll});
            }

            return result;
        }

        bool options_did_change(const ViewRenderOptions &last, const ViewRenderOptions &current) const override
        {
            return last.top_level != current.top_level ||
                   last.scale != current.scale ||
                   last.viewport.ll.x != current.viewport.ll.x ||
                   last.viewport.ll.y != current.viewport.ll.y ||
                   last.viewport.ur.x != current.viewport.ur.x ||
                   last.viewport.ur.y != current.viewport.ur.y ||
                   last.antialiasing_enabled != current.antialiasing_enabled ||
                   last.layer_name_visible != current.layer_name_visible ||
                   last.purpose_visible != current.purpose_visible;
        }

    private:
        struct NodePathOutlineCache
        {
            ViewShapesHandle source;
            std::unordered_map<const Path *, std::vector<Polygon>> outlines;
        };
        std::unordered_map<HierarchyId, NodePathOutlineCache, HierarchyIdHash> path_outline_cache_by_node_;

        // Shared across every node this stage renders, and across every
        // frame for this stage's own lifetime - see
        // draw_view_shapes_blend2d's own doc comment for why this needs
        // to be stage-wide (not per-node/per-call) to actually hit: the
        // real repetition is the same pin name/font size/color recurring
        // across many DIFFERENT nodes (distinct standard-cell Abstracts
        // drawn from one small library), not within any single one.
        // Unlike `path_outline_cache_by_node_` above, entries here never
        // need invalidating - a (label, font_key, color) triple's own
        // rendered ink never changes - so this only ever grows, bounded
        // by the real number of distinct such triples a design's own
        // content contains (typically small - hundreds to low thousands,
        // each entry a few KB at most).
        std::unordered_map<int, BLFont> font_cache_;
        std::unordered_map<TextBitmapCacheKey, CachedTextBitmap, TextBitmapCacheKeyHash> text_bitmap_cache_;

        uint32_t thread_count_ = 8;

        static Rect node_local_bbox(const Root &root, const HierarchyId &id)
        {
            if (const LayoutId *layout_id = std::get_if<LayoutId>(&id))
                return layout_declared_bbox(root, *layout_id);
            return abstract_declared_bbox(root, std::get<AbstractId>(id));
        }
    };
}
