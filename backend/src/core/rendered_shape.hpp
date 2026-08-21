#pragma once
#include "../database/database.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include <memory>
#include <optional>
#include <vector>

namespace le
{
    /// @brief A Shape resolved to the ViewLayer it belongs to. Lives in
    /// `core` (UPDATES.md item 16) rather than `pipeline` because it's
    /// `pipeline`'s output type but also `render`'s input type - both
    /// modules need it independently of each other.
    struct RenderedShape
    {
        Shape shape;
        ViewLayerId view_layer;

        /// The selectable object this Shape came from (UPDATES.md 7.1) -
        /// nullopt for the BOUNDARY shape, which isn't tied to any
        /// Terminal/Obstruction and isn't selectable.
        std::optional<SelectionRef> origin;

        /// The exact database ShapeId this RenderedShape was generated
        /// from (GenerateShapesStage pushes exactly one RenderedShape per
        /// ShapeId - see its own comment) - nullopt only for the
        /// synthetic BOUNDARY shape, which has no backing Shape row.
        /// Selection (Scene::SelectedObject) is keyed on this, not on
        /// `origin` - see scene.hpp's own comment for why selection is
        /// shape-granular.
        std::optional<ShapeId> shape_id;

        /// (*path_outlines)[i] == Geometry::path_to_polygons(shape.paths[i]) -
        /// each path's buffered outline (flat ends, miter joins),
        /// precomputed once here (pipeline's GenerateShapesStage is cached
        /// per-AbstractId, not per-frame - see its own doc comment) rather
        /// than in Renderer::transform_to_pixels (which reruns on every
        /// pan/zoom, viewport_version is in its cache key) or draw_group
        /// (which reruns on every build_picture recompute). Lets
        /// draw_group fill/outline a PATH the same way it already does a
        /// POLYGON - a real filled region with a thin boundary - instead
        /// of a solid stroke along the centerline, which used to read as
        /// an opaque block regardless of the layer's fill pattern (see
        /// BENCHMARKS.md).
        ///
        /// shared_ptr, not a plain vector: pipeline's
        /// FilterByViewportAndSizeStage copies surviving RenderedShapes
        /// wholesale on every pan/zoom (result.push_back(s)) - that was
        /// already true before this field existed (RenderedShape::shape's
        /// own rects/polygons/paths get copied the same way), but a plain
        /// vector-of-vector-of-Polygon here made every such copy
        /// meaningfully heavier (confirmed via BM_RunReused_PanOnly
        /// regressing ~8ms to ~50ms - see BENCHMARKS.md). A shared_ptr
        /// makes copying a RenderedShape cost the same O(1) refcount bump
        /// regardless of how much outline geometry a path-heavy Shape
        /// holds. Default member initializer (an empty vector, not null)
        /// means every RenderedShape - not just the ones
        /// GenerateShapesStage explicitly sets this for - has a safely
        /// dereferenceable path_outlines, including the BOUNDARY shape
        /// (no paths, never sets this) and every test that constructs a
        /// RenderedShape directly without mentioning it.
        std::shared_ptr<const std::vector<std::vector<Polygon>>> path_outlines = std::make_shared<const std::vector<std::vector<Polygon>>>();
    };

    /// @brief A shape too small to render normally (bbox under 1 pixel in
    /// both dimensions at the Scene's current scale - see pipeline's
    /// TinyShapesByViewportStage) - just enough to draw a single
    /// device-pixel dot so the user can see *something* is there, per
    /// UPDATES.md item 6. Deliberately not a RenderedShape: no `origin`
    /// (SelectionRef) at all, so Pipeline::hit_test_point/hit_test_rect
    /// (which only ever see the *normal* filter stages' output, never this
    /// type) can't select one even by accident - "not selectable" holds by
    /// construction, not by an exclusion check somewhere.
    struct TinyShapeDot
    {
        Point location; // dbu-space bbox center
        ViewLayerId view_layer;
    };
}
