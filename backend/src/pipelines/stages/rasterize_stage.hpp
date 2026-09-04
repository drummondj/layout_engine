#pragma once

#include "../../core/placement_geometry.hpp"
#include "../../database/database.hpp"
#include "../../view_style/view_style.hpp"
#include "../default_typeface.hpp"
#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "hierarchy_resolver_stage.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkDashPathEffect.h"

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
    inline SkColor to_sk_color(Color c) { return SkColorSetARGB(c.a, c.r, c.g, c.b); }

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
    /// a future layer-visibility feature can skip a hidden layer's whole
    /// group with one lookup rather than checking every shape - neither
    /// of which a flat, per-shape iteration could do as directly. Fill/
    /// stroke paint is constructed once per layer group, not once per
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
    /// Deliberately simpler than the pre-restart draw_group (git history):
    /// no FillPattern shader tiling (flat fill_color only), no CUT-style
    /// cross pattern, no sub-pixel hairline-collapse special case - each a
    /// real, deliberate scope cut for this first Rasterization pass, not
    /// an oversight (PIPELINE_REFACTOR_BENCHMARK_RESULTS.md's own
    /// convention of noting what's deferred rather than silently
    /// dropping it). A 0-width Path draws as a hairline (SkPaint stroke
    /// width 0, Skia's own "always exactly 1 device pixel" convention) -
    /// this one nicety survives because it needs no extra code, not
    /// because it was prioritized over the others. `antialiasing_enabled`
    /// is ViewRenderOptions::antialiasing_enabled - see that field's own
    /// comment for why it defaults false.
    inline void draw_view_shapes(SkCanvas &canvas, const ViewLayerShapes &shapes_by_layer, const ViewLayerSet &view_layers, double scale, bool antialiasing_enabled)
    {
        for (const ViewLayerId &view_layer_id : view_layers.all())
        {
            const auto group_it = shapes_by_layer.find(view_layer_id);
            if (group_it == shapes_by_layer.end() || group_it->second.empty())
                continue; // no shapes on this layer - a future visibility check would also short-circuit right here

            const ViewLayerData *layer = view_layers.get(view_layer_id);
            if (!layer)
                continue;

            const ViewLayerStyle &style = layer->style;
            const bool has_fill = style.fill_color.a > 0;
            const bool has_outline = style.outline_color.a > 0;
            if (!has_fill && !has_outline)
                continue;

            SkPaint fill;
            fill.setAntiAlias(antialiasing_enabled);
            fill.setStyle(SkPaint::kFill_Style);
            fill.setColor(to_sk_color(style.fill_color));

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

            SkPaint text_paint;
            text_paint.setAntiAlias(antialiasing_enabled);
            text_paint.setColor(to_sk_color(style.outline_color));

            for (const Shape &shape : group_it->second)
            {
                for (const Rect &r : shape.rects)
                {
                    const SkRect rect = SkRect::MakeLTRB(
                        static_cast<SkScalar>(r.ll.x), static_cast<SkScalar>(r.ll.y),
                        static_cast<SkScalar>(r.ur.x), static_cast<SkScalar>(r.ur.y));
                    if (has_fill)
                        canvas.drawRect(rect, fill);
                    if (has_outline)
                        canvas.drawRect(rect, stroke);
                }

                for (const Polygon &poly : shape.polygons)
                {
                    const SkPath path = to_sk_path(poly, /*close=*/true);
                    if (has_fill)
                        canvas.drawPath(path, fill);
                    if (has_outline)
                        canvas.drawPath(path, stroke);
                }

                for (const Path &p : shape.paths)
                {
                    SkPaint path_stroke = has_outline ? stroke : fill;
                    path_stroke.setStyle(SkPaint::kStroke_Style);
                    path_stroke.setStrokeWidth(static_cast<SkScalar>(p.width)); // 0 == hairline
                    canvas.drawPath(to_sk_path(p.polygon, /*close=*/false), path_stroke);
                }

                for (const Text &text : shape.texts)
                {
                    const SkScalar pixel_size = static_cast<SkScalar>(text.size * scale);
                    if (pixel_size <= 0)
                        continue;

                    SkFont font(default_typeface(), pixel_size);
                    font.setEdging(antialiasing_enabled ? SkFont::Edging::kAntiAlias : SkFont::Edging::kAlias);

                    // Counters the active canvas matrix's own scale+flip
                    // (see this function's own doc comment) so the label
                    // renders upright at its real declared pixel size,
                    // the same save/translate/scale(1,-1)-counter-flip/
                    // drawString/restore idiom the pre-restart
                    // draw_placement_labels used, generalized to also
                    // cancel a non-1:1 scale (that code operated in
                    // already-pixel-space content, so its own counter-
                    // scale was always exactly {1,-1}).
                    canvas.save();
                    canvas.translate(static_cast<SkScalar>(text.location.x), static_cast<SkScalar>(text.location.y));
                    canvas.scale(static_cast<SkScalar>(1.0 / scale), static_cast<SkScalar>(-1.0 / scale));
                    canvas.drawString(text.label.c_str(), 0, 0, font, text_paint);
                    canvas.restore();
                }
            }
        }
    }

    /// @brief One node's own rasterized image - just its own direct
    /// `shapes` (RasterizeStage never draws a node's own placements/
    /// children - ComposeStage composites those, PIPELINE_REFACTOR.md's
    /// own stage split).
    struct RasterizedImage
    {
        sk_sp<SkImage> image;

        /// @brief The dbu point mapping to `image`'s own bottom-left
        /// pixel corner (image pixel (0, image->height())) - ComposeStage
        /// needs this to place the image correctly; an SkImage alone
        /// carries no notion of *where* in dbu space it represents.
        Point local_origin;
    };

    /// @brief RasterizeStage's own output - one RasterizedImage per
    /// surviving node, plus the culled HierarchyResolverOutput this was
    /// built from (a plain shared_ptr copy - see ViewRenderOptions::
    /// view_layers' own comment for why a second graph input isn't used
    /// instead): ComposeStage needs both the images AND each node's own
    /// placement_data/transform to composite them, and every
    /// MemoizingStage in this module takes exactly one InputData - no
    /// join_node has been needed anywhere else in this pipeline, and
    /// bundling here avoids introducing the first one.
    struct RasterizeOutput
    {
        std::unordered_map<HierarchyId, RasterizedImage, HierarchyIdHash> images;
        HierarchyResolverStage::OutputHandle culled;
    };

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

                draw_view_shapes(*canvas, data.shapes ? *data.shapes : kEmptyShapes, view_layers, options.scale, options.antialiasing_enabled);

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
                   last.antialiasing_enabled != current.antialiasing_enabled;
        }

    private:
        static Rect node_local_bbox(const Root &root, const HierarchyId &id)
        {
            if (const LayoutId *layout_id = std::get_if<LayoutId>(&id))
                return layout_declared_bbox(root, *layout_id);
            return abstract_declared_bbox(root, std::get<AbstractId>(id));
        }
    };
}
