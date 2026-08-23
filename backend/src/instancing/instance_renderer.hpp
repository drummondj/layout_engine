#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../pipeline/stages/filter_by_layer_visibility_stage.hpp"
#include "../pipeline/stages/filter_by_viewport_and_size_stage.hpp"
#include "../pipeline/stages/generate_abstract_shapes_stage.hpp"
#include "../pipeline/stages/generate_layout_shapes_stage.hpp"
#include "../render/draw_helpers.hpp"
#include "../render/pixel_types.hpp"
#include "../render/stages/build_layout_picture_stage.hpp"
#include "../render/stages/build_overlay_picture_stage.hpp"
#include "../render/stages/build_ruler_overlay_picture_stage.hpp"
#include "../render/stages/compose_with_overlays_stage.hpp"
#include "../render/stages/rasterize_stage.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRect.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <spdlog/spdlog.h>
#include <tuple>
#include <vector>

namespace le
{
    /// @brief Resolves `Placement -> Design` (Migration Step 3 Phase B)
    /// into cached, per-instance-transformable `SkPicture`s - the one
    /// place allowed to link both `pipeline` (to resolve an arbitrary
    /// child Design's own dbu-space RenderedShapes - Placement.
    /// reference_design can point at ANY Design in Root, not just the
    /// Scene's own currently-displayed one, which is all `render`'s own
    /// stage classes can structurally reach - see src/render/'s own
    /// CLAUDE.md bullet) and `render` (to build/compose the resulting
    /// SkPictures). See the Step 3 plan's own "Flagging for explicit
    /// sign-off" section for why this needed its own module rather than
    /// living inside `src/render/stages/` as first sketched.
    ///
    /// "Local pixel space": every cached picture here is recorded as
    /// `local_pixel = dbu_local * scale` - the content's own raw dbu
    /// coordinates scaled, with NO pan subtraction. This is what makes one
    /// cached picture replayable, unchanged, at every different placement
    /// location/orientation anywhere in the hierarchy (see
    /// to_instance_matrix's own comment in draw_helpers.hpp for why the
    /// algebra requires this exact convention) - every internal
    /// to_instance_matrix call in this class therefore always passes
    /// `parent_pan = Point{0, 0}` too, never scene.pan() - Phase C's own
    /// job is wiring the single outermost picture this produces into the
    /// real Scene's own pan/viewport at final rasterize time, not this
    /// class. It does mean a scale change (a zoom) invalidates every
    /// cached picture at once (see Epoch below), but a placement's own
    /// location/orientation never does. A known, accepted consequence
    /// (matches this project's own MVP framing, CLAUDE.md's own opening
    /// paragraph): raw dbu coordinates far from the origin lose SkScalar
    /// (32-bit float) precision at high zoom the same way any Skia
    /// coordinate would - not a new gap Phase B introduces, just one this
    /// design doesn't paper over with a hidden per-instance origin shift.
    ///
    /// Builds a fresh, call-local GenerateAbstractShapesStage/
    /// GenerateLayoutShapesStage/FilterByViewportAndSizeStage/
    /// FilterByLayerVisibilityStage set inside build_abstract_picture/
    /// build_layout_picture_uncached, rather than sharing either a
    /// caller's Pipeline or persistent members of its own - deliberately,
    /// not just for simplicity. This class's own real caching (the two
    /// {id, remaining_depth}-keyed maps below) already means
    /// build_abstract_picture/build_layout_picture_uncached are only ever
    /// reached on a genuine cache MISS, so a persistent VersionedStage
    /// here would never legitimately hit - and for
    /// build_layout_picture_uncached specifically, persistence would be
    /// actively unsafe: its own placement loop calls resolve_design_picture,
    /// which can recurse back into build_layout_picture_uncached for a
    /// DIFFERENT LayoutId (a nested sub-block) - a shared, persistent
    /// GenerateLayoutShapesStage's single VersionedStage slot would get
    /// overwritten by that recursive call, leaving the OUTER call's own
    /// `dbu_shapes` (a reference into that slot, captured before the loop)
    /// dangling by the time it's used afterward. A fresh local instance
    /// per call can't be evicted out from under itself this way, since a
    /// recursive call gets its own, entirely separate instance. The
    /// viewport-filter stage is driven by a throwaway internal Scene sized
    /// to fully enclose the content being recorded (not the real Scene's
    /// own current pan/viewport) - a cached, reusable instance picture
    /// must hold its FULL content regardless of the CURRENT viewport (it
    /// has to remain valid at every future pan/zoom until scale itself
    /// changes); the real Scene is still passed to
    /// FilterByLayerVisibilityStage, so a child instance's own layer
    /// visibility toggles (M1/OBSTRUCTION/etc.) still match the top-level
    /// view's own current settings.
    ///
    /// Also owns the final "compose a displayable frame" step for a
    /// Layout view (render_layout_frame, Migration Step 3 Phase C) -
    /// broadening this class's own scope slightly past pure instance-
    /// picture resolution, but kept here rather than in `Renderer` (which
    /// deliberately doesn't link `instancing`/`pipeline` - same boundary
    /// reasoning as everywhere else in this class) or duplicated ad hoc in
    /// `api.cpp` (which stays a thin C-API wrapper). Owns its own
    /// BuildOverlayPictureStage/BuildRulerOverlayPictureStage/
    /// RasterizeStage(x4)/ComposeWithOverlaysStage rather than reaching
    /// into a caller's `Renderer` for them: `Renderer::rasterize`/
    /// `compose_with_overlays` always key off `Renderer`'s own *Abstract*-
    /// path `build_picture_stage_.version()` internally, regardless of
    /// what picture is actually passed in - reusing those two methods with
    /// a Layout-sourced picture would silently rasterize/compose against a
    /// stale cache key untouched by anything InstanceRenderer does, the
    /// same class of staleness bug already found and fixed once in this
    /// file (see build_abstract_picture's own comment). `RasterizeStage`/
    /// `ComposeWithOverlaysStage` are both already generic over a caller-
    /// supplied version number for exactly this reason, so a second,
    /// independent set of instances here is the correct fix, not a
    /// workaround. Also owns its own BuildOverlayPictureStage/
    /// BuildRulerOverlayPictureStage rather than reading `Renderer`'s own
    /// output picture: `ComposeWithOverlaysStage::run` needs the *stage
    /// object* itself (for its own `.version()`, part of its cache key),
    /// not just the picture it last produced, and `Renderer`'s own
    /// instances are private - a small, harmless duplication (their
    /// content is Scene-only, never view-dependent, so caching it twice
    /// costs a little memory/CPU, not correctness).
    class InstanceRenderer
    {
    public:
        /// Resolves design_id's own current view - its Layout (recursed
        /// with remaining_depth - 1) if it has one AND remaining_depth >
        /// 0, otherwise its Abstract - into a cached local-pixel-space
        /// SkPicture. Returns an empty sk_sp if neither resolves (logged
        /// once per DesignId, not once per Placement - matters at
        /// 1,000,000 placements of one bad reference). remaining_depth is
        /// an explicit parameter, not read from Scene - see the Step 3
        /// plan's own Phase B "Scope boundary with Phase C" section.
        sk_sp<SkPicture> resolve_design_picture(const Root &root, DesignId design_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            ensure_epoch(root, view_layers, scene, scale);

            const auto key = std::tuple{design_id, remaining_depth};
            if (auto it = design_pictures_.find(key); it != design_pictures_.end())
                return it->second;

            sk_sp<SkPicture> picture = build_design_picture(root, design_id, remaining_depth, view_layers, scene, scale);
            design_pictures_.emplace(key, picture);
            return picture;
        }

