#pragma once

#include "stages/hierarchy_resolver_stage.hpp"

#include <blend2d/blend2d.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace le
{
    /// @brief One node's own rasterized image - just its own direct
    /// `shapes` (a Rasterize stage never draws a node's own placements/
    /// children - ComposeStage composites those). `BLImage` owns its own
    /// pixel storage (refcounted, cheap to copy), the direct output of
    /// RasterizeBlend2DStage's own `BLContext` - ComposeStage composites
    /// it natively in Blend2D, with no format conversion.
    /// @brief A label on one of a node's drawn shapes, in the node's own
    /// dbu. Rasterize records it instead of drawing it; ComposeStage draws
    /// it upright, once per visible instance of the node, after
    /// compositing, so a rotated or mirrored placement (or view) never
    /// turns its text.
    struct NodeLabel
    {
        /// @brief Points into the node's chunk, which the RasterizeOutput's
        /// `culled` keeps alive.
        const std::string *text = nullptr;

        /// @brief The text's baseline-left origin; for a placement name,
        /// unused (`placement_box` places it).
        BLPoint anchor_dbu;

        /// @brief A placement name's placement rect: the name goes in its
        /// on-screen bottom-left corner, truncated to its on-screen width.
        std::optional<Rect> placement_box;

        /// @brief The font's pixel size, already clamped.
        int font_key = 0;
        BLRgba32 color;
    };

    struct RasterizedImage
    {
        BLImage image;

        /// @brief The dbu point mapping to `image`'s own bottom-left
        /// pixel corner (image pixel (0, image.height())) - ComposeStage
        /// needs this to place the image correctly; a BLImage alone
        /// carries no notion of *where* in dbu space it represents.
        Point local_origin;

        /// @brief The node's shapes on purposes not drawn under its
        /// placements (purpose_draws_under_placements), same size and
        /// origin as `image`, which then holds only the rest. ComposeStage
        /// draws it after the children, so a design's own wires and pins
        /// cover its cells' content. Empty when the node has no child
        /// content to go between, and `image` holds every shape.
        BLImage over_placements;

        /// @brief The node needed more than kMaxImageDimensionPx pixels at
        /// this zoom; `image` holds only its first kMaxImageDimensionPx.
        bool cropped = false;

        /// @brief The node's labels, on shapes this image draws.
        std::vector<NodeLabel> labels;
    };

    /// @brief A Rasterize stage's own output - one RasterizedImage per
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

    /// @brief RasterizeBlend2DStage's own `OutputHandle` (MemoizingStage's
    /// own `shared_ptr<const OutputData>` alias, tbb_core.hpp). Defined
    /// standalone here (rather than every consumer writing
    /// `RasterizeBlend2DStage::OutputHandle`) so ComposeStage - and
    /// anything else downstream of the Rasterize stage - depends on this
    /// shared output header alone.
    using RasterizeOutputHandle = std::shared_ptr<const RasterizeOutput>;
}
