#pragma once
#include <cstdint>

namespace le
{
    /// @brief Common interface for the leaf (no-upstream-of-its-own) shape-
    /// generation stages - GenerateAbstractShapesStage and
    /// GenerateLayoutShapesStage (Migration Step 3). Every downstream stage
    /// (FilterByViewportAndSizeStage, TinyShapesByViewportStage) only ever
    /// needs its upstream's own version() to compose its own cache key -
    /// never the concrete run() call itself (that stays non-virtual on each
    /// concrete class, called directly by Pipeline, which always knows
    /// which concrete type - and which id type, AbstractId or LayoutId -
    /// it's holding). This lets one shared FilterByViewportAndSizeStage/
    /// TinyShapesByViewportStage implementation serve both the Abstract
    /// and Layout paths without templating or duplicating either - they
    /// were never really "Abstract-only" in behavior, only incidentally
    /// typed that way.
    class ShapeGenerationStage
    {
    public:
        virtual ~ShapeGenerationStage() = default;
        virtual uint64_t version() const = 0;
        virtual uint64_t call_count() const = 0;
    };
}
