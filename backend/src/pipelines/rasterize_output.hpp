#pragma once

#include "stages/hierarchy_resolver_stage.hpp"

#include <blend2d/blend2d.h>

#include <memory>
#include <unordered_map>

namespace le
{
    /// @brief One node's own rasterized image - just its own direct
    /// `shapes` (a Rasterize stage never draws a node's own placements/
    /// children - ComposeStage composites those, PIPELINE_REFACTOR.md's
    /// own stage split). `BLImage` owns its own pixel storage (refcounted,
    /// cheap to copy - Blend2D's own analog of `sk_sp<SkImage>`), the
    /// direct output of RasterizeBlend2DStage's own `BLContext` - no
    /// format conversion/wrapping needed now that ComposeStage composites
    /// natively in Blend2D too (RasterizeBlend2DStage is the only
    /// Rasterize backend; the generic Skia/Blend2D-swappable pipeline and
    /// this struct's own former `sk_sp<SkImage>`-wrapping design are gone,
    /// PIPELINE_REFACTOR_BENCHMARK_RESULTS.md).
    struct RasterizedImage
    {
        BLImage image;

        /// @brief The dbu point mapping to `image`'s own bottom-left
        /// pixel corner (image pixel (0, image.height())) - ComposeStage
        /// needs this to place the image correctly; a BLImage alone
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

    /// @brief RasterizeBlend2DStage's own `OutputHandle` (MemoizingStage's
    /// own `shared_ptr<const OutputData>` alias, tbb_core.hpp). Defined
    /// standalone here (rather than every consumer writing
    /// `RasterizeBlend2DStage::OutputHandle`) so ComposeStage - and
    /// anything else downstream of the Rasterize stage - depends on this
    /// shared output header alone.
    using RasterizeOutputHandle = std::shared_ptr<const RasterizeOutput>;
}