        /// Builds/returns layout_id's own fully-composited local-pixel-space
        /// picture: its own direct content (GenerateLayoutShapesStage) plus,
        /// per Placement, its resolved reference_design's own picture
        /// (resolve_design_picture above) drawn through its own SkMatrix
        /// (Geometry::instance_transform + to_instance_matrix). Exposed
        /// directly (not only reachable via resolve_design_picture) so a
        /// caller that already has a LayoutId - a test, or Phase C's
        /// eventual top-level "currently viewed Layout" - can call it
        /// without needing an owning Design.
        sk_sp<SkPicture> build_layout_picture(const Root &root, LayoutId layout_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            ensure_epoch(root, view_layers, scene, scale);

            const auto key = std::tuple{layout_id, remaining_depth};
            if (auto it = layout_pictures_.find(key); it != layout_pictures_.end())
                return it->second;

            sk_sp<SkPicture> picture = build_layout_picture_uncached(root, layout_id, remaining_depth, view_layers, scene, scale);
            layout_pictures_.emplace(key, picture);
            return picture;
        }

        uint64_t design_picture_recompute_count() const { return recompute_count_; }

        /// @brief Renders the full displayable frame for a Layout view -
        /// mirrors Renderer::render's own role for the Abstract path, but
        /// for `layout_id`'s own resolved, hierarchical content instead.
        /// `hierarchy_depth` is Scene::hierarchy_depth() itself (the FULL
        /// depth budget, not yet decremented) - the top-level call always
        /// consumes one level on its own (it's already showing
        /// `layout_id`'s own content), so build_layout_picture is invoked
        /// with `max(0, hierarchy_depth - 1)`, matching the Step 3 plan's
        /// own Phase C recursion rule ("Top-level call: remaining_depth =
        /// hierarchy_depth - 1").
        ///
        /// Composes build_layout_picture's own local-pixel-space output
        /// (pan={0,0}) into the real Scene's own pixel space (grid drawn
        /// first, then the local picture placed via to_instance_matrix
        /// with an identity InstanceTransform - no rotation, this is the
        /// top view itself - so its own translation collapses to exactly
        /// `-scene.pan() * scene.scale()`, matching every other design
        /// picture's own `pixel = (dbu - pan) * scale` convention), then
        /// runs that through this class's own rasterize/compose chain
        /// (see this class's own doc comment for why it's a second,
        /// independent chain rather than reusing `Renderer`'s). No tiny-
        /// shapes or selection-overlay content yet for a Layout view (both
        /// pass an empty picture/map) - InstanceRenderer doesn't produce
        /// TinyShapeDots, and placement selection is explicitly deferred
        /// scope (Step 3's own "whole-placement only" decision) - a real,
        /// documented gap, not a silent omission.
        const PixelBuffer &render_layout_frame(const Root &root, LayoutId layout_id, int hierarchy_depth, const ViewLayerSet &view_layers, const Scene &scene)
        {
            const int remaining_depth = std::max(0, hierarchy_depth - 1);
            const sk_sp<SkPicture> local_picture = build_layout_picture(root, layout_id, remaining_depth, view_layers, scene, scene.scale());

            // A synthetic "did the frame's own inputs change" version -
            // recompute_count_ stands in for "did InstanceRenderer's own
            // cached content change" (coarser than layout_id's own
            // picture specifically, matching this class's whole-epoch
            // invalidation philosophy elsewhere); pan/viewport/scale are
            // this compose step's own additional inputs build_layout_picture's
            // own cache doesn't already account for (it's pan-independent
            // by design).
            const auto frame_key = std::tuple{layout_id, hierarchy_depth, recompute_count_, scene.pan().x, scene.pan().y, scene.viewport_width_px(), scene.viewport_height_px(), scene.scale()};
            design_frame_version_stage_.get(frame_key, [] { return 0; });
            const uint64_t design_version = design_frame_version_stage_.version();

            SkPictureRecorder recorder;
            SkCanvas *canvas = recorder.beginRecording(SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));
            draw_grid(*canvas, scene);
            if (local_picture)
            {
                const Geometry::InstanceTransform identity{.linear = {1, 0, 0, 1}, .translation = {0, 0}};
                canvas->save();
                canvas->concat(to_instance_matrix(identity, scene.pan(), scene.scale()));
                canvas->drawPicture(local_picture);
                canvas->restore();
            }
            const sk_sp<SkPicture> design_picture = recorder.finishRecordingAsPicture();

