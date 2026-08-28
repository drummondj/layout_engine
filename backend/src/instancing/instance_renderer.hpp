#pragma once
#include "../core/placement_geometry.hpp"
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../pipeline/stages/filter_by_layer_visibility_stage.hpp"
#include "../pipeline/stages/filter_by_viewport_and_size_stage.hpp"
#include "../pipeline/stages/generate_abstract_shapes_stage.hpp"
#include "../pipeline/stages/generate_layout_shapes_stage.hpp"
#include "../render/draw_helpers.hpp"
#include "../render/pixel_types.hpp"
#include "../render/render.hpp"
#include "../render/stages/build_layout_picture_stage.hpp"
#include "../render/stages/build_overlay_picture_stage.hpp"
#include "../render/stages/build_ruler_overlay_picture_stage.hpp"
#include "../render/stages/build_selection_overlay_picture_stage.hpp"
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
    /// Builds a fresh, call-local FilterByViewportAndSizeStage/
    /// FilterByLayerVisibilityStage pair inside build_abstract_picture/
    /// build_layout_picture_uncached (see those methods' own comments) -
    /// their own cache key composes a throwaway `cull_scene`'s
    /// `viewport_version()` as a proxy for its real content, and two
    /// calls with a different `scale` could otherwise collide on that
    /// same proxy value and wrongly reuse output culled at the wrong
    /// threshold; a fresh instance per call can't go stale this way,
    /// since it has no prior key to accidentally collide with.
    ///
    /// `generate_abstract_stage_`/`generate_layout_stage_` (below), by
    /// contrast, ARE persistent, shared members - safe despite this
    /// class's own recursive resolution (`resolve_design_picture` ->
    /// `build_layout_picture` -> `build_layout_picture_uncached`, which
    /// can recurse back into itself for a DIFFERENT LayoutId, a nested
    /// sub-block) because `build_abstract_picture`/
    /// `build_layout_picture_uncached` copy each stage's own `run()`
    /// result into a local `std::vector<RenderedShape>` immediately,
    /// before the placement loop runs, rather than holding a reference
    /// into the stage's own single-slot cache across that recursive
    /// call - the exact fix for a real bug this class used to work
    /// around by constructing BOTH stages fresh per call instead (a
    /// shared single-slot cache being overwritten by a nested recursive
    /// call for a different key would otherwise leave an outer, still-
    /// referenced value silently aliasing the inner call's data by the
    /// time it's used afterward, in record_local_picture). This class's
    /// own real caching (the two {id, remaining_depth}-keyed maps below)
    /// already means neither shape-generation stage is ever actually hit
    /// twice for the same key within one epoch regardless - sharing them
    /// is a design-clarity fix, not a cache-hit-rate one.
    ///
    /// Also owns the final "compose a displayable frame" step for a
    /// Layout view (render_layout_frame, Migration Step 3 Phase C) -
    /// broadening this class's own scope slightly past pure instance-
    /// picture resolution, but kept here rather than in `Renderer` (which
    /// deliberately doesn't link `instancing`/`pipeline` - same boundary
    /// reasoning as everywhere else in this class) or duplicated ad hoc in
    /// `api.cpp` (which stays a thin C-API wrapper). `render_layout_frame`
    /// takes a `Renderer&` (the same one `api.cpp`'s `LeHandle` already
    /// owns) and calls straight through its `design_rasterize_stage()`/
    /// `tiny_shapes_rasterize_stage()`/`selection_rasterize_stage()`/
    /// `ruler_rasterize_stage()`/`compose_stage()` accessors - those five
    /// classes are already fully generic over a caller-supplied version
    /// number (RasterizeStage/ComposeWithOverlaysStage take every
    /// upstream version as an explicit parameter, nothing hardcoded
    /// internally), so the real Renderer-owned instances are safe to
    /// share directly rather than duplicate: `Scene::current_abstract()`/
    /// `current_layout()` are mutually exclusive (api.cpp's
    /// `le_render_pixel_buffer` is a strict if/else between
    /// `Renderer::render()` and this method), so there's never a
    /// concurrent/interleaved call from both paths - but each
    /// RasterizeStage's own key is just `{upstream_version}`, with no
    /// domain discriminator, and both callers' version counters start at
    /// 0 independently, so two completely different pictures can land on
    /// the exact same small version number the first time each domain is
    /// used - wrongly serving the OTHER domain's stale cached frame
    /// (a real bug this class's own tests caught -
    /// SwitchingFromAbstractViewToLayoutViewThroughTheSameRendererDoesNotShowStaleContent,
    /// instancing_test.cpp). render_layout_frame ORs every version number
    /// it feeds into the shared stages with `kLayoutVersionDomainTag`
    /// (its own private constant, see its own body) to keep this
    /// class's own numbering entirely disjoint from Renderer's
    /// Abstract-path one - no real session will ever recompute either
    /// path anywhere near 2^63 times. Still owns its own BuildOverlayPictureStage/
    /// BuildRulerOverlayPictureStage/BuildSelectionOverlayPictureStage
    /// rather than reaching into `Renderer`'s: `ComposeWithOverlaysStage::run`
    /// needs the *stage object* itself (for its own `.version()`, part of
    /// its cache key) for the overlay stage specifically, and this
    /// trio's own content is Scene-only, never view-dependent - caching
    /// it twice costs a little memory/CPU, not correctness, unlike the
    /// four RasterizeStages/ComposeWithOverlaysStage above, which hold
    /// real rasterized surfaces and do real compositing work.
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

        /// The sub-pixel instance-collapsing threshold (MIGRATION_REVIEW.md
        /// item 2, build_layout_picture_uncached's own comment) - a
        /// Placement whose own transformed world bbox is smaller than this
        /// many PIXELS (in both dimensions, at the current scale) is drawn
        /// as a single BOUNDARY-colored dot instead of being resolved/drawn
        /// in full. Settable (not a compile-time constant) so this can be
        /// tuned/tested against a real design without rebuilding the
        /// threshold's own call sites - defaults to 100px, deliberately
        /// larger than a single device pixel, while this value is still
        /// being tuned against real content (temporary/testing default -
        /// tighten back down once a good value is confirmed).
        double min_visible_instance_pixels() const { return min_visible_instance_pixels_; }
        void set_min_visible_instance_pixels(double pixels) { min_visible_instance_pixels_ = pixels; }

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
        /// Builds `layout_id`'s own TOP-level picture fresh every call
        /// (bypassing build_layout_picture's own {LayoutId, remaining_depth}
        /// cache entirely - see build_layout_picture_uncached's own
        /// comment) with `local_origin = scene.pan()`, so its own baked
        /// SkScalar coordinates are `(dbu - scene.pan()) * scene.scale()` -
        /// already the real Scene's own final pixel space, exactly
        /// TransformToPixelsStage's own `pixel = (dbu - pan) * scale`
        /// convention - drawn with no further transform at all below.
        /// This trades away this one picture's own pan-independence (a
        /// pure pan-drag now re-generates the top level's own direct
        /// content - die area/rows/tracks/blockages/routes/ports - every
        /// frame instead of reusing a cached picture unchanged) for
        /// correctness at high zoom on a real design's real absolute DBU
        /// coordinates (commonly hundreds of thousands to millions) -
        /// see build_layout_picture_uncached's own comment for the
        /// precision failure this fixes. Every *nested* Placement's own
        /// resolved picture (resolve_design_picture, still reached from
        /// inside build_layout_picture_uncached's own placement loop)
        /// keeps its existing local_origin={0,0} convention and its
        /// existing cache untouched - only this outermost call's own
        /// direct content pays the extra cost, not the placement-count-
        /// scaling part BENCHMARKS.md's stress test measures.
        /// ensure_epoch is called explicitly here (normally a job the
        /// bypassed build_layout_picture wrapper does) so the recursive
        /// resolve_design_picture calls inside still see a fresh epoch.
        /// No tiny-shapes or selection-overlay content yet for a Layout
        /// view (both pass an empty picture/map) - InstanceRenderer
        /// doesn't produce TinyShapeDots, and placement selection is
        /// explicitly deferred scope (Step 3's own "whole-placement
        /// only" decision) - a real, documented gap, not a silent
        /// omission.
        const PixelBuffer &render_layout_frame(const Root &root, LayoutId layout_id, int hierarchy_depth, const ViewLayerSet &view_layers, const Scene &scene, Renderer &renderer)
        {
            const int remaining_depth = std::max(0, hierarchy_depth - 1);
            ensure_epoch(root, view_layers, scene, scene.scale());

            // Single-slot cache keyed on scene.viewport_version() (bumped
            // by any pan/scale/viewport-size change, TransformToPixelsStage's
            // own established convention) plus the same content-change
            // triggers ensure_epoch's own Epoch already tracks - so a
            // repeated call at an unchanged pan/scale (a ruler-only or
            // overlay-only redraw) still reuses this picture instead of
            // re-running GenerateLayoutShapesStage/re-resolving every
            // Placement on every single frame, while a real pan/scale
            // change correctly rebuilds with the new local_origin below.
            // Deliberately its own VersionedStage, not a reuse of
            // layout_pictures_ (which stays keyed pan-independently,
            // {LayoutId, remaining_depth} only, for nested/reused
            // sub-block content - see build_layout_picture_uncached's own
            // comment for why the top level can't share that convention).
            const auto top_picture_key = std::tuple{layout_id, remaining_depth, root.mutation_version(), view_layers.generation(), scene.visibility_version(), scene.viewport_version()};
            const sk_sp<SkPicture> &local_picture = top_layout_picture_stage_.get(top_picture_key, [&]
            { return build_layout_picture_uncached(root, layout_id, remaining_depth, view_layers, scene, scene.scale(), scene.pan()); });

            // top_layout_picture_stage_'s own version() already composes
            // every real trigger this needs (content mutation/visibility/
            // pan/scale/viewport-size, via viewport_version() - see its
            // own key above) - reused directly as this compose step's
            // "did the design content change" signal instead of a second,
            // separately-tracked frame_key (this file's own established
            // "compose off an upstream stage's own version()" pattern,
            // same as every VersionedStage-based cache key elsewhere).
            const uint64_t design_version = top_layout_picture_stage_.version();

            SkPictureRecorder recorder;
            SkCanvas *canvas = recorder.beginRecording(SkRect::MakeWH(static_cast<SkScalar>(scene.viewport_width_px()), static_cast<SkScalar>(scene.viewport_height_px())));
            draw_grid(*canvas, scene);
            if (local_picture)
                canvas->drawPicture(local_picture);
            const sk_sp<SkPicture> design_picture = recorder.finishRecordingAsPicture();

            const std::optional<double> dbu_per_um = technology_dbu_per_um(root);
            const sk_sp<SkPicture> &overlay_picture = build_overlay_picture_stage_.run(scene, dbu_per_um);
            const sk_sp<SkPicture> &ruler_overlay_picture = build_ruler_overlay_picture_stage_.run(scene, dbu_per_um);
            static const sk_sp<SkPicture> kEmptyPicture; // no tiny-shapes content for a Layout view yet

            // E1 (BUGS_AND_ENHANCEMENTS.md) - `shapes` is unused by this
            // stage (see its own comment), so an empty map is fine here;
            // `layout_id`/`remaining_depth` let it resolve a selected
            // PlacementId's own world bbox (src/core/placement_geometry.hpp).
            static const std::map<ViewLayerId, std::vector<RenderedShape>> kEmptyShapes;
            const sk_sp<SkPicture> &selection_overlay_picture = build_selection_overlay_picture_stage_.run(scene, root, kEmptyShapes, layout_id, remaining_depth);

            // kLayoutVersionDomainTag: renderer's four RasterizeStage
            // instances (and compose_stage()) are SHARED with Renderer's
            // own Abstract-path render() (see Renderer::design_rasterize_
            // stage()'s own comment) - each RasterizeStage's own cache key
            // is just {upstream_version}, a bare uint64_t with no domain
            // discriminator, trusting the caller's own numbering is
            // globally injective. Abstract-path's build_picture_stage_
            // etc. and Layout-path's design_version/etc. are each
            // independent VersionedStage counters that start at 0 - two
            // completely different pictures can trivially land on the
            // exact same small version number the first time each domain
            // is used, which would wrongly serve the OTHER domain's
            // stale, cached RasterizedFrame (a real bug this class's own
            // tests caught - SwitchingFromAbstractViewToLayoutViewThroughTheSameRendererDoesNotShowStaleContent,
            // instancing_test.cpp). OR-ing this tag into every version
            // number fed into the shared stages keeps this domain's own
            // numbering entirely disjoint from Renderer's Abstract-path
            // one (no real session will ever recompute either path
            // anywhere near 2^63 times), without needing RasterizeStage/
            // ComposeWithOverlaysStage's own generic key shape to change.
            constexpr uint64_t kLayoutVersionDomainTag = uint64_t{1} << 63;

            return renderer.compose_stage().run(renderer.design_rasterize_stage(), renderer.tiny_shapes_rasterize_stage(), renderer.selection_rasterize_stage(), renderer.ruler_rasterize_stage(), build_overlay_picture_stage_,
                                                 kLayoutVersionDomainTag | design_version, kLayoutVersionDomainTag, kLayoutVersionDomainTag | build_selection_overlay_picture_stage_.version(), kLayoutVersionDomainTag | build_ruler_overlay_picture_stage_.version(),
                                                 design_picture, kEmptyPicture, overlay_picture, selection_overlay_picture, ruler_overlay_picture, scene);
        }

    private:
        struct Epoch
        {
            uint64_t root_mutation_version = 0;
            uint64_t view_layers_generation = 0;
            uint64_t visibility_version = 0;
            double scale = -1.0;
            double min_visible_instance_pixels = -1.0;

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
                .min_visible_instance_pixels = min_visible_instance_pixels_,
            };
            if (current == epoch_)
                return;

            epoch_ = current;
            design_pictures_.clear();
            layout_pictures_.clear();
        }

        // design_is_resolvable/resolved_local_bbox/layout_declared_bbox/
        // abstract_declared_bbox used to be private statics of this class
        // - relocated to src/core/placement_geometry.hpp (E1,
        // BUGS_AND_ENHANCEMENTS.md) so render's own
        // BuildSelectionOverlayPictureStage can compute a selected
        // Placement's own world bbox using the exact same implementation
        // (render doesn't link instancing - see that header's own
        // comment for the full reasoning). Called unqualified below
        // (both live in namespace le).

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

            // FilterByViewportAndSizeStage/FilterByLayerVisibilityStage
            // stay fresh, call-local instances, not persistent class
            // members - their own cache key composes `cull_scene.
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
            FilterByViewportAndSizeStage viewport_stage;
            FilterByLayerVisibilityStage layer_stage;

            // generate_abstract_stage_ IS a persistent, shared member
            // (see this class's own comment) - copy its result rather
            // than binding a reference, so a recursive call elsewhere in
            // this same frame can't leave `dbu_shapes` aliasing the
            // wrong Design's data by the time record_local_picture below
            // uses it.
            const std::vector<RenderedShape> dbu_shapes = generate_abstract_stage_.run(root, abstract_id, view_layers);
            return record_local_picture(generate_abstract_stage_, viewport_stage, layer_stage, dbu_shapes, view_layers, scene, scale, {}, {}, abstract_declared_bbox(root, abstract_id));
        }

        // `local_origin` (default {0,0}, the existing/normal convention
        // for every cached recursive call below) shifts this picture's
        // own "local pixel space" to `(dbu - local_origin) * scale`
        // instead of the class comment's own plain `dbu_local * scale` -
        // see record_local_picture's own comment for why: it only
        // matters (non-default) for render_layout_frame's own TOP-level,
        // uncached call, where `dbu` is the real design's own absolute
        // DBU coordinates (commonly in the hundreds of thousands to
        // millions for a real chip) rather than a small, already-local
        // sub-block's own coordinates - passing scene.pan() there keeps
        // this picture's own baked-in SkScalar (32-bit float) values
        // small regardless of scale (BUGS_AND_ENHANCEMENTS.md B1
        // follow-up: real DBU coordinates times a real zoom scale
        // routinely exceed float32's exact-integer range, ~16.7M,
        // collapsing adjacent screen pixels onto the same tile-pattern
        // phase - a dense checkerboard/moire, worse the more zoomed in).
        sk_sp<SkPicture> build_layout_picture_uncached(const Root &root, LayoutId layout_id, int remaining_depth, const ViewLayerSet &view_layers, const Scene &scene, double scale, Point local_origin = Point{0, 0})
        {
            recompute_count_++;

            // FilterByViewportAndSizeStage/FilterByLayerVisibilityStage
            // stay fresh, call-local instances - see build_abstract_
            // picture's own comment for the scale-staleness reason.
            FilterByViewportAndSizeStage viewport_stage;
            FilterByLayerVisibilityStage layer_stage;

            // generate_layout_stage_ IS a persistent, shared member (see
            // this class's own comment) - copy its result into a real
            // local rather than binding a reference, BEFORE the
            // recursive placement loop below runs. That loop calls
            // resolve_design_picture, which can recurse back into
            // build_layout_picture_uncached for a DIFFERENT LayoutId (a
            // nested sub-block) - reusing this same shared stage for
            // that recursive call would overwrite its single
            // VersionedStage slot; a `dbu_shapes` bound BY REFERENCE to
            // that slot would silently end up aliasing the recursive
            // call's own data by the time it's used afterward, in
            // record_local_picture (a real bug this class's own tests
            // exercise -
            // TwoLevelHierarchyRecursesAndPlacesTheLeafRelativeToItsSubBlock,
            // where the sub-block is a distinct LayoutId from the top).
            // A local copy, taken before the recursion, can't be
            // affected by what the shared stage's slot holds afterward.
            const std::vector<RenderedShape> dbu_shapes = generate_layout_stage_.run(root, layout_id, view_layers);

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

            // Same sub-pixel-threshold SHAPE as TinyShapesByViewportStage
            // already uses for individual shapes (min_visible_dbu =
            // threshold_px / scale, tiny_shapes_by_viewport_stage.hpp) -
            // an instance whose own transformed world bbox is smaller than
            // this in BOTH dimensions is collapsed to just its own
            // boundary rect outline (no fill, no internal content) instead
            // of being resolved/drawn as a full picture. This is a real
            // perf fix, not just a visual one: resolve_design_picture's own
            // cache already dedupes the expensive per-Design *build* across
            // placements of the same {DesignId, remaining_depth}, but the
            // per-placement canvas->save()/concat()/drawPicture()/restore()
            // recorded into THIS picture below is paid unconditionally,
            // once per placement, regardless of on-screen size - at
            // 1,000,000 placements that's 1,000,000 real recording calls
            // even when most of them are sub-pixel and would only ever
            // read back as noise. Skipping resolve_design_picture entirely
            // for these also means no recursion into a tiny sub-block's own
            // (potentially large) placement list at all. Unlike
            // TinyShapesByViewportStage's own fixed 1px, this class's own
            // threshold is settable (min_visible_instance_pixels()) rather
            // than hardcoded to 1 - see its own doc comment for why: at a
            // large threshold, a single collapsed-to-a-point dot would
            // lose the instance's own real size/position entirely, while
            // its own boundary rect (however small) still shows both.
            std::vector<PixelRect> tiny_instance_rects;
            const double min_visible_dbu = min_visible_instance_pixels_ / scale;

            for (PlacementId placement_id : root.get_layout_placements(layout_id))
            {
                const PlacementData *placement = root.get_placement(placement_id);
                if (!placement || !placement->location || !placement->reference_design.valid())
                    continue;

                // Checked before the sub-pixel logic below: a genuinely
                // unresolvable reference_design (valid DesignId, but
                // neither an Abstract nor a reachable Layout) must still
                // go through resolve_design_picture - not be silently
                // treated as a sub-pixel instance - so it's (a) skipped
                // entirely (no phantom dot at its own location; there's
                // really nothing there) and (b) still logged once per
                // DesignId, exactly as before this reordering.
                // resolved_local_bbox's own Rect{} fallback for
                // "unresolved" would otherwise be indistinguishable from a
                // legitimately zero-sized declared bbox and get collapsed
                // into a dot below.
                if (!design_is_resolvable(root, placement->reference_design, remaining_depth))
                {
                    resolve_design_picture(root, placement->reference_design, remaining_depth, view_layers, scene, scale);
                    continue;
                }

                const Rect child_local_bbox = resolved_local_bbox(root, placement->reference_design, remaining_depth);
                const Orientation orientation = placement->orientation.value_or(Orientation::N);
                const Geometry::InstanceTransform transform = Geometry::instance_transform(orientation, child_local_bbox, *placement->location);
                const Rect world_bbox = Geometry::transform_bbox(transform, child_local_bbox);

                // Folded into content_bbox either way (dot or full picture)
                // - a Layout with only placements and no diearea/own-content
                // shapes (e.g. this class's own tests) would otherwise leave
                // content_bbox degenerate, under-reporting
                // BuildLayoutPictureStage's own recorded bounds relative to
                // what's actually drawn (SkPicture's own bounds don't clip
                // recording, but a too-small reported cullRect risks a
                // future caller's quickReject wrongly culling real content -
                // see record_local_picture's own comment).
                expand(world_bbox);

                const double width = static_cast<double>(world_bbox.ur.x - world_bbox.ll.x);
                const double height = static_cast<double>(world_bbox.ur.y - world_bbox.ll.y);
                if (width < min_visible_dbu && height < min_visible_dbu)
                {
                    tiny_instance_rects.push_back(PixelRect{
                        .ll = PixelPoint{.x = static_cast<double>(world_bbox.ll.x - local_origin.x) * scale, .y = static_cast<double>(world_bbox.ll.y - local_origin.y) * scale},
                        .ur = PixelPoint{.x = static_cast<double>(world_bbox.ur.x - local_origin.x) * scale, .y = static_cast<double>(world_bbox.ur.y - local_origin.y) * scale},
                    });
                    continue;
                }

                sk_sp<SkPicture> child_picture = resolve_design_picture(root, placement->reference_design, remaining_depth, view_layers, scene, scale);
                if (!child_picture)
                    continue;

                instances.push_back(BuildLayoutPictureStage::ResolvedInstance{
                    .transform = to_instance_matrix(transform, local_origin, scale),
                    .picture = std::move(child_picture),
                });
            }

            return record_local_picture(generate_layout_stage_, viewport_stage, layer_stage, dbu_shapes, view_layers, scene, scale, instances, tiny_instance_rects, content_bbox, local_origin);
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
        //
        // `local_origin` (default {0,0}) - see build_layout_picture_uncached's
        // own comment; threaded straight through into the two places this
        // function itself bakes `dbu * scale` into final SkScalar
        // coordinates (`pixel_shapes`/`bounds` below), matching the shift
        // already applied to `instances`' own transforms by the caller.
        sk_sp<SkPicture> record_local_picture(const ShapeGenerationStage &generate_stage, FilterByViewportAndSizeStage &viewport_stage, FilterByLayerVisibilityStage &layer_stage, const std::vector<RenderedShape> &dbu_shapes, const ViewLayerSet &view_layers, const Scene &scene, double scale, const std::vector<BuildLayoutPictureStage::ResolvedInstance> &instances, const std::vector<PixelRect> &tiny_instance_rects, Rect declared_bbox, Point local_origin = Point{0, 0})
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
            const auto pixel_shapes = transform_shapes_to_pixel_space(layer_filtered, local_origin, scale);

            const SkRect bounds = SkRect::MakeLTRB(
                static_cast<SkScalar>(static_cast<double>(content_bbox.ll.x - local_origin.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ll.y - local_origin.y) * scale),
                static_cast<SkScalar>(static_cast<double>(content_bbox.ur.x - local_origin.x) * scale), static_cast<SkScalar>(static_cast<double>(content_bbox.ur.y - local_origin.y) * scale));

            return BuildLayoutPictureStage::run(bounds, pixel_shapes, instances, tiny_instance_rects, view_layers);
        }

        // See min_visible_instance_pixels()'s own doc comment for why this
        // is a temporary/testing default, not a settled value.
        double min_visible_instance_pixels_ = 100.0;

        Epoch epoch_;
        std::map<std::tuple<DesignId, int>, sk_sp<SkPicture>> design_pictures_;
        std::map<std::tuple<LayoutId, int>, sk_sp<SkPicture>> layout_pictures_;
        std::set<DesignId> unresolved_logged_;
        uint64_t recompute_count_ = 0;

        // Persistent, shared shape-generation stages - see this class's
        // own doc comment for why sharing these is safe (their own
        // result is always copied, not referenced, before any recursive
        // call that could otherwise evict their single-slot cache out
        // from under a still-live reference).
        GenerateAbstractShapesStage generate_abstract_stage_;
        GenerateLayoutShapesStage generate_layout_stage_;

        // render_layout_frame's own single-slot cache for the TOP-level
        // picture specifically - see that method's own comment for why
        // it can't share layout_pictures_ (pan-dependent local_origin,
        // unlike every other entry in that map).
        VersionedStage<std::tuple<LayoutId, int, uint64_t, uint64_t, uint64_t, uint64_t>, sk_sp<SkPicture>> top_layout_picture_stage_;

        // render_layout_frame's own build-stage trio - still a second,
        // independent set of instances from Renderer's own (see this
        // class's own doc comment: Scene-only content, never view-
        // dependent, so duplicating it costs a little memory/CPU, not
        // correctness). The four RasterizeStages/ComposeWithOverlaysStage
        // Renderer also owns are NOT duplicated here anymore - reached
        // directly through the `Renderer&` render_layout_frame now takes,
        // via its design_rasterize_stage()/tiny_shapes_rasterize_stage()/
        // selection_rasterize_stage()/ruler_rasterize_stage()/
        // compose_stage() accessors.
        BuildOverlayPictureStage build_overlay_picture_stage_;
        BuildRulerOverlayPictureStage build_ruler_overlay_picture_stage_;
        // E1 (BUGS_AND_ENHANCEMENTS.md) - same class Renderer's own
        // Abstract path uses for its selection outline, owned as a
        // second, independent instance here for the exact same reason as
        // the two overlay stages just above (see this class's own doc
        // comment).
        BuildSelectionOverlayPictureStage build_selection_overlay_picture_stage_;
    };
}
