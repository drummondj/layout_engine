#pragma once

#include "../pipeline_options.hpp"
#include "../tbb_core.hpp"
#include "hierarchy_resolver_stage.hpp"
#include "rasterize_stage.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace le
{
    /// @brief Raw RGBA8888 pixel view into a RasterizedFrame's own
    /// `surface` - mirrors api.hpp's LePixelBuffer contract exactly
    /// (top-left origin, y increasing downward, premultiplied alpha,
    /// row_bytes possibly exceeding width * 4 - always index by it) so
    /// wiring this up to le_render_pixel_buffer() later needs no format
    /// translation. `data` is a non-owning view into `surface`'s own
    /// backing memory - never valid on its own, see RasterizedFrame.
    struct PixelBuffer
    {
        const uint8_t *data = nullptr;
        int width = 0;
        int height = 0;
        std::size_t row_bytes = 0;
    };

    /// @brief ComposeStage's own output - the whole Warm tier's final
    /// image. Bundles PixelBuffer together with the SkSurface that owns
    /// its backing memory (PixelBuffer itself holds no ownership) so the
    /// pixels stay alive for as long as this OutputHandle (a shared_ptr,
    /// MemoizingStage's own convention) does. `empty` is true when
    /// nothing could be composed at all (top_level itself wasn't in
    /// RasterizeStage's own output - e.g. a null Root, or top_level
    /// exceeding RasterizeStage's own pixel-dimension sanity clamp) -
    /// distinguishes "genuinely nothing to show" from a real all-
    /// transparent frame, which `buffer.data == nullptr` alone wouldn't.
    struct RasterizedFrame
    {
        sk_sp<SkSurface> surface;
        PixelBuffer buffer;
        bool empty = true;
    };

    /// @brief Warm-tier stage 3 (PIPELINE_REFACTOR.md): composites every
    /// surviving node's own RasterizeStage image into one final image,
    /// walking the same placement_data ViewportCullStage already pruned
    /// to what's visible. A node with no placements just IS its own
    /// image (no compositing needed, no copy either - composed_cache
    /// below hands back the same sk_sp<SkImage> RasterizeStage produced);
    /// a node WITH placements gets its own fresh canvas, its own image
    /// drawn first, then each surviving child's own *fully composed*
    /// image (recursing depth-first via compose_node) drawn on top at
    /// the pixel position/orientation its own ViewPlacementData::transform
    /// implies - see child_image_to_parent_dbu_matrix's own comment for
    /// the exact derivation. Recursion depth is bounded by
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
    /// (ViewRenderOptions::scale's own doc comment, RasterizeStage's own
    /// doc comment): a placement whose own orientation isn't N bakes that
    /// rotation/reflection into the composited pixels of its own child
    /// image, including any text labels drawn within it by RasterizeStage
    /// - a rotated placement's own labels render rotated/mirrored too,
    /// unlike the pre-restart SkPicture-based design (UprightTextCanvas,
    /// git history), which corrected this at replay time regardless of
    /// nesting. Deferred, not fixed here - a real trade-off of this
    /// design choice to weigh in the planned before/after comparison
    /// against the SkPicture approach, not an oversight.
    class ComposeStage : public MemoizingStage<RasterizeStage::OutputHandle, RasterizedFrame, ViewRenderOptions>
    {
    public:
        explicit ComposeStage(oneapi::tbb::flow::graph &g, std::string label = "Compose")
            : MemoizingStage(g, std::move(label)) {}

    protected:
        RasterizedFrame compute(const RasterizeStage::OutputHandle &input, const ViewRenderOptions &options) override
        {
            RasterizedFrame frame;
            if (input == nullptr || input->culled == nullptr)
                return frame;

            const auto own_it = input->images.find(options.top_level);
            if (own_it == input->images.end() || !own_it->second.image)
                return frame; // degrade - top_level itself wasn't rasterized

            const sk_sp<SkImage> &top_own_image = own_it->second.image;
            const SkImageInfo info = SkImageInfo::Make(top_own_image->width(), top_own_image->height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType);
            sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
            if (!surface)
                return frame;

            SkCanvas *canvas = surface->getCanvas();
            canvas->clear(SK_ColorTRANSPARENT);

            std::unordered_map<HierarchyId, sk_sp<SkImage>, HierarchyIdHash> composed_cache;
            draw_node_and_children(*canvas, options.top_level, own_it->second, *input->culled, input->images, composed_cache, options.scale);

            SkPixmap pixmap;
            if (surface->peekPixels(&pixmap))
            {
                frame.buffer = PixelBuffer{
                    .data = static_cast<const uint8_t *>(pixmap.addr()),
                    .width = pixmap.width(),
                    .height = pixmap.height(),
                    .row_bytes = pixmap.rowBytes(),
                };
                frame.empty = false;
            }
            frame.surface = std::move(surface);
            return frame;
        }

    private:
        /// @brief Draws `own`'s own image onto `canvas` (already at
        /// `own.image`'s own pixel dimensions), then each of `id`'s own
        /// surviving placements' fully-composed child image on top, at
        /// the position/orientation its own transform implies.
        static void draw_node_and_children(
            SkCanvas &canvas, const HierarchyId &id, const RasterizedImage &own,
            const HierarchyResolverOutput &culled,
            const std::unordered_map<HierarchyId, RasterizedImage, HierarchyIdHash> &rasterized,
            std::unordered_map<HierarchyId, sk_sp<SkImage>, HierarchyIdHash> &composed_cache,
            double scale)
        {
            canvas.drawImage(own.image, 0, 0);

            const auto view_data_it = culled.view_data.find(id);
            if (view_data_it == culled.view_data.end())
                return;

            for (const ViewPlacementData &placement : view_data_it->second.placement_data)
            {
                const sk_sp<SkImage> child_image = compose_node(placement.id, culled, rasterized, composed_cache, scale);
                if (!child_image)
                    continue; // degrade - child wasn't rasterized (e.g. RasterizeStage's own pixel-dimension clamp)

                const auto child_raw_it = rasterized.find(placement.id);
                if (child_raw_it == rasterized.end())
                    continue;

                // canvas is a plain, untransformed pixel canvas (own.image
                // was just drawn onto it at raw pixel (0,0), not through
                // any dbu transform) - so child_image_to_parent_dbu_matrix
                // alone isn't enough here, unlike RasterizeStage's own
                // canvas (which has a persistent dbu-to-pixel transform
                // already active before any shape is drawn). One further
                // step, dbu_to_pixel_matrix (own's own local_origin, the
                // PARENT's own dbu origin - not the child's), converts the
                // child matrix's own parent-dbu output into this canvas's
                // own actual pixel space, composed once via
                // SkMatrix::Concat rather than two separate concat() calls.
                const SkMatrix child_to_parent_dbu = child_image_to_parent_dbu_matrix(
                    placement.transform, child_raw_it->second.local_origin, child_image->height(), scale);
                const SkMatrix parent_dbu_to_pixel = dbu_to_pixel_matrix(own.local_origin, own.image->height(), scale);
                const SkMatrix combined = SkMatrix::Concat(parent_dbu_to_pixel, child_to_parent_dbu);

                canvas.save();
                canvas.concat(combined);
                canvas.drawImage(child_image, 0, 0);
                canvas.restore();
            }
        }

        /// @brief `id`'s own fully-composed image (its own RasterizeStage
        /// image, with every surviving child drawn on top, recursively) -
        /// memoized in `composed_cache` for the duration of one Compose
        /// call, so a shared id reached via more than one placement is
        /// composited once. Returns nullptr if `id` was never rasterized
        /// at all (degrade, don't crash - the caller just skips drawing
        /// this child).
        static sk_sp<SkImage> compose_node(
            const HierarchyId &id, const HierarchyResolverOutput &culled,
            const std::unordered_map<HierarchyId, RasterizedImage, HierarchyIdHash> &rasterized,
            std::unordered_map<HierarchyId, sk_sp<SkImage>, HierarchyIdHash> &composed_cache,
            double scale)
        {
            if (const auto it = composed_cache.find(id); it != composed_cache.end())
                return it->second;

            const auto rasterized_it = rasterized.find(id);
            if (rasterized_it == rasterized.end() || !rasterized_it->second.image)
                return nullptr;

            const RasterizedImage &own = rasterized_it->second;
            const auto view_data_it = culled.view_data.find(id);
            const bool has_placements = view_data_it != culled.view_data.end() && !view_data_it->second.placement_data.empty();
            if (!has_placements)
            {
                // No children to draw on top - this node's own already-
                // rasterized image already IS its own final composed
                // image, so hand it back directly (a shared_ptr copy),
                // not a redundant re-draw onto an identical fresh canvas.
                composed_cache.emplace(id, own.image);
                return own.image;
            }

            const SkImageInfo info = SkImageInfo::Make(own.image->width(), own.image->height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType);
            sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
            if (!surface)
            {
                composed_cache.emplace(id, own.image); // degrade - own content only, no children
                return own.image;
            }

            SkCanvas *canvas = surface->getCanvas();
            canvas->clear(SK_ColorTRANSPARENT);
            draw_node_and_children(*canvas, id, own, culled, rasterized, composed_cache, scale);

            const sk_sp<SkImage> result = surface->makeImageSnapshot();
            composed_cache.emplace(id, result);
            return result;
        }

        /// @brief The same dbu-to-pixel transform RasterizeStage bakes
        /// into its own per-node canvas (translate(0,H); scale(S,-S);
        /// translate(-origin.x,-origin.y), one matrix instead of three
        /// canvas calls) - expressed standalone here because
        /// draw_node_and_children's own canvas, unlike RasterizeStage's,
        /// has NO transform active (own.image is drawn as raw pixels at
        /// (0,0), matching its own already-rasterized pixel dimensions
        /// exactly) - so mapping a child's own transform-derived parent-dbu
        /// point the rest of the way into this canvas's own actual pixel
        /// space needs this step spelled out explicitly, composed with
        /// child_image_to_parent_dbu_matrix below via SkMatrix::Concat.
        static SkMatrix dbu_to_pixel_matrix(Point origin, int pixel_height, double scale)
        {
            SkMatrix matrix;
            matrix.setAll(
                static_cast<SkScalar>(scale), 0, static_cast<SkScalar>(-scale * static_cast<double>(origin.x)),
                0, static_cast<SkScalar>(-scale), static_cast<SkScalar>(scale * static_cast<double>(origin.y) + pixel_height),
                0, 0, 1);
            return matrix;
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
        /// same y-flip RasterizeStage's own canvas setup applies, run
        /// backward), composed with the placement's own {linear,
        /// translation} (Geometry::InstanceTransform - see Geometry::compose's
        /// own doc comment for the same "outer applied to inner" shape,
        /// one level of nesting only here). Worked in plain doubles, not
        /// Geometry::apply_linear/Point (whose int64_t fields can't
        /// represent the fractional child_pixel_height/scale term).
        static SkMatrix child_image_to_parent_dbu_matrix(
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

            SkMatrix matrix;
            matrix.setAll(
                static_cast<SkScalar>(a / scale), static_cast<SkScalar>(-b / scale), static_cast<SkScalar>(translation_x),
                static_cast<SkScalar>(c / scale), static_cast<SkScalar>(-d / scale), static_cast<SkScalar>(translation_y),
                0, 0, 1);
            return matrix;
        }
    };
}