            const std::optional<double> dbu_per_um = technology_dbu_per_um(root);
            const sk_sp<SkPicture> &overlay_picture = build_overlay_picture_stage_.run(scene, dbu_per_um);
            const sk_sp<SkPicture> &ruler_overlay_picture = build_ruler_overlay_picture_stage_.run(scene, dbu_per_um);
            static const sk_sp<SkPicture> kEmptyPicture; // no tiny-shapes/selection content for a Layout view yet

            return compose_stage_.run(rasterize_design_stage_, rasterize_tiny_stage_, rasterize_selection_stage_, rasterize_ruler_stage_, build_overlay_picture_stage_,
                                       design_version, 0, 0, build_ruler_overlay_picture_stage_.version(),
                                       design_picture, kEmptyPicture, overlay_picture, kEmptyPicture, ruler_overlay_picture, scene);
        }

    private:
        struct Epoch
        {
            uint64_t root_mutation_version = 0;
            uint64_t view_layers_generation = 0;
            uint64_t visibility_version = 0;
            double scale = -1.0;

            bool operator==(const Epoch &) const = default;
        };

        // Whole-map clear (not per-entry invalidation) on any epoch
        // mismatch - root.mutation_version()/view_layers.generation()/
        // scene.visibility_version() are all global monotonic counters
        // with no existing per-entity dirty-tracking anywhere in this
        // codebase; this is the direct multi-entry generalization of a
        // single VersionedStage's own "any key mismatch discards the old
        // value." scale is part of the epoch (not the per-entry key)
        // specifically so a zoom-drag can't accumulate one new cache
        // generation per frame with no eviction.
        void ensure_epoch(const Root &root, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            const Epoch current{
                .root_mutation_version = root.mutation_version(),
                .view_layers_generation = view_layers.generation(),
                .visibility_version = scene.visibility_version(),
                .scale = scale,
            };
            if (current == epoch_)
                return;

            epoch_ = current;
            design_pictures_.clear();
            layout_pictures_.clear();
        }

