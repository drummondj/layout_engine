#pragma once

#include "../../core/placement_geometry.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
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
            ctx.set_stroke_style(color);
            ctx.stroke_line(BLLine(0, 0, s, 0));
            ctx.stroke_line(BLLine(0, s / 2, s, s / 2));
            ctx.stroke_line(BLLine(s / 2, 0, s / 2, s / 2));
            ctx.stroke_line(BLLine(0, s / 2, 0, s));
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

    /// @brief Blend2D sibling of rasterize_stage.hpp's own `draw_view_shapes` -
    /// same per-layer/per-shape structure, same shapes_index-or-fallback
    /// viewport-culling dispatch, same path_outline_cache (Geometry::
    /// path_to_polygons' own std::vector<Polygon> result needs no
    /// backend-specific storage) - only the actual draw calls differ
    /// (BLContext instead of SkCanvas). Text (Shape.texts) is deliberately
    /// NOT drawn here - a scoped-out gap for this first Blend2D pass (see
    /// PIPELINE_REFACTOR_BENCHMARK_RESULTS.md), not an oversight; text is
    /// a small fraction of draw calls next to ROUTE/TERMINAL/OBSTRUCTION/
    /// via geometry, so its absence shouldn't meaningfully skew a raw
    /// rasterization-throughput comparison against Skia.
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
    /// `use_opaque_fast_path`: when true, a fully-opaque fill/stroke color
    /// (alpha == 255) uses BL_COMP_OP_SRC_COPY (a pure overwrite, no
    /// destination read/blend) instead of the default BL_COMP_OP_SRC_OVER -
    /// Blend2D's own fast path for exactly this case. Most of this
    /// project's real ViewLayerStyles use a translucent fill_color (alpha
    /// 100, layer_style()'s own convention, view_style.hpp) - expected to
    /// matter more for opaque strokes/outlines than fills; see
    /// PIPELINE_REFACTOR_BENCHMARK_RESULTS.md for the measured answer
    /// rather than assuming.
    inline void draw_view_shapes_blend2d(
        BLContext &ctx, const ViewLayerShapes &shapes_by_layer, const ViewShapesIndexHandle &shapes_index, const Rect &query_bbox,
        const ViewLayerSet &view_layers, double scale, bool use_opaque_fast_path,
        const std::unordered_map<std::string, bool> &layer_name_visible, const std::unordered_map<ViewLayerPurpose, bool> &purpose_visible,
        std::unordered_map<const Path *, std::vector<Polygon>> &path_outline_cache)
    {
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

            // Gated on color.a == 255, not just use_opaque_fast_path alone -
            // tried removing this gate entirely (always SRC_COPY,
            // PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) and measured zero
            // benefit even then (still within noise of plain SRC_OVER,
            // same as the gated version), so there's no performance
            // reason to accept SRC_COPY's own real correctness cost for
            // a translucent color: it doesn't blend with the destination
            // at all, it overwrites it outright, alpha included - a
            // translucent fill/stroke drawn this way would stop showing
            // whatever was drawn underneath it (an earlier ViewLayer on
            // the same per-node image - this project's own layer_style()
            // convention, fill.a = 100, relies on real alpha blending for
            // exactly this), confirmed directly by sampling an
            // overlapping-translucent-layers pixel under each mode (same
            // file). Kept gated to the one case where SRC_COPY and
            // SRC_OVER are actually equivalent (a fully opaque color has
            // nothing underneath left to blend anyway).
            const BLCompOp fill_comp_op = (use_opaque_fast_path && style.fill_color.a == 255) ? BL_COMP_OP_SRC_COPY : BL_COMP_OP_SRC_OVER;
            const BLCompOp stroke_comp_op = (use_opaque_fast_path && style.outline_color.a == 255) ? BL_COMP_OP_SRC_COPY : BL_COMP_OP_SRC_OVER;

            auto set_fill = [&]
            {
                ctx.set_comp_op(fill_comp_op);
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
                ctx.set_comp_op(stroke_comp_op);
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
                for (const Rect &r : shape.rects)
                {
                    if (bbox_is_sub_pixel(r.ur.x - r.ll.x, r.ur.y - r.ll.y, scale))
                        continue;
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
    /// options_did_change. Text is not drawn (see draw_view_shapes_blend2d's
    /// own doc comment) - not a byte-for-byte feature match with
    /// RasterizeStage, an explicit, scoped choice for this first pass.
    ///
    /// `thread_count_`/`use_opaque_fast_path_` are plain mutable settings
    /// (set_thread_count/set_use_opaque_fast_path below), not constructor
    /// parameters - keeps this class's own constructor signature identical
    /// to RasterizeStage's (`(graph, label)`), which is what lets
    /// ViewRenderPipelineImpl's single `RasterizeStageT rasterize_(graph_,
    /// label + ".Rasterize")` construction site work unchanged for either
    /// backend; a benchmark wanting a specific thread count/fast-path
    /// configuration calls the setters once after construction instead.
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
        void set_use_opaque_fast_path(bool enabled) { use_opaque_fast_path_ = enabled; }

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
                    use_opaque_fast_path_, options.layer_name_visible, options.purpose_visible, node_outline_cache.outlines);

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

        uint32_t thread_count_ = 8;
        bool use_opaque_fast_path_ = false;

        static Rect node_local_bbox(const Root &root, const HierarchyId &id)
        {
            if (const LayoutId *layout_id = std::get_if<LayoutId>(&id))
                return layout_declared_bbox(root, *layout_id);
            return abstract_declared_bbox(root, std::get<AbstractId>(id));
        }
    };
}
