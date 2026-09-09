#pragma once

#include "stages/hierarchy_resolver_stage.hpp"

#include "include/core/SkImage.h"

#include <memory>
#include <unordered_map>

namespace le
{
    /// @brief One node's own rasterized image - just its own direct
    /// `shapes` (a Rasterize stage never draws a node's own placements/
    /// children - ComposeStage composites those, PIPELINE_REFACTOR.md's
    /// own stage split). Shared by every Rasterize backend
    /// (RasterizeStage/Skia, RasterizeBlend2DStage/Blend2D, ...) - a
    /// non-Skia backend rasterizes into its own native surface type
    /// internally, then wraps its final pixel buffer into an `SkImage`
    /// here so ComposeStage (Skia-based) needs no changes at all
    /// regardless of which backend produced a given node's own image.
    struct RasterizedImage
    {
        sk_sp<SkImage> image;

        /// @brief The dbu point mapping to `image`'s own bottom-left
        /// pixel corner (image pixel (0, image->height())) - ComposeStage
        /// needs this to place the image correctly; an SkImage alone
        /// carries no notion of *where* in dbu space it represents.
        Point local_origin;
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

    /// @brief A Rasterize stage's own `OutputHandle` (MemoizingStage's own
    /// `shared_ptr<const OutputData>` alias, tbb_core.hpp) - identical
    /// regardless of which concrete Rasterize stage produced it, since
    /// every Rasterize backend instantiates the exact same
    /// `MemoizingStage<HierarchyResolverStage::OutputHandle, RasterizeOutput,
    /// ViewRenderOptions>`. Defined standalone here (rather than every
    /// consumer writing `RasterizeStage::OutputHandle`) so ComposeStage
    /// - and anything else downstream of a Rasterize stage - depends on
    /// this shared output header alone, never on which concrete backend
    /// (Skia, Blend2D, ...) is actually wired in.
    using RasterizeOutputHandle = std::shared_ptr<const RasterizeOutput>;
}