        // Resolves design_id's own current view exactly like
        // build_design_picture below, but without recording anything -
        // used by a placing parent to get its own child's local_bbox for
        // Geometry::instance_transform's own orientation math. Mirrors
        // build_design_picture's own dispatch rule so the two can never
        // disagree about which view (Layout vs. Abstract) a given
        // {DesignId, remaining_depth} resolves to.
        static Rect resolved_local_bbox(const Root &root, DesignId design_id, int remaining_depth)
        {
            const LayoutId layout_id = root.get_design_layout(design_id);
            if (remaining_depth > 0 && layout_id.valid())
                return layout_declared_bbox(root, layout_id);

            const AbstractId abstract_id = root.get_design_abstract(design_id);
            if (abstract_id.valid())
                return abstract_declared_bbox(root, abstract_id);

            return Rect{};
        }

        static Rect layout_declared_bbox(const Root &root, LayoutId layout_id)
        {
            if (const Shape *diearea = root.get_shape(root.get_layout_diearea(layout_id)))
                if (auto b = Geometry::bbox(*diearea))
                    return *b;
            return Rect{};
        }

        // AbstractData.origin is deliberately NOT applied here - a known,
        // accepted gap, no ORIGIN-bearing test data driving it yet (see
        // Geometry::instance_transform's own doc comment).
        static Rect abstract_declared_bbox(const Root &root, AbstractId abstract_id)
        {
            const AbstractData *abstract = root.get_abstract(abstract_id);
            if (abstract && abstract->size)
                return Rect{.ll = Point{0, 0}, .ur = *abstract->size};

            if (const Shape *boundary = root.get_shape(root.get_abstract_boundary(abstract_id)))
                if (auto b = Geometry::bbox(*boundary))
                    return *b;

            return Rect{};
        }

        sk_sp<SkPicture> build_design_picture(const Root &root, DesignId design_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            const LayoutId layout_id = root.get_design_layout(design_id);
            if (remaining_depth > 0 && layout_id.valid())
                return build_layout_picture(root, layout_id, remaining_depth - 1, view_layers, scene, scale);

            const AbstractId abstract_id = root.get_design_abstract(design_id);
            if (abstract_id.valid())
                return build_abstract_picture(root, abstract_id, view_layers, scene, scale);

            if (unresolved_logged_.insert(design_id).second)
                spdlog::warn("InstanceRenderer: Placement references DesignId {} which resolves to neither an Abstract nor a Layout (or the Layout is unreachable at remaining_depth {}) - skipping every Placement of it", to_string(design_id), remaining_depth);
            return nullptr;
        }

        sk_sp<SkPicture> build_abstract_picture(const Root &root, AbstractId abstract_id, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            recompute_count_++;

            // Fresh, call-local stages, not persistent class members - see
            // this class's own comment on why the caching Pipeline's own
            // stage classes normally provide is illusory here (this
            // function is only ever reached on a real resolve_design_
            // picture/build_layout_picture cache MISS in the first place)
            // and, worse, actively wrong for FilterByViewportAndSizeStage
            // specifically: its own key composes `cull_scene.
            // viewport_version()`, but a fresh throwaway Scene's version
            // counter only reflects HOW MANY setters were called, not
            // WHAT was set - two calls with different `scale` (hence a
            // different sub-pixel-culling threshold, `min_visible_dbu` in
            // that stage's own code) but the same downstream generate-stage
            // version would land on the exact same {version, version}
            // key, wrongly reusing dbu-space output culled at the WRONG
            // scale. A member VersionedStage instance would have made this
            // a real, hard-to-see-in-review staleness bug rather than a
            // no-op - a fresh instance can't go stale since it has no
            // prior key to accidentally collide with.
            GenerateAbstractShapesStage generate_stage;
            FilterByViewportAndSizeStage viewport_stage;
            FilterByLayerVisibilityStage layer_stage;

            const auto &dbu_shapes = generate_stage.run(root, abstract_id, view_layers);
            return record_local_picture(generate_stage, viewport_stage, layer_stage, dbu_shapes, view_layers, scene, scale, {}, abstract_declared_bbox(root, abstract_id));
        }

