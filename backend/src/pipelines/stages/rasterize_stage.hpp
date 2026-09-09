#pragma once

#include "../../core/placement_geometry.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../view_style/view_style.hpp"
#include "../default_typeface.hpp"
#include "../draw_helpers.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "../rasterize_output.hpp"
#include "../upright_text_canvas.hpp"
#include "hierarchy_resolver_stage.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkDashPathEffect.h"

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

    inline SkPath to_sk_path(const Polygon &polygon, bool close)
    {
        SkPathBuilder builder;
        bool first = true;
        for (const Point &p : polygon.points)
        {
            const SkPoint sk_point = SkPoint::Make(static_cast<SkScalar>(p.x), static_cast<SkScalar>(p.y));
            if (first)
            {
                builder.moveTo(sk_point);
                first = false;
            }
            else
            {
                builder.lineTo(sk_point);
            }
        }
        if (close)
            builder.close();
        return builder.detach();
    }

    /// @brief Draws `shapes_by_layer` onto `canvas`, one ViewLayer group at
    /// a time, walking `view_layers.all()` (that ViewLayerSet's own
    /// bottom-to-top insertion/z-order - HierarchyResolverStage's own
    /// ViewLayerShapes comment) so draw order matches real layer stacking
    /// regardless of the order shapes happened to be collected in, and so
    /// a hidden layer's whole group can be skipped with one lookup rather
    /// than checking every shape - neither of which a flat, per-shape
    /// iteration could do as directly. Fill/stroke paint is constructed
    /// once per layer group, not once per
    /// shape (hoisted out of the per-shape loop below), since one
    /// ViewLayerStyle applies to every shape a group holds.
    ///
    /// Assumes `canvas`'s own current matrix already maps raw dbu
    /// coordinates directly to this canvas's own pixel space
    /// (RasterizeStage::compute()'s own translate+scale+flip setup) -
    /// every rect/polygon/path below is drawn using its own raw dbu-space
    /// coordinates unchanged, with no further per-shape math, EXCEPT
    /// text: `scale` is needed there specifically to counter the active
    /// canvas matrix's own scale factor (and y-flip) so a label's own
    /// declared `size` renders at a real, upright pixel size instead of
    /// being further distorted by whatever the active dbu-to-pixel scale
    /// happens to be - see the text loop's own comment.
    ///
    /// Ported from the pre-restart draw_group (git history), adapted for
    /// this function's own dbu-space drawing (draw_group operated in
    /// already-pixel-space content): FillPattern shader tiling
    /// (pattern_shader, draw_helpers.hpp - the tiled shader's own local
    /// matrix is scaled by `scale` to counter the ambient canvas matrix's
    /// own scale, so the tile reads as a fixed on-screen pixel density
    /// rather than stretching with zoom; the old design's own
    /// `pattern_phase_px` pan-phase compensation has no equivalent here -
    /// each node's own raster surface has its own local origin baked into
    /// the canvas matrix already, so the pattern's phase is stable per
    /// node/zoom-tick but not guaranteed pixel-identical to a sibling
    /// node's own phase - a cosmetic nuance, not a correctness one),
    /// FillPattern::CROSS drawn as an explicit "X" (draw_cross) instead of
    /// a tiled shader, and a sub-pixel-width Path drawn as a single
    /// hairline centerline stroke rather than Geometry::path_to_polygons'
    /// buffered (extension-aware) outline - imperceptibly different
    /// on-screen at that size, and avoids a real, if rare, degenerate/
    /// empty-polygon buffer result at effectively-zero width (e.g. Track/
    /// GCellGrid's own deliberate width=0 synthetic lines, which must
    /// always take this branch, never the buffered one).
    ///
    /// A0-width Path (Track/GCellGrid) always takes the hairline branch
    /// above (SkPaint stroke width 0, Skia's own "always exactly 1 device
    /// pixel" convention) regardless of scale - `path_to_polygons` isn't
    /// extension/buffer-safe for a truly zero-width centerline (the LEF/
    /// DEF default half-width end-cap extension there would itself be
    /// zero, degenerating to a flat, possibly self-intersecting result).
    ///
    /// `antialiasing_enabled` is ViewRenderOptions::antialiasing_enabled -
    /// see that field's own comment for why it defaults false.
    /// `layer_name_visible`/`purpose_visible` are ViewRenderOptions' own
    /// same-named fields - a hidden layer's whole group is skipped in one
    /// is_view_layer_visible check, before its own shapes are even looked
    /// at.
    ///
    /// Text sizing/positioning mirrors the pre-restart split between
    /// draw_group's own per-shape terminal/route label loop
    /// (kLabelWidthRatio applied to Text::size - itself
    /// Geometry::local_width_at's raw dbu result - floored at
    /// kMinLabelPixelSize, centered at `text.location`, never truncated)
    /// and draw_placement_labels (kPlacementLabelHeightRatio already baked
    /// into Text::size at construction time - HierarchyResolverStage's own
    /// placement_name_shape - floored the same way, anchored at the box's
    /// own bottom-left corner with a fixed on-screen padding, truncated to
    /// fit via truncate_text_to_width): the two are told apart here by
    /// whether the current ViewLayer *is*
    /// `view_layers.placement_name_view_layer()` - a placement-name Shape
    /// is the only kind ever pushed there, with `rects`/`texts` kept
    /// index-parallel (rects[i] is texts[i]'s own reference box) precisely
    /// so this function can recover the per-label available width without
    /// Text itself needing a width field of its own.
    /// `path_outline_cache` memoizes Geometry::path_to_polygons per Path
    /// (real bg::buffer work, not cheap) keyed by that Path's own stable
    /// address - stable because it points into `shapes_by_layer`'s own
    /// backing storage, itself owned by a shared_ptr<const ViewLayerShapes>
    /// HierarchyResolverStage's own MemoizingStage caching keeps alive
    /// unchanged across every pan/zoom-only tick. Passed in by the caller
    /// (RasterizeStage, a real member field invalidated whenever that
    /// shared_ptr's own identity changes - RasterizeStage's own comment)
    /// rather than owned here, since this is a free function with no
    /// state of its own between calls. Without this, a real routed
    /// design's own thousands-to-millions of wire Path segments each paid
    /// a fresh buffer computation on *every* Warm-tier tick, not just
    /// once per Cold recompute - measured as a 7-9x Rasterize slowdown
    /// (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md) before this cache existed,
    /// the same caching granularity the pre-restart pipeline's own
    /// RenderedShape::path_outlines already used (computed once at
    /// shape-generation/Cold time - git history), just keyed differently
    /// since Shape itself has no field of its own to cache into (a
    /// schema.py change, deliberately avoided here).
    ///
    /// `shapes_index`/`query_bbox`: per-shape viewport culling
    /// (HierarchyResolverStage's own ViewShapesIndexHandle comment) -
    /// when a given ViewLayerId has a spatial index, only the shapes
    /// whose own bbox actually overlaps `query_bbox` (the node's own
    /// render bbox, RasterizeStage::compute()'s own `local_bbox`) are
    /// visited at all, instead of every shape the layer holds. Falls
    /// back to the full, unindexed scan when `shapes_index` is null or
    /// has no entry for a given layer (a defensive degrade, not an
    /// expected path - every real ViewData built by
    /// HierarchyResolverStage carries one) - correctness never depends
    /// on the index existing, only performance does. This is the fix for
    /// the per-shape culling gap ViewportCullStage's own doc comment
    /// names (that stage only culls placements/instances, one level up);
    /// see PIPELINE_REFACTOR_BENCHMARK_RESULTS.md for the before/after.
    inline void draw_view_shapes(
        SkCanvas &canvas, const ViewLayerShapes &shapes_by_layer, const ViewShapesIndexHandle &shapes_index, const Rect &query_bbox,
        const ViewLayerSet &view_layers, double scale, bool antialiasing_enabled,
        const std::unordered_map<std::string, bool> &layer_name_visible, const std::unordered_map<ViewLayerPurpose, bool> &purpose_visible,
        std::unordered_map<const Path *, std::vector<Polygon>> &path_outline_cache)
    {
        const ViewLayerId placement_name_layer_id = view_layers.placement_name_view_layer();

        for (const ViewLayerId &view_layer_id : view_layers.all())
        {
            const auto group_it = shapes_by_layer.find(view_layer_id);
            if (group_it == shapes_by_layer.end() || group_it->second.empty())
                continue; // no shapes on this layer

            const ViewLayerData *layer = view_layers.get(view_layer_id);
            if (!layer)
                continue;
            if (!is_view_layer_visible(layer_name_visible, purpose_visible, layer->layer_name, layer->purpose))
                continue;

            const ViewLayerStyle &style = layer->style;
            const bool has_fill = style.fill_color.a > 0;
            const bool has_outline = style.outline_color.a > 0;
            if (!has_fill && !has_outline)
                continue;

            const bool is_cross = style.fill_pattern == FillPattern::CROSS;
            const bool is_placement_name_layer = view_layer_id == placement_name_layer_id;

            SkPaint fill;
            fill.setAntiAlias(antialiasing_enabled);
            fill.setStyle(SkPaint::kFill_Style);
            if (sk_sp<SkShader> shader = pattern_shader(style.fill_pattern, to_sk_color(style.outline_color)))
            {
                // Cancels the ambient canvas matrix's own dbu-to-pixel
                // scale (RasterizeStage::compute's own translate+scale+
                // flip setup) so pattern_shader's fixed-pixel-size tile
                // reads at a constant on-screen density regardless of
                // zoom - see this function's own doc comment.
                shader = shader->makeWithLocalMatrix(SkMatrix::Scale(static_cast<SkScalar>(scale), static_cast<SkScalar>(-scale)));
                // A paint's alpha still modulates its shader's own output
                // alpha even though its RGB is ignored - leaving
                // fill_color (translucent) as this paint's color would
                // silently wash out an already-opaque pattern pixel.
                fill.setShader(std::move(shader));
                fill.setAlphaf(1.0f);
            }
            else
            {
                fill.setColor(to_sk_color(style.fill_color));
            }

            SkPaint stroke;
            stroke.setAntiAlias(antialiasing_enabled);
            stroke.setStyle(SkPaint::kStroke_Style);
            stroke.setColor(to_sk_color(style.outline_color));
            if (style.dashed)
            {
                // Dash lengths specified in dbu (4/scale), not a fixed
                // pixel count, so the dash pattern's own on-screen size
                // stays roughly constant across zoom once drawn through
                // the active dbu-to-pixel canvas matrix - approximate,
                // not pixel-perfect, but avoids a dash pattern that
                // visibly grows/shrinks with scale.
                const SkScalar dash_length = static_cast<SkScalar>(4.0 / scale);
                stroke.setPathEffect(SkDashPathEffect::Make({dash_length, dash_length}, 0.0f));
            }

            // Only the "X" itself (draw_cross below) uses this - the
            // surrounding cut rect/polygon boundary still draws with the
            // plain hairline `stroke` above, same as every other layer's
            // outline.
            SkPaint cross_stroke = stroke;
            if (is_cross)
                cross_stroke.setStrokeWidth(kViaCrossStrokeWidth);

            SkPaint text_paint;
            text_paint.setAntiAlias(antialiasing_enabled);
            text_paint.setColor(to_sk_color(style.outline_color));

            // Extracted from the per-shape loop below so it can be
            // invoked either for every shape in this layer's own vector
            // (no index / no entry for this layer) or just the subset a
            // spatial-index query returns (see this function's own doc
            // comment) - identical body either way, no behavior change
            // from before this was a lambda.
            auto draw_one_shape = [&](const Shape &shape)
            {
                for (const Rect &r : shape.rects)
                {
                    if (bbox_is_sub_pixel(r.ur.x - r.ll.x, r.ur.y - r.ll.y, scale))
                        continue;
                    const SkRect rect = SkRect::MakeLTRB(
                        static_cast<SkScalar>(r.ll.x), static_cast<SkScalar>(r.ll.y),
                        static_cast<SkScalar>(r.ur.x), static_cast<SkScalar>(r.ur.y));
                    if (is_cross)
                    {
                        if (has_outline)
                            draw_cross(canvas, rect, cross_stroke);
                        if (has_outline)
                            canvas.drawRect(rect, stroke);
                        continue;
                    }
                    if (has_fill)
                        canvas.drawRect(rect, fill);
                    if (has_outline)
                        canvas.drawRect(rect, stroke);
                }

                for (const Polygon &poly : shape.polygons)
                {
                    if (polygon_is_sub_pixel(poly, scale))
                        continue;
                    const SkPath path = to_sk_path(poly, /*close=*/true);
                    if (is_cross)
                    {
                        if (has_outline)
                            draw_cross(canvas, path.getBounds(), cross_stroke);
                    }
                    else if (has_fill)
                        canvas.drawPath(path, fill);
                    if (has_outline)
                        canvas.drawPath(path, stroke);
                }

                for (const Path &p : shape.paths)
                {
                    // Sub-pixel on screen (or a deliberately zero-width
                    // synthetic line, Track/GCellGrid) - a single hairline
                    // centerline stroke instead of a buffered, extension-
                    // aware outline; see this function's own doc comment.
                    if (p.width * scale < 1.0)
                    {
                        SkPaint path_stroke = has_outline ? stroke : fill;
                        path_stroke.setStyle(SkPaint::kStroke_Style);
                        path_stroke.setStrokeWidth(0); // hairline
                        canvas.drawPath(to_sk_path(p.polygon, /*close=*/false), path_stroke);
                        continue;
                    }

                    // Real square-ended (LEF/DEF default half-width
                    // extension) stroked outline, fill first (the layer's
                    // real pattern, not a solid stroke) then a thin
                    // outline-colored boundary, then a thin centerline
                    // stroke on top so the path still reads as a wire
                    // rather than just another filled/outlined shape.
                    // Cached (see this function's own doc comment) -
                    // real bg::buffer work, not cheap enough to redo on
                    // every pan/zoom tick for every routed Path.
                    auto outline_it = path_outline_cache.find(&p);
                    if (outline_it == path_outline_cache.end())
                        outline_it = path_outline_cache.emplace(&p, Geometry::path_to_polygons(p)).first;
                    for (const Polygon &outline : outline_it->second)
                    {
                        const SkPath outline_path = to_sk_path(outline, /*close=*/true);
                        if (has_fill)
                            canvas.drawPath(outline_path, fill);
                        if (has_outline)
                            canvas.drawPath(outline_path, stroke);
                    }
                    if (has_outline)
                        canvas.drawPath(to_sk_path(p.polygon, /*close=*/false), stroke);
                }

                if (is_placement_name_layer)
                {
                    // Placement name labels: height-ratio font size
                    // (already baked into text.size, dbu, at construction
                    // time) floored at a fixed on-screen minimum,
                    // bottom-left-anchored with a small constant on-screen
                    // padding (added here, in already-counter-scaled local
                    // space, not baked into the dbu-space translate, so it
                    // stays a constant inset regardless of zoom), and
                    // truncated to fit the placement's own on-screen width
                    // via the index-paired shape.rects entry - see this
                    // function's own doc comment.
                    for (std::size_t i = 0; i < shape.texts.size(); ++i)
                    {
                        const Text &text = shape.texts[i];
                        const double pixel_size = std::max(text.size * scale, kMinLabelPixelSize);
                        if (i >= shape.rects.size())
                            continue;
                        const double width_px = static_cast<double>(shape.rects[i].ur.x - shape.rects[i].ll.x) * scale;
                        const double available_width_px = width_px - 2.0 * kPlacementLabelPaddingPx;
                        if (available_width_px <= 0.0)
                            continue;

                        SkFont font(default_typeface(), static_cast<SkScalar>(pixel_size));
                        font.setEdging(antialiasing_enabled ? SkFont::Edging::kAntiAlias : SkFont::Edging::kAlias);

                        const std::string truncated = truncate_text_to_width(text.label, font, static_cast<SkScalar>(available_width_px));
                        if (truncated.empty())
                            continue;

                        canvas.save();
                        canvas.translate(static_cast<SkScalar>(text.location.x), static_cast<SkScalar>(text.location.y));
                        canvas.drawString(truncated.c_str(), static_cast<SkScalar>(kPlacementLabelPaddingPx), static_cast<SkScalar>(kPlacementLabelPaddingPx), font, text_paint);
                        canvas.restore();
                    }
                    return; // this shape's own placement-name text is handled above - don't also fall into the generic text loop below
                }

                for (const Text &text : shape.texts)
                {
                    const double pixel_size = std::max(text.size * scale * kLabelWidthRatio, kMinLabelPixelSize);

                    SkFont font(default_typeface(), static_cast<SkScalar>(pixel_size));
                    font.setEdging(antialiasing_enabled ? SkFont::Edging::kAntiAlias : SkFont::Edging::kAlias);

                    // Counters the active canvas matrix's own scale+flip
                    // (see this function's own doc comment) so the label
                    // renders upright at its real declared pixel size.
                    canvas.save();
                    canvas.translate(static_cast<SkScalar>(text.location.x), static_cast<SkScalar>(text.location.y));
                    canvas.drawString(text.label.c_str(), 0, 0, font, text_paint);
                    canvas.restore();
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

    /// @brief Warm-tier stage 2 (PIPELINE_REFACTOR.md): rasterizes every
    /// surviving node's own direct `shapes` (post ViewportCullStage)
    /// into its own independent pixel image - real bitmaps, not
    /// SkPicture recordings (a deliberate choice over the pre-restart
    /// design's own per-node SkPicture + drawPicture compositing: that
    /// design's own cache-reuse benefit is illusory here anyway, since a
    /// zoom change forces every node to re-render regardless of which
    /// representation is cached - see PIPELINE_REFACTOR_BENCHMARK_RESULTS.md's
    /// own commit history for the discussion. A proper before/after
    /// comparison against the SkPicture approach is still a planned
    /// follow-up, not yet done).
    ///
    /// Every node shares the exact same ViewRenderOptions::scale
    /// (pixels-per-dbu) - ComposeStage's own compositing is then a plain
    /// translate+rotate per placement (Manhattan orientations only, see
    /// Geometry::orientation_linear), never a resample, since no two
    /// images ever differ in their own effective resolution.
    ///
    /// A node's own rasterization bbox is its declared bbox
    /// (layout_declared_bbox/abstract_declared_bbox, core/placement_geometry.hpp
    /// - the same bbox a *parent* already uses to size its own placement
    /// of this node) - EXCEPT `options.top_level` itself, which uses
    /// `options.viewport` directly instead. This asymmetry is
    /// deliberate, not an oversight: a top-level design can be
    /// arbitrarily large (the whole point of ViewportCullStage), so
    /// rasterizing its own full declared bbox regardless of what's
    /// actually visible would reintroduce the exact "cost independent of
    /// viewport" problem that stage just fixed - see the pre-restart
    /// design's own equivalent asymmetry (BuildLayoutPictureStage's own
    /// doc comment, git history: "only the top-level Layout node's own
    /// picture is what render_layout_frame hands ... TiledRasterizePictureStage").
    /// A NESTED node isn't further viewport-clamped - hierarchy_depth is
    /// typically small (2-3 levels) and per-node sizes shrink descending
    /// the hierarchy, so this is a known, accepted simplification, not a
    /// structural fix - revisit if a real fixture shows otherwise.
    ///
    /// A node whose own required pixel dimensions exceed kMaxDimensionPx
    /// (a sanity clamp against a pathological scale/bbox combination, not
    /// a real design limit) is skipped entirely (absent from `images`) -
    /// ComposeStage degrades by simply not drawing that node, rather than
    /// this stage crashing or allocating an unbounded raster surface.
    class RasterizeStage : public MemoizingStage<HierarchyResolverStage::OutputHandle, RasterizeOutput, ViewRenderOptions>
    {
    public:
        explicit RasterizeStage(oneapi::tbb::flow::graph &g, std::string label = "Rasterize")
            : MemoizingStage(g, std::move(label)) {}

    protected:
        RasterizeOutput compute(const HierarchyResolverStage::OutputHandle &culled, const ViewRenderOptions &options) override
        {
            RasterizeOutput result;
            result.culled = culled;
            if (culled == nullptr || options.root == nullptr)
                return result;

            static const ViewLayerSet kEmptyViewLayers;
            const ViewLayerSet &view_layers = options.view_layers != nullptr ? *options.view_layers : kEmptyViewLayers;
            static const ViewLayerShapes kEmptyShapes;

            constexpr int kMaxDimensionPx = 8192;

            for (const auto &[id, data] : culled->view_data)
            {
                const Rect local_bbox = (id == options.top_level) ? options.viewport : node_local_bbox(*options.root, id);

                const double width_dbu = static_cast<double>(local_bbox.ur.x - local_bbox.ll.x);
                const double height_dbu = static_cast<double>(local_bbox.ur.y - local_bbox.ll.y);
                const int pixel_width = std::clamp(static_cast<int>(std::ceil(width_dbu * options.scale)), 1, kMaxDimensionPx);
                const int pixel_height = std::clamp(static_cast<int>(std::ceil(height_dbu * options.scale)), 1, kMaxDimensionPx);

                const SkImageInfo info = SkImageInfo::Make(pixel_width, pixel_height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
                sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
                if (!surface)
                    continue; // degrade - skip this node rather than crash

                SkCanvas *canvas = surface->getCanvas();
                canvas->clear(SK_ColorTRANSPARENT);

                // Maps raw dbu coordinates directly to this canvas's own
                // pixel space: dbu y increases upward, pixel y increases
                // downward, hence the negative y scale - applied once
                // here rather than per-shape (draw_view_shapes' own doc
                // comment).
                canvas->translate(0, static_cast<SkScalar>(pixel_height));
                canvas->scale(static_cast<SkScalar>(options.scale), static_cast<SkScalar>(-options.scale));
                canvas->translate(static_cast<SkScalar>(-local_bbox.ll.x), static_cast<SkScalar>(-local_bbox.ll.y));

                // UprightTextCanvas (pipelines.old, re-ported) intercepts
                // every text draw and replaces the CTM with a
                // translation+uniform-scale-only matrix for that one draw
                // (discarding this canvas's own y-flip reflection
                // component), so a label renders upright/correctly sized
                // without draw_view_shapes' own text loop needing its own
                // manual per-label save/scale(1/scale,-1/scale)/restore
                // counter-transform - see upright_text_canvas.hpp's own
                // doc comment for why this decomposition is exact (not
                // approximate) for this codebase's own transform chain,
                // and PIPELINE_REFACTOR_BENCHMARK_RESULTS.md for this
                // swap's own measured overhead (an SkPaintFilterCanvas
                // virtual-dispatch + matrix-decomposition per text draw)
                // against the manual approach it replaces.
                UprightTextCanvas upright_canvas(canvas);

                // Per-NODE path-outline cache (keyed on this node's own
                // `id`, not on `culled` itself): a real Cold recompute
                // upstream gives this exact node a brand-new `data.shapes`
                // shared_ptr, invalidating just its own cached entry, but
                // ViewportCullStage runs BETWEEN HierarchyResolverStage and
                // this stage and hands every node a fresh wrapper object
                // on every single call regardless of whether the viewport
                // actually affected it - keying on `culled` itself (an
                // earlier version of this cache) invalidated on every pan
                // tick for every node, measured providing no real benefit
                // at all (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md). Keying
                // on `data.shapes` per node instead survives exactly the
                // pan/zoom-only ticks it needs to (that pointer is only
                // ever reassigned by a real HierarchyResolverStage
                // recompute reaching this specific node), while still
                // never growing past this design's own real node count
                // (bounded by macro/hierarchy variety, not by pan ticks or
                // shape count) - and holding this node's own current
                // `data.shapes` copy as the cache's own key value (not a
                // bare pointer) means the moment it's superseded, this is
                // the only extra reference keeping the OLD version alive,
                // so overwriting it here doesn't artificially extend
                // anything's lifetime beyond what HierarchyResolverStage's
                // own cache already does. See draw_view_shapes' own doc
                // comment for why buffering Path outlines needs caching at
                // all.
                NodePathOutlineCache &node_outline_cache = path_outline_cache_by_node_[id];
                if (node_outline_cache.source != data.shapes)
                {
                    node_outline_cache.outlines.clear();
                    node_outline_cache.source = data.shapes;
                }

                draw_view_shapes(
                    upright_canvas, data.shapes ? *data.shapes : kEmptyShapes, data.shapes_index, local_bbox, view_layers, options.scale,
                    options.antialiasing_enabled, options.layer_name_visible, options.purpose_visible, node_outline_cache.outlines);

                result.images.emplace(id, RasterizedImage{surface->makeImageSnapshot(), local_bbox.ll});
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
                   last.view_layers != current.view_layers ||
                   last.antialiasing_enabled != current.antialiasing_enabled ||
                   last.layer_name_visible != current.layer_name_visible ||
                   last.purpose_visible != current.purpose_visible;
        }

    private:
        // See compute()'s own comment on why this is keyed per-node
        // (HierarchyId) rather than on `culled` as a whole.
        struct NodePathOutlineCache
        {
            ViewShapesHandle source;
            std::unordered_map<const Path *, std::vector<Polygon>> outlines;
        };
        std::unordered_map<HierarchyId, NodePathOutlineCache, HierarchyIdHash> path_outline_cache_by_node_;

        static Rect node_local_bbox(const Root &root, const HierarchyId &id)
        {
            if (const LayoutId *layout_id = std::get_if<LayoutId>(&id))
                return layout_declared_bbox(root, *layout_id);
            return abstract_declared_bbox(root, std::get<AbstractId>(id));
        }
    };
}
