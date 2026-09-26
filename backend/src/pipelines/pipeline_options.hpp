#pragma once

#include "../core/flightlines.hpp"
#include "../database/database.hpp"
#include "../view_style/view_style.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace le
{
    /// @brief Options shared by every stage of ViewRenderPipeline (see
    /// PIPELINE_REFACTOR.md's own "Structure" section) - Cold, Warm, and
    /// (eventually) Hot alike. Every stage wired into the same
    /// oneapi::tbb::flow::graph must share this exact type (tbb_core.hpp's
    /// MemoizingStage is templated on one PipelineOptions type per graph),
    /// even though a given stage - e.g. LayerGenerationStage - only reads
    /// the subfield(s) it actually depends on, via its own
    /// options_did_change() override. Named for the whole pipeline, not
    /// "Cold", precisely because fields like `viewport` below only matter
    /// to Warm/Hot stages - a Cold-only name would be misleading the
    /// moment those stages join the same graph.
    struct ViewRenderOptions
    {
        /// @brief Non-owning pointer to the Root every Cold-tier stage
        /// reads from - shared context, not part of any one stage's own
        /// InputData (ViewRenderPipeline, view_render_pipeline.hpp, wires
        /// LayerGenerationStage's own OutputHandle directly into
        /// HierarchyResolverStage's InputData via a real make_edge; the
        /// Root pointer has to travel some other way, since it isn't part
        /// of that upstream output). Mirrors the pre-restart PipelineOptions'
        /// own PipelineContext pattern (backend/CLAUDE.md) for the same
        /// reason. Never null-checked by a stage before use - each
        /// degrades to an empty/default output instead (same convention
        /// as a null LeHandle in api.cpp).
        const Root *root = nullptr;

        /// @brief Root::mutation_version() at the time this options
        /// snapshot was taken. Every Cold-tier stage's own
        /// options_did_change() compares this field (directly, or via a
        /// narrower per-stage generation derived from it) since any
        /// database mutation can in principle affect what that stage
        /// produces.
        std::uint64_t root_mutation_version = 0;

        /// @brief Which AbstractId or LayoutId the caller wants shapes
        /// for - HierarchyResolver's own top-level starting point.
        std::variant<AbstractId, LayoutId> top_level;

        /// @brief How many further Placement -> Design levels a Layout
        /// view recurses into - HierarchyResolverStage's own doc comment
        /// has the exact semantics (0 shows only top_level's own direct
        /// content; each further unit lets one more Placement -> Layout
        /// hop actually resolve and get visited, falling back to a
        /// placement's own Abstract once a Layout hop is no longer
        /// possible - not a blanket "always show at least the Abstract"
        /// rule at any depth).
        int hierarchy_depth = 0;

        /// @brief Warm tier's own viewport, in dbu, in top_level's own
        /// coordinate space (ViewportCullStage's own doc comment) - a
        /// Placement's own bbox at any deeper level is only in that same
        /// space once composed through every ancestor placement's own
        /// transform on the way down, which is exactly what culling does.
        Rect viewport;

        /// @brief Warm tier's own pixels-per-dbu-unit scale, shared by
        /// every node's own rasterization (RasterizeBlend2DStage) so
        /// composing them (ComposeStage) is a plain translate+rotate per
        /// placement, never a resample - see RasterizeBlend2DStage's own
        /// doc comment.
        double scale = 1.0;

        /// @brief Whether the render antialiases. Compared in
        /// RasterizeBlend2DStage::options_did_change so a live toggle
        /// still forces a recompute, but currently unused by
        /// draw_view_shapes_blend2d's own draw calls: Blend2D has exactly
        /// one BLRenderingQuality value (BL_RENDERING_QUALITY_ANTIALIAS,
        /// always on, rasterize_blend2d_stage.hpp's own comment), unlike
        /// the earlier Skia-based RasterizeStage this field was
        /// originally written for, which genuinely could and did disable
        /// per-draw-call antialiasing.
        bool antialiasing_enabled = false;

        /// @brief Per-layer-name and per-purpose visibility toggles - a
        /// ViewLayer draws only if BOTH its own layer-name entry (if any)
        /// and its own purpose entry (if any) say visible; an unset key
        /// in either map means visible (matches LeHandle::is_layer_name_visible/
        /// is_purpose_visible's own "unknown key -> visible" default, and
        /// draw_view_shapes_blend2d's own is_view_layer_visible mirrors
        /// that same logic exactly - see its own comment). Plain values,
        /// not a shared_ptr (RasterizeBlend2DStage's own ViewLayerSet
        /// content travels via HierarchyResolverOutput::view_layers
        /// instead, echoed
        /// forward through the make_edge chain - hierarchy_resolver_stage.hpp's
        /// own comment, not through this options struct): these two maps
        /// are small (at most one entry per real layer/purpose, never
        /// per-shape), so options_did_change() can just compare them by
        /// real content
        /// equality instead of needing the caller to track its own
        /// version counter and hand back a fresh shared_ptr on every
        /// actual change. Empty by default (nothing hidden) - a caller
        /// wanting LeHandle's own long-standing "ROW/TRACK_PREFERRED/
        /// TRACK_NON_PREFERRED/GCELLGRID hidden by default" convention
        /// copies LeHandle::layer_name_visibility()/purpose_visibility()
        /// in here directly (api.cpp's own view_render_options_for).
        std::unordered_map<std::string, bool> layer_name_visible;
        std::unordered_map<ViewLayerPurpose, bool> purpose_visible;

        /// @brief The user's own in-progress rubber-band drag rectangle
        /// (select or zoom), already resolved to a normalized dbu-space
        /// Rect by LeHandle::drag_rect_dbu() - nullopt when no drag is in
        /// progress. A plain snapshot value, like every other field here,
        /// not a live LeHandle* - this struct never carries live mutable
        /// UI state, only values already resolved at the point a caller
        /// (api.cpp's own view_render_options_for) builds one. Drawn by
        /// ComposeStage directly (see that stage's own doc comment) - no
        /// separate overlay stage/node, so the ghost rectangle lives in
        /// this same graph rather than a second one.
        std::optional<Rect> drag_rect_dbu;

        /// @brief Which kind of drag `drag_rect_dbu` represents - only
        /// meaningful when `drag_rect_dbu` has a value. Selects which of
        /// ComposeStage's own two drag-rect color pairs (draw_helpers.hpp)
        /// to draw with: a plain rubber-band select drag vs. a
        /// drag-to-zoom gesture get different colors so a user can tell
        /// them apart while dragging (LeHandle::DragKind's own doc comment).
        bool drag_is_zoom = false;

        /// @brief Every currently-selected piece's own dbu-space geometry
        /// (`LeHandle::selection()`, already resolved to plain `Shape`s by
        /// the caller - api.cpp's own view_render_options_for - the same
        /// per-`SelectedObject`-kind resolution the pre-restart
        /// `pipelines.old/stages/selection_overlay_stage.hpp` used:
        /// `ShapePiece` via `Geometry::extract_piece`, `RowId` via
        /// `row_footprint_bbox`, `RegionId` via its own `RegionData::rects`
        /// directly, `PlacementId` via `placement_world_bbox`). One
        /// `Shape` per selected piece, not indexed by which kind it came
        /// from - `ComposeStage` only needs to stroke an outline around
        /// each one, not know which selection-variant alternative
        /// produced it. Empty when nothing is selected.
        std::vector<Shape> selected_piece_outlines;

        /// @brief The selected placements' flightlines (NEW_FEATURES_SEPT_2026.md
        /// item 5, core/flightlines.hpp) - empty unless the FLIGHTLINE
        /// purpose is visible. Drawn in `flightline_color`.
        std::vector<Flightline> flightlines_dbu;

        /// @brief The Resize tool's hover indicator (NEW_FEATURES_SEPT_2026.md
        /// item 3) - the selected piece's edge/segment under the mouse that a
        /// click would grab. Changes only alongside `mouse_version`.
        std::optional<std::array<Point, 2>> resize_hover_segment_dbu;
        Color flightline_color;

        /// @brief Bumped whenever `flightlines_dbu` changes (api.cpp's own
        /// flightline cache) - `ComposeStage::options_did_change` compares
        /// this, since toggling FLIGHTLINE visibility, or `link` changing
        /// connectivity, changes nothing any upstream stage rasterizes.
        std::uint64_t flightline_version = 0;

        /// @brief `LeHandle::selection_version()` at the time this
        /// snapshot was taken - `ComposeStage::options_did_change` compares
        /// this (cheap) rather than deep-comparing
        /// `selected_piece_outlines` by value (`Shape`/`Rect`/`Polygon`
        /// have no `operator==` in this codebase - same reason
        /// `drag_rect_dbu` above is compared field-by-field instead of
        /// wholesale).
        std::uint64_t selection_version = 0;

        /// @brief The grid-snapped dbu point the mouse cursor currently
        /// sits over (`LeHandle::snapped_mouse_position()`) - nullopt when
        /// no mouse position has been set. Shown regardless of mode/
        /// selectability (the cursor marker is meant to be visible at all
        /// times a position is known, not just in Select mode).
        std::optional<Point> cursor_snapped_position_dbu;

        /// @brief `LeHandle::mouse_version()` at the time this snapshot
        /// was taken - covers `cursor_snapped_position_dbu` and the
        /// other mouse-driven overlays (the same mouse-move/mode events, `LeHandle::mouse_version()`'s own doc comment) for
        /// `ComposeStage::options_did_change`, the same cheap-version-
        /// instead-of-deep-compare reasoning `selection_version` above
        /// uses.
        std::uint64_t mouse_version = 0;

        /// @brief The live Move gesture's own ghost-preview geometry
        /// (UPDATES.md item 21) - each moving piece's own *original*
        /// (pre-offset) dbu-space geometry, copied directly from
        /// `LeHandle::move().moving_geometry` (already one-piece `Shape`s,
        /// snapshotted at arm/re-arm time - no further resolution needed,
        /// unlike `selected_piece_outlines` above). Empty whenever Move
        /// isn't armed, has no anchor yet, or no mouse position is set
        /// (mirrors `LeHandle::move_delta()`'s own nullopt conditions) -
        /// `ComposeStage` draws nothing when this is empty, regardless of
        /// `move_ghost_offset_dbu`'s own value.
        std::vector<Shape> move_ghost_pieces_dbu;

        /// @brief The offset (`LeHandle::move_delta()`) every entry of
        /// `move_ghost_pieces_dbu` should be translated by before drawing -
        /// applied in dbu space, before mapping to pixels, so the preview
        /// traces the exact geometry Move would actually commit (not a
        /// pixel-space translation of the already-projected outline).
        /// Value-initialized: the Resize and Placement Move ghosts are
        /// pre-placed and never set this - left uninitialized it was stack
        /// garbage, drawing their ghosts somewhere off-screen.
        Point move_ghost_offset_dbu{};

        /// @brief Every ruler's own committed dbu-space points
        /// (`LeHandle::rulers()`, one entry per `Ruler` - `Ruler::finished`
        /// itself isn't carried across, since a finished and still-active
        /// ruler draw identically, pipelines.old's own `draw_ruler_polyline`
        /// doc comment). A polyline with fewer than 2 points draws
        /// nothing (no segment yet) - `ComposeStage` doesn't special-case
        /// this, the drawing loop just naturally has nothing to iterate.
        std::vector<std::vector<Point>> ruler_polylines_dbu;

        /// @brief The live, not-yet-committed ruler segment's own end
        /// point (`LeHandle::ruler_next_point()`) - nullopt unless
        /// `LeHandle::mode() == Mode::RULER` *and* there's an active
        /// (unfinished, non-empty) ruler to extend, matching
        /// pipelines.old's own `MouseOverlayStage` gating exactly. The
        /// segment's own start point is always the last entry's own last
        /// point in `ruler_polylines_dbu` - no separate field needed,
        /// since a ghost only ever exists when that polyline is real and
        /// non-empty.
        std::optional<Point> ruler_ghost_point_dbu;

        /// @brief Database units per micron (`TechnologyData::
        /// database_units_microns`) - every ruler distance/tick-spacing
        /// computation works in real microns, not raw dbu, so this is
        /// needed to convert. 0 (rather than `std::optional`) means
        /// "unavailable" (no Technology yet, or a non-positive value) -
        /// `ComposeStage` treats <= 0 as "skip ruler drawing entirely",
        /// the same guard pipelines.old's own `draw_ruler_segment` used
        /// its `std::optional<double>` parameter for.
        double ruler_dbu_per_um = 0.0;

        /// @brief On-screen text size (px) for every ruler label -
        /// `LeHandle::ruler_label_size_px()`, runtime-configurable
        /// (`le_ruler_label_size`/`le_set_ruler_label_size`).
        double ruler_label_size_px = 11.0;

        /// @brief Largest on-screen size (px) a shape/placement label grows
        /// to - `LeHandle::label_size_px()`, the Settings panel's label font
        /// size (NEW_FEATURES_SEPT_2026.md item 9); draw_helpers.hpp's
        /// kMaxLabelPixelSize is the default. Compared in
        /// RasterizeBlend2DStage::options_did_change.
        double label_max_size_px = 24.0;

        /// @brief `LeHandle::ruler_version()` at the time this snapshot
        /// was taken - covers `ruler_polylines_dbu` (bumped only on a
        /// real ruler change: a point added, a ruler finished, rulers
        /// cleared - never on mouse move alone). The live ghost segment
        /// doesn't need its own version here - it's driven by mouse
        /// position/mode, already covered by `mouse_version` above.
        std::uint64_t ruler_version = 0;

        /// @brief Background dbu grid spacing (`LeHandle::
        /// minor_grid_spacing()`/`major_grid_spacing()`) - drawn only for
        /// `top_level` itself (the currently-displayed content, not any
        /// nested placement's own composited image - pipelines.old's own
        /// `BuildDesignPictureStage` drew this only for the Abstract/
        /// Layout actually being viewed too, never per-instance) by
        /// `RasterizeBlend2DStage`, not `ComposeStage` - this is Cold/
        /// Warm-tier design-adjacent content (drawn through the same
        /// per-node dbu-to-pixel transform real geometry uses), not
        /// Hot-tier interactive chrome.
        std::int64_t minor_grid_spacing_dbu = 5;
        std::int64_t major_grid_spacing_dbu = 50;

        /// @brief The current Abstract's own LEF `ORIGIN` point
        /// (`AbstractData::origin`, defaulting to dbu (0,0) when unset -
        /// `api.cpp`'s own `view_render_options_for` resolves this),
        /// nullopt when `top_level` isn't an `AbstractId` at all (a
        /// Layout view has no origin marker - matching pipelines.old's
        /// own scope, which never drew one for `BuildLayoutPictureStage`
        /// either). Drawn by `RasterizeBlend2DStage` alongside the grid
        /// above, for the same "only for top_level itself" reason.
        std::optional<Point> abstract_origin_dbu;
    };
}