        sk_sp<SkPicture> build_layout_picture_uncached(const Root &root, LayoutId layout_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale)
        {
            recompute_count_++;

            // Fresh, call-local stages (see build_abstract_picture's own
            // comment for the scale-staleness reason) - doubly required
            // here, not just an efficiency nicety: this function's own
            // placement loop below calls resolve_design_picture, which can
            // recurse back into build_layout_picture_uncached for a
            // DIFFERENT LayoutId (a nested sub-block). A persistent,
            // shared `generate_layout_` member would have its single
            // VersionedStage slot overwritten by that recursive call
            // before this (outer) call reaches its own record_local_picture
            // below - `dbu_shapes`, a reference bound to the slot's THEN-
            // current vector, would be left dangling (the reassigned
            // std::vector's old heap buffer freed) by the time it's
            // actually used. A real bug this class's own tests exercise
            // (TwoLevelHierarchyRecursesAndPlacesTheLeafRelativeToItsSubBlock,
            // where the sub-block is a distinct LayoutId from the top) -
            // it "worked" under a plain Debug build's own allocator
            // behavior, exactly the kind of UB that's silent until it
            // isn't (a sanitizer build, a different allocator, a future
            // change to VersionedStage's own storage). A fresh local
            // instance per call can't be evicted out from under itself by
            // a recursive call, since the recursive call gets its own,
            // entirely separate instance.
            GenerateLayoutShapesStage generate_stage;
            FilterByViewportAndSizeStage viewport_stage;
            FilterByLayerVisibilityStage layer_stage;

            const auto &dbu_shapes = generate_stage.run(root, layout_id, view_layers);

            std::vector<BuildLayoutPictureStage::ResolvedInstance> instances;
            Rect content_bbox = layout_declared_bbox(root, layout_id);
            bool have_content_bbox = content_bbox.ll.x != content_bbox.ur.x || content_bbox.ll.y != content_bbox.ur.y;

            auto expand = [&](Rect r)
            {
                if (!have_content_bbox)
                {
                    content_bbox = r;
                    have_content_bbox = true;
                    return;
                }
                content_bbox.ll.x = std::min(content_bbox.ll.x, r.ll.x);
                content_bbox.ll.y = std::min(content_bbox.ll.y, r.ll.y);
                content_bbox.ur.x = std::max(content_bbox.ur.x, r.ur.x);
                content_bbox.ur.y = std::max(content_bbox.ur.y, r.ur.y);
            };

            for (PlacementId placement_id : root.get_layout_placements(layout_id))
            {
                const PlacementData *placement = root.get_placement(placement_id);
                if (!placement || !placement->location || !placement->reference_design.valid())
                    continue;

                sk_sp<SkPicture> child_picture = resolve_design_picture(root, placement->reference_design, remaining_depth, view_layers, scene, scale);
                if (!child_picture)
                    continue;

                const Rect child_local_bbox = resolved_local_bbox(root, placement->reference_design, remaining_depth);
                const Orientation orientation = placement->orientation.value_or(Orientation::N);
                const Geometry::InstanceTransform transform = Geometry::instance_transform(orientation, child_local_bbox, *placement->location);

                // The instance's own transformed world bbox is folded into
                // content_bbox below - a Layout with only placements and
                // no diearea/own-content shapes (e.g. this class's own
                // tests) would otherwise leave content_bbox degenerate,
                // under-reporting BuildLayoutPictureStage's own recorded
                // bounds relative to what's actually drawn (SkPicture's
                // own bounds don't clip recording, but a too-small
                // reported cullRect risks a future caller's quickReject
                // wrongly culling real content - see record_local_picture's
                // own comment).
                expand(Geometry::transform_bbox(transform, child_local_bbox));

                instances.push_back(BuildLayoutPictureStage::ResolvedInstance{
                    .transform = to_instance_matrix(transform, Point{0, 0}, scale),
                    .picture = std::move(child_picture),
                });
            }

            return record_local_picture(generate_stage, viewport_stage, layer_stage, dbu_shapes, view_layers, scene, scale, instances, content_bbox);
        }

