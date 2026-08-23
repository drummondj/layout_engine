#pragma once
#include "../core/rendered_shape.hpp"
#include "../database/database.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include "stages/filter_by_layer_visibility_stage.hpp"
#include "stages/filter_by_viewport_and_size_stage.hpp"
#include "stages/generate_abstract_shapes_stage.hpp"
#include "stages/generate_layout_shapes_stage.hpp"
#include "stages/tiny_shapes_by_layer_visibility_stage.hpp"
#include "stages/tiny_shapes_by_viewport_stage.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace le
{
    /// @brief Owns one instance of each stage class above and chains them
    /// into two parallel pairs of data-flow paths (Migration Step 3) - one
    /// pair for a Scene's currently displayed Abstract, one for its
    /// currently displayed Layout - construct one per Scene-equivalent
    /// lifetime and reuse it across repeated calls (e.g. every interactive
    /// frame); a fresh instance recomputes everything on its first call.
    ///
    ///   GenerateAbstractShapesStage -> FilterByViewportAndSizeStage -> FilterByLayerVisibilityStage   (run())
    ///   GenerateAbstractShapesStage -> TinyShapesByViewportStage -> TinyShapesByLayerVisibilityStage   (run_tiny_shapes())
    ///   GenerateLayoutShapesStage   -> FilterByViewportAndSizeStage -> FilterByLayerVisibilityStage   (run_layout())
    ///   GenerateLayoutShapesStage   -> TinyShapesByViewportStage -> TinyShapesByLayerVisibilityStage   (run_layout_tiny_shapes())
    ///
    /// FilterByViewportAndSizeStage/FilterByLayerVisibilityStage/
    /// TinyShapesByViewportStage/TinyShapesByLayerVisibilityStage have
    /// nothing Abstract- or Layout-specific in their own logic (see
    /// ShapeGenerationStage's own comment) - each is simply instantiated
    /// twice, once per path, rather than templated or duplicated under a
    /// different name.
    ///
    /// Every public method below keeps the exact signature it had before
    /// UPDATES.md item 16's refactor (a `const Root&`/`const ViewLayerSet&`
    /// parameter some methods no longer use in their own body - see e.g.
    /// filter_by_viewport_and_size - is kept anyway, deliberately, so
    /// every existing caller - api.cpp, pipeline_test.cpp,
    /// pipeline_benchmark.cpp - needed zero changes); each is now a
    /// one-line delegate to its corresponding stage member. See each stage
    /// class's own doc comment (in src/pipeline/stages/) for its cache key
    /// and why it's shaped that way - this class no longer needs to
    /// document them itself, which is the whole point: a stage's caching
    /// concern lives with that stage, not scattered across whichever class
    /// happens to own it.
    ///
    /// UPDATES.md item 16 also asked for "each stage class in it's own
    /// .hpp file" - the stage classes plus RenderedShape/TinyShapeDot,
    /// ShapeGenerationStage, and VersionedStage each live in their own file
    /// under src/pipeline/stages/ (the first three) or src/core/ (the
    /// last two); this file stays a thin aggregator (`#include` of all of
    /// them, plus this Pipeline class) so every existing consumer that
    /// `#include`s pipeline.hpp keeps working unchanged - `pipeline` is a
    /// header-only (INTERFACE) CMake library, so splitting the file needed
    /// no build-system changes either.
    class Pipeline
    {
    public:
        const std::vector<RenderedShape> &generate_shapes(const Root &root, AbstractId abstract_id, const ViewLayerSet &view_layers)
        {
            return generated_abstract_.run(root, abstract_id, view_layers);
        }

        const std::vector<RenderedShape> &generate_layout_shapes(const Root &root, LayoutId layout_id, const ViewLayerSet &view_layers)
        {
            return generated_layout_.run(root, layout_id, view_layers);
        }

        const std::vector<RenderedShape> &filter_by_viewport_and_size(const Root &root, const std::vector<RenderedShape> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            (void)root;
            (void)view_layers;
            return viewport_filtered_abstract_.run(generated_abstract_, shapes, scene);
        }

        const std::vector<RenderedShape> &filter_by_viewport_and_size_layout(const Root &root, const std::vector<RenderedShape> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            (void)root;
            (void)view_layers;
            return viewport_filtered_layout_.run(generated_layout_, shapes, scene);
        }

        const std::map<ViewLayerId, std::vector<RenderedShape>> &filter_by_layer_visibility(const Root &root, const std::vector<RenderedShape> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            (void)root;
            return layer_filtered_abstract_.run(viewport_filtered_abstract_, shapes, scene, view_layers);
        }

        const std::map<ViewLayerId, std::vector<RenderedShape>> &filter_by_layer_visibility_layout(const Root &root, const std::vector<RenderedShape> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            (void)root;
            return layer_filtered_layout_.run(viewport_filtered_layout_, shapes, scene, view_layers);
        }

        const std::vector<TinyShapeDot> &tiny_shapes_by_viewport(const Root &root, AbstractId abstract_id, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &shapes = generate_shapes(root, abstract_id, view_layers);
            return tiny_shapes_viewport_filtered_abstract_.run(generated_abstract_, shapes, scene, view_layers);
        }

        const std::vector<TinyShapeDot> &tiny_shapes_by_viewport_layout(const Root &root, LayoutId layout_id, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &shapes = generate_layout_shapes(root, layout_id, view_layers);
            return tiny_shapes_viewport_filtered_layout_.run(generated_layout_, shapes, scene, view_layers);
        }

        const std::map<ViewLayerId, std::vector<Point>> &tiny_shapes_by_layer_visibility(const Root &root, const std::vector<TinyShapeDot> &tiny_shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            (void)root;
            return tiny_shapes_layer_filtered_abstract_.run(tiny_shapes_viewport_filtered_abstract_, tiny_shapes, scene, view_layers);
        }

        const std::map<ViewLayerId, std::vector<Point>> &tiny_shapes_by_layer_visibility_layout(const Root &root, const std::vector<TinyShapeDot> &tiny_shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            (void)root;
            return tiny_shapes_layer_filtered_layout_.run(tiny_shapes_viewport_filtered_layout_, tiny_shapes, scene, view_layers);
        }

        /// @brief Run all three Abstract-path stages for the Scene's
        /// current_abstract().
        const std::map<ViewLayerId, std::vector<RenderedShape>> &run(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &generated = generate_shapes(root, scene.current_abstract(), view_layers);
            const auto &viewport_filtered = filter_by_viewport_and_size(root, generated, scene, view_layers);
            return filter_by_layer_visibility(root, viewport_filtered, scene, view_layers);
        }

        /// @brief Run all three Layout-path stages for `layout_id` -
        /// mirrors `run` above; only the top-level Layout's own direct
        /// content (rows/tracks/gcell grids/blockages/routes/physical
        /// ports/die area - see GenerateLayoutShapesStage's own comment) -
        /// placed-instance rendering is a separate, picture-cache-based
        /// mechanism (Migration Step 3 Phase B), not part of this
        /// RenderedShape-map path at all. Takes `layout_id` directly rather
        /// than reading a `scene.current_layout()`-style accessor -
        /// `Scene` gains that concept in Phase C, which will update this
        /// method's own signature then; kept Phase A self-contained
        /// (no `Scene` changes) until that phase.
        const std::map<ViewLayerId, std::vector<RenderedShape>> &run_layout(const Root &root, LayoutId layout_id, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &generated = generate_layout_shapes(root, layout_id, view_layers);
            const auto &viewport_filtered = filter_by_viewport_and_size_layout(root, generated, scene, view_layers);
            return filter_by_layer_visibility_layout(root, viewport_filtered, scene, view_layers);
        }

        /// @brief Run both tiny-shape stages for the Scene's
        /// current_abstract() - mirrors `run` above for the parallel
        /// sub-pixel-dot path (UPDATES.md item 6, see TinyShapeDot's own
        /// comment for why this is a separate chain rather than folded
        /// into `run`'s own).
        const std::map<ViewLayerId, std::vector<Point>> &run_tiny_shapes(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &tiny_shapes = tiny_shapes_by_viewport(root, scene.current_abstract(), scene, view_layers);
            return tiny_shapes_by_layer_visibility(root, tiny_shapes, scene, view_layers);
        }

        /// @brief Run both tiny-shape stages for `layout_id` - mirrors
        /// `run_tiny_shapes` above; see `run_layout`'s own comment for why
        /// `layout_id` is an explicit parameter rather than a
        /// `scene.current_layout()`-style accessor for now.
        const std::map<ViewLayerId, std::vector<Point>> &run_layout_tiny_shapes(const Root &root, LayoutId layout_id, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &tiny_shapes = tiny_shapes_by_viewport_layout(root, layout_id, scene, view_layers);
            return tiny_shapes_by_layer_visibility_layout(root, tiny_shapes, scene, view_layers);
        }

        /// @brief Topmost-layer-first point hit-test (UPDATES.md 7.1 items
        /// 1-3) against an already-filtered map (typically `run`'s own
        /// output - already viewport-culled and visibility-filtered, so
        /// this only ever scans what's actually on screen, not the whole
        /// design). Not a VersionedStage-backed stage like the classes
        /// above - `dbu_point` changes on every call, so there's no
        /// reusable output to cache. Reverse-iterates `shapes` (its
        /// std::map key order is bottom-up stacking order - see
        /// FilterByLayerVisibilityStage's own comment - so reverse means
        /// topmost first), skipping a ViewLayer the Scene has made
        /// unselectable (Scene::is_view_layer_selectable) and any
        /// RenderedShape with no `origin` (the BOUNDARY shape - not
        /// selectable). Returns the first hit's origin plus a copy of
        /// just the single rect/polygon/path piece that was actually hit
        /// (Geometry::find_hit_piece) - not the whole RenderedShape's
        /// Shape, which can bundle several rects/polygons/paths together
        /// (e.g. several RECTs in one LEF PORT); hovering must highlight
        /// only the one piece under the cursor, not the whole group.
        /// First-match-within-a-layer wins for two overlapping shapes on
        /// the same layer, an accepted MVP limitation. nullopt if nothing
        /// was hit. Works identically against either `run`'s or
        /// `run_layout`'s own output - a Layout's own direct content
        /// (rows/tracks/etc.) is selectable the same way an Abstract's
        /// is; placed-instance (Placement) selection is a separate,
        /// bbox-only mechanism (Migration Step 3 Phase B), not this path.
        static std::optional<HoverTarget> hit_test_point(const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const ViewLayerSet &view_layers, const Scene &scene, Point dbu_point)
        {
            for (auto it = shapes.rbegin(); it != shapes.rend(); ++it)
            {
                const ViewLayerData *data = view_layers.get(it->first);
                if (data && !scene.is_view_layer_selectable(data->layer_name, data->purpose))
                    continue;

                for (const auto &rs : it->second)
                {
                    if (!rs.origin)
                        continue;

                    if (auto piece = Geometry::find_hit_piece(rs.shape, dbu_point))
                        return HoverTarget{.origin = *rs.origin, .outline = piece->outline, .shape_id = rs.shape_id};
                }
            }

            return std::nullopt;
        }

        /// @brief Rubber-band enclosure hit-test (UPDATES.md 7.1 item 5:
        /// "all selectable shapes on all layers completely enclosed by
        /// the selection rectangle") against an already-filtered map
        /// (typically `run`'s own output). Unlike hit_test_point, scans
        /// every layer (no topmost-only restriction - rule 5 says "all
        /// layers") and collects every match, in no particular order.
        /// Same unselectable-layer/no-origin skip as hit_test_point.
        ///
        /// Piece-level, like hit_test_point (via Geometry::
        /// fully_enclosed_pieces rather than the coarser fully_enclosed) -
        /// one RenderedShape can bundle several rects/polygons/paths
        /// together (e.g. several RECT statements in one PORT), and a
        /// drag enclosing only some of them must report only those, not
        /// treat the whole bundle - and by extension every other piece
        /// sharing the same origin, however many there are - as one
        /// selected unit. Returns one HoverTarget per enclosed piece, so
        /// dragging over several disjoint pieces of the same object (e.g.
        /// a Terminal with multiple ports, or an Obstruction with many
        /// RECT/PATH/POLYGON items) yields one independently-selected
        /// entry per piece, exactly like shift-clicking each individually
        /// would (see Scene::select's dedup-by-(origin, piece)).
        static std::vector<HoverTarget> hit_test_rect(const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const ViewLayerSet &view_layers, const Scene &scene, Rect dbu_rect)
        {
            std::vector<HoverTarget> result;

            for (const auto &[view_layer_id, group] : shapes)
            {
                const ViewLayerData *data = view_layers.get(view_layer_id);
                if (data && !scene.is_view_layer_selectable(data->layer_name, data->purpose))
                    continue;

                for (const auto &rs : group)
                {
                    if (!rs.origin)
                        continue;

                    for (auto &piece : Geometry::fully_enclosed_pieces(dbu_rect, rs.shape))
                        result.push_back(HoverTarget{.origin = *rs.origin, .outline = std::move(piece.outline), .shape_id = rs.shape_id});
                }
            }

            return result;
        }

        // Number of times each stage actually recomputed - exposed purely
        // to make cache hits/misses observable in tests.
        uint64_t generate_calls() const { return generated_abstract_.call_count(); }
        uint64_t viewport_filter_calls() const { return viewport_filtered_abstract_.call_count(); }
        uint64_t layer_filter_calls() const { return layer_filtered_abstract_.call_count(); }
        uint64_t tiny_shapes_viewport_filter_calls() const { return tiny_shapes_viewport_filtered_abstract_.call_count(); }
        uint64_t tiny_shapes_layer_filter_calls() const { return tiny_shapes_layer_filtered_abstract_.call_count(); }

        uint64_t generate_layout_calls() const { return generated_layout_.call_count(); }
        uint64_t viewport_filter_layout_calls() const { return viewport_filtered_layout_.call_count(); }
        uint64_t layer_filter_layout_calls() const { return layer_filtered_layout_.call_count(); }
        uint64_t tiny_shapes_viewport_filter_layout_calls() const { return tiny_shapes_viewport_filtered_layout_.call_count(); }
        uint64_t tiny_shapes_layer_filter_layout_calls() const { return tiny_shapes_layer_filtered_layout_.call_count(); }

    private:
        GenerateAbstractShapesStage generated_abstract_;
        FilterByViewportAndSizeStage viewport_filtered_abstract_;
        FilterByLayerVisibilityStage layer_filtered_abstract_;
        TinyShapesByViewportStage tiny_shapes_viewport_filtered_abstract_;
        TinyShapesByLayerVisibilityStage tiny_shapes_layer_filtered_abstract_;

        GenerateLayoutShapesStage generated_layout_;
        FilterByViewportAndSizeStage viewport_filtered_layout_;
        FilterByLayerVisibilityStage layer_filtered_layout_;
        TinyShapesByViewportStage tiny_shapes_viewport_filtered_layout_;
        TinyShapesByLayerVisibilityStage tiny_shapes_layer_filtered_layout_;
    };
}
