#pragma once

#include "../draw_helpers.hpp"
#include "../pipeline_options.hpp"
#include "../rasterize_output.hpp"
#include "../tbb_core.hpp"
#include "hierarchy_resolver_stage.hpp"
#include "rasterize_blend2d_stage.hpp"

#include <blend2d/blend2d.h>
#include <fmt/format.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace le
{
    /// @brief Raw RGBA8888 pixel view into a RasterizedFrame's own
    /// `pixel_data` - mirrors api.hpp's LePixelBuffer contract exactly
    /// (top-left origin, y increasing downward, premultiplied alpha,
    /// row_bytes possibly exceeding width * 4 - always index by it) so
    /// wiring this up to le_render_pixel_buffer() later needs no format
    /// translation. `data` is a non-owning view into `pixel_data`'s own
    /// backing memory - never valid on its own, see RasterizedFrame.
    struct PixelBuffer
    {
        const uint8_t *data = nullptr;
        int width = 0;
        int height = 0;
        std::size_t row_bytes = 0;
    };

    /// @brief ComposeStage's own output - the whole Warm tier's final
    /// image. `pixel_data` owns the final, already-byte-swapped RGBA
    /// bytes `buffer` points into (compute()'s own doc comment has the
    /// BGRA-to-RGBA swap rationale) - simpler than the earlier Skia
    /// design's `sk_sp<SkSurface>` member, since there's no surface/
    /// context object that needs to stay alive once that one copy is
    /// made. `empty` is true when nothing could be composed at all
    /// (top_level itself wasn't in RasterizeBlend2DStage's own output -
    /// e.g. a null Root, or top_level exceeding that stage's own pixel-
    /// dimension sanity clamp) - distinguishes "genuinely nothing to
    /// show" from a real all-transparent frame, which `buffer.data ==
    /// nullptr` alone wouldn't.
    struct RasterizedFrame
    {
        std::vector<uint8_t> pixel_data;
        PixelBuffer buffer;
        bool empty = true;
    };

    /// @brief Warm-tier stage 3 (PIPELINE_REFACTOR.md): composites every
    /// surviving node's own RasterizeBlend2DStage image into one final
    /// image, walking the same placement_data ViewportCullStage already
    /// pruned to what's visible. A node with no placements just IS its
    /// own image (no compositing needed, no copy either - composed_cache
    /// below hands back the same BLImage RasterizeBlend2DStage produced,
    /// a cheap refcounted copy - Blend2D's own analog of sk_sp<SkImage>'s
    /// COW semantics); a node WITH placements gets its own fresh
    /// BLContext, its own image drawn first, then each surviving child's
    /// own *fully composed* image (recursing depth-first via
    /// compose_node) drawn on top at the pixel position/orientation its
    /// own ViewPlacementData::transform implies - see
    /// child_image_to_parent_dbu_matrix's own comment for the exact
    /// derivation. Recursion depth is bounded by
    /// ViewRenderOptions::hierarchy_depth (small in practice, 2-3 levels)
    /// - no explicit depth guard needed.
    ///
    /// composed_cache (local to one compute() call, not a member) means a
    /// shared id placed by more than one parent is composited once, not
    /// once per parent - the same reachability/dedup principle
    /// HierarchyResolverStage and ViewportCullStage already apply, one
    /// stage further down.
    ///
    /// A known, accepted gap of the "raster bitmap per node" design
    /// (ViewRenderOptions::scale's own doc comment, RasterizeBlend2DStage's
    /// own doc comment): a placement whose own orientation isn't N bakes
    /// that rotation/reflection into the composited pixels of its own
    /// child image, including any text labels drawn within it by
    /// RasterizeBlend2DStage - a rotated placement's own labels render
    /// rotated/mirrored too, unlike the pre-restart SkPicture-based
    /// design (`pipelines.old`'s own UprightTextCanvas), which corrected
    /// this at replay time regardless of nesting. Confirmed by direct
    /// visual test (three placements, N/FN/FS, a real terminal label
    /// mirroring right along with each flipped one) and left unfixed by
    /// explicit decision, not merely deferred pending a future
    /// evaluation: a correct fix needs text drawn as a separate overlay
    /// pass after compositing (each label positioned at its own final,
    /// already-composited screen coordinates, independent of whatever
    /// per-node bitmap it originated from), a real architecture change,
    /// not a small patch to RasterizeBlend2DStage's own current per-node
    /// text-drawing code.
    class ComposeStage : public MemoizingStage<RasterizeOutputHandle, RasterizedFrame, ViewRenderOptions>
    {
    public:
        explicit ComposeStage(oneapi::tbb::flow::graph &g, std::string label = "Compose")
            : MemoizingStage(g, std::move(label)) {}

    protected:
        RasterizedFrame compute(const RasterizeOutputHandle &input, const ViewRenderOptions &options) override
        {
            RasterizedFrame frame;
            if (input == nullptr || input->culled == nullptr)
                return frame;

            const auto own_it = input->images.find(options.top_level);
            if (own_it == input->images.end() || own_it->second.image.is_empty())
                return frame; // degrade - top_level itself wasn't rasterized

            const BLImage &top_own_image = own_it->second.image;
            const int width = top_own_image.width();
            const int height = top_own_image.height();

            BLImage final_image(width, height, BL_FORMAT_PRGB32);
            BLContext ctx(final_image);
            ctx.clear_all();

            std::unordered_map<HierarchyId, BLImage, HierarchyIdHash> composed_cache;
            draw_node_and_children(ctx, options.top_level, own_it->second, *input->culled, input->images, composed_cache, options.scale);

            // Overlay passes - each drawn directly onto the already-fully-
            // composed context, last, rather than as a separate stage/node
            // (see draw_drag_rect_overlay's own doc comment for why). As
            // more of these accumulate (select/edit highlighting, hover,
            // ruler - the Hot-tier gap this class's own doc comment names),
            // each gets its own similarly-scoped function and its own call
            // here, rather than one function's worth of inline drawing
            // logic per layer.
            draw_drag_rect_overlay(ctx, options, height);
            draw_selection_overlay(ctx, options, height);
            draw_hover_overlay(ctx, options, height);
            draw_move_ghost_overlay(ctx, options, height);
            draw_ruler_overlay(ctx, options, height);
            draw_cursor_overlay(ctx, options, height);

            ctx.end();

            // Blend2D's own BL_FORMAT_PRGB32 is premultiplied BGRA in
            // memory on this little-endian target (confirmed against
            // RasterizeBlend2DStage's own earlier finding, before this
            // format-conversion step existed at all), while the external
            // LePixelBuffer contract (api.hpp's own doc comment, and
            // independently le_tcl_shim.cpp's dump_png_cmd, which tags
            // this exact buffer kRGBA_8888_SkColorType) requires literal
            // RGBA order - so an explicit per-pixel channel swap is
            // unavoidable here; there's no BLFormat that natively yields
            // RGBA order to sidestep it with a format choice alone.
            BLImageData image_data;
            if (final_image.get_data(&image_data) == BL_SUCCESS)
            {
                const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
                frame.pixel_data.resize(row_bytes * static_cast<std::size_t>(height));

                const auto *src_base = static_cast<const uint8_t *>(image_data.pixel_data);
                for (int y = 0; y < height; ++y)
                {
                    const uint8_t *src_row = src_base + static_cast<std::ptrdiff_t>(y) * image_data.stride;
                    uint8_t *dst_row = frame.pixel_data.data() + static_cast<std::size_t>(y) * row_bytes;
                    for (int x = 0; x < width; ++x)
                    {
                        const uint8_t *src_px = src_row + static_cast<std::ptrdiff_t>(x) * 4;
                        uint8_t *dst_px = dst_row + static_cast<std::size_t>(x) * 4;
                        dst_px[0] = src_px[2]; // R <- B
                        dst_px[1] = src_px[1]; // G <- G
                        dst_px[2] = src_px[0]; // B <- R
                        dst_px[3] = src_px[3]; // A <- A
                    }
                }

                frame.buffer = PixelBuffer{
                    .data = frame.pixel_data.data(),
                    .width = width,
                    .height = height,
                    .row_bytes = row_bytes,
                };
                frame.empty = false;
            }
            return frame;
        }

        /// @brief The drag rectangle changes on every mouse-move during a
        /// drag, independent of whatever RasterizeBlend2DStage produced
        /// (dragging a selection/zoom rectangle never itself changes any
        /// rasterized content) - without this override, MemoizingStage's
        /// own default (tbb_core.hpp, "false": recompute only when this
        /// stage's own InputData identity changes) would mean the ghost
        /// rectangle never actually updates while the mouse moves, only
        /// on the next unrelated recompute. Compares drag_rect_dbu/
        /// drag_is_zoom field-by-field (Rect/Point have no operator== in
        /// this codebase) rather than any other ViewRenderOptions field -
        /// those are already covered by however they affect
        /// RasterizeBlend2DStage's own version(), which
        /// ViewRenderPipeline::run() already cascades through
        /// would_recompute() before ever reaching this stage.
        bool options_did_change(const ViewRenderOptions &last, const ViewRenderOptions &current) const override
        {
            if (last.drag_rect_dbu.has_value() != current.drag_rect_dbu.has_value())
                return true;
            if (last.drag_rect_dbu.has_value())
            {
                const Rect &a = *last.drag_rect_dbu;
                const Rect &b = *current.drag_rect_dbu;
                if (a.ll.x != b.ll.x || a.ll.y != b.ll.y || a.ur.x != b.ur.x || a.ur.y != b.ur.y)
                    return true;
            }
            if (last.drag_is_zoom != current.drag_is_zoom)
                return true;

            if (last.selection_version != current.selection_version)
                return true;

            if (last.mouse_version != current.mouse_version)
                return true;

            return last.ruler_version != current.ruler_version;
        }

    private:
        /// @brief Draws the select/zoom rubber-band drag-rectangle ghost
        /// overlay (ViewRenderOptions::drag_rect_dbu/drag_is_zoom's own
        /// doc comment) directly onto `ctx` - a no-op when no drag is in
        /// progress. Drawn last, directly here, rather than as a separate
        /// stage/node: it's cheap (a fill + a stroke, no rasterization of
        /// its own to cache) and this way the whole ghost-rectangle
        /// feature lives in ComposeStage's own one graph, matching the
        /// pre-restart design's own choice to draw the equivalent mouse
        /// overlay as one final un-rasterized pass rather than a cached
        /// picture (src/pipelines.old/stages/compose_stage.hpp, git
        /// history).
        ///
        /// `pixel_height` is `ctx`'s own image's pixel height (the
        /// caller's own `height` - exactly `options.viewport` rasterized
        /// at `options.scale`, RasterizeBlend2DStage's own convention for
        /// `id == options.top_level`), so mapping `drag_rect_dbu` (in
        /// that same top_level-local dbu space) into pixel space needs
        /// only that one scale/origin, the same translate+scale+y-flip
        /// convention used everywhere else in this module.
        static void draw_drag_rect_overlay(BLContext &ctx, const ViewRenderOptions &options, int pixel_height)
        {
            if (!options.drag_rect_dbu.has_value())
                return;

            const Rect &drag = *options.drag_rect_dbu;
            const auto to_pixel_x = [&](int64_t dbu_x)
            { return static_cast<double>(dbu_x - options.viewport.ll.x) * options.scale; };
            const auto to_pixel_y = [&](int64_t dbu_y)
            { return static_cast<double>(pixel_height) - static_cast<double>(dbu_y - options.viewport.ll.y) * options.scale; };

            const double left = to_pixel_x(drag.ll.x);
            const double top = to_pixel_y(drag.ur.y);
            const double right = to_pixel_x(drag.ur.x);
            const double bottom = to_pixel_y(drag.ll.y);
            const BLRect rect(left, top, right - left, bottom - top);

            const Color &fill_color = options.drag_is_zoom ? kZoomDragRectFillColor : kDragRectFillColor;
            const Color &stroke_color = options.drag_is_zoom ? kZoomDragRectStrokeColor : kDragRectStrokeColor;

            ctx.set_fill_style(to_bl_color(fill_color));
            ctx.fill_rect(rect);

            ctx.set_stroke_style(to_bl_color(stroke_color));
            ctx.set_stroke_width(kDragRectStrokeWidth);
            ctx.stroke_rect(rect);
        }

        /// @brief Draws a white outline (UPDATES.md item 7) around every
        /// currently-selected piece's own geometry
        /// (`ViewRenderOptions::selected_piece_outlines`, already resolved
        /// to dbu-space `Shape`s by the caller - api.cpp's own
        /// view_render_options_for) - a no-op when nothing is selected.
        /// Same `to_pixel` mapping/pixel-space reasoning as
        /// `draw_drag_rect_overlay` above (`ctx` has no ambient transform
        /// here), and the same shared `stroke_piece_outline` helper
        /// (`draw_helpers.hpp`) `draw_hover_overlay`/`draw_move_ghost_overlay`
        /// will reuse too, once those land.
        static void draw_selection_overlay(BLContext &ctx, const ViewRenderOptions &options, int pixel_height)
        {
            if (options.selected_piece_outlines.empty())
                return;

            const auto to_pixel = [&](Point p)
            {
                return BLPoint(
                    static_cast<double>(p.x - options.viewport.ll.x) * options.scale,
                    static_cast<double>(pixel_height) - static_cast<double>(p.y - options.viewport.ll.y) * options.scale);
            };

            ctx.set_stroke_style(to_bl_color(kSelectionOutlineColor));
            ctx.set_stroke_width(kSelectionOutlineStrokeWidth);
            for (const Shape &piece : options.selected_piece_outlines)
                stroke_piece_outline(ctx, piece, to_pixel);
        }

        /// @brief Draws a yellow outline (UPDATES.md 7.1) around the
        /// currently-hovered piece's own geometry
        /// (`ViewRenderOptions::hover_outline_dbu`, already resolved by
        /// the caller - api.cpp's own `le_set_mouse_position`) - a no-op
        /// when nothing is hovered, including whenever hover isn't a
        /// meaningful affordance at all (Ruler/Edit mode, or no mouse
        /// position set) - `hover_outline_dbu` is nullopt in every such
        /// case, so this function itself doesn't need to know about mode.
        static void draw_hover_overlay(BLContext &ctx, const ViewRenderOptions &options, int pixel_height)
        {
            if (!options.hover_outline_dbu.has_value())
                return;

            const auto to_pixel = [&](Point p)
            {
                return BLPoint(
                    static_cast<double>(p.x - options.viewport.ll.x) * options.scale,
                    static_cast<double>(pixel_height) - static_cast<double>(p.y - options.viewport.ll.y) * options.scale);
            };

            ctx.set_stroke_style(to_bl_color(kHoverOutlineColor));
            ctx.set_stroke_width(kHoverOutlineStrokeWidth);
            stroke_piece_outline(ctx, *options.hover_outline_dbu, to_pixel);
        }

        /// @brief Draws the live Move gesture's own dashed, translucent
        /// ghost preview (UPDATES.md item 21) -
        /// `ViewRenderOptions::move_ghost_pieces_dbu`, each translated by
        /// `move_ghost_offset_dbu` in dbu space *before* mapping to pixels
        /// (so the preview traces the exact geometry Move would actually
        /// commit, not a pixel-space translation of the already-projected
        /// outline) - a no-op when nothing is being moved.
        static void draw_move_ghost_overlay(BLContext &ctx, const ViewRenderOptions &options, int pixel_height)
        {
            if (options.move_ghost_pieces_dbu.empty())
                return;

            const Point &offset = options.move_ghost_offset_dbu;
            const auto to_pixel = [&](Point p)
            {
                p.x += offset.x;
                p.y += offset.y;
                return BLPoint(
                    static_cast<double>(p.x - options.viewport.ll.x) * options.scale,
                    static_cast<double>(pixel_height) - static_cast<double>(p.y - options.viewport.ll.y) * options.scale);
            };

            BLArray<double> dash_array;
            dash_array.append(kMoveGhostDashOnPx);
            dash_array.append(kMoveGhostDashOffPx);

            ctx.set_stroke_style(to_bl_color(kMoveGhostColor));
            ctx.set_stroke_width(kMoveGhostStrokeWidth);
            ctx.set_stroke_dash_array(dash_array);
            ctx.set_stroke_dash_offset(0.0);
            for (const Shape &piece : options.move_ghost_pieces_dbu)
                stroke_piece_outline(ctx, piece, to_pixel);
            ctx.set_stroke_dash_array(BLArray<double>()); // don't leak the dash state into any overlay drawn after this one
        }

        /// @brief Draws a small red box (UPDATES.md 7.1 item 1) centered
        /// on the grid-snapped mouse position
        /// (`ViewRenderOptions::cursor_snapped_position_dbu`) - a no-op if
        /// no mouse position has been set. Shown regardless of mode
        /// (unlike `draw_hover_overlay` above) - the cursor marker is
        /// meant to be visible at all times a position is known, matching
        /// the pre-restart `draw_cursor`'s own doc comment.
        static void draw_cursor_overlay(BLContext &ctx, const ViewRenderOptions &options, int pixel_height)
        {
            if (!options.cursor_snapped_position_dbu.has_value())
                return;

            const Point &snapped = *options.cursor_snapped_position_dbu;
            const double cx = static_cast<double>(snapped.x - options.viewport.ll.x) * options.scale;
            const double cy = static_cast<double>(pixel_height) - static_cast<double>(snapped.y - options.viewport.ll.y) * options.scale;
            const double half = kCursorBoxSizePx / 2.0;
            const BLRect rect(cx - half, cy - half, kCursorBoxSizePx, kCursorBoxSizePx);

            ctx.set_stroke_style(to_bl_color(kCursorBoxColor));
            ctx.set_stroke_width(kCursorBoxStrokeWidth);
            ctx.stroke_rect(rect);
        }

        /// @brief Draws one ruler segment (UPDATES.md item 13): the line
        /// itself, a point marker at each end (`p1`'s only if not a ghost -
        /// the ghost's own leading end is already marked by
        /// `draw_cursor_overlay`'s own snap box), dynamic major/minor tick
        /// marks measured relative to the segment's own start (tape-
        /// measure semantics, not the absolute background grid), and the
        /// segment's own point-to-point distance label near `p1` - ported
        /// term-for-term from pipelines.old/draw_helpers.hpp's own
        /// draw_ruler_segment. Perpendicular direction is computed in
        /// *pixel* space so tick length/spacing reads consistently on
        /// screen regardless of the segment's own angle - a free-form
        /// (non-orthogonal) segment gets the exact same treatment as an
        /// orthogonal one, no special-casing needed. No-op if the segment
        /// is degenerate (zero on-screen length).
        ///
        /// Every label (tick values, this segment's own distance, and -
        /// via a separate call from draw_ruler_overlay below - a ruler's
        /// own running total) always draws in solid `kRulerColor`,
        /// regardless of `is_ghost` - only the line/points/ticks
        /// themselves switch to the translucent `kRulerGhostColor` for
        /// the live segment, matching the pre-restart version's own
        /// behavior exactly (its own draw_ruler_label never took an
        /// is_ghost parameter at all).
        template <typename ToPixel>
        void draw_ruler_segment(
            BLContext &ctx, const ToPixel &to_pixel, double dbu_per_um, Point p0_dbu, Point p1_dbu, bool is_ghost,
            const MonospaceFontEntry &font_entry, int font_key)
        {
            const BLPoint p0_px = to_pixel(p0_dbu);
            const BLPoint p1_px = to_pixel(p1_dbu);

            const double seg_dx = p1_px.x - p0_px.x;
            const double seg_dy = p1_px.y - p0_px.y;
            const double seg_len = std::sqrt(seg_dx * seg_dx + seg_dy * seg_dy);
            if (seg_len <= 0.0)
                return;
            const double ux = seg_dx / seg_len;
            const double uy = seg_dy / seg_len;
            const double perp_x = -uy;
            const double perp_y = ux;

            const BLRgba32 line_color = to_bl_color(is_ghost ? kRulerGhostColor : kRulerColor);

            ctx.set_stroke_style(line_color);
            ctx.set_stroke_width(kRulerStrokeWidth);
            ctx.stroke_line(BLLine(p0_px.x, p0_px.y, p1_px.x, p1_px.y));

            ctx.set_fill_style(line_color);
            ctx.fill_circle(BLCircle(p0_px.x, p0_px.y, kRulerPointRadius));
            if (!is_ghost)
                ctx.fill_circle(BLCircle(p1_px.x, p1_px.y, kRulerPointRadius));

            const double dx_dbu = static_cast<double>(p1_dbu.x - p0_dbu.x);
            const double dy_dbu = static_cast<double>(p1_dbu.y - p0_dbu.y);
            const double length_um = std::sqrt(dx_dbu * dx_dbu + dy_dbu * dy_dbu) / dbu_per_um;

            auto draw_label = [&](BLPoint anchor_px, double offset_px, const std::string &text)
            {
                if (!font_entry.font.is_valid() || font_entry.cell_width <= 0.0 || text.empty())
                    return;
                const double text_width = font_entry.cell_width * static_cast<double>(text.size());
                const double x0 = perp_x < 0.0 ? -text_width : 0.0;
                const BLPoint origin(anchor_px.x + perp_x * offset_px + x0, anchor_px.y + perp_y * offset_px);
                draw_monospace_label_blend2d(ctx, font_entry, font_key, glyph_bitmap_cache_, text, to_bl_color(kRulerColor), origin);
            };

            const double pixels_per_um = seg_len / (length_um > 0.0 ? length_um : 1.0);
            const double major_um = ruler_major_tick_spacing_um(pixels_per_um);
            const double minor_um = major_um / 10.0;
            const bool major_visible = major_um * pixels_per_um >= kMinRulerMajorTickPixelSpacing;
            const bool minor_visible = minor_um * pixels_per_um >= kMinRulerMinorTickPixelSpacing;

            if (minor_um > 0.0 && length_um > 0.0)
            {
                const int64_t max_k = static_cast<int64_t>(std::floor(length_um / minor_um));
                for (int64_t k = 1; k <= max_k; ++k)
                {
                    const bool is_major = (k % 10 == 0);
                    if (is_major ? !major_visible : !minor_visible)
                        continue;

                    const double t = (static_cast<double>(k) * minor_um) / length_um;
                    const BLPoint tick_center(p0_px.x + t * seg_dx, p0_px.y + t * seg_dy);
                    const double half_len = (is_major ? kRulerMajorTickLengthPx : kRulerMinorTickLengthPx) / 2.0;

                    ctx.set_stroke_style(line_color);
                    ctx.set_stroke_width(kRulerStrokeWidth);
                    ctx.stroke_line(BLLine(
                        tick_center.x + perp_x * half_len, tick_center.y + perp_y * half_len,
                        tick_center.x - perp_x * half_len, tick_center.y - perp_y * half_len));

                    if (is_major)
                    {
                        const int decimals = std::max(0, -static_cast<int>(std::floor(std::log10(minor_um))));
                        draw_label(tick_center, half_len + 4.0, fmt::format("{:.{}f}", static_cast<double>(k) * minor_um, decimals));
                    }
                }
            }

            draw_label(p1_px, kRulerMajorTickLengthPx, fmt::format("{:.3f} um", length_um));
        }

        /// @brief Draws every ruler (`ViewRenderOptions::ruler_polylines_dbu`)
        /// plus the live, not-yet-committed segment
        /// (`ruler_ghost_point_dbu`) - a no-op if `ruler_dbu_per_um` isn't
        /// available (no Technology yet). Ported from pipelines.old's own
        /// `draw_ruler_polyline` (each committed polyline, plus - once it
        /// has 2+ points - a "total: " running-length label at its own
        /// last point, offset to the opposite side from that last
        /// segment's own distance label so the two don't overlap) and
        /// `MouseOverlayStage`'s own live-ghost-segment gating (only the
        /// *last* polyline can have an active ghost extending it, matching
        /// `LeHandle::ruler_next_point`'s own "the last entry is the
        /// active ruler" invariant).
        void draw_ruler_overlay(BLContext &ctx, const ViewRenderOptions &options, int pixel_height)
        {
            if (options.ruler_dbu_per_um <= 0.0)
                return;

            const auto to_pixel = [&](Point p)
            {
                return BLPoint(
                    static_cast<double>(p.x - options.viewport.ll.x) * options.scale,
                    static_cast<double>(pixel_height) - static_cast<double>(p.y - options.viewport.ll.y) * options.scale);
            };

            const BLFontFace &font_face = default_blend2d_font_face();
            const int font_key = std::max(1, static_cast<int>(std::lround(options.ruler_label_size_px)));
            auto font_it = monospace_font_cache_.find(font_key);
            if (font_it == monospace_font_cache_.end())
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
                font_it = monospace_font_cache_.emplace(font_key, std::move(entry)).first;
            }
            const MonospaceFontEntry &font_entry = font_it->second;

            for (const std::vector<Point> &polyline : options.ruler_polylines_dbu)
            {
                if (polyline.size() < 2)
                    continue;

                double total_um = 0.0;
                for (std::size_t i = 0; i + 1 < polyline.size(); ++i)
                {
                    const double dx = static_cast<double>(polyline[i + 1].x - polyline[i].x);
                    const double dy = static_cast<double>(polyline[i + 1].y - polyline[i].y);
                    total_um += std::sqrt(dx * dx + dy * dy) / options.ruler_dbu_per_um;
                }

                for (std::size_t i = 0; i + 1 < polyline.size(); ++i)
                    draw_ruler_segment(ctx, to_pixel, options.ruler_dbu_per_um, polyline[i], polyline[i + 1], /*is_ghost=*/false, font_entry, font_key);

                const Point &last = polyline.back();
                const Point &second_last = polyline[polyline.size() - 2];
                const BLPoint last_px = to_pixel(last);

                const double seg_dx = static_cast<double>(last.x - second_last.x);
                const double seg_dy = static_cast<double>(last.y - second_last.y);
                const double seg_len = std::sqrt(seg_dx * seg_dx + seg_dy * seg_dy);
                const double perp_x = seg_len > 0.0 ? -(seg_dy / seg_len) : 0.0;
                const double perp_y = seg_len > 0.0 ? (seg_dx / seg_len) : 1.0;

                // Opposite side from the last segment's own point-to-point
                // distance label (drawn inside draw_ruler_segment, offset
                // along +perp) - two labels sharing the same side and a
                // small offset gap can overlap once text width is
                // accounted for; opposite sides never compete for the
                // same space.
                if (font_entry.font.is_valid() && font_entry.cell_width > 0.0)
                {
                    const std::string text = fmt::format("total: {:.3f} um", total_um);
                    const double text_width = font_entry.cell_width * static_cast<double>(text.size());
                    const double x0 = (-perp_x) < 0.0 ? -text_width : 0.0;
                    const BLPoint origin(
                        last_px.x + (-perp_x) * kRulerMajorTickLengthPx + x0,
                        last_px.y + (-perp_y) * kRulerMajorTickLengthPx);
                    draw_monospace_label_blend2d(ctx, font_entry, font_key, glyph_bitmap_cache_, text, to_bl_color(kRulerColor), origin);
                }
            }

            // The live ghost segment - only ever extends the *last*
            // polyline's own last point (LeHandle::ruler_next_point's own
            // "the last entry is the active ruler" invariant); api.cpp
            // only ever populates ruler_ghost_point_dbu when that
            // invariant holds, but this stays defensive rather than
            // assuming a non-empty options.ruler_polylines_dbu.
            if (options.ruler_ghost_point_dbu.has_value() && !options.ruler_polylines_dbu.empty() && !options.ruler_polylines_dbu.back().empty())
            {
                const Point &from = options.ruler_polylines_dbu.back().back();
                draw_ruler_segment(ctx, to_pixel, options.ruler_dbu_per_um, from, *options.ruler_ghost_point_dbu, /*is_ghost=*/true, font_entry, font_key);
            }
        }

        /// @brief Draws `own`'s own image onto `ctx` (already at
        /// `own.image`'s own pixel dimensions), then each of `id`'s own
        /// surviving placements' fully-composed child image on top, at
        /// the position/orientation its own transform implies.
        static void draw_node_and_children(
            BLContext &ctx, const HierarchyId &id, const RasterizedImage &own,
            const HierarchyResolverOutput &culled,
            const std::unordered_map<HierarchyId, RasterizedImage, HierarchyIdHash> &rasterized,
            std::unordered_map<HierarchyId, BLImage, HierarchyIdHash> &composed_cache,
            double scale)
        {
            ctx.blit_image(BLPoint(0, 0), own.image);

            const auto view_data_it = culled.view_data.find(id);
            if (view_data_it == culled.view_data.end())
                return;

            for (const ViewPlacementData &placement : view_data_it->second.placement_data)
            {
                const BLImage child_image = compose_node(placement.id, culled, rasterized, composed_cache, scale);
                if (child_image.is_empty())
                    continue; // degrade - child wasn't rasterized (e.g. RasterizeBlend2DStage's own pixel-dimension clamp)

                const auto child_raw_it = rasterized.find(placement.id);
                if (child_raw_it == rasterized.end())
                    continue;

                // ctx's own image is a plain, untransformed pixel canvas
                // (own.image was just drawn onto it at raw pixel (0,0),
                // not through any dbu transform) - so
                // child_image_to_parent_dbu_matrix alone isn't enough
                // here, unlike RasterizeBlend2DStage's own context (which
                // has a persistent dbu-to-pixel transform already active
                // before any shape is drawn). One further step,
                // dbu_to_pixel_matrix (own's own local_origin, the
                // PARENT's own dbu origin - not the child's), converts
                // the child matrix's own parent-dbu output into this
                // context's own actual pixel space, composed once via
                // combine_outer_after_inner rather than two separate
                // ctx.set_transform() calls.
                const BLMatrix2D child_to_parent_dbu = child_image_to_parent_dbu_matrix(
                    placement.transform, child_raw_it->second.local_origin, child_image.height(), scale);
                const BLMatrix2D parent_dbu_to_pixel = dbu_to_pixel_matrix(own.local_origin, own.image.height(), scale);
                const BLMatrix2D combined = combine_outer_after_inner(parent_dbu_to_pixel, child_to_parent_dbu);

                ctx.save();
                ctx.set_transform(combined);
                ctx.blit_image(BLPoint(0, 0), child_image);
                ctx.restore();
            }
        }

        /// @brief `id`'s own fully-composed image (its own
        /// RasterizeBlend2DStage image, with every surviving child drawn
        /// on top, recursively) - memoized in `composed_cache` for the
        /// duration of one Compose call, so a shared id reached via more
        /// than one placement is composited once. Returns an empty
        /// BLImage if `id` was never rasterized at all (degrade, don't
        /// crash - the caller just skips drawing this child).
        static BLImage compose_node(
            const HierarchyId &id, const HierarchyResolverOutput &culled,
            const std::unordered_map<HierarchyId, RasterizedImage, HierarchyIdHash> &rasterized,
            std::unordered_map<HierarchyId, BLImage, HierarchyIdHash> &composed_cache,
            double scale)
        {
            if (const auto it = composed_cache.find(id); it != composed_cache.end())
                return it->second;

            const auto rasterized_it = rasterized.find(id);
            if (rasterized_it == rasterized.end() || rasterized_it->second.image.is_empty())
                return BLImage();

            const RasterizedImage &own = rasterized_it->second;
            const auto view_data_it = culled.view_data.find(id);
            const bool has_placements = view_data_it != culled.view_data.end() && !view_data_it->second.placement_data.empty();
            if (!has_placements)
            {
                // No children to draw on top - this node's own already-
                // rasterized image already IS its own final composed
                // image, so hand it back directly (a cheap refcounted
                // BLImage copy), not a redundant re-draw onto an
                // identical fresh image.
                composed_cache.emplace(id, own.image);
                return own.image;
            }

            BLImage result(own.image.width(), own.image.height(), BL_FORMAT_PRGB32);
            BLContext ctx(result);
            ctx.clear_all();
            draw_node_and_children(ctx, id, own, culled, rasterized, composed_cache, scale);
            ctx.end();

            composed_cache.emplace(id, result);
            return result;
        }

        /// @brief The same dbu-to-pixel transform RasterizeBlend2DStage
        /// bakes into its own per-node context (translate(0,H); scale(S,
        /// -S); translate(-origin.x,-origin.y), one matrix instead of
        /// three separate calls) - expressed standalone here because
        /// draw_node_and_children's own context, unlike
        /// RasterizeBlend2DStage's, has NO transform active (own.image is
        /// drawn as raw pixels at (0,0), matching its own already-
        /// rasterized pixel dimensions exactly) - so mapping a child's
        /// own transform-derived parent-dbu point the rest of the way
        /// into this context's own actual pixel space needs this step
        /// spelled out explicitly, composed with
        /// child_image_to_parent_dbu_matrix below via
        /// combine_outer_after_inner.
        ///
        /// BLMatrix2D's own (m00, m01, m10, m11, m20, m21) constructor
        /// maps a point as `x' = x*m00 + y*m10 + m20; y' = x*m01 +
        /// y*m11 + m21` (core/matrix.h) - a different field-role
        /// convention from Skia's SkMatrix::setAll(a,b,c,d,e,f,...)
        /// (`x'=a*x+b*y+c; y'=d*x+e*y+f`) this was ported from, so each
        /// term below is placed by matching the actual x'/y' equation,
        /// not by copying setAll's own argument positions.
        static BLMatrix2D dbu_to_pixel_matrix(Point origin, int pixel_height, double scale)
        {
            return BLMatrix2D(
                scale, 0.0,
                0.0, -scale,
                -scale * static_cast<double>(origin.x), scale * static_cast<double>(origin.y) + static_cast<double>(pixel_height));
        }

        /// @brief The matrix mapping a pixel (u, v) of a CHILD's own
        /// rasterized image to the corresponding point in the PARENT's
        /// own dbu space, given the placement's own InstanceTransform
        /// (maps a point in the child's own dbu space into the parent's)
        /// and the child image's own local_origin (the dbu point at its
        /// own bottom-left pixel corner, RasterizedImage's own comment).
        /// Composed with dbu_to_pixel_matrix above (that composition's
        /// own comment) rather than concatenated alone, since this
        /// matrix's own output (parent dbu) isn't directly drawable pixel
        /// space by itself.
        ///
        /// Derivation: child image pixel (u, v) -> child dbu is itself an
        /// affine map (child dbu.x = local_origin.x + u/scale; child
        /// dbu.y = local_origin.y + (child_pixel_height - v)/scale - the
        /// same y-flip RasterizeBlend2DStage's own context setup applies,
        /// run backward), composed with the placement's own {linear,
        /// translation} (Geometry::InstanceTransform - see Geometry::compose's
        /// own doc comment for the same "outer applied to inner" shape,
        /// one level of nesting only here). Worked in plain doubles, not
        /// Geometry::apply_linear/Point (whose int64_t fields can't
        /// represent the fractional child_pixel_height/scale term).
        static BLMatrix2D child_image_to_parent_dbu_matrix(
            const Geometry::InstanceTransform &transform, Point child_local_origin, int child_pixel_height, double scale)
        {
            const double a = static_cast<double>(transform.linear.a);
            const double b = static_cast<double>(transform.linear.b);
            const double c = static_cast<double>(transform.linear.c);
            const double d = static_cast<double>(transform.linear.d);

            const double origin_x = static_cast<double>(child_local_origin.x);
            const double origin_y = static_cast<double>(child_local_origin.y) + static_cast<double>(child_pixel_height) / scale;

            const double translation_x = a * origin_x + b * origin_y + static_cast<double>(transform.translation.x);
            const double translation_y = c * origin_x + d * origin_y + static_cast<double>(transform.translation.y);

            return BLMatrix2D(
                a / scale, -b / scale,
                c / scale, -d / scale,
                translation_x, translation_y);
        }

        /// @brief Composes two BLMatrix2D in the same "apply inner first,
        /// then outer" sense as Skia's SkMatrix::Concat(outer, inner) -
        /// derived from scratch and verified by direct substitution
        /// (result.map_point(x,y) == outer.map_point(inner.map_point(x,y))
        /// for arbitrary x,y) rather than relying on Blend2D's own
        /// matrix-multiply/transform API without being certain of its
        /// exact composition-order convention - a silent sign/order error
        /// here would mis-position every nested/rotated placement, so
        /// this was worked out and checked by hand instead of guessed.
        static BLMatrix2D combine_outer_after_inner(const BLMatrix2D &outer, const BLMatrix2D &inner)
        {
            return BLMatrix2D(
                inner.m00 * outer.m00 + inner.m01 * outer.m10,
                inner.m00 * outer.m01 + inner.m01 * outer.m11,
                inner.m10 * outer.m00 + inner.m11 * outer.m10,
                inner.m10 * outer.m01 + inner.m11 * outer.m11,
                inner.m20 * outer.m00 + inner.m21 * outer.m10 + outer.m20,
                inner.m20 * outer.m01 + inner.m21 * outer.m11 + outer.m21);
        }

        // Ruler-label glyph rendering (draw_ruler_overlay/draw_ruler_segment
        // above) reuses RasterizeBlend2DStage's own monospace-glyph-bitmap-
        // cache machinery (rasterize_blend2d_stage.hpp) - a separate
        // instance here, not shared with any RasterizeBlend2DStage in the
        // same pipeline, same "own private cache, not shared across stage
        // types" convention that stage's own path_outline_cache_by_node_
        // already uses. Bounded the same way that stage's own caches are
        // (GlyphBitmapCacheKey's own doc comment): ruler_label_size_px is
        // one user-wide setting, not per-shape variable, so this adds at
        // most a small, fixed handful of extra distinct font sizes.
        std::unordered_map<int, MonospaceFontEntry> monospace_font_cache_;
        std::unordered_map<GlyphBitmapCacheKey, CachedGlyphBitmap, GlyphBitmapCacheKeyHash> glyph_bitmap_cache_;
    };
}
