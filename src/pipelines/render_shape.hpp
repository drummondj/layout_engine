#pragma once

#include "../database/database.hpp"

namespace le
{
    /// @brief The render-cache-specific analog of ShapeData
    /// (generated/database/shape.hpp) - HierarchyResolverStage's own cache
    /// (ViewLayerShapes) holds these instead of the full database Shape.
    /// Keeps only the 4 fields any pipeline stage downstream of
    /// HierarchyResolverStage ever reads (RasterizeBlend2DStage's own
    /// draw_view_shapes_blend2d, Geometry::bbox/get_label_location/
    /// local_width_at) - dropped relative to ShapeData: the 7 ownership
    /// ids + .layer + .purpose (grouping into ViewLayerShapes by
    /// ViewLayerId already carries that information - see
    /// resolve_view_layer, hierarchy_resolver_stage.hpp), the three
    /// *_iterates vectors (always consumed and cleared by expand_iterates
    /// before a Shape reaches the cache), the three *_masks vectors,
    /// .spacing/.design_rule_width/.except_pg_net, and .vias/.via_iterates
    /// (always consumed and expanded into sibling Shapes by
    /// append_via_shapes, via_shapes.hpp, before a Shape reaches the
    /// cache) - none read by any pipeline stage. Deliberately NOT a
    /// per-geometry-kind tagged union/variant: a single Shape routinely
    /// carries more than one kind at once (a placement-name Shape's own
    /// index-paired rects+texts; a LEF PORT/OBS LAYER clause's own
    /// multiple RECT/POLYGON statements; a via's own synthesized
    /// rects+polygons), so a union can't represent what's actually
    /// produced today.
    ///
    /// sizeof(RenderShape) == 32 bytes (4 CompactVectors, one pointer each,
    /// the same type ShapeData's lists use, so to_render_shape moves them)
    /// - HierarchyResolverStage's cache of these dominates process memory.
    /// The static_assert below exists specifically to catch a future field
    /// addition silently eroding this saving - if it fires, that's a
    /// deliberate choice to re-check, not a bug to just relax.
    struct RenderShape
    {
        CompactVector<Rect> rects;
        CompactVector<Polygon> polygons;
        CompactVector<Path> paths;
        CompactVector<Text> texts;
    };

    static_assert(sizeof(RenderShape) == 4 * sizeof(CompactVector<Rect>),
                  "RenderShape grew beyond its 4 one-pointer lists - "
                  "re-check whether the new field is genuinely needed by a "
                  "downstream pipeline consumer before adding it (see this "
                  "struct's own doc comment for the field-usage audit).");

    /// @brief Converts an already-fully-resolved Shape (expand_iterates +
    /// append_via_shapes + view-layer resolution already applied) into its
    /// lean cache form - call this at the LAST moment before a shape
    /// enters HierarchyResolverStage's own cache, never earlier: every
    /// other pass over a Shape (expand_iterates, resolve_view_layer,
    /// append_via_shapes) still needs the full ShapeData's other fields.
    /// Takes `shape` by value so a caller holding an rvalue (the common
    /// case - a local Shape/temporary built just for this push) moves its
    /// 4 kept vectors in for free; a caller with only a `const Shape&`
    /// pays exactly one copy of 4 vectors - strictly cheaper than copying
    /// all 12 the way the pre-existing `push_back(*shape)` call sites did.
    inline RenderShape to_render_shape(Shape shape)
    {
        return RenderShape{
            .rects = std::move(shape.rects),
            .polygons = std::move(shape.polygons),
            .paths = std::move(shape.paths),
            .texts = std::move(shape.texts),
        };
    }
}