        // Shared core of both build_abstract_picture and
        // build_layout_picture_uncached: filters `dbu_shapes` against a
        // throwaway Scene sized to fully enclose their own content (not
        // the real Scene's current viewport - see this class's own
        // comment), converts to local pixel space (pan={0,0}, see this
        // class's own comment), and records the result plus `instances`
        // into one SkPicture via BuildLayoutPictureStage (which serves the
        // no-instances Abstract-leaf case just as well as the
        // has-instances Layout case - "record shapes, then draw zero or
        // more already-resolved instances on top" is exactly the same
        // operation either way).
        //
        // `declared_bbox` (from the caller: the design's own
        // AbstractData.size/Layout diearea, already unioned with every
        // placed instance's own transformed extent for
        // build_layout_picture_uncached's own call - see its own comment)
        // is unioned here with `dbu_shapes`'s own actual bbox to produce
        // `bounds` - both the throwaway cull-Scene's own viewport sizing
        // and BuildLayoutPictureStage's own SkPictureRecorder bounds.
        sk_sp<SkPicture> record_local_picture(const ShapeGenerationStage &generate_stage, FilterByViewportAndSizeStage &viewport_stage, FilterByLayerVisibilityStage &layer_stage, const std::vector<RenderedShape> &dbu_shapes, const ViewLayerSet &view_layers, const Scene &scene, double scale, const std::vector<BuildLayoutPictureStage::ResolvedInstance> &instances, Rect declared_bbox)
        {
            // declared_bbox is always a real (if possibly degenerate,
            // e.g. Rect{}) Rect, never absent - every call site
            // (build_abstract_picture/build_layout_picture_uncached)
            // already guarantees at least the design's own declared
            // size/diearea, so content_bbox itself is never genuinely
            // "no content at all"; only its extent varies.
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

            // A throwaway Scene whose only job is giving
            // FilterByViewportAndSizeStage a viewport rect that fully
            // encloses content_bbox at `scale`, so nothing real gets
            // culled - not the real Scene's own current pan/viewport
            // (see this class's own comment). +2px margin absorbs
            // floating-point rounding at the exact boundary.
            Scene cull_scene;
            cull_scene.set_scale(scale);
            cull_scene.set_pan(content_bbox.ll);
            const double width_dbu = static_cast<double>(content_bbox.ur.x - content_bbox.ll.x);
            const double height_dbu = static_cast<double>(content_bbox.ur.y - content_bbox.ll.y);
            cull_scene.set_viewport_size(static_cast<int>(width_dbu * scale) + 2, static_cast<int>(height_dbu * scale) + 2);

            const auto &viewport_filtered = viewport_stage.run(generate_stage, dbu_shapes, cull_scene);
            const auto &layer_filtered = layer_stage.run(viewport_stage, viewport_filtered, scene, view_layers);
            const auto pixel_shapes = transform_shapes_to_pixel_space(layer_filtered, Point{0, 0}, scale);

            const SkRect bounds = SkRect::MakeLTRB(
                static_cast<SkScalar>(static_cast<double>(content_bbox.ll.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ll.y) * scale),
                static_cast<SkScalar>(static_cast<double>(content_bbox.ur.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ur.y) * scale));

            return BuildLayoutPictureStage::run(bounds, pixel_shapes, instances, view_layers);
        }

        Epoch epoch_;
        std::map<std::tuple<DesignId, int>, sk_sp<SkPicture>> design_pictures_;
        std::map<std::tuple<LayoutId, int>, sk_sp<SkPicture>> layout_pictures_;
        std::set<DesignId> unresolved_logged_;
        uint64_t recompute_count_ = 0;

        // render_layout_frame's own rasterize/compose chain - a second,
        // independent set of instances from Renderer's own (see this
        // class's own doc comment for why sharing would be unsafe).
        // design_frame_version_stage_'s own "value" is never read, only
        // its version() - a plain reuse of VersionedStage purely for its
        // built-in "did this key change" tracking (see render_layout_frame's
        // own comment).
        VersionedStage<std::tuple<LayoutId, int, uint64_t, int64_t, int64_t, int, int, double>, int> design_frame_version_stage_;
        BuildOverlayPictureStage build_overlay_picture_stage_;
        BuildRulerOverlayPictureStage build_ruler_overlay_picture_stage_;
        RasterizeStage rasterize_design_stage_;
        RasterizeStage rasterize_tiny_stage_;
        RasterizeStage rasterize_selection_stage_;
        RasterizeStage rasterize_ruler_stage_;
        ComposeWithOverlaysStage compose_stage_;
    };
}
