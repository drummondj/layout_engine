#pragma once
#include "../database/database.hpp"
#include "../view_style/view_style.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace le
{
    // Selectable object references - the object kinds that currently have a
    // rendered geometric representation in an Abstract view. Extend this
    // variant as more kinds (e.g. Instance, once a Layout/placement view
    // exists) become selectable.
    using SelectionRef = std::variant<TerminalId, ObstructionId>;

    /// @brief The result of a point hit-test (UPDATES.md 7.1): which
    /// selectable object was hit, plus a copy of the specific piece of
    /// geometry that was actually hit (one RenderedShape's own Shape, not
    /// the whole Terminal/Obstruction's combined geometry - hover
    /// highlighting currently outlines just this piece; see
    /// Pipeline::hit_test_point). Stored as a geometry copy (not just
    /// `origin`) so Renderer can redraw the outline without needing Root
    /// access.
    struct HoverTarget
    {
        SelectionRef origin;
        Shape outline;

        /// The exact ShapeId the hit RenderedShape was generated from
        /// (RenderedShape::shape_id) - nullopt only if the hit somehow
        /// landed on a RenderedShape with no backing Shape row (shouldn't
        /// happen for anything with an `origin`, but not assumed).
        ///
        /// Deliberately carries no piece index into `shape_id`'s own raw
        /// Root geometry (UPDATES.md item 21): `outline` here is copied
        /// from the *Pipeline's* rendered geometry, which can still have
        /// more entries than Root's own raw rects/polygons/paths for a
        /// Shape with RECT/POLYGON/PATH ITERATE statements (the expanded
        /// concrete copies are appended past the raw indices at render
        /// time - see generate_shapes_stage.hpp's expand_iterates) - so a
        /// piece index into *this* geometry isn't always addressable in
        /// Root. Callers that need a piece addressable in Root
        /// (Scene::select/Move) re-hit-test directly against Root's own
        /// raw ShapeData instead (see api.cpp's le_mouse_up) - this
        /// struct's own `outline` stays the rendered geometry, fine for
        /// hover's purely visual highlight.
        std::optional<ShapeId> shape_id;
    };

    /// @brief One selected piece (UPDATES.md item 21 - piece-granular;
    /// originally shape-level per an earlier property-viewer redesign,
    /// UPDATES.md 7) - the exact rect/polygon/path that was clicked/
    /// dragged to select it, identified by its owning Shape's id plus
    /// which entry of that Shape's own rects/polygons/paths it is
    /// (`piece_kind`/`piece_index` - see Geometry::HitPiece, which
    /// Scene::select()'s callers build these from). The Property Viewer
    /// still resolves "the selected object" to the *owning Shape*
    /// (le_object_property_at/le_selected_object_ref in api.cpp only
    /// ever read `shape_id`, ignoring which piece) - properties are
    /// per-Shape, not per-piece, so that deliberately didn't change; only
    /// rendering the selection outline and Move (UPDATES.md item 21) care
    /// which specific piece this is. Geometry itself is looked up on
    /// demand (from Root for Move, from the Pipeline's generated shapes
    /// for rendering the highlight - see BuildSelectionOverlayPictureStage)
    /// rather than stored here, unlike HoverTarget::outline, which still
    /// carries its own geometry copy for hover rendering.
    struct SelectedObject
    {
        ShapeId shape_id;
        PieceKind piece_kind = PieceKind::RECT;
        size_t piece_index = 0;
    };

    /// @brief Per-handle mutable view state: which Abstract is displayed,
    /// the viewport transform, per-layer visibility, and selection. Distinct
    /// from the persistent Root database - the pipeline reads from this,
    /// events write into it.
    class Scene
    {
    public:
        // --- Currently displayed Abstract ---
        // Switching Abstracts clears selection, hover, and rulers -
        // selection/hover hold TerminalId/ObstructionId values scoped to
        // whichever Abstract they were selected/hovered in (they're plain
        // {index,generation} pool handles, not namespaced by Abstract),
        // so leaving them set after switching risks a stale reference
        // that, at best, matches nothing in the new Abstract (id from the
        // old one simply isn't present) and at worst - since Terminals/
        // Obstructions across all Abstracts share the same underlying
        // Pool - happens to collide with an unrelated object's reused
        // pool slot in the new one, highlighting/selecting the wrong
        // shape entirely. Rulers (UPDATES.md item 13) are plain dbu
        // Points with no Abstract scoping at all - left uncleared they'd
        // just go on being drawn, at the same raw coordinates, over
        // whatever design happens to occupy that part of the new
        // Abstract's own unrelated coordinate space. A no-op (no clear,
        // no version bumps) if `id` is the same Abstract already
        // displayed.
        void set_current_abstract(AbstractId id)
        {
            if (id == current_abstract_)
                return;

            current_abstract_ = id;
            clear_selection();
            clear_hover();
            clear_rulers();
        }
        AbstractId current_abstract() const { return current_abstract_; }

        // --- Viewport transform: pixel = (dbu - pan) * scale ---
        // pan/scale/viewport_size each bump viewport_version() - a cheap
        // change signal for callers (e.g. PipelineCache) that would
        // otherwise need to snapshot and compare these fields by value.
        void set_pan(Point pan)
        {
            pan_ = pan;
            ++viewport_version_;
        }
        Point pan() const { return pan_; }

        // Ignores non-positive values (keeps the last valid scale) rather
        // than let a bad zoom value from a caller divide-by-zero downstream
        // (e.g. the pipeline's `1px / scale` sub-pixel threshold).
        void set_scale(double pixels_per_dbu)
        {
            if (pixels_per_dbu > 0.0)
            {
                scale_ = pixels_per_dbu;
                ++viewport_version_;
            }
        }
        double scale() const { return scale_; }

        void set_viewport_size(int width_px, int height_px)
        {
            viewport_width_px_ = width_px;
            viewport_height_px_ = height_px;
            ++viewport_version_;
        }
        int viewport_width_px() const { return viewport_width_px_; }
        int viewport_height_px() const { return viewport_height_px_; }

        // Monotonic counter bumped by any of the three setters above -
        // cheap for a caller to compare instead of snapshotting pan/scale/
        // viewport size by value.
        uint64_t viewport_version() const { return viewport_version_; }

        // Fits `bbox` into the current viewport (already set via
        // set_viewport_size) with `padding_px` of margin on every side:
        // uniform scale (no stretch, bounded by whichever axis is tighter)
        // and pan centering the content. Falls back to scale 1.0 / pan
        // (0, 0) if bbox is nullopt (nothing to fit, e.g. an empty
        // Abstract) or the viewport has non-positive size, rather than
        // dividing by zero.
        void fit_to_content(std::optional<Rect> bbox, int64_t padding_px)
        {
            if (!bbox || viewport_width_px_ <= 0 || viewport_height_px_ <= 0)
            {
                set_scale(1.0);
                set_pan(Point{0, 0});
                return;
            }

            const double content_width = static_cast<double>(bbox->ur.x - bbox->ll.x);
            const double content_height = static_cast<double>(bbox->ur.y - bbox->ll.y);
            const double usable_width_px = viewport_width_px_ - 2 * padding_px;
            const double usable_height_px = viewport_height_px_ - 2 * padding_px;

            // Each axis's own limit on scale, skipped (treated as
            // unbounded) when that axis's content span is zero - a
            // degenerate single-line/point bbox shouldn't force scale to
            // infinity via a divide-by-zero.
            const double scale_x = content_width > 0 ? usable_width_px / content_width : std::numeric_limits<double>::infinity();
            const double scale_y = content_height > 0 ? usable_height_px / content_height : std::numeric_limits<double>::infinity();
            double scale = std::min(scale_x, scale_y);
            if (!std::isfinite(scale) || scale <= 0.0)
                scale = 1.0;

            // Renderer's pixel transform maps (dbu - pan) * scale to pixel
            // space, i.e. pan is the dbu point that lands at pixel (0, 0) -
            // not the viewport center. To center the content, pan is offset
            // from the bbox's own lower-left corner by half of the leftover
            // (non-content) space on each axis - using the full viewport
            // size here (not the padding-reduced usable size), since
            // padding is symmetric and cancels out of the centering offset.
            const int64_t pan_x = bbox->ll.x - static_cast<int64_t>((viewport_width_px_ / scale - content_width) / 2.0);
            const int64_t pan_y = bbox->ll.y - static_cast<int64_t>((viewport_height_px_ / scale - content_height) / 2.0);

            set_scale(scale);
            set_pan(Point{pan_x, pan_y});
        }

        // --- Grid spacing (dbu) ---
        // Defaults assume the common "1 dbu = 1nm" convention (i.e. a
        // Technology declared with DATABASE MICRONS 1000, which most real
        // PDKs use) - 5 and 50 dbu then read as the requested 5nm minor /
        // 50nm major defaults. Not otherwise unit-aware (Scene has no
        // Technology reference to convert against) - a caller on a
        // Technology with different units should set explicit dbu values.
        // Non-positive values are rejected (keeps the last valid spacing),
        // same guard as set_scale, and setters bump visibility_version()
        // since the grid is part of the rendered picture (see
        // Renderer::draw_grid) - unlike layer selectability below, this
        // does need to invalidate the render cache.
        void set_minor_grid_spacing(int64_t dbu)
        {
            if (dbu > 0)
            {
                minor_grid_spacing_ = dbu;
                ++visibility_version_;
            }
        }
        int64_t minor_grid_spacing() const { return minor_grid_spacing_; }

        void set_major_grid_spacing(int64_t dbu)
        {
            if (dbu > 0)
            {
                major_grid_spacing_ = dbu;
                ++visibility_version_;
            }
        }
        int64_t major_grid_spacing() const { return major_grid_spacing_; }

        // --- Mouse position (screen pixels) + grid-snapped dbu position ---
        // Set by the frontend on every pointer move (see le_set_mouse_position)
        // - screen/image pixel space (top-left origin, y down, matching
        // le_render_pixel_buffer()'s output and le_zoom's x/y), not
        // Renderer's own pre-Y-flip pixel space. Deliberately its own
        // version counter, not visibility_version - a mouse move must not
        // invalidate Renderer's (expensive, design-sized) rasterized
        // picture cache; see Renderer::compose_with_overlays for how the
        // mouse overlay stays cheap to redraw independently of it.
        void set_mouse_position(int32_t x_px, int32_t y_px)
        {
            mouse_x_px_ = x_px;
            mouse_y_px_ = y_px;
            has_mouse_position_ = true;
            ++mouse_version_;
        }

        // Call when the pointer leaves the viewport (or before the first
        // pointer event) so the cursor overlay stops showing a stale
        // position rather than sticking at the last-known one.
        void clear_mouse_position()
        {
            if (has_mouse_position_)
            {
                has_mouse_position_ = false;
                ++mouse_version_;
            }
        }

        bool has_mouse_position() const { return has_mouse_position_; }
        uint64_t mouse_version() const { return mouse_version_; }

        // The raw stored pixel position (screen/image space - see
        // set_mouse_position) - 0/0 if has_mouse_position() is false.
        // Exposed for callers that need the pixel coordinate itself, not
        // just its dbu equivalent (mouse_dbu_position) - e.g. a
        // keyboard-triggered zoom (le_key_down's LE_KEY_ZOOM) anchored at
        // "wherever the mouse currently is", the same anchor semantics
        // le_zoom's own x/y parameter already has.
        int32_t mouse_x_px() const { return mouse_x_px_; }
        int32_t mouse_y_px() const { return mouse_y_px_; }

        // Converts a screen/image pixel coordinate (top-left origin, y
        // down - same convention as set_mouse_position) to dbu space,
        // undoing rasterize()'s Y-flip the same way le_zoom's own
        // pixel->dbu conversion does - see render.hpp's PixelShape/
        // Renderer::rasterize comments for why pan/scale describe the
        // pre-flip transform while a screen pixel coordinate is
        // post-flip. scale_ is always positive by construction
        // (set_scale rejects non-positive values), so no divide-by-zero
        // guard is needed here. Shared by mouse_dbu_position() (the
        // currently stored mouse position) and click/drag-select
        // handling (an arbitrary x/y from a mouse-down/up event, not
        // necessarily the currently stored position - see
        // le_mouse_down/le_mouse_up).
        Point pixel_to_dbu(int32_t x_px, int32_t y_px) const
        {
            const double dbu_x = static_cast<double>(pan_.x) + static_cast<double>(x_px) / scale_;
            const double dbu_y = static_cast<double>(pan_.y) + (static_cast<double>(viewport_height_px_) - static_cast<double>(y_px)) / scale_;
            return Point{static_cast<int64_t>(dbu_x), static_cast<int64_t>(dbu_y)};
        }

        // The dbu point currently under the mouse - nullopt if no position
        // has been set yet (see has_mouse_position).
        std::optional<Point> mouse_dbu_position() const
        {
            if (!has_mouse_position_)
                return std::nullopt;

            return pixel_to_dbu(mouse_x_px_, mouse_y_px_);
        }

        // mouse_dbu_position() rounded to the nearest multiple of the
        // minor grid spacing (round-to-nearest, not truncation - a mouse
        // position exactly between two grid points snaps to whichever the
        // division rounds to). nullopt under the same conditions as
        // mouse_dbu_position().
        std::optional<Point> snapped_mouse_position() const
        {
            const std::optional<Point> dbu = mouse_dbu_position();
            if (!dbu)
                return std::nullopt;

            auto snap = [spacing = minor_grid_spacing_](int64_t v)
            {
                return static_cast<int64_t>(std::llround(static_cast<double>(v) / static_cast<double>(spacing))) * spacing;
            };
            return Point{snap(dbu->x), snap(dbu->y)};
        }

        // --- Drag-select / drag-zoom gesture (UPDATES.md 7.1 items 5-6, 9.3) ---
        // Which high-level action a drag gesture should perform once it
        // ends - Scene itself doesn't interpret this, it's purely a label
        // the caller (le_mouse_down/le_zoom_drag_down, and later
        // le_mouse_up) attaches to and reads back, so the same rubber-band
        // machinery below serves both left-button drag-select and
        // right-button drag-zoom without duplicating it.
        enum class DragKind
        {
            SELECT,
            ZOOM,
        };

        // Tracks a mouse-down-to-mouse-up rubber-band gesture in screen
        // pixels (top-left origin, y down - same convention as
        // set_mouse_position). The frontend calls begin_drag on
        // mouse-down and end_drag on mouse-up (see le_mouse_down/
        // le_mouse_up), which decide there whether the gesture was a
        // plain click or an actual drag (by comparing the down/up pixel
        // distance against a small threshold) and perform the
        // corresponding action - Scene itself doesn't know which
        // interpretation applies, it just tracks the raw gesture state
        // (plus which `kind` was requested) and derives drag_rect_dbu()
        // from it plus the current mouse position. `kind` defaults to
        // SELECT so the existing le_mouse_down call site (left-button
        // drag-select) needs no change.
        void begin_drag(int32_t x_px, int32_t y_px, DragKind kind = DragKind::SELECT)
        {
            dragging_ = true;
            drag_start_x_px_ = x_px;
            drag_start_y_px_ = y_px;
            drag_kind_ = kind;
            ++mouse_version_; // so the drag-rect overlay (Renderer, see UPDATES.md 7.1 item 5's live rectangle) starts showing immediately
        }

        void end_drag()
        {
            dragging_ = false;
            ++mouse_version_; // so the drag-rect overlay stops showing
        }

        bool is_dragging() const { return dragging_; }
        DragKind drag_kind() const { return drag_kind_; }
        int32_t drag_start_x_px() const { return drag_start_x_px_; }
        int32_t drag_start_y_px() const { return drag_start_y_px_; }

        // The drag rectangle in dbu space, normalized (ll <= ur regardless
        // of which direction the drag went) - nullopt if no drag is in
        // progress, or no mouse position has been set yet (the drag's
        // "current" corner - mirrors mouse_dbu_position()'s own nullopt
        // condition).
        std::optional<Rect> drag_rect_dbu() const
        {
            if (!dragging_)
                return std::nullopt;

            const std::optional<Point> current = mouse_dbu_position();
            if (!current)
                return std::nullopt;

            const Point start = pixel_to_dbu(drag_start_x_px_, drag_start_y_px_);
            return Rect{
                .ll = Point{std::min(start.x, current->x), std::min(start.y, current->y)},
                .ur = Point{std::max(start.x, current->x), std::max(start.y, current->y)},
            };
        }

        // --- Mode (UPDATES.md item 11) ---
        // Select mode is the only mode where mouse interaction (le_mouse_up)
        // changes the current selection; Edit mode restricts mouse
        // interaction to editing whatever is already selected (behavior
        // TBD - UPDATES.md item 11's own "Details of how objects are
        // edited to follow"), so the selection can only change back in
        // Select mode. Ruler mode (UPDATES.md item 13) is documented in
        // the Rulers section below.
        enum class Mode
        {
            SELECT,
            EDIT,
            RULER,
        };

        // Leaving Ruler mode always finishes whatever ruler was active
        // (see finish_active_ruler) - keeps "an active ruler exists only
        // in Ruler mode" an invariant callers (render, tests) can rely on
        // rather than a coincidence. The hover outline (draw_hover_outline)
        // is a Select-mode-only affordance - it signals "this is a
        // selection candidate", which isn't meaningful while placing
        // ruler points or (Edit mode's own interaction, still TBD) - so
        // leaving SELECT clears it immediately, rather than leaving a
        // stale highlight visible until the next mouse move happens to
        // land somewhere with nothing under it (le_set_mouse_position
        // additionally gates *computing* a fresh hover on SELECT mode
        // going forward - see its own comment). Bumps mouse_version_ on
        // an actual mode change only - the ghost-ruler overlay's content
        // is mode-dependent (BuildOverlayPictureStage) and needs to
        // invalidate on mode switch.
        void set_mode(Mode mode)
        {
            if (mode == mode_)
                return;
            if (mode_ == Mode::RULER)
                finish_active_ruler();
            if (mode_ == Mode::EDIT)
                end_move(); // leaving Edit mode cancels an in-progress (not yet committed) Move, same reasoning as finishing an active ruler above
            mode_ = mode;
            if (mode_ != Mode::SELECT)
                clear_hover();
            ++mouse_version_;
        }
        Mode mode() const { return mode_; }

        // --- Rulers (UPDATES.md item 13) ---
        // A ruler is a polyline of already-committed (grid-snapped)
        // points; `finished` is set once the user presses Esc to stop
        // adding to it (see le_finish_ruler) or leaves Ruler mode (see
        // set_mode above). Multiple rulers can exist at once - starting
        // a new one never clears an existing one (resolved design
        // decision, UPDATES.md item 13). The *last* entry is "the active
        // ruler" new clicks append to, if and only if it exists and
        // isn't finished - this class maintains that as an invariant
        // rather than something callers have to check for themselves.
        struct Ruler
        {
            std::vector<Point> points;
            bool finished = false;
        };

        // --- Move (UPDATES.md item 21) ---
        // Transient, uncommitted drag state for the two-click Move
        // gesture in Edit mode - the direct analog of Ruler above (armed/
        // anchored/free-form flag instead of a committed polyline), see
        // arm_move()/move_set_anchor()/move_delta()/end_move(). Never
        // itself mutates Root - a committed Move is recorded as an
        // ordinary editing::Transaction of per-piece update steps by the
        // caller (see api.cpp's le_mouse_up EDIT-mode branch), once the
        // second click lands. Piece-granular (UPDATES.md item 21's
        // follow-up): `moving_pieces` is a direct copy of `selection_` at
        // arm time, so Move moves exactly whichever pieces are selected,
        // not necessarily every piece of their owning Shapes.
        // `moving_geometry` is a snapshot of each moving *piece's* own
        // current one-piece geometry (Geometry::extract_piece), parallel
        // to `moving_pieces`, taken once at arm_move()/re-arm time (Move
        // never changes *which* pieces are moving mid-gesture, only the
        // offset) - this keeps the render-side ghost overlay
        // (BuildOverlayPictureStage) Root-agnostic, the same reason it
        // doesn't take a Root reference anywhere else.
        struct MoveState
        {
            bool armed = false;
            std::optional<Point> anchor;
            std::vector<SelectedObject> moving_pieces;
            std::vector<Shape> moving_geometry;
            bool free_form = false;
        };

        // Arms Move: snapshots the current selection (a no-op, stays
        // unarmed, if the selection is empty - nothing to move).
        // `geometry` must be parallel to Scene::selection() at the moment
        // of the call (one one-piece Shape per selected piece, in the
        // same order - see Geometry::extract_piece) - api.cpp's
        // arm_move_unlocked builds it from Root right before calling
        // this, since Scene itself has no Root access. Does not itself
        // check Mode - callers gate this on Mode::EDIT (see api.cpp's
        // LE_KEY_MOVE handler).
        void arm_move(std::vector<Shape> geometry)
        {
            if (selection_.empty())
                return;

            move_.armed = true;
            move_.anchor.reset();
            move_.moving_pieces = selection_;
            move_.moving_geometry = std::move(geometry);
            ++mouse_version_;
        }

        // Re-snapshots the ghost-preview geometry for the *current*
        // moving_pieces, leaving armed/anchor/free_form untouched -
        // unlike arm_move(), which also resets the anchor and rebuilds
        // moving_pieces from the current selection. For when something
        // *other* than Move itself changed the moving pieces' geometry
        // while a move is still armed - namely undo/redo (UPDATES.md
        // item 21): committing move A stays armed for a follow-up move,
        // but if the user then undoes move A instead, the armed move's
        // own moving_geometry snapshot (taken at arm/re-arm time) still
        // reflects move A's *result*, not the reverted state - a
        // subsequent move without this refresh would ghost-preview from
        // the wrong base position. A no-op if Move isn't armed - nothing
        // to refresh. `geometry` must be parallel to moving_pieces, same
        // convention as arm_move's own parameter.
        void refresh_move_geometry(std::vector<Shape> geometry)
        {
            if (!move_.armed)
                return;

            move_.moving_geometry = std::move(geometry);
            ++mouse_version_;
        }

        // Commits the current mouse position as the move's anchor point
        // (the first click of the two-click gesture) - a no-op (returns
        // false) if Move isn't armed, an anchor is already set, or no
        // mouse position is available yet.
        bool move_set_anchor()
        {
            if (!move_.armed || move_.anchor || !has_mouse_position_)
                return false;

            move_.anchor = snapped_mouse_position();
            ++mouse_version_;
            return true;
        }

        // The offset the moving shapes would be translated by right now
        // (or the ghost preview should show) - snapped_mouse_position()
        // minus the anchor, then unless `free_form`, constrained to
        // whichever axis has the larger magnitude (the other pinned to
        // 0) - the same orthogonal-by-default rule Ruler::ruler_next_point
        // already uses, just as a relative delta instead of an absolute
        // point. nullopt if not armed, no anchor yet, or no mouse
        // position.
        std::optional<Point> move_delta(bool free_form) const
        {
            if (!move_.armed || !move_.anchor)
                return std::nullopt;

            const std::optional<Point> snapped = snapped_mouse_position();
            if (!snapped)
                return std::nullopt;

            const int64_t dx = snapped->x - move_.anchor->x;
            const int64_t dy = snapped->y - move_.anchor->y;
            if (free_form)
                return Point{dx, dy};

            return std::llabs(dx) >= std::llabs(dy) ? Point{dx, 0} : Point{0, dy};
        }

        // Clears all Move state - called both on commit (the second
        // click, after the caller has already applied move_delta() to
        // every moving shape and recorded the resulting Transaction) and
        // on cancel (Escape, sharing LE_KEY_FINISH_RULER's handler - see
        // api.cpp). A no-op if Move wasn't armed.
        void end_move()
        {
            if (!move_.armed && move_.moving_pieces.empty())
                return;

            move_ = MoveState{};
            ++mouse_version_;
        }

        const MoveState &move() const { return move_; }

        // Same dedup-then-bump pattern as set_ruler_free_form - api.cpp
        // resyncs this from LE_KEY_SHIFT on every key event, right next
        // to its own ruler_free_form_ resync.
        void set_move_free_form(bool free_form)
        {
            if (free_form != move_.free_form)
            {
                move_.free_form = free_form;
                ++mouse_version_;
            }
        }
        bool move_free_form() const { return move_.free_form; }

        // The point a click would commit right now (or the ghost
        // preview should show) - grid-snapped (snapped_mouse_position()),
        // then unless `free_form` or there's no active ruler with a
        // prior committed point, constrained to whichever axis has the
        // larger delta from that point (the other axis pinned to it
        // exactly) - UPDATES.md item 13's "orthogonal by default, shift
        // for non-orthogonal". `free_form` is passed in rather than read
        // from held keys internally - see set_ruler_free_form's own
        // comment for why this class stays agnostic of api.hpp's
        // LE_KEY_SHIFT value.
        std::optional<Point> ruler_next_point(bool free_form) const
        {
            const std::optional<Point> snapped = snapped_mouse_position();
            if (!snapped)
                return std::nullopt;

            const bool has_active_last_point = !rulers_.empty() && !rulers_.back().finished && !rulers_.back().points.empty();
            if (free_form || !has_active_last_point)
                return snapped;

            const Point &last = rulers_.back().points.back();
            const int64_t dx = std::llabs(snapped->x - last.x);
            const int64_t dy = std::llabs(snapped->y - last.y);
            return dx >= dy ? Point{snapped->x, last.y} : Point{last.x, snapped->y};
        }

        // Commits ruler_next_point(free_form). If there's no active
        // ruler (none exist yet, or the last one is finished), this
        // starts a new one - unless the most recently finished ruler
        // exists and its own last point is within
        // kNewRulerMinDistancePx (converted through the current scale)
        // of the new point, in which case this is a no-op: a guard
        // against a stray click landing so close to where the last ruler
        // just finished that it would spawn a spurious near-zero-length
        // new ruler right on top of it.
        void add_ruler_point(bool free_form)
        {
            const std::optional<Point> point = ruler_next_point(free_form);
            if (!point)
                return;

            const bool has_active = !rulers_.empty() && !rulers_.back().finished;
            if (has_active && !rulers_.back().points.empty())
            {
                const Point &last = rulers_.back().points.back();
                if (last.x == point->x && last.y == point->y)
                    return; // identical to the last committed point - a
                            // no-op rather than growing the polyline with
                            // a zero-length final segment (a real observed
                            // case: it broke the "total: " label, whose
                            // own perpendicular-direction math degenerates
                            // for a zero-length last segment).
            }

            if (!has_active)
            {
                if (!rulers_.empty() && !rulers_.back().points.empty())
                {
                    const Point &last_finished = rulers_.back().points.back();
                    const double dx = static_cast<double>(point->x - last_finished.x);
                    const double dy = static_cast<double>(point->y - last_finished.y);
                    const double pixel_distance = std::sqrt(dx * dx + dy * dy) * scale_;
                    if (pixel_distance < kNewRulerMinDistancePx)
                        return;
                }
                rulers_.emplace_back();
            }
            rulers_.back().points.push_back(*point);
            ++ruler_version_;
        }

        // Marks the active ruler (if any) finished - a no-op if there
        // isn't one. Called both by the Esc-to-finish gesture
        // (le_finish_ruler) and by set_mode when leaving Ruler mode.
        void finish_active_ruler()
        {
            if (!rulers_.empty() && !rulers_.back().finished)
            {
                rulers_.back().finished = true;
                ++ruler_version_;
            }
        }

        // Ensures Ruler mode is active and finishes whatever ruler was
        // already in progress, ready for the next click to start a
        // fresh one (still subject to add_ruler_point's own distance
        // guard against restarting too close to the just-abandoned
        // ruler). Called for every LE_KEY_RULER_MODE / le_set_mode(RULER)
        // - including when mode is already RULER, which is what lets
        // 'r' double as an explicit "abandon the current ruler"
        // shortcut (a plain set_mode(RULER) would no-op when the mode
        // doesn't actually change) - this is also why hover needs its
        // own explicit clear here rather than relying on set_mode's own
        // (mode may already be RULER, so set_mode's own early-return
        // would never run its hover-clearing side effect for a re-entry).
        void reset_ruler_mode()
        {
            finish_active_ruler();
            clear_hover();
            if (mode_ != Mode::RULER)
            {
                mode_ = Mode::RULER;
                ++mouse_version_;
            }
        }

        void clear_rulers()
        {
            if (!rulers_.empty())
            {
                rulers_.clear();
                ++ruler_version_;
            }
        }

        const std::vector<Ruler> &rulers() const { return rulers_; }

        // Bumped only on a real ruler change (a point added, a ruler
        // finished, rulers cleared) - never on mouse move alone, so a
        // cached stage keyed on this doesn't have to re-walk every
        // ruler's geometry on every pointer event (same reasoning as
        // selection_version()).
        uint64_t ruler_version() const { return ruler_version_; }

        // Whether the current/next ruler point should ignore the
        // orthogonal constraint - a plain, key-code-agnostic flag;
        // api.cpp resyncs it (from its own knowledge of LE_KEY_SHIFT,
        // which this class doesn't know the meaning of) on every key
        // event, so render-side code can read "free-form active right
        // now" every frame without needing to know what LE_KEY_SHIFT
        // means. Dedups (only bumps mouse_version_ on an actual change)
        // since a resync happens on every key event, not just shift's.
        void set_ruler_free_form(bool free_form)
        {
            if (free_form != ruler_free_form_)
            {
                ruler_free_form_ = free_form;
                ++mouse_version_;
            }
        }
        bool ruler_free_form() const { return ruler_free_form_; }

        // The on-screen text size (px) for every ruler label - tick
        // values, each segment's own point-to-point distance, and a
        // ruler's running total. Runtime-configurable (le_ruler_label_size/
        // le_set_ruler_label_size) rather than a fixed style constant,
        // since legible label size varies with display/preferences.
        // Bumps visibility_version_ (not a dedicated counter), the same
        // signal set_minor_grid_spacing/set_major_grid_spacing already
        // use for "affects rendered content" - both ruler overlay stages
        // (BuildOverlayPictureStage/BuildRulerOverlayPictureStage) already
        // key on visibility_version() alongside ruler_version(), so this
        // invalidates them correctly with no new plumbing. Non-positive
        // values are rejected (keeps the last valid size), same guard as
        // set_minor_grid_spacing.
        void set_ruler_label_size_px(double px)
        {
            if (px > 0.0)
            {
                ruler_label_size_px_ = px;
                ++visibility_version_;
            }
        }
        double ruler_label_size_px() const { return ruler_label_size_px_; }

        // --- Held keys (UPDATES.md 7) ---
        // A generic set of currently-held key codes, set by the frontend
        // via press_key/release_key (see le_key_down/le_key_up) on every
        // key-down/key-up event - decoupled from any specific gesture
        // (mouse clicks, future keyboard shortcuts) so those can query
        // "is X held" internally without needing modifier/key state
        // threaded through their own call's parameter list (e.g.
        // le_mouse_up reading is_key_held for shift-click/shift-drag,
        // rather than taking a shift parameter itself). Key codes are
        // opaque ints here - api.hpp's LeKeyCode enum gives them stable,
        // platform-independent meaning at the C API boundary; Scene
        // itself doesn't interpret them.
        void press_key(int32_t key_code) { held_keys_.insert(key_code); }
        void release_key(int32_t key_code) { held_keys_.erase(key_code); }
        bool is_key_held(int32_t key_code) const { return held_keys_.contains(key_code); }

        // Call when the widget/window receiving key events loses focus
        // (see le_clear_all_keys) - a key's matching release is not
        // guaranteed to still reach a widget that no longer has focus by
        // the time the physical key comes up, so without this a modifier
        // held at the moment of a focus loss would stay "held" from this
        // API's point of view indefinitely, silently changing later
        // gestures that consult it (e.g. every future click reading as
        // shift-click) until that same key happens to be pressed and
        // released again while focused.
        void clear_all_keys() { held_keys_.clear(); }

        // --- Hover (UPDATES.md 7.1) ---
        // Which selectable object (if any) the mouse currently sits over,
        // set by the frontend's pointer-move handler via a hit-test
        // against the currently rendered shapes (see le_set_mouse_position
        // and Pipeline::hit_test_point). No separate version counter,
        // unlike mouse position - set_hover/clear_hover are only ever
        // called from within le_set_mouse_position, which already
        // unconditionally bumps mouse_version_ on every call, so
        // Renderer's overlay picture (keyed on mouse_version_) already
        // redraws on every pointer move; a second counter would be
        // redundant bookkeeping for no extra invalidation precision.
        void set_hover(std::optional<HoverTarget> hover) { hovered_ = std::move(hover); }
        void clear_hover() { hovered_.reset(); }
        const std::optional<HoverTarget> &hover() const { return hovered_; }

        // --- Layer visibility (defaults to visible until toggled) ---
        // Two independent axes, deliberately *not* per-ViewLayerId: by
        // layer name (every purpose-column of that ViewLayerRow, e.g.
        // toggling "M1" off hides both M1/TERMINAL and M1/OBSTRUCTION) and
        // by purpose (every layer with that purpose, e.g. toggling
        // OBSTRUCTION off hides every layer's obstructions at once) -
        // matching a layer-visibility widget's row-header/column-header
        // checkboxes rather than one checkbox per grid cell. A given
        // ViewLayer's effective visibility is the AND of both axes - see
        // is_view_layer_visible().
        void set_layer_name_visible(std::string layer_name, bool visible)
        {
            layer_name_visible_[std::move(layer_name)] = visible;
            ++visibility_version_;
        }

        bool is_layer_name_visible(const std::string &layer_name) const
        {
            auto it = layer_name_visible_.find(layer_name);
            return it == layer_name_visible_.end() ? true : it->second;
        }

        void set_purpose_visible(ViewLayerPurpose purpose, bool visible)
        {
            purpose_visible_[purpose] = visible;
            ++visibility_version_;
        }

        bool is_purpose_visible(ViewLayerPurpose purpose) const
        {
            auto it = purpose_visible_.find(purpose);
            return it == purpose_visible_.end() ? true : it->second;
        }

        // The actual per-ViewLayer question Pipeline::filter_by_layer_visibility
        // filters on: visible only if both its layer-name axis and its
        // purpose axis are visible.
        bool is_view_layer_visible(const std::string &layer_name, ViewLayerPurpose purpose) const
        {
            return is_layer_name_visible(layer_name) && is_purpose_visible(purpose);
        }

        // Monotonic counter bumped by set_layer_name_visible/set_purpose_visible -
        // cheap for a caller to compare instead of comparing both maps by value.
        uint64_t visibility_version() const { return visibility_version_; }

        // --- Layer selectability (defaults to selectable until toggled) ---
        // Same two-axis shape as visibility above, but deliberately doesn't
        // bump any version counter: unlike visibility, nothing here caches
        // on it yet - it's consulted by a future hit-testing/click-to-select
        // path (not implemented yet), not by Pipeline/Renderer, so there's
        // no cache to invalidate.
        void set_layer_name_selectable(std::string layer_name, bool selectable)
        {
            layer_name_selectable_[std::move(layer_name)] = selectable;
        }

        bool is_layer_name_selectable(const std::string &layer_name) const
        {
            auto it = layer_name_selectable_.find(layer_name);
            return it == layer_name_selectable_.end() ? true : it->second;
        }

        void set_purpose_selectable(ViewLayerPurpose purpose, bool selectable)
        {
            purpose_selectable_[purpose] = selectable;
        }

        bool is_purpose_selectable(ViewLayerPurpose purpose) const
        {
            auto it = purpose_selectable_.find(purpose);
            return it == purpose_selectable_.end() ? true : it->second;
        }

        bool is_view_layer_selectable(const std::string &layer_name, ViewLayerPurpose purpose) const
        {
            return is_layer_name_selectable(layer_name) && is_purpose_selectable(purpose);
        }

        // --- Selection ---
        // Renderer draws a white outline around every selected shape (see
        // Renderer::build_picture/build_overlay_picture,
        // draw_selection_outline/draw_selected_piece_outline) -
        // selection_version_ lets build_picture's (and, since piece
        // tracking, build_overlay_picture's) cache know when the
        // selection changes, decoupled from viewport_version_/
        // visibility_version_ (selection is neither a viewport nor a
        // layer-visibility concern). Only bumped on an actual change, not
        // a redundant no-op call (matches clear_mouse_position's own
        // convention) - a no-op bump would invalidate the design picture
        // cache for nothing.
        //
        // Dedup is by (shape_id, piece_kind, piece_index) identity
        // (selected_keys_, an ordered std::set of that tuple - Id<Tag>
        // already has operator<=>, and PieceKind/size_t compare
        // trivially, so no custom hash is needed) - UPDATES.md item 21's
        // piece-granular selection: two different pieces of the same
        // Shape are two independent selected entries, not deduped
        // against each other, but re-selecting the exact same piece
        // twice (e.g. shift-clicking it again, or a drag-select
        // re-enclosing it) still no-ops. This still needs to stay cheap
        // per call: le_mouse_up's drag-select branch (api.cpp) calls
        // select() once per enclosed piece, and a real design can put
        // hundreds of thousands of pieces under one shared Obstruction's
        // OBS block.
        void select(ShapeId shape_id, PieceKind piece_kind = PieceKind::RECT, size_t piece_index = 0)
        {
            if (!selected_keys_.insert({shape_id, piece_kind, piece_index}).second)
                return; // duplicate, no-op

            selection_.push_back(SelectedObject{.shape_id = shape_id, .piece_kind = piece_kind, .piece_index = piece_index});
            ++selection_version_;
        }

        void deselect(ShapeId shape_id, PieceKind piece_kind = PieceKind::RECT, size_t piece_index = 0)
        {
            if (selected_keys_.erase({shape_id, piece_kind, piece_index}) == 0)
                return;

            std::erase_if(selection_,
                           [&](const SelectedObject &selected)
                           { return selected.shape_id == shape_id && selected.piece_kind == piece_kind && selected.piece_index == piece_index; });
            ++selection_version_;
        }

        void clear_selection()
        {
            if (!selection_.empty())
            {
                selection_.clear();
                selected_keys_.clear();
                ++selection_version_;
            }
        }

        // True if *any* piece of `shape_id`'s own Shape is currently
        // selected - a whole-shape convenience query (e.g. "is this
        // object selected at all"), not piece-precise. A linear scan
        // over the current selection - fine since, unlike select()
        // itself, nothing performance-sensitive calls this today (no
        // per-enclosed-piece loop reaches it).
        bool is_selected(ShapeId shape_id) const
        {
            return std::ranges::any_of(selection_, [&](const SelectedObject &selected)
                                        { return selected.shape_id == shape_id; });
        }

        const std::vector<SelectedObject> &selection() const { return selection_; }
        uint64_t selection_version() const { return selection_version_; }

    private:
        std::set<std::tuple<ShapeId, PieceKind, size_t>> selected_keys_;
        AbstractId current_abstract_;
        Point pan_{0, 0};
        double scale_ = 1.0;
        int viewport_width_px_ = 0;
        int viewport_height_px_ = 0;
        uint64_t viewport_version_ = 0;
        int64_t minor_grid_spacing_ = 5;
        int64_t major_grid_spacing_ = 50;
        int32_t mouse_x_px_ = 0;
        int32_t mouse_y_px_ = 0;
        bool has_mouse_position_ = false;
        uint64_t mouse_version_ = 0;
        Mode mode_ = Mode::SELECT;
        std::vector<Ruler> rulers_;
        uint64_t ruler_version_ = 0;
        bool ruler_free_form_ = false;
        MoveState move_;
        double ruler_label_size_px_ = 11.0;
        // Minimum on-screen distance (px, converted via the current
        // scale) a new ruler's first point must be from the most
        // recently finished ruler's last point - see add_ruler_point.
        // A placeholder, tunable like every other threshold constant.
        static constexpr double kNewRulerMinDistancePx = 20.0;
        bool dragging_ = false;
        DragKind drag_kind_ = DragKind::SELECT;
        int32_t drag_start_x_px_ = 0;
        int32_t drag_start_y_px_ = 0;
        std::unordered_set<int32_t> held_keys_;
        std::optional<HoverTarget> hovered_;
        std::unordered_map<std::string, bool> layer_name_visible_;
        std::unordered_map<ViewLayerPurpose, bool> purpose_visible_;
        uint64_t visibility_version_ = 0;
        std::unordered_map<std::string, bool> layer_name_selectable_;
        std::unordered_map<ViewLayerPurpose, bool> purpose_selectable_;
        std::vector<SelectedObject> selection_;
        // signature (piece_signature) -> index into selection_ - see
        // select()'s own comment for why this exists.
        std::unordered_multimap<size_t, size_t> selection_index_;
        uint64_t selection_version_ = 0;
    };
}
