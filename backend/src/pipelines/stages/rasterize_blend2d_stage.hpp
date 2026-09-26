#pragma once

#include "../../core/placement_geometry.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../blend2d_font.hpp"
#include "../draw_helpers.hpp"
#include "../pipeline_options.hpp"
#include "../port_markers.hpp"
#include "../rasterize_output.hpp"
#include "../render_shape.hpp"
#include "../tbb_core.hpp"
#include "hierarchy_resolver_stage.hpp"

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

    /// @brief Draws the background dot grid (major/minor tiers) plus
    /// solid axis lines at dbu (x=0)/(y=0) - UPDATES.md 5.1, ported from
    /// pipelines.old/draw_helpers.hpp's own `draw_grid`. Only ever called
    /// for `id == options.top_level` (`RasterizeBlend2DStage::compute()`'s
    /// own call site) - drawing this per-node/per-placement too would
    /// bake a misaligned, independently-scaled grid into every nested
    /// child image, which then composites incorrectly once `ComposeStage`
    /// blits it into its parent; the pre-restart version had the same
    /// "top_level only" scope for the same reason (`BuildDesignPictureStage`,
    /// never `BuildLayoutPictureStage`/an instance's own picture).
    ///
    /// Unlike the pre-restart version - recorded into an already-pixel-
    /// space picture with no ambient transform of its own, so it needed
    /// manual dbu-to-pixel math throughout - `ctx` here already has a
    /// live dbu-to-pixel transform active (the same translate+scale+flip
    /// `compute()` sets up before any real geometry draws), so every dot/
    /// line below is drawn directly in dbu coordinates and left to that
    /// transform; every *fixed on-screen size* (dot radius, axis-line
    /// width isn't fixed but colors/style are unaffected either way) is
    /// divided by `scale` first, the same "1.0 / scale" convention this
    /// module's own `kViaCrossStrokeWidth` already uses, so it still
    /// renders at a constant pixel size regardless of zoom.
    ///
    /// `visible_dbu` is the exact dbu-space rectangle `ctx`'s own image
    /// covers (`options.viewport` for the top-level case) - unlike the
    /// pre-restart version, which had to reconstruct this from
    /// `pan`/`scale`/`viewport_width_px`/`viewport_height_px`, this stage
    /// already has it on hand as `local_bbox`.
    inline void draw_grid_blend2d(BLContext &ctx, const Rect &visible_dbu, double scale, int64_t minor_spacing, int64_t major_spacing)
    {
        if (scale <= 0.0)
            return;

        ctx.set_stroke_style(to_bl_color(kAxisLineColor));
        if (visible_dbu.ll.x <= 0 && visible_dbu.ur.x >= 0)
            ctx.stroke_line(BLLine(0.0, static_cast<double>(visible_dbu.ll.y), 0.0, static_cast<double>(visible_dbu.ur.y)));
        if (visible_dbu.ll.y <= 0 && visible_dbu.ur.y >= 0)
            ctx.stroke_line(BLLine(static_cast<double>(visible_dbu.ll.x), 0.0, static_cast<double>(visible_dbu.ur.x), 0.0));

        if (minor_spacing <= 0 || major_spacing <= 0)
            return;

        // The first grid line at or above `min_value` on a lattice spaced
        // `spacing` apart - std::ceil handles a negative min_value
        // correctly too.
        auto first_line = [](int64_t min_value, int64_t spacing)
        { return spacing * static_cast<int64_t>(std::ceil(static_cast<double>(min_value) / static_cast<double>(spacing))); };

        const double dot_radius = kGridDotRadius / scale;
        const bool minor_visible = static_cast<double>(minor_spacing) * scale >= kMinGridDotPixelSpacing;

        if (minor_visible)
        {
            ctx.set_fill_style(to_bl_color(kMinorGridColor));
            for (int64_t x = first_line(visible_dbu.ll.x, minor_spacing); x <= visible_dbu.ur.x; x += minor_spacing)
                for (int64_t y = first_line(visible_dbu.ll.y, minor_spacing); y <= visible_dbu.ur.y; y += minor_spacing)
                {
                    if (x % major_spacing == 0 && y % major_spacing == 0)
                        continue; // drawn as a major dot below instead
                    ctx.fill_circle(BLCircle(static_cast<double>(x), static_cast<double>(y), dot_radius));
                }
        }

        if (static_cast<double>(major_spacing) * scale >= kMinGridDotPixelSpacing)
        {
            // Once zoomed out far enough that the minor tier itself is
            // hidden, the major dots are the only grid left on screen -
            // drawing them in the bolder kMajorGridColor at that point
            // would visually claim there's still a finer tier being
            // contrasted against, when there isn't; kMinorGridColor reads
            // as "the finest grid currently visible" instead.
            ctx.set_fill_style(to_bl_color(minor_visible ? kMajorGridColor : kMinorGridColor));
            for (int64_t x = first_line(visible_dbu.ll.x, major_spacing); x <= visible_dbu.ur.x; x += major_spacing)
                for (int64_t y = first_line(visible_dbu.ll.y, major_spacing); y <= visible_dbu.ur.y; y += major_spacing)
                    ctx.fill_circle(BLCircle(static_cast<double>(x), static_cast<double>(y), dot_radius));
        }
    }

    /// @brief Draws a fixed on-screen-size "+" cross at the Abstract's own
    /// origin point (UPDATES.md 5.4) - not necessarily dbu (0,0); an
    /// Abstract's origin is wherever its own LEF `ORIGIN` statement placed
    /// it (`AbstractData::origin`). Fixed size regardless of `scale`, same
    /// "marks a reference point, not geometry that should grow with zoom"
    /// rationale as `kCursorBoxSizePx` - both the stroke width and the
    /// marker's own half-size are divided by `scale` before drawing
    /// through the ambient transform, same convention `draw_grid_blend2d`
    /// above uses for its own dot radius.
    inline void draw_origin_marker_blend2d(BLContext &ctx, Point origin_dbu, double scale)
    {
        if (scale <= 0.0)
            return;

        const double cx = static_cast<double>(origin_dbu.x);
        const double cy = static_cast<double>(origin_dbu.y);
        const double half = kOriginMarkerSizePx / scale;

        ctx.set_stroke_style(to_bl_color(kOriginMarkerColor));
        ctx.set_stroke_width(kOriginMarkerStrokeWidth / scale);
        ctx.stroke_line(BLLine(cx - half, cy, cx + half, cy));
        ctx.stroke_line(BLLine(cx, cy - half, cx, cy + half));
    }

    /// @brief Cache key for `render_glyph_bitmap` below - a single ASCII
    /// character's own rendered ink depends only on which character it is,
    /// its own on-screen pixel size (`font_key` - the same already-rounded,
    /// already-[kMinLabelPixelSize, kMaxLabelPixelSize]-clamped size
    /// draw_view_shapes_blend2d's text loop computes per label), and its
    /// fill color (a ViewLayer's own outline_color, constant for every
    /// shape on that layer - see draw_view_shapes_blend2d's own per-layer
    /// setup). Rendering (and caching) each glyph directly at its own
    /// real target size, rather than always at one fixed reference size
    /// and scaling the blit to fit, was tried and measured slower overall
    /// (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) - Blend2D's scaled
    /// `blit_image` overload resamples every destination pixel, real
    /// per-blit cost that a plain 1:1 point blit doesn't pay; rendering
    /// once per distinct size instead keeps every blit unscaled, at the
    /// cost of a bigger (but still small and fixed) cache key space -
    /// `font_key` only ever takes one of ~15 distinct clamped-and-rounded
    /// values, so this stays just as bounded as a size-independent key,
    /// only wider by that same small constant factor.
    ///
    /// A per-CHARACTER cache, not a per-STRING one (an earlier version of
    /// this cache, PIPELINE_REFACTOR_BENCHMARK_RESULTS.md - see that
    /// entry's own postmortem): a whole-string cache's own key space
    /// grows with the number of distinct STRINGS a design contains, which
    /// is unbounded for content like a Placement's own name (unique per
    /// instance, unlike a Terminal's own pin name, which repeats heavily
    /// across instances of one library cell) - confirmed as a real,
    /// reported OOM (continuous zooming with hierarchy_depth=0, so every
    /// visible label is a placement name) that a whole-string cache
    /// cannot ever bound, no matter how the rest of its lifetime is
    /// scoped. A per-character cache is bounded by the size of the
    /// alphabet actually used (printable ASCII, well under 128) times the
    /// number of distinct font sizes (~15, clamped) times the number of
    /// distinct colors (small, one per physical layer) - regardless of
    /// how many distinct strings, or how unique each one is, a design's
    /// own content ever contains.
    struct GlyphBitmapCacheKey
    {
        char ch;
        int font_key;
        uint32_t color;

        bool operator==(const GlyphBitmapCacheKey &other) const noexcept
        {
            return ch == other.ch && font_key == other.font_key && color == other.color;
        }
    };

    struct GlyphBitmapCacheKeyHash
    {
        std::size_t operator()(const GlyphBitmapCacheKey &key) const noexcept
        {
            std::size_t h = std::hash<char>()(key.ch);
            h = h * 31 + static_cast<std::size_t>(key.font_key);
            h = h * 31 + static_cast<std::size_t>(key.color);
            return h;
        }
    };

    struct CachedGlyphBitmap
    {
        // Default-constructed (0x0) means "nothing to draw" (whitespace,
        // or any other glyph with no ink) - the caller checks
        // width()/height() rather than treating every cache entry as
        // real ink.
        BLImage image;
        // Offset, in the reference font's own pixel space (kMaxLabelPixelSize),
        // from this character's own pen origin (baseline, left edge of
        // its own monospace cell) to the image's own top-left corner.
        BLPoint offset;
    };

    /// @brief Renders one ASCII character in `font` (sized for one of the
    /// ~15 distinct clamped/rounded pixel sizes a label can resolve to -
    /// GlyphBitmapCacheKey's own doc comment for why size IS part of this
    /// cache's key, unlike an earlier version of this function) once into
    /// a small, tightly-sized BLImage - the direct analog of Skia's own
    /// internal glyph-bitmap cache, which Blend2D's `fill_utf8_text` has
    /// no equivalent of (it shapes and fills each glyph run as vector
    /// paths on every single call - measured at ~0.84us/call for a
    /// 2-character label, PIPELINE_REFACTOR_BENCHMARK_RESULTS.md).
    ///
    /// Uses `BLFont::get_glyph_outlines` directly on a single mapped
    /// glyph ID rather than `fill_glyph_run` on a shaped multi-glyph run
    /// (this function's own earlier, whole-string form) - simpler AND
    /// correct here specifically because `font` is assumed monospace
    /// (`RasterizeBlend2DStage`'s own `default_blend2d_font_face()`,
    /// DejaVu Sans Mono): a monospace font's own glyph *shapes* still
    /// need real per-glyph outlines (this function), but its *layout* is
    /// just "one fixed-width cell per character" (draw_view_shapes_blend2d's
    /// own text loop), with no need to reproduce Blend2D's own shaped-run
    /// positioning (kerning, combining marks, etc.) that a proportional
    /// font would require to lay out correctly.
    inline CachedGlyphBitmap render_glyph_bitmap(const BLFont &font, char ch, BLRgba32 color)
    {
        CachedGlyphBitmap result;

        BLGlyphBuffer gb;
        const char text[1] = {ch};
        gb.set_utf8_text(text, 1);
        font.shape(gb);
        if (gb.size() == 0)
            return result; // shouldn't happen for a real ASCII byte, but degrade to blank rather than crash

        BLPath path;
        font.get_glyph_outlines(gb.content()[0], path);

        BLBox bounds;
        path.get_bounding_box(&bounds);
        if (!(bounds.x0 < bounds.x1) || !(bounds.y0 < bounds.y1))
            return result; // no ink (space, or any other empty glyph)

        const double x0 = bounds.x0 - 1.0;
        const double y0 = bounds.y0 - 1.0;
        const double x1 = bounds.x1 + 1.0;
        const double y1 = bounds.y1 + 1.0;
        const int width = std::max(1, static_cast<int>(std::ceil(x1 - x0)));
        const int height = std::max(1, static_cast<int>(std::ceil(y1 - y0)));

        result.image = BLImage(width, height, BL_FORMAT_PRGB32);
        result.offset = BLPoint(x0, y0);

        BLContext glyph_ctx(result.image);
        glyph_ctx.clear_all();
        glyph_ctx.set_fill_style(color);
        glyph_ctx.translate(-x0, -y0);
        glyph_ctx.fill_path(path);
        glyph_ctx.end();

        return result;
    }

    /// @brief One entry of `RasterizeBlend2DStage::monospace_font_cache_`
    /// below - a BLFont built at one specific clamped/rounded pixel size
    /// (`font_key`), plus that size's own fixed monospace cell width
    /// (measured once, via a single representative character - a
    /// monospace font's own advance is identical for every printable
    /// character by construction, so any one character's own measured
    /// advance is the whole font's own fixed cell width at this size).
    struct MonospaceFontEntry
    {
        BLFont font;
        double cell_width = 0.0;
    };

    /// @brief Truncates `text` to fit within `max_width_px` when rendered
    /// in a monospace font whose own fixed per-character advance is
    /// `cell_width`: monospace means "how many characters fit" is just
    /// `floor(max_width_px / cell_width)`, no per-candidate
    /// `measureText`-style search needed.
    ///
    /// Truncates from the BEGINNING (keeps the label's own trailing
    /// characters) and prepends "..." - per explicit direction: a real
    /// instance name's own most identifying part (e.g. a numeric suffix or leaf
    /// cell name in a hierarchical path like "top/sub_block/cell_042") is
    /// usually at the end, so keeping the tail and dropping the head
    /// preserves more of what a user actually needs to read.
    inline std::string truncate_monospace_label(const std::string &text, double cell_width, double max_width_px)
    {
        if (cell_width <= 0.0)
            return text; // no usable font - caller already checked cell_width > 0 before drawing anything, defensive only

        const int max_chars = static_cast<int>(std::floor(max_width_px / cell_width));
        if (static_cast<int>(text.size()) <= max_chars)
            return text;

        static const std::string kEllipsis = "...";
        if (max_chars <= static_cast<int>(kEllipsis.size()))
            return ""; // not even room for "..." - same degrade as truncate_text_to_width's own equivalent case

        const std::size_t keep = static_cast<std::size_t>(max_chars) - kEllipsis.size();
        return kEllipsis + text.substr(text.size() - keep);
    }

    /// @brief Draws `label` one monospace character at a time, starting
    /// at `device_origin` (already mapped into device/pixel space - the
    /// caller's own job, draw_view_shapes_blend2d's text loop below).
    /// `font_entry` is looked up/built by the caller, keyed by the same
    /// `font_key` `glyph_bitmap_cache` is also keyed by (RasterizeBlend2DStage's
    /// own `monospace_font_cache_`) - every glyph this call draws is
    /// rendered directly at its own real on-screen size (via
    /// `render_glyph_bitmap`/`glyph_bitmap_cache` - GlyphBitmapCacheKey's
    /// own doc comment has the full "why per-character, not per-string,
    /// and why per-size" rationale), so every blit below is a plain 1:1
    /// point blit, never a resampled/scaled one.
    ///
    /// Monospace-only: `pen_x` simply advances by one fixed
    /// `font_entry.cell_width` per character - correct layout for a
    /// monospace font (RasterizeBlend2DStage's own
    /// `default_blend2d_font_face()`, DejaVu Sans Mono) specifically
    /// because every glyph shares the same advance width by construction;
    /// a proportional font would need each character's own real advance
    /// (Blend2D's own shaped-run positioning, `BLGlyphRun::placement_data`)
    /// instead, which this function deliberately does not attempt -
    /// simplicity was chosen over pixel-parity with Skia's own
    /// proportional-font rendering for this backend, per explicit
    /// direction, not an oversight.
    inline void draw_monospace_label_blend2d(
        BLContext &ctx, const MonospaceFontEntry &font_entry, int font_key,
        std::unordered_map<GlyphBitmapCacheKey, CachedGlyphBitmap, GlyphBitmapCacheKeyHash> &glyph_bitmap_cache,
        const std::string &label, BLRgba32 color, const BLPoint &device_origin)
    {
        if (label.empty() || !font_entry.font.is_valid() || font_entry.cell_width <= 0.0)
            return;

        ctx.save();
        ctx.set_transform(BLMatrix2D::make_identity());
        double pen_x = device_origin.x;
        for (unsigned char raw_ch : label)
        {
            const char ch = static_cast<char>(raw_ch);
            const GlyphBitmapCacheKey key{ch, font_key, color.value};
            auto it = glyph_bitmap_cache.find(key);
            if (it == glyph_bitmap_cache.end())
                it = glyph_bitmap_cache.emplace(key, render_glyph_bitmap(font_entry.font, ch, color)).first;

            const CachedGlyphBitmap &glyph = it->second;
            if (glyph.image.width() > 0 && glyph.image.height() > 0)
                ctx.blit_image(BLPoint(pen_x + glyph.offset.x, device_origin.y + glyph.offset.y), glyph.image);
            pen_x += font_entry.cell_width;
        }
        ctx.restore();
    }

    /// @brief Draws one node's own direct shapes - per-layer/per-shape
    /// structure, shapes_index-or-fallback viewport-culling dispatch, and
    /// path_outline_cache (Geometry::path_to_polygons' own
    /// std::vector<Polygon> result) via BLContext draw calls.
    ///
    /// Text (Shape.texts) draws two cases: the generic per-shape
    /// TERMINAL/ROUTE label (kLabelWidthRatio-scaled, clamped to
    /// [kMinLabelPixelSize, kMaxLabelPixelSize], centered at
    /// `text.location`, never truncated), and the placement-name label
    /// (index-paired with shape.rects, bottom-left-anchored, truncated to
    /// fit via `truncate_monospace_label` - that function's own doc
    /// comment has the full rationale).
    ///
    /// Every label is drawn one monospace character at a time
    /// (`draw_monospace_label_blend2d` below) via a per-CHARACTER
    /// rendered-glyph-bitmap cache (`render_glyph_bitmap`/
    /// `GlyphBitmapCacheKey`'s own doc comment has the full rationale -
    /// short version: a per-STRING cache's own key space is unbounded for
    /// content like a Placement's own name, which is unique per instance;
    /// a per-CHARACTER cache is bounded by the alphabet actually used
    /// regardless of how many distinct/unique strings a design contains)
    /// rather than a fresh vector shape+fill per occurrence. This maps
    /// `text.location` through the context's own current
    /// `final_transform()` once to get its real device-pixel position,
    /// draws under a plain identity transform at that point (a cached
    /// glyph's own bitmap already being sized in real device pixels, no
    /// further multiplication by the ambient dbu-to-pixel scale), then
    /// restores - correct once this context is confirmed to never carry a
    /// rotation component within a single node's own render pass - see
    /// compose_stage.hpp's own doc comment for the cross-node rotation/
    /// flip case this doesn't handle.
    ///
    /// Blend2D has no Skia-style "stroke width 0 means always exactly 1
    /// device pixel" hairline convention - a sub-pixel-on-screen (or
    /// deliberately zero-width, Track/GCellGrid) Path instead gets an
    /// explicit `1.0 / scale` stroke width, a real, on-screen-1-pixel-ish
    /// line at the current zoom.
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
        std::unordered_map<int, MonospaceFontEntry> &monospace_font_cache,
        std::unordered_map<GlyphBitmapCacheKey, CachedGlyphBitmap, GlyphBitmapCacheKeyHash> &glyph_bitmap_cache,
        double requested_min_label_px = kMinLabelPixelSize, double max_label_px = kMaxLabelPixelSize)
    {
        // The Settings panel's min/max label font sizes (NEW_FEATURES_SEPT_2026.md
        // item 9, ViewRenderOptions::label_min_size_px/label_max_size_px):
        // every label is clamped to [min, max]; a min set above the max
        // yields to it.
        const double min_label_px = std::min(requested_min_label_px, max_label_px);

        // `monospace_font_cache` and `glyph_bitmap_cache` are owned by the
        // CALLER (RasterizeBlend2DStage - its own `monospace_font_cache_`/
        // `glyph_bitmap_cache_` members) and passed in by reference here,
        // deliberately shared across every node this stage renders (and
        // across frames, for the lifetime of the stage) rather than
        // scoped to one call of this function - the real repetition this
        // backend can exploit (the same character, e.g. "A"/"0"/"_",
        // recurring across many different labels drawn from a small
        // alphabet) is *cross-node*, not intra-node: HierarchyResolverStage
        // renders each distinct AbstractId's own content exactly once
        // regardless of placement count (backend/CLAUDE.md's own
        // HierarchyResolver bullet - "a design placed N times is still
        // resolved once"), so within any single call of THIS function a
        // given Terminal's own pin name (or Placement's own instance
        // name) appears at most once - an earlier, per-call-scoped
        // version of an earlier (per-STRING) cache design measured a
        // 100% miss rate against this exact fact
        // (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md).
        const BLFontFace &font_face = default_blend2d_font_face();
        const ViewLayerId placement_layer_id = view_layers.placement_view_layer();
        const ViewLayerId port_marker_layer_id = view_layers.port_marker_view_layer();

        // Shared by both the generic per-shape text loop and the
        // placement-name branch below (draw_one_shape) - looks up
        // `monospace_font_cache`'s own entry for `font_key`, building
        // (and measuring the fixed cell width of) a fresh BLFont on a
        // cache miss. Declared once here rather than duplicated in both
        // call sites.
        auto get_or_build_monospace_font = [&](int font_key) -> MonospaceFontEntry &
        {
            auto it = monospace_font_cache.find(font_key);
            if (it == monospace_font_cache.end())
            {
                MonospaceFontEntry entry;
                if (font_face.is_valid())
                {
                    entry.font.create_from_face(font_face, static_cast<float>(font_key));
                    BLGlyphBuffer gb;
                    gb.set_utf8_text("0", 1);
                    entry.font.shape(gb);
                    BLTextMetrics metrics;
                    entry.font.get_text_metrics(gb, metrics);
                    entry.cell_width = metrics.advance.x;
                }
                it = monospace_font_cache.emplace(font_key, std::move(entry)).first;
            }
            return it->second;
        };

        // PORT_MARKER draws first, under everything else - otherwise a
        // port's marker would cover its own label, drawn with the port's
        // layer (NEW_FEATURES_SEPT_2026.md item 28). Markers sit outside
        // their ports, so nothing else they overlap hides a real shape.
        std::vector<ViewLayerId> draw_order = view_layers.all();
        std::ranges::stable_partition(draw_order, [&](ViewLayerId id)
                                      { return id == port_marker_layer_id; });
        for (const ViewLayerId &view_layer_id : draw_order)
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
            // BOUNDARY/PLACEMENT/GCELLGRID/REGION
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
            const bool is_placement_layer = view_layer_id == placement_layer_id;
            const bool is_port_marker_layer = view_layer_id == port_marker_layer_id;

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
                    // Blend2D analog of the earlier Skia backend's own
                    // pattern_shader/makeWithLocalMatrix scale-compensation -
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
                // renders nothing at all here, unlike Skia's own SkPaint
                // hairline default. 1.0 / scale is the
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

            auto draw_one_shape = [&](const RenderShape &shape)
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

                // A port marker never shrinks below kMinPortMarkerPixelSize
                // (NEW_FEATURES_SEPT_2026.md item 28) - grown about the
                // port's edge, so it's never sub-pixel either.
                std::optional<std::vector<Polygon>> enlarged_marker;
                if (is_port_marker_layer)
                    enlarged_marker = enlarged_port_marker(shape.polygons, scale);
                for (const Polygon &poly : enlarged_marker ? *enlarged_marker : shape.polygons)
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

                if (!any_geometry_drawn)
                    return; // no visible geometry to attach a label to - draw nothing

                if (is_placement_layer)
                {
                    // Placement name labels: floored/capped font size (no
                    // kLabelWidthRatio here - unlike a Terminal/ROUTE
                    // label, text.size already bakes in
                    // kPlacementLabelHeightRatio at construction time,
                    // hierarchy_resolver_stage.hpp's own
                    // placement_shape), bottom-left-anchored with a
                    // small constant on-screen padding, truncated to fit
                    // the placement's own on-screen width via the
                    // index-paired shape.rects entry - see this function's
                    // own top-level doc comment and
                    // truncate_monospace_label's own doc comment.
                    for (std::size_t i = 0; i < shape.texts.size(); ++i)
                    {
                        const Text &text = shape.texts[i];
                        const double pixel_size = std::clamp(text.size * scale, min_label_px, max_label_px);
                        if (i >= shape.rects.size())
                            continue;
                        const double width_px = static_cast<double>(shape.rects[i].ur.x - shape.rects[i].ll.x) * scale;
                        const double available_width_px = width_px - 2.0 * kPlacementLabelPaddingPx;
                        if (available_width_px <= 0.0)
                            continue;

                        const int font_key = std::max(1, static_cast<int>(std::lround(pixel_size)));
                        const MonospaceFontEntry &font_entry = get_or_build_monospace_font(font_key);

                        const std::string truncated = truncate_monospace_label(text.label, font_entry.cell_width, available_width_px);
                        if (truncated.empty())
                            continue;

                        // Bottom-left-anchored with a small constant
                        // on-screen padding - this backend
                        // works entirely in already-mapped device-pixel
                        // space (this function's own top-level doc
                        // comment), so the padding is added directly here:
                        // right (+x) and up the screen (-y, since device
                        // pixel y increases downward) from the placement's
                        // own raw bottom-left device point.
                        const BLPoint box_origin = ctx.final_transform().map_point(text.location.x, text.location.y);
                        const BLPoint device_origin(box_origin.x + kPlacementLabelPaddingPx, box_origin.y - kPlacementLabelPaddingPx);
                        draw_monospace_label_blend2d(ctx, font_entry, font_key, glyph_bitmap_cache, truncated, stroke_color, device_origin);
                    }
                    return; // this shape's own placement-name text is handled above - don't also fall into the generic text loop below
                }

                for (const Text &text : shape.texts)
                {
                    // Clamped at both ends: kMinLabelPixelSize keeps a
                    // label legible when its own local geometry is tiny;
                    // kMaxLabelPixelSize keeps it from growing without
                    // bound when zoomed in close (a label only needs to
                    // stay readable, not track the geometry's own on-screen
                    // size 1:1) - and, since font_key below is also this
                    // cache's own font-size dimension, bounds how many
                    // distinct sizes monospace_font_cache/glyph_bitmap_cache
                    // ever need to hold regardless of how far a user zooms
                    // in (GlyphBitmapCacheKey's own doc comment).
                    const double pixel_size = std::clamp(text.size * scale * kLabelWidthRatio, min_label_px, max_label_px);
                    const int font_key = std::max(1, static_cast<int>(std::lround(pixel_size)));
                    const MonospaceFontEntry &font_entry = get_or_build_monospace_font(font_key);

                    // Maps this shape's own dbu-space label origin through
                    // the context's current (translate+scale+flip) transform
                    // once, to a real device-pixel point - see this
                    // function's own top-level doc comment for why that's
                    // sufficient here (no accumulated instance rotation to
                    // defend against within one node's own render pass).
                    const BLPoint device_origin = ctx.final_transform().map_point(text.location.x, text.location.y);
                    draw_monospace_label_blend2d(ctx, font_entry, font_key, glyph_bitmap_cache, text.label, stroke_color, device_origin);
                }
            };

            const std::vector<RenderShape> &shapes = group_it->second;
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
                for (const RenderShape &shape : shapes)
                    draw_one_shape(shape);
            }
        }
    }

    /// @brief The Rasterize stage - Blend2D's own JIT-compiled
    /// rasterization pipeline, multi-threaded tiled rendering, tried and
    /// kept (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) after beating the
    /// original Skia-based RasterizeStage (side experiment, since
    /// removed) by 1.35-2.9x with zero tuning on this project's own
    /// "millions of small shapes" workload - now the only Rasterize
    /// backend, and the sole thing ViewRenderPipeline (view_render_pipeline.hpp)
    /// wires in (no more backend-swappable template). Both generic
    /// per-shape text and placement-name labels are drawn (see
    /// draw_view_shapes_blend2d's own doc comment), via a monospace font.
    ///
    /// `thread_count_` is a plain mutable setting (set_thread_count
    /// below), not a constructor parameter - a benchmark wanting a
    /// specific thread count calls the setter once after construction.
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
                // A nested node covers everything it draws (ViewData::extent),
                // not just its declared boundary - content outside a cell's
                // boundary still shows one level up.
                const Rect local_bbox = (id == options.top_level) ? options.viewport : data.extent;

                const double width_dbu = static_cast<double>(local_bbox.ur.x - local_bbox.ll.x);
                const double height_dbu = static_cast<double>(local_bbox.ur.y - local_bbox.ll.y);
                const int pixel_width = std::clamp(static_cast<int>(std::ceil(width_dbu * options.scale)), 1, kMaxDimensionPx);
                const int pixel_height = std::clamp(static_cast<int>(std::ceil(height_dbu * options.scale)), 1, kMaxDimensionPx);

                BLImage image(pixel_width, pixel_height, BL_FORMAT_PRGB32);

                BLContextCreateInfo create_info{};
                create_info.thread_count = thread_count_;
                BLContext ctx(image, create_info);
                ctx.clear_all();

                // dbu y increases upward, pixel y increases downward.
                ctx.translate(0.0, static_cast<double>(pixel_height));
                ctx.scale(options.scale, -options.scale);
                ctx.translate(static_cast<double>(-local_bbox.ll.x), static_cast<double>(-local_bbox.ll.y));

                // Background grid + Abstract origin marker - only for
                // top_level itself (see draw_grid_blend2d's own doc
                // comment for why baking these into a nested placement's
                // own image would be wrong), drawn first so real design
                // geometry sits on top of it, not underneath.
                if (id == options.top_level)
                {
                    draw_grid_blend2d(ctx, local_bbox, options.scale, options.minor_grid_spacing_dbu, options.major_grid_spacing_dbu);
                    if (options.abstract_origin_dbu.has_value())
                        draw_origin_marker_blend2d(ctx, *options.abstract_origin_dbu, options.scale);
                }

                NodePathOutlineCache &node_outline_cache = path_outline_cache_by_node_[id];
                if (node_outline_cache.source != data.shapes)
                {
                    node_outline_cache.outlines.clear();
                    node_outline_cache.source = data.shapes;
                }

                draw_view_shapes_blend2d(
                    ctx, data.shapes ? *data.shapes : kEmptyShapes, data.shapes_index, local_bbox, view_layers, options.scale,
                    options.layer_name_visible, options.purpose_visible, node_outline_cache.outlines,
                    monospace_font_cache_, glyph_bitmap_cache_, options.label_min_size_px, options.label_max_size_px);

                ctx.end();

                // ComposeStage now composites BLImages natively (Blend2D
                // is the only Rasterize backend, PIPELINE_REFACTOR_BENCHMARK_RESULTS.md -
                // the generic Skia/Blend2D-swappable pipeline and its own
                // sk_sp<SkImage>-wrapping shim this replaced are gone), so
                // `image` itself is the finished RasterizedImage - no
                // format conversion/copy needed at all.
                result.images.emplace(id, RasterizedImage{std::move(image), local_bbox.ll});
            }

            return result;
        }

        bool options_did_change(const ViewRenderOptions &last, const ViewRenderOptions &current) const override
        {
            if (last.top_level != current.top_level ||
                last.scale != current.scale ||
                last.viewport.ll.x != current.viewport.ll.x ||
                last.viewport.ll.y != current.viewport.ll.y ||
                last.viewport.ur.x != current.viewport.ur.x ||
                last.viewport.ur.y != current.viewport.ur.y ||
                last.antialiasing_enabled != current.antialiasing_enabled ||
                last.layer_name_visible != current.layer_name_visible ||
                last.purpose_visible != current.purpose_visible ||
                last.minor_grid_spacing_dbu != current.minor_grid_spacing_dbu ||
                last.major_grid_spacing_dbu != current.major_grid_spacing_dbu ||
                last.label_min_size_px != current.label_min_size_px ||
                last.label_max_size_px != current.label_max_size_px)
                return true;

            // Point has no operator== in this codebase - field-by-field,
            // same convention ComposeStage's own drag_rect_dbu comparison
            // uses. Rarely actually differs independent of top_level
            // (switching Abstracts already changes that), but cheap
            // enough to compare directly rather than assume.
            if (last.abstract_origin_dbu.has_value() != current.abstract_origin_dbu.has_value())
                return true;
            if (last.abstract_origin_dbu.has_value() &&
                (last.abstract_origin_dbu->x != current.abstract_origin_dbu->x || last.abstract_origin_dbu->y != current.abstract_origin_dbu->y))
                return true;

            return false;
        }

        // pipeline_stage_benchmark cache-stat hooks (tbb_core.hpp) - one
        // rasterized BLImage per surviving node; bytes assume PRGB32 (4
        // bytes/pixel, this stage's own actual output format) since
        // BLImage exposes no direct byte-size accessor, only
        // width()/height() - an approximation, noted as such.
        std::size_t estimate_output_object_count(const RasterizeOutput &output) const override
        {
            return output.images.size();
        }

        std::size_t estimate_output_bytes(const RasterizeOutput &output) const override
        {
            std::size_t bytes = 0;
            for (const auto &[id, image] : output.images)
                bytes += static_cast<std::size_t>(image.image.width()) * static_cast<std::size_t>(image.image.height()) * 4;
            return bytes;
        }

    private:
        struct NodePathOutlineCache
        {
            ViewShapesHandle source;
            std::unordered_map<const Path *, std::vector<Polygon>> outlines;
        };
        std::unordered_map<HierarchyId, NodePathOutlineCache, HierarchyIdHash> path_outline_cache_by_node_;

        // Both shared across every node this stage renders, and across
        // every frame for this stage's own lifetime - built/populated
        // lazily, on demand, inside draw_view_shapes_blend2d (not here)
        // since a font is only ever needed for a `font_key` some label
        // actually resolves to. The real repetition this backend can
        // exploit is *cross-node* (the same character/size/color
        // recurring across many different labels), not intra-node - see
        // draw_view_shapes_blend2d's own doc comment.
        //
        // Unlike `path_outline_cache_by_node_` above, entries in either
        // map below never need invalidating - a (font_key)'s own BLFont,
        // or a (character, font_key, color)'s own rendered ink, never
        // changes - and unlike an earlier, since-replaced per-STRING
        // glyph-bitmap cache design (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md),
        // `glyph_bitmap_cache_` is bounded by construction: at most (the
        // size of the alphabet actually used - printable ASCII, well
        // under 128) times (~15 distinct clamped/rounded font sizes)
        // times (the number of distinct colors a design's own
        // ViewLayerSet defines - small, one per physical layer) entries,
        // regardless of how many distinct or unique strings (e.g.
        // Placement names, each unique per instance) a design's own
        // content contains - a real, reported unbounded-memory-growth bug
        // in the per-STRING predecessor this replaced.
        std::unordered_map<int, MonospaceFontEntry> monospace_font_cache_;
        std::unordered_map<GlyphBitmapCacheKey, CachedGlyphBitmap, GlyphBitmapCacheKeyHash> glyph_bitmap_cache_;

        uint32_t thread_count_ = 8;
    };
}
