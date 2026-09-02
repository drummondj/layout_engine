#pragma once
#include "../../core/rendered_shape.hpp"
#include "../../database/database.hpp"
#include "../../geometry/geometry.hpp"
#include "../../scene/scene.hpp"
#include "../../view_style/view_style.hpp"
#include "../draw_helpers.hpp"
#include "../pipeline_options.hpp"
#include "../pixel_types.hpp"
#include "../synchronous_stage_runner.hpp"
#include "build_layout_picture_stage.hpp"
#include "layer_visibility_filter_stage.hpp"
#include "viewport_filter_stage.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkRect.h"
#include <map>
#include <vector>

namespace le
{
    /// @brief Shared core of "cull already-generated dbu-space content to
    /// an enclosing viewport, filter by real layer visibility, transform
    /// to pixel space, draw own-content + instances + tiny-instance
    /// outlines, record an SkPicture" - the tail shared by every
    /// HierarchyResolver graph node (HierarchyAbstractLeafStage's own
    /// Abstract content, HierarchyLayoutNodeStage's own Layout content)
    /// and by render_layout_frame's own un-nodeified top-level picture.
    /// Moved verbatim out of the old recursive
    /// HierarchyResolver::record_local_picture (see git history for the
    /// pre-graph version) - same content_bbox expansion over dbu_shapes,
    /// same two-Scene cull-vs-visibility trick below.
    inline sk_sp<SkPicture> record_local_picture(
        const std::vector<RenderedShape> &dbu_shapes, uint64_t geometry_data_version,
        SynchronousStageRunner<ViewportFilterStage, std::vector<RenderedShape>, std::vector<RenderedShape>> &viewport_runner,
        SynchronousStageRunner<LayerVisibilityFilterStage, std::vector<RenderedShape>, std::map<ViewLayerId, std::vector<RenderedShape>>> &layer_runner,
        const ViewLayerSet &view_layers, const Scene &scene, double scale,
        const std::vector<BuildLayoutPictureStage::ResolvedInstance> &instances, const std::vector<PixelRect> &tiny_instance_rects, Rect declared_bbox, Point local_origin = Point{0, 0},
        // Empty for every caller except build_top_layout_picture
        // (hierarchy_resolver.hpp) - see HierarchyLayoutNodeStage's own
        // constructor comment for why placement name labels
        // (BUGS_AND_ENHANCEMENTS.md E13) are deliberately not threaded
        // through any recursively-cached node the way tiny_instance_rects
        // is.
        const std::vector<PlacementLabel> &placement_labels = {})
    {
        ZoneScopedN("HierarchyResolver: record_local_picture");
        Rect content_bbox = declared_bbox;
        for (const RenderedShape &rs : dbu_shapes)
        {
            const std::optional<Rect> shape_bbox = Geometry::bbox(rs.shape);
            if (!shape_bbox)
                continue;
            content_bbox.ll.x = std::min(content_bbox.ll.x, shape_bbox->ll.x);
            content_bbox.ll.y = std::min(content_bbox.ll.y, shape_bbox->ll.y);
            content_bbox.ur.x = std::max(content_bbox.ur.x, shape_bbox->ur.x);
            content_bbox.ur.y = std::max(content_bbox.ur.y, shape_bbox->ur.y);
        }

        // A throwaway Scene whose only job is giving ViewportFilterStage
        // a viewport rect that fully encloses content_bbox at `scale`,
        // so nothing real gets culled - not the real Scene's own
        // current pan/viewport. +2px margin absorbs floating-point
        // rounding at the exact boundary.
        Scene cull_scene;
        cull_scene.set_scale(scale);
        cull_scene.set_pan(content_bbox.ll);
        const double width_dbu = static_cast<double>(content_bbox.ur.x - content_bbox.ll.x);
        const double height_dbu = static_cast<double>(content_bbox.ur.y - content_bbox.ll.y);
        cull_scene.set_viewport_size(static_cast<int>(width_dbu * scale) + 2, static_cast<int>(height_dbu * scale) + 2);

        // Viewport culling uses cull_scene (the throwaway enclosing
        // viewport above) - but layer VISIBILITY must use the real
        // `scene`, not cull_scene: FilterByLayerVisibilityStage checks
        // Scene::is_view_layer_visible against the user's actual layer
        // show/hide toggles, which a fresh cull_scene has no knowledge of
        // (every layer defaults to visible on a just-constructed Scene) -
        // using cull_scene here would silently ignore every real
        // visibility toggle for anything drawn inside a cached instance
        // picture. Two different Scenes for two different purposes, not
        // a copy-paste slip.
        PipelineOptions viewport_options;
        viewport_options.ctx.scene = &cull_scene;
        // scene.viewport_version() (the REAL Scene passed in), not
        // cull_scene's own - cull_scene is a fresh Scene constructed
        // every single call, mutated via the exact same three set_scale/
        // set_pan/set_viewport_size calls in the same order every time,
        // so its own viewport_version() is a CONSTANT across every call
        // to record_local_picture regardless of how much the real scale
        // actually changed. ViewportFilterStage::options_did_change
        // compares this value specifically to decide whether to re-cull
        // at the new scale (see its own doc comment: "recomputing
        // whenever [data_version or scene.viewport_version()] moves") -
        // with the constant from cull_scene, that check always saw "no
        // change" after the first call for a given data_version, so the
        // sub-pixel cull decision made on the very first render of a
        // Layout got permanently frozen at whatever scale was active
        // then, never revisited on any later zoom/pan - a real bug (not
        // hypothetical: reproduced by zooming in 15x past the original
        // scale within one session and the culled content still didn't
        // reappear), only escaping notice because it only bites content
        // near the sub-pixel threshold when the FIRST render after a
        // design/database change happens at an unusually small scale.
        viewport_options.viewport.viewport_version = scene.viewport_version();
        viewport_options.viewport.scale = scale;

        PipelineOptions layer_options;
        layer_options.ctx.scene = &scene;
        layer_options.ctx.view_layers = &view_layers;
        layer_options.viewport.visibility_version = scene.visibility_version();

        const std::vector<RenderedShape> &viewport_filtered = viewport_runner.run(dbu_shapes, geometry_data_version, viewport_options);
        // viewport_runner.last_version() (bumped only when ViewportFilterStage
        // actually recomputed - see its own doc comment: "its own
        // data_version arrives automatically as whatever ViewportFilterStage's
        // node last emitted"), not geometry_data_version again - the two
        // runners aren't wired via a real make_edge here (SynchronousStageRunner,
        // not a graph edge), so layer_runner has no way to know
        // viewport_filtered's own content changed unless told via its
        // own data_version input. Passing geometry_data_version straight
        // through (unchanged whenever only the live scale/viewport
        // changed, not the underlying database) made LayerVisibilityFilterStage
        // treat every later scale change as a no-op cache hit, silently
        // re-serving the FIRST scale's own filtered/culled shape set
        // forever after - the second half of the same bug class as
        // viewport_options.viewport.viewport_version above, one stage
        // further downstream.
        const std::map<ViewLayerId, std::vector<RenderedShape>> &layer_filtered = layer_runner.run(viewport_filtered, viewport_runner.last_version(), layer_options);
        const auto pixel_shapes = transform_shapes_to_pixel_space(layer_filtered, local_origin, scale);

        const SkRect bounds = SkRect::MakeLTRB(
            static_cast<SkScalar>(static_cast<double>(content_bbox.ll.x - local_origin.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ll.y - local_origin.y) * scale),
            static_cast<SkScalar>(static_cast<double>(content_bbox.ur.x - local_origin.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ur.y - local_origin.y) * scale));

        // Labels aren't real Shapes (see placement_labels's own default-
        // argument comment above), so they never reach
        // LayerVisibilityFilterStage the way layer_filtered above did -
        // this is the PLACEMENT_NAME row's own visibility checkbox
        // actually taking effect, using the real `scene` (not cull_scene,
        // same reasoning as layer_options above).
        const bool placement_names_visible = scene.is_view_layer_visible("PLACEMENT_NAME", ViewLayerPurpose::PLACEMENT_NAME);
        return BuildLayoutPictureStage::run(bounds, pixel_shapes, instances, tiny_instance_rects, placement_labels, view_layers, scene.antialiasing_enabled(), placement_names_visible);
    }
}
