#pragma once

#include "api.hpp"

#include "../core/flightlines.hpp"
#include "../core/object_filters.hpp"
#include "placement_move.hpp"
#include "shape_resize.hpp"
#include "../database/database.hpp"
#include "../editing/editing.hpp"
#include "../pipelines/view_render_pipeline.hpp"
#include "../pipelines/view_style.hpp"

#include <oneapi/tbb/global_control.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// The real, C++-only definition behind the opaque LeHandle - never exposed
// in api.hpp. Owns everything needed to load a design and render it: the
// pipelines module's ViewRenderPipeline (one per handle, not one per call,
// so its MemoizingStage caching works across calls) plus every piece of
// per-handle mutable view/interaction state (current Abstract/Layout, pan/
// zoom, layer visibility, selection, rulers, Move-drag state,
// interaction mode). `le::Root`/`le::ViewLayerSet` are separate members
// (the persistent database and its rendering-layer view respectively) -
// this struct owns the mutable *view* of them, not their own content.
// It has its own header (rather than living privately in api.cpp) so
// `api/tests/le_handle_test.cpp` can construct and exercise it directly.
//
// `mutex_` exists because this handle genuinely is called from more than
// one thread: le_gui.cpp's background render thread invokes
// le_render_pixel_buffer (a single call can run for seconds on a large
// design), while the GUI's main thread and the Tcl console thread both
// reach the same pipelines/Root/view state via ordinary calls
// (le_set_mouse_position, le_mouse_down/up, le_get_mode, ...).
//
// A std::shared_mutex, not a plain std::mutex - le_render_pixel_buffer
// itself and every genuinely read-only le_* function (one that reads
// handle->root/view_layers/etc. and mutates nothing
// reachable by another caller - le_render_pixel_buffer specifically only
// touches its own private view_render_pipeline, which no other exported
// function ever references) take a shared (reader) lock via
// std::shared_lock, so plain UI queries (current mode, hierarchy depth,
// library/design listing, selection count, mouse position, tooltip) can
// run concurrently with an in-progress render instead of blocking behind
// it for however long it takes - this is what le_gui.cpp's own per-frame
// panel reads rely on to stay live and unblocked during a slow render.
// Every function that actually *mutates* handle/Root state - every
// generated le_create_X/le_update_X/le_delete_X, le_set_*, le_mouse_*,
// le_key_*, le_read_lef/_def/_verilog, le_link_unresolved_instances, ... -
// still takes a std::unique_lock (exclusive), so a real edit still
// correctly waits for an in-progress render to finish rather than racing
// it.
//
// A handful of functions look read-only but secretly rebuild a lazily-
// cached value on the handle (ensure_view_layers_current()'s own
// view_layers rebuild, used by le_layer_count/_at/le_purpose_count/_at;
// the single-slot cached_object_properties used by
// le_object_property_count/_at) - these use a double-checked pattern
// (check under a shared lock first, only escalate to a brief unique_lock
// to actually rebuild when the cache is genuinely stale, which only
// happens right after a real edit, not during a steady render) rather
// than a blanket unique_lock, so they stay on the fast, concurrent-safe
// path in the common case - see each one's own comment in api.cpp.
// generated/api/search.inc's own get_<type>/search_result_<type>_at pair
// is a *third*, different shape of hazard - it unconditionally rewrites a
// shared per-class search-result cache on every call, by design (a fresh
// query, not a staleness check), so it can't be made shared-lock-safe the
// same way; those functions still take a unique_lock deliberately.
//
// See every exported function's own std::shared_lock/std::unique_lock for
// the actual enforcement; see le_destroy's doc comment in api.hpp for the
// one function that can't be covered by the handle's own mutex.
struct LeHandle
{
    le::Root root;
    le::ViewLayerSet view_layers;

    // Flightlines - api.cpp's
    // flightlines_for fills this from le_render_pixel_buffer's
    // shared-lock read path, hence `mutable` plus its own mutex. Two
    // levels: the whole-Layout net endpoint index is rebuilt only when
    // Root mutates (or the Layout changes); the lines themselves also on
    // a selection change. `version` is ViewRenderOptions::flightline_version.
    struct FlightlineCache
    {
        std::mutex mutex;
        bool index_valid = false;
        uint64_t index_mutation_version = 0;
        le::LayoutId index_layout;
        le::NetEndpointIndex index;
        bool lines_valid = false;
        uint64_t lines_mutation_version = 0;
        uint64_t lines_selection_version = 0;
        int lines_max_fanout = 0;
        le::LayoutId lines_layout;
        std::vector<le::Flightline> lines;
        uint64_t version = 0;
    };
    mutable FlightlineCache flightline_cache;
    // How many Layout-view selections were answered from the render tree
    // (api.cpp's layout_candidates) rather than a whole-Layout scan - for
    // tests. Only touched under the handle's write lock.
    mutable std::uint64_t render_tree_selections = 0;
    // root.mutation_version() as of the most recent view_layers rebuild -
    // see api.cpp's own ensure_view_layers_current() (view_layers must be
    // rebuilt after any technology change, e.g. a plain le_create_layer
    // call, not only after le_read_lef).
    // UINT64_MAX (never a real mutation_version(), which starts at 0 and
    // only increases) rather than 0, so "never built yet" can't
    // accidentally alias a real, already-current version.
    uint64_t view_layers_built_at_version = UINT64_MAX;

    // Blend2D-backed. Text (Shape.texts - both terminal/route labels and
    // truncated, bottom-
    // left-anchored placement-name labels) is drawn via a monospace font
    // (rasterize_blend2d_stage.hpp).
    le::ViewRenderPipeline view_render_pipeline;

    // Undo/redo stack + command-recall log - every
    // generated le_create_X/le_update_X/le_delete_X function records
    // itself into whatever transaction is currently recording (see
    // command_history.is_recording()); Move
    // (le_mouse_up's Edit-mode branch) and le_repl_eval (the Tcl-side
    // wrapper every typed console command goes through) are the two
    // callers that bracket one with begin()/end().
    le::editing::CommandHistory command_history;
    std::shared_mutex mutex_;
    // Extensions' per-handle state (le::ext::ExtensionContext::data<T>()),
    // keyed by extension name and type. State lives here, not in globals,
    // because le_shell and the le_tcl module each link their own copy of
    // every extension. Its own mutex, so a lookup never waits on a render.
    std::mutex extension_data_mutex;
    std::unordered_map<std::string, std::shared_ptr<void>> extension_data;

    // Whether le_render_pixel_buffer is currently doing real work on this
    // handle, for a caller (the GUI's "rendering..." indicator) that
    // wants to know without blocking behind the render itself. Deliberately a plain
    // std::atomic<bool>, read/written with no lock - le_is_rendering()
    // must NOT take mutex_ (that's the exact mutex the render itself
    // holds for its own entire duration - see mutex_'s own doc comment -
    // so a lock-taking getter would just block until the render it's
    // reporting on already finished, defeating the whole point). Safe
    // without one: mutex_ already serializes every real le_* call
    // including le_render_pixel_buffer itself, so at most one thread is
    // ever writing this for a given handle at a time; a plain atomic is
    // exactly the right tool for "one writer under a different lock,
    // arbitrary lock-free readers".
    std::atomic<bool> is_rendering_{false};

    // Event-driven render wake-up: a render thread calls
    // le_wait_for_render_needed(handle) (api.hpp) rather than polling on
    // a fixed sleep interval (which burns CPU at idle and needs
    // machine-specific tuning). It blocks with zero CPU cost until
    // render_needed_ is true, then clears it and returns.
    // render_needed_ is a level, not an edge - notify_render_needed() only
    // ever sets it (never toggles/clears it itself), and a waiter only
    // ever clears it right before acting on it, under the same
    // render_needed_mutex_ - so a notify that arrives before anyone is
    // waiting yet (e.g. the very first le_set_viewport_size call, before
    // le_gui.cpp has even spawned the render thread) is never lost, and
    // several notifies arriving while the render thread is still busy on
    // a previous render coalesce into exactly one more render afterward
    // (view_render_options_for reads live handle state at call time, not
    // a queued snapshot, so that one extra render already reflects every
    // intervening change - nothing further to replay). A plain
    // std::mutex/std::condition_variable pair, not lock-free like
    // is_rendering_/gui_show_requested_ above - a real block/wake (not
    // just a flag a poller happens to notice) needs a condvar to wait on,
    // not another atomic.
    std::mutex render_needed_mutex_;
    std::condition_variable render_needed_cv_;
    bool render_needed_ = false;
    // Set between le_begin_command and le_end_command: a Tcl command
    // (e.g. a loop of deletes) renders once when it ends, not after each
    // mutation it makes. Written under render_needed_mutex_; read
    // lock-free by renders_held().
    std::atomic<bool> renders_held_{false};
    // Set by cancel_render_wait; wakes a waiter even while renders are
    // held, so window teardown never waits on a command that won't end
    // (a typed `exit` never reaches le_end_command). Under
    // render_needed_mutex_.
    bool render_wait_cancelled_ = false;

    // Called by HandleWriteLock's destructor (below) - every
    // std::unique_lock<std::shared_mutex> acquisition on mutex_ is, by
    // that mutex's own doc comment above, always a real mutation, so
    // notifying unconditionally on release, from one single mechanical
    // wrapper type, is what guarantees no future mutating call site can
    // forget to trigger a re-render the way a hand-picked list of
    // version-counter bump sites could (root_mutation_version_,
    // viewport_version_, selection_version_, mouse_version_,
    // ruler_version_, visibility_version_ are bumped from well over a
    // dozen separate call sites across this file alone). Over-notifying
    // for a unique_lock that didn't actually change anything rendering
    // cares about is harmless - le_wait_for_render_needed's own caller
    // (render_thread_loop, le_gui.cpp) just calls le_render_pixel_buffer
    // once, which cheaply no-ops via ViewRenderPipeline::would_recompute()
    // when nothing relevant changed (view_render_pipeline.hpp's own
    // run() doc comment).
    void notify_render_needed()
    {
        {
            std::lock_guard<std::mutex> lock(render_needed_mutex_);
            render_needed_ = true;
        }
        render_needed_cv_.notify_one();
    }

    // Blocks the calling thread (le_gui.cpp's own render thread) until
    // notify_render_needed() has been called at least once since the
    // last time this returned. No timeout, no polling - see
    // render_needed_'s own doc comment above for why a lost-wakeup can't
    // happen here.
    void wait_for_render_needed()
    {
        std::unique_lock<std::mutex> lock(render_needed_mutex_);
        render_needed_cv_.wait(lock, [this]
                               { return render_wait_cancelled_ || (render_needed_ && !renders_held_); });
        if (render_wait_cancelled_)
            render_wait_cancelled_ = false;
        else
            render_needed_ = false;
    }

    // Wakes wait_for_render_needed() even while renders are held. A level,
    // like render_needed_: a cancel before anyone waits isn't lost.
    void cancel_render_wait()
    {
        {
            std::lock_guard<std::mutex> lock(render_needed_mutex_);
            render_wait_cancelled_ = true;
        }
        render_needed_cv_.notify_all();
    }

    // Defers wait_for_render_needed() until release_renders(); mutations
    // in between still mark a render as needed.
    void hold_renders()
    {
        std::lock_guard<std::mutex> lock(render_needed_mutex_);
        renders_held_ = true;
    }

    void release_renders()
    {
        {
            std::lock_guard<std::mutex> lock(render_needed_mutex_);
            renders_held_ = false;
        }
        render_needed_cv_.notify_one();
    }

    bool renders_held() const { return renders_held_.load(std::memory_order_relaxed); }

    // Process-wide cap on how many threads
    // oneTBB's default arena may use for this handle's pipeline flow
    // graph (ViewRenderPipeline, plus any stage-internal parallelism) -
    // all of it runs on the SAME implicit default TBB arena unless told
    // otherwise, so one process-wide
    // oneapi::tbb::global_control is enough to cap every one of them
    // without threading a reference through each pipeline). global_control
    // has no setter of its own - only construction/destruction sets its
    // limit for as long as it's alive - so le_set_max_concurrency
    // destroys and reconstructs this in place under a new limit rather
    // than mutating it; std::optional makes that destroy-then-reconstruct
    // possible. max_concurrency_ mirrors the limit currently in effect so
    // le_max_concurrency() doesn't need to ask oneapi::tbb::global_control
    // (which has no public getter) what it's currently set to. Defaults
    // to 8.
    int32_t max_concurrency_ = 8;
    std::optional<oneapi::tbb::global_control> concurrency_control_{
        std::in_place, oneapi::tbb::global_control::max_allowed_parallelism, 8};

    // A Tcl console's own `show_gui` command sets this to signal the process's dedicated
    // GUI-owning thread (see src/gui/le_gui.hpp) that it should open its
    // window now, then returns immediately so the console prompt keeps
    // working - the console thread and the GUI thread are two different
    // OS threads sharing this same LeHandle, like the split is_rendering_
    // above documents. Same reasoning for staying a
    // lock-free std::atomic<bool>, test-and-cleared by
    // le_take_show_gui_request rather than read via a separate getter -
    // a request is a one-shot edge, not a level, so whichever thread
    // observes it first (there's only ever one GUI-thread reader) should
    // consume it, not leave it for a second poll to see stale.
    std::atomic<bool> gui_show_requested_{false};
    // `close_gui`'s request - same
    // one-shot, test-and-cleared shape as gui_show_requested_ above.
    std::atomic<bool> gui_close_requested_{false};

    // GUI components (src/gui/components/) mutate state two different
    // ways: a direct le_* call (mouse pan/zoom/select/move - the right
    // choice for anything needing per-frame responsiveness), or - for
    // actions that should appear in the command history (layer/purpose
    // visibility+selectability, hierarchy depth) - a *queued* Tcl command
    // string, so the action leaves the same command-history trail a typed
    // command would. The GUI thread has no Tcl interpreter of
    // its own to evaluate one directly (src/gui/ deliberately has no
    // Tcl/SWIG dependency - see le_gui.hpp's own doc comment), so it can
    // only leave the command here for whichever thread *does* own one -
    // le_shell.cpp's own console thread, via its readline event hook
    // (run_interactive's own comment) - to pick up and evaluate through
    // le_repl_eval shortly after, same as it would a typed line. A real
    // mutex (not lock-free, unlike is_rendering_/gui_show_requested_
    // above) since this is a genuine multi-item FIFO, not a single flag/
    // bit; contention is a non-issue (pushed only on a user click,
    // popped only a few times a second at most).
    std::mutex pending_tcl_commands_mutex_;
    std::deque<std::string> pending_tcl_commands_;
    // Backing storage for le_take_next_pending_tcl_command()'s own
    // returned pointer - the popped std::string itself would otherwise
    // be destroyed the moment it's removed from pending_tcl_commands_
    // above, before the caller ever reads through the pointer.
    std::string last_popped_tcl_command_;

    // Single-slot cache backing le_object_property_count/le_object_
    // property_at - rebuilt whenever a different LeObjectRef is
    // requested. le::PropertyValue (generated/property.hpp) doubles as
    // LeProperty's string-owning backing store directly - no separate
    // wrapper type needed, its shape already matches LeProperty
    // field-for-field.
    LeObjectRef cached_object_property_ref{.kind = -1, .index = UINT32_MAX, .generation = 0};
    std::vector<le::PropertyValue> cached_object_properties;

    // Backs every le_X_property_path function (get_properties dot-path
    // lookups). le::PropertyValue owns its own
    // std::string storage, and the LeProperty handed back to the caller
    // is just raw c_str() pointers into that storage (same convention as
    // every to_c(PropertyValue) call in this file) - those pointers must
    // point somewhere that outlives the function call, not a local
    // std::optional<PropertyValue>/vector that gets destroyed the moment
    // le_X_property_path returns. This single slot is that backing
    // store, "valid until the next call" like every other single-slot
    // cache above. (A short, small-string-optimized value can appear to
    // survive in a freed local by luck; a longer heap-allocated one comes
    // back corrupted.)
    le::PropertyValue cached_property_path_value;

    // Set by every le_X_property_path function right before it logs a
    // parse/validation error via spdlog::error (a malformed path or an
    // unrecognized field/hop), reset to false at the top of the next
    // such call - an errno-style single-flag signal, not a message
    // queue, so get_properties (le_tcl_procs.tcl) can still tell "this
    // path was genuinely invalid" apart from "structurally valid, just
    // resolves to nothing" (e.g. a list hop with zero elements) even
    // though both cases return the same all-null LeProperty - see
    // le_property_path_failed's own api.hpp comment.
    bool last_property_path_failed = false;

    // === Per-handle view/interaction state ===
    //
    /// @brief One selected piece - the exact rect/polygon/path that was clicked/
    /// dragged to select it, identified by its owning Shape's id plus
    /// which entry of that Shape's own rects/polygons/paths it is
    /// (`piece_kind`/`piece_index` - see Geometry::HitPiece, which
    /// LeHandle::select()'s callers build these from). The Property
    /// Viewer still resolves "the selected object" to the *owning Shape*
    /// (le_object_property_at/le_selected_object_ref in api.cpp only
    /// ever read `shape_id`, ignoring which piece) - properties are
    /// per-Shape, not per-piece; only rendering the selection outline
    /// and Move care which specific piece this is. Geometry itself is
    /// looked up on demand from Root (api.cpp's view_render_options_for
    /// resolves it for the outline) rather than stored here.
    ///
    /// One alternative of SelectedObject's variant below - every
    /// Terminal/Obstruction selection (Abstract view) and every
    /// Blockage/Route/PhysicalPort selection (Layout view) is a ShapePiece,
    /// since each has a real backing Shape; Row/Placement/Region (no
    /// backing Shape at all - see their own schema.py comments) use a
    /// bare-id alternative instead.
    struct ShapePiece
    {
        le::ShapeId shape_id;
        le::PieceKind piece_kind = le::PieceKind::RECT;
        size_t piece_index = 0;

        friend auto operator<=>(const ShapePiece &, const ShapePiece &) = default;
    };

    /// @brief One selected top-level object - a ShapePiece for
    /// anything with real backing Shape geometry (Terminal/Obstruction/
    /// Blockage/Route/PhysicalPort), or a bare id for a kind with none
    /// (RowId/RegionId - synthesized geometry; PlacementId - its own
    /// geometry is its reference design's). A std::variant, not
    /// ShapePiece with more optional fields, so "which kind is this" is
    /// never ambiguous and every consumer (select/deselect/
    /// view_render_options_for/le_selected_object_ref)
    /// is forced to handle every alternative explicitly (std::visit)
    /// rather than silently ignoring an unset field. Every alternative
    /// already has operator<=> (Id<Tag> generated, ShapePiece defaulted
    /// above), so the variant gets one for free - lets selected_keys_
    /// below be a plain std::set<SelectedObject> instead of a parallel
    /// hand-rolled tuple structure.
    using SelectedObject = std::variant<ShapePiece, le::RowId, le::PlacementId, le::RegionId>;

        // --- Currently displayed Abstract ---
        // Switching Abstracts clears selection and rulers - selection
        // holds ShapeId values scoped to whichever Abstract they were
        // selected in (they're plain
        // {index,generation} pool handles, not namespaced by Abstract),
        // so leaving them set after switching risks a stale reference
        // that, at best, matches nothing in the new Abstract (id from the
        // old one simply isn't present) and at worst - since Terminals/
        // Obstructions across all Abstracts share the same underlying
        // Pool - happens to collide with an unrelated object's reused
        // pool slot in the new one, highlighting/selecting the wrong
        // shape entirely. Rulers are plain dbu
        // Points with no Abstract scoping at all - left uncleared they'd
        // just go on being drawn, at the same raw coordinates, over
        // whatever design happens to occupy that part of the new
        // Abstract's own unrelated coordinate space. A no-op (no clear,
        // no version bumps) if `id` is the same Abstract already
        // displayed.
        void set_current_abstract(le::AbstractId id)
        {
            if (id == current_abstract_)
                return;

            current_abstract_ = id;
            clear_selection();
            clear_rulers();
        }
        le::AbstractId current_abstract() const { return current_abstract_; }

        // --- Currently displayed Layout ---
        // A second, independent "current view" tracker mirroring
        // current_abstract_'s own shape exactly (same selection/
        // ruler-clearing reasoning applies). Deliberately NOT the same
        // field as this handle's own
        // current_layout_id (below) - that one is the generated TCL
        // surface's own default-scope tracker (what get_rows/get_placements/
        // etc. read), a genuinely separate concept from this GUI-rendering
        // one - two trackers, moved together by whichever api.cpp caller
        // changes the view, see that field's own comment for the full
        // reasoning, which applies here unchanged.
        void set_current_layout(le::LayoutId id)
        {
            if (id == current_layout_id_)
                return;

            current_layout_id_ = id;
            clear_selection();
            clear_rulers();
        }
        le::LayoutId current_layout() const { return current_layout_id_; }

        // --- Flightline fanout limit ---
        // A net with more than this many endpoints besides the selected
        // pin (a high-fanout clock/reset net) draws no flightlines; 0 means
        // no limit. Negative values are ignored, like set_hierarchy_depth.
        // Part of api.cpp's flightline cache key, so a change redraws.
        void set_flightline_max_fanout(int max_fanout)
        {
            if (max_fanout >= 0)
                flightline_max_fanout_ = max_fanout;
        }
        int flightline_max_fanout() const { return flightline_max_fanout_; }

        // --- Hierarchy depth ---
        // How many further levels of Placement -> Design a Layout view
        // recurses into before falling back to a placed instance's own
        // Abstract - see HierarchyResolverStage's own doc comment for the
        // exact recursion rule. 0 (the default) means
        // every placement falls back straight to its Abstract - the
        // cheapest, always-safe starting point for a freshly opened
        // Layout view. Negative values are rejected (same "ignore invalid
        // values" convention as set_scale below) rather than silently
        // clamped, so a caller passing a bad value finds out via its own
        // return value staying unchanged, not a silently-different one.
        void set_hierarchy_depth(int depth)
        {
            if (depth < 0 || depth == hierarchy_depth_)
                return;

            hierarchy_depth_ = depth;
            ++hierarchy_version_;
        }
        int hierarchy_depth() const { return hierarchy_depth_; }
        uint64_t hierarchy_version() const { return hierarchy_version_; }

        // --- Viewport transform: pixel = (dbu - pan) * scale ---
        // pan/scale/viewport_size each bump viewport_version() - a cheap
        // change signal for callers (e.g. PipelineCache) that would
        // otherwise need to snapshot and compare these fields by value.
        void set_pan(le::Point pan)
        {
            pan_ = pan;
            ++viewport_version_;
        }
        le::Point pan() const { return pan_; }

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
        void fit_to_content(std::optional<le::Rect> bbox, int64_t padding_px)
        {
            if (!bbox || viewport_width_px_ <= 0 || viewport_height_px_ <= 0)
            {
                set_scale(1.0);
                set_pan(le::Point{0, 0});
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

            // The rasterizer's pixel transform maps (dbu - pan) * scale to pixel
            // space, i.e. pan is the dbu point that lands at pixel (0, 0) -
            // not the viewport center. To center the content, pan is offset
            // from the bbox's own lower-left corner by half of the leftover
            // (non-content) space on each axis - using the full viewport
            // size here (not the padding-reduced usable size), since
            // padding is symmetric and cancels out of the centering offset.
            const int64_t pan_x = bbox->ll.x - static_cast<int64_t>((viewport_width_px_ / scale - content_width) / 2.0);
            const int64_t pan_y = bbox->ll.y - static_cast<int64_t>((viewport_height_px_ / scale - content_height) / 2.0);

            set_scale(scale);
            set_pan(le::Point{pan_x, pan_y});
        }

        // --- Grid spacing (dbu) ---
        // Defaults assume the common "1 dbu = 1nm" convention (i.e. a
        // Technology declared with DATABASE MICRONS 1000, which most real
        // PDKs use) - 5 and 50 dbu then read as the requested 5nm minor /
        // 50nm major defaults. Not otherwise unit-aware (this handle has
        // no Technology reference to convert against) - a caller on a
        // Technology with different units should set explicit dbu values.
        // Non-positive values are rejected (keeps the last valid spacing),
        // same guard as set_scale, and setters bump visibility_version()
        // since the grid is part of the rendered picture
        // (RasterizeBlend2DStage's draw_grid_blend2d) - unlike layer
        // selectability below, this
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
        // le_render_pixel_buffer()'s output and le_zoom's x/y), not the
        // rasterizer's pre-Y-flip pixel space. Deliberately its own
        // version counter, not visibility_version - a mouse move must not
        // invalidate the expensive, design-sized rasterized images; only
        // ComposeStage's cheap overlay pass redraws.
        // Dedups (only bumps mouse_version_ on an actual change) the same
        // way set_ruler_free_form's own comment describes - le_gui.cpp's
        // own forward_mouse_input calls this unconditionally every single
        // GUI frame the mouse merely sits over the layout view, not only
        // on an actual move (ImGui reports the same MousePos every frame
        // between real OS pointer events). Without this dedup, a
        // perfectly still mouse would wake the event-driven render thread
        // (le_gui.cpp) and recompute every frame forever.
        void set_mouse_position(int32_t x_px, int32_t y_px)
        {
            if (has_mouse_position_ && mouse_x_px_ == x_px && mouse_y_px_ == y_px)
                return;
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
        // undoing the rasterizer's Y-flip the same way le_zoom's own
        // pixel->dbu conversion does - pan/scale describe the pre-flip
        // transform while a screen pixel coordinate is post-flip. scale_
        // is always positive by construction
        // (set_scale rejects non-positive values), so no divide-by-zero
        // guard is needed here. Shared by mouse_dbu_position() (the
        // currently stored mouse position) and click/drag-select
        // handling (an arbitrary x/y from a mouse-down/up event, not
        // necessarily the currently stored position - see
        // le_mouse_down/le_mouse_up).
        le::Point pixel_to_dbu(int32_t x_px, int32_t y_px) const
        {
            const double dbu_x = static_cast<double>(pan_.x) + static_cast<double>(x_px) / scale_;
            const double dbu_y = static_cast<double>(pan_.y) + (static_cast<double>(viewport_height_px_) - static_cast<double>(y_px)) / scale_;
            return le::Point{static_cast<int64_t>(dbu_x), static_cast<int64_t>(dbu_y)};
        }

        // The dbu point currently under the mouse - nullopt if no position
        // has been set yet (see has_mouse_position).
        std::optional<le::Point> mouse_dbu_position() const
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
        std::optional<le::Point> snapped_mouse_position() const
        {
            const std::optional<le::Point> dbu = mouse_dbu_position();
            if (!dbu)
                return std::nullopt;

            auto snap = [spacing = minor_grid_spacing_](int64_t v)
            {
                return static_cast<int64_t>(std::llround(static_cast<double>(v) / static_cast<double>(spacing))) * spacing;
            };
            return le::Point{snap(dbu->x), snap(dbu->y)};
        }

        // --- Drag-select / drag-zoom gesture ---
        // Which high-level action a drag gesture should perform once it
        // ends - this handle itself doesn't interpret this, it's purely a
        // label the caller (le_mouse_down/le_zoom_drag_down, and later
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
        // corresponding action - this handle itself doesn't know which
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
            ++mouse_version_; // so the drag-rect overlay (ComposeStage) starts showing immediately
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
        std::optional<le::Rect> drag_rect_dbu() const
        {
            if (!dragging_)
                return std::nullopt;

            const std::optional<le::Point> current = mouse_dbu_position();
            if (!current)
                return std::nullopt;

            const le::Point start = pixel_to_dbu(drag_start_x_px_, drag_start_y_px_);
            return le::Rect{
                .ll = le::Point{std::min(start.x, current->x), std::min(start.y, current->y)},
                .ur = le::Point{std::max(start.x, current->x), std::max(start.y, current->y)},
            };
        }

        // --- Mode ---
        // Select mode is the only mode where mouse interaction (le_mouse_up)
        // changes the current selection; Edit mode restricts mouse
        // interaction to editing whatever is already selected (Move,
        // Resize, Delete), so the selection can only change back in
        // Select mode. Ruler mode is documented in the Rulers section below.
        enum class Mode
        {
            SELECT,
            EDIT,
            RULER,
        };

        // Leaving Ruler mode always finishes whatever ruler was active
        // (see finish_active_ruler) - keeps "an active ruler exists only
        // in Ruler mode" an invariant callers (render, tests) can rely on
        // rather than a coincidence. Bumps mouse_version_ on
        // an actual mode change only - the ghost-ruler overlay's content
        // is mode-dependent (ComposeStage) and needs to invalidate on
        // mode switch.
        void set_mode(Mode mode)
        {
            if (mode == mode_)
                return;
            if (mode_ == Mode::RULER)
                finish_active_ruler();
            if (mode_ == Mode::EDIT)
            {
                end_move();   // leaving Edit mode cancels an in-progress (not yet committed) Move, same reasoning as finishing an active ruler above
                end_resize(); // ...and likewise Resize
            }
            mode_ = mode;
            ++mouse_version_;
        }
        Mode mode() const { return mode_; }

        // --- Rulers ---
        // A ruler is a polyline of already-committed (grid-snapped)
        // points; `finished` is set once the user presses Esc to stop
        // adding to it (see le_finish_ruler) or leaves Ruler mode (see
        // set_mode above). Multiple rulers can exist at once - starting
        // a new one never clears an existing one. The *last* entry is "the active
        // ruler" new clicks append to, if and only if it exists and
        // isn't finished - this struct maintains that as an invariant
        // rather than something callers have to check for themselves.
        struct Ruler
        {
            std::vector<le::Point> points;
            bool finished = false;
        };

        // --- Move ---
        // Transient, uncommitted drag state for the two-click Move
        // gesture in Edit mode - the direct analog of Ruler above (armed/
        // anchored/free-form flag instead of a committed polyline), see
        // arm_move()/move_set_anchor()/move_delta()/end_move(). Never
        // itself mutates Root - a committed Move is recorded as an
        // ordinary editing::Transaction of per-piece update steps by the
        // caller (see api.cpp's le_mouse_up EDIT-mode branch), once the
        // second click lands. Piece-granular: `moving_pieces` is a direct
        // copy of `selection_` at
        // arm time, so Move moves exactly whichever pieces are selected,
        // not necessarily every piece of their owning Shapes.
        // `moving_geometry` is a snapshot of each moving *piece's* own
        // current one-piece geometry (Geometry::extract_piece), parallel
        // to `moving_pieces`, taken once at arm_move()/re-arm time (Move
        // never changes *which* pieces are moving mid-gesture, only the
        // offset) - this keeps the render-side ghost overlay
        // (ComposeStage) Root-agnostic.
        //
        // Placement moves ride the same
        // state: `anchor_raw` is the anchor's own unsnapped dbu position
        // (a Placement's delta comes from the raw mouse - its own
        // snapping, placement_snap_mode(), replaces the user-grid snap).
        // Reset on every arm/re-arm.
        struct MoveState
        {
            bool armed = false;
            std::optional<le::Point> anchor;
            std::optional<le::Point> anchor_raw;
            std::vector<SelectedObject> moving_pieces;
            std::vector<le::ShapeData> moving_geometry;
            bool free_form = false;
        };

        // Arms Move: snapshots the current selection (a no-op, stays
        // unarmed, if the selection is empty - nothing to move).
        // `geometry` must be parallel to LeHandle::selection() at the
        // moment of the call (one one-piece Shape per selected piece, in
        // the same order - see Geometry::extract_piece) - api.cpp's
        // arm_move_unlocked builds it from Root right before calling
        // this, since this handle's own view state has no Root access
        // here. Does not itself check Mode - callers gate this on
        // Mode::EDIT (see api.cpp's LE_KEY_MOVE handler).
        void arm_move(std::vector<le::ShapeData> geometry)
        {
            if (selection_.empty())
                return;

            end_resize(); // one Edit-mode tool at a time
            move_.armed = true;
            move_.anchor.reset();
            move_.anchor_raw.reset();
            move_.moving_pieces = selection_;
            move_.moving_geometry = std::move(geometry);
            ++mouse_version_;
        }

        // Re-snapshots the ghost-preview geometry for the *current*
        // moving_pieces, leaving armed/anchor/free_form untouched -
        // unlike arm_move(), which also resets the anchor and rebuilds
        // moving_pieces from the current selection. For when something
        // *other* than Move itself changed the moving pieces' geometry
        // while a move is still armed - namely undo/redo: committing move
        // A stays armed for a follow-up move,
        // but if the user then undoes move A instead, the armed move's
        // own moving_geometry snapshot (taken at arm/re-arm time) still
        // reflects move A's *result*, not the reverted state - a
        // subsequent move without this refresh would ghost-preview from
        // the wrong base position. A no-op if Move isn't armed - nothing
        // to refresh. `geometry` must be parallel to moving_pieces, same
        // convention as arm_move's own parameter.
        void refresh_move_geometry(std::vector<le::ShapeData> geometry)
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
            move_.anchor_raw = mouse_dbu_position();
            ++mouse_version_;
            return true;
        }

        // Placement counterpart of move_delta: the *unsnapped* mouse
        // offset from anchor_raw, axis-constrained the same way unless
        // `free_form`. nullopt until anchored (the ghost, like a shape
        // move's, only shows from the first click), or with no mouse
        // position.
        std::optional<le::Point> move_raw_delta(bool free_form) const
        {
            if (!move_.armed || !move_.anchor_raw)
                return std::nullopt;

            const std::optional<le::Point> raw = mouse_dbu_position();
            if (!raw)
                return std::nullopt;

            const int64_t dx = raw->x - move_.anchor_raw->x;
            const int64_t dy = raw->y - move_.anchor_raw->y;
            if (free_form)
                return le::Point{dx, dy};
            return std::llabs(dx) >= std::llabs(dy) ? le::Point{dx, 0} : le::Point{0, dy};
        }

        // The PlacementIds among moving_pieces, in selection order - empty
        // unless Move is armed with at least one Placement selected (the
        // secondary toolbar's own visibility condition).
        std::vector<le::PlacementId> moving_placements() const
        {
            std::vector<le::PlacementId> ids;
            for (const SelectedObject &selected : move_.moving_pieces)
                if (const le::PlacementId *id = std::get_if<le::PlacementId>(&selected))
                    ids.push_back(*id);
            return ids;
        }

        // --- Resize ---
        // A tool like Move, armed in Edit mode (arm_resize - one of the two
        // at a time), then driven by two clicks, like Move: while nothing is
        // grabbed, `hover` is the selected piece's edge/segment under the
        // mouse (highlighted, and the GUI's resize cursor); a click on it
        // grabs it (`grab`: which piece and handle, the piece's original
        // one-piece geometry, and the raw dbu click point), the ghost
        // follows the mouse, and a second click commits it (api.cpp).
        // Escape cancels a grab (and disarms when nothing is grabbed);
        // otherwise it stays armed across grabs until leaving Edit mode.
        struct ResizeGrab
        {
            ShapePiece piece;
            le::ResizeHandle handle;
            le::ShapeData original;
            le::Point start;
        };
        struct ResizeState
        {
            bool armed = false;
            std::optional<ResizeGrab> grab;
            std::optional<le::ResizeHandleSegment> hover;
        };

        // Arms Resize, disarming Move - a no-op with nothing selected.
        // Callers gate on Mode::EDIT (api.cpp's arm_resize_unlocked).
        void arm_resize()
        {
            if (selection_.empty())
                return;
            end_move();
            resize_.armed = true;
            resize_.grab.reset();
            ++mouse_version_;
        }

        void begin_resize_grab(ResizeGrab grab)
        {
            if (!resize_.armed)
                return;
            resize_.grab = std::move(grab);
            resize_.hover.reset();
            ++mouse_version_;
        }

        // The hover indicator - api.cpp's le_set_mouse_position recomputes
        // it on every mouse move while armed and nothing is grabbed.
        void set_resize_hover(std::optional<le::ResizeHandleSegment> hover)
        {
            const auto same = [](const std::optional<le::ResizeHandleSegment> &a, const std::optional<le::ResizeHandleSegment> &b)
            {
                if (a.has_value() != b.has_value())
                    return false;
                return !a || (a->a.x == b->a.x && a->a.y == b->a.y && a->b.x == b->b.x && a->b.y == b->b.y && a->axis == b->axis);
            };
            if (same(resize_.hover, hover))
                return;
            resize_.hover = hover;
            ++mouse_version_;
        }

        void end_resize_grab()
        {
            if (!resize_.grab)
                return;
            resize_.grab.reset();
            ++mouse_version_;
        }

        void end_resize()
        {
            if (!resize_.armed && !resize_.grab)
                return;
            resize_ = ResizeState{};
            ++mouse_version_;
        }

        const ResizeState &resize() const { return resize_; }

        // Per piece kind (PieceKind's own ordinal): what a resized
        // edge/segment snaps to - persists across grabs, USER_GRID by
        // default (always available). Bumps mouse_version_ on a real
        // change so a live ghost re-snaps immediately.
        // Vias and via arrays share the path slot (le::shape_snap_slot) -
        // they snap only when moved.
        void set_shape_snap_mode(le::PieceKind kind, le::ShapeSnapMode mode)
        {
            le::ShapeSnapMode &slot = shape_snap_modes_[static_cast<size_t>(le::shape_snap_slot(kind))];
            if (slot == mode)
                return;
            slot = mode;
            ++mouse_version_;
        }
        le::ShapeSnapMode shape_snap_mode(le::PieceKind kind) const { return shape_snap_modes_[static_cast<size_t>(le::shape_snap_slot(kind))]; }

        // What a moving Placement's location snaps to (the secondary
        // toolbar's snap buttons) - persists across moves, SITE by
        // default. Bumps mouse_version_ on a real change so the ghost
        // re-snaps immediately.
        void set_placement_snap_mode(le::PlacementSnapMode mode)
        {
            if (mode == placement_snap_mode_)
                return;
            placement_snap_mode_ = mode;
            ++mouse_version_;
        }
        le::PlacementSnapMode placement_snap_mode() const { return placement_snap_mode_; }

        // The offset the moving shapes would be translated by right now
        // (or the ghost preview should show) - snapped_mouse_position()
        // minus the anchor, then unless `free_form`, constrained to
        // whichever axis has the larger magnitude (the other pinned to
        // 0) - the same orthogonal-by-default rule ruler_next_point
        // already uses, just as a relative delta instead of an absolute
        // point. nullopt if not armed, no anchor yet, or no mouse
        // position.
        std::optional<le::Point> move_delta(bool free_form) const
        {
            if (!move_.armed || !move_.anchor)
                return std::nullopt;

            const std::optional<le::Point> snapped = snapped_mouse_position();
            if (!snapped)
                return std::nullopt;

            const int64_t dx = snapped->x - move_.anchor->x;
            const int64_t dy = snapped->y - move_.anchor->y;
            if (free_form)
                return le::Point{dx, dy};

            return std::llabs(dx) >= std::llabs(dy) ? le::Point{dx, 0} : le::Point{0, dy};
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
        // exactly) - "orthogonal by default, shift for non-orthogonal".
        // `free_form` is passed in rather than read
        // from held keys internally - see set_ruler_free_form's own
        // comment for why this handle stays agnostic of api.hpp's
        // LE_KEY_SHIFT value.
        std::optional<le::Point> ruler_next_point(bool free_form) const
        {
            const std::optional<le::Point> snapped = snapped_mouse_position();
            if (!snapped)
                return std::nullopt;

            const bool has_active_last_point = !rulers_.empty() && !rulers_.back().finished && !rulers_.back().points.empty();
            if (free_form || !has_active_last_point)
                return snapped;

            const le::Point &last = rulers_.back().points.back();
            const int64_t dx = std::llabs(snapped->x - last.x);
            const int64_t dy = std::llabs(snapped->y - last.y);
            return dx >= dy ? le::Point{snapped->x, last.y} : le::Point{last.x, snapped->y};
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
            const std::optional<le::Point> point = ruler_next_point(free_form);
            if (!point)
                return;

            const bool has_active = !rulers_.empty() && !rulers_.back().finished;
            if (has_active && !rulers_.back().points.empty())
            {
                const le::Point &last = rulers_.back().points.back();
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
                    const le::Point &last_finished = rulers_.back().points.back();
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
        // doesn't actually change).
        void reset_ruler_mode()
        {
            finish_active_ruler();
            // Through set_mode, not a bare mode_ assignment, so leaving Edit
            // mode this way ('r' from Edit) also disarms Move/Resize.
            set_mode(Mode::RULER);
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
        // which this handle doesn't know the meaning of) on every key
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
        // use for "affects rendered content", so ruler overlays redraw
        // with no new plumbing. Non-positive
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

        // Smallest / largest on-screen size (px) a shape or placement label
        // is drawn at - labels scale with their geometry between the two
        // (the Settings panel's min/max label font sizes). Same visibility_version_
        // signal and non-positive guard as set_ruler_label_size_px. Not
        // cross-checked against each other (setting them in either order
        // must work) - a min above the max yields to it at draw time.
        void set_label_min_size_px(double px)
        {
            if (px > 0.0)
            {
                label_min_size_px_ = px;
                ++visibility_version_;
            }
        }
        double label_min_size_px() const { return label_min_size_px_; }
        void set_label_max_size_px(double px)
        {
            if (px > 0.0)
            {
                label_max_size_px_ = px;
                ++visibility_version_;
            }
        }
        double label_max_size_px() const { return label_max_size_px_; }

        // Colors picked in the Layers panel, by row name (a Layer's name,
        // or a pseudo-row's) - applied
        // on top of the default palette by every view_layers rebuild
        // (api.cpp's rebuild_view_layers) and by the render graph's own
        // LayerGenerationStage (ViewRenderOptions::layer_color_overrides).
        // A name with no row yet (a settings file read before the LEF) is
        // kept and applies once the row exists. Opaque - alpha is ignored.
        void set_layer_color(const std::string &row_name, le::Color color)
        {
            color.a = 255;
            layer_color_overrides_[row_name] = color;
            view_layers.set_row_color(row_name, color);
        }
        // True if `row_name` had a color to drop - the caller then rebuilds
        // view_layers so the row goes back to its default.
        bool reset_layer_color(const std::string &row_name) { return layer_color_overrides_.erase(row_name) > 0; }
        const std::map<std::string, le::Color> &layer_color_overrides() const { return layer_color_overrides_; }
        void set_layer_color_overrides(std::map<std::string, le::Color> overrides) { layer_color_overrides_ = std::move(overrides); }

        // Grid spacing (um) from a settings file read before any
        // Technology existed to convert it to dbu - applied by api.cpp once
        // one does.
        std::optional<double> pending_minor_grid_um;
        std::optional<double> pending_major_grid_um;

        // --- Held keys ---
        // A generic set of currently-held key codes, set by the frontend
        // via press_key/release_key (see le_key_down/le_key_up) on every
        // key-down/key-up event - decoupled from any specific gesture
        // (mouse clicks, future keyboard shortcuts) so those can query
        // "is X held" internally without needing modifier/key state
        // threaded through their own call's parameter list (e.g.
        // le_mouse_up reading is_key_held for shift-click/shift-drag,
        // rather than taking a shift parameter itself). Key codes are
        // opaque ints here - api.hpp's LeKeyCode enum gives them stable,
        // platform-independent meaning at the C API boundary; this
        // handle itself doesn't interpret them.
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

        // --- Layer visibility (defaults to visible until toggled, except
        // TRACK_PREFERRED/TRACK_NON_PREFERRED/ROW/GCELLGRID, pre-seeded
        // invisible below) ---
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

        void set_purpose_visible(le::ViewLayerPurpose purpose, bool visible)
        {
            purpose_visible_[purpose] = visible;
            ++visibility_version_;
        }

        bool is_purpose_visible(le::ViewLayerPurpose purpose) const
        {
            auto it = purpose_visible_.find(purpose);
            return it == purpose_visible_.end() ? true : it->second;
        }

        // The actual per-ViewLayer visibility question rendering asks:
        // visible only if both its layer-name axis and its
        // purpose axis are visible.
        bool is_view_layer_visible(const std::string &layer_name, le::ViewLayerPurpose purpose) const
        {
            return is_layer_name_visible(layer_name) && is_purpose_visible(purpose);
        }

        // Read-only access to both maps directly, for a caller (api.cpp's
        // own view_render_options_for) building a ViewRenderOptions
        // snapshot to hand to the new pipelines module - copied by value
        // there rather than threading a reference/pointer into
        // ViewRenderOptions, which otherwise has no dependency on this
        // handle's own type at all.
        const std::unordered_map<std::string, bool> &layer_name_visibility() const { return layer_name_visible_; }
        const std::unordered_map<std::string, bool> &layer_name_selectability() const { return layer_name_selectable_; }
        const std::unordered_map<le::ViewLayerPurpose, bool> &purpose_visibility() const { return purpose_visible_; }

        // Monotonic counter bumped by set_layer_name_visible/set_purpose_visible
        // - cheap for a caller to compare instead of comparing both maps by
        // value.
        uint64_t visibility_version() const { return visibility_version_; }

        // --- Layer selectability (defaults to selectable until toggled,
        // except TRACK_PREFERRED/TRACK_NON_PREFERRED/GCELLGRID, pre-seeded
        // non-selectable below) ---
        // Same two-axis shape as visibility above, but deliberately doesn't
        // bump any version counter: it's consulted only by hit-testing
        // (is_view_layer_selectable), never by rendering, so there's no
        // cache to invalidate.
        void set_layer_name_selectable(std::string layer_name, bool selectable)
        {
            layer_name_selectable_[std::move(layer_name)] = selectable;
        }

        bool is_layer_name_selectable(const std::string &layer_name) const
        {
            auto it = layer_name_selectable_.find(layer_name);
            return it == layer_name_selectable_.end() ? true : it->second;
        }

        void set_purpose_selectable(le::ViewLayerPurpose purpose, bool selectable)
        {
            purpose_selectable_[purpose] = selectable;
        }

        bool is_purpose_selectable(le::ViewLayerPurpose purpose) const
        {
            auto it = purpose_selectable_.find(purpose);
            return it == purpose_selectable_.end() ? true : it->second;
        }

        bool is_view_layer_selectable(const std::string &layer_name, le::ViewLayerPurpose purpose) const
        {
            return is_layer_name_selectable(layer_name) && is_purpose_selectable(purpose);
        }

        // --- Placement.type / Route.use filters (LeObjectFilter) ---
        // Per-value visibility and selectability, on top of the purpose
        // axes above: a Placement or Route draws only if its purpose and
        // its value are both visible. Values are matched upper-case
        // (le::to_filter_value); everything is visible and selectable by
        // default. Hiding a value bumps visibility_version_.
        void set_object_filter_visible(int32_t filter, const std::string &value, bool visible)
        {
            if (std::set<std::string> *values = filter_values(hidden_objects_, filter))
            {
                set_membership(*values, le::to_filter_value(value), !visible);
                ++visibility_version_;
            }
        }

        bool is_object_filter_visible(int32_t filter, const std::string &value) const
        {
            const std::set<std::string> *values = filter_values(hidden_objects_, filter);
            return !values || !values->contains(le::to_filter_value(value));
        }

        void set_object_filter_selectable(int32_t filter, const std::string &value, bool selectable)
        {
            if (std::set<std::string> *values = filter_values(unselectable_objects_, filter))
                set_membership(*values, le::to_filter_value(value), !selectable);
        }

        bool is_object_filter_selectable(int32_t filter, const std::string &value) const
        {
            const std::set<std::string> *values = filter_values(unselectable_objects_, filter);
            return !values || !values->contains(le::to_filter_value(value));
        }

        // Hidden values (rendering and selection) and unselectable ones
        // (selection only).
        const le::ObjectFilterSets &hidden_objects() const { return hidden_objects_; }
        const le::ObjectFilterSets &unselectable_objects() const { return unselectable_objects_; }

        // le_object_filter_value_count/_at's LE_OBJECT_FILTER_PLACEMENT_TYPE
        // list, rebuilt when Root has changed since. Read under a shared
        // lock, so it has a mutex of its own.
        const std::vector<std::string> &placement_type_values() const
        {
            std::lock_guard<std::mutex> lock(placement_type_values_mutex_);
            if (placement_type_values_version_ != root.mutation_version())
            {
                placement_type_values_ = le::placement_type_values(root);
                placement_type_values_version_ = root.mutation_version();
            }
            return placement_type_values_;
        }

        // --- Selection ---
        // ComposeStage draws a white outline around every selected piece -
        // selection_version_ lets its cache know when the
        // selection changes, decoupled from viewport_version_/
        // visibility_version_ (selection is neither a viewport nor a
        // layer-visibility concern). Only bumped on an actual change, not
        // a redundant no-op call (matches clear_mouse_position's own
        // convention) - a no-op bump would invalidate the composed frame
        // for nothing.
        //
        // Dedup is by SelectedObject identity (selected_keys_, an ordered
        // std::set<SelectedObject> - every alternative already has
        // operator<=>, so no custom hash/tuple wrapper is needed) -
        // piece-granular selection: two different
        // pieces of the same Shape are two independent selected entries,
        // not deduped against each other, but re-selecting the exact same
        // piece (or the same whole Row/Placement/Region) twice (e.g.
        // shift-clicking it again, or a drag-select re-enclosing it)
        // still no-ops. This still needs to stay cheap per call:
        // le_mouse_up's drag-select branch (api.cpp) calls select() once
        // per enclosed piece, and a real design can put hundreds of
        // thousands of pieces under one shared Obstruction's OBS block.
        void select(le::ShapeId shape_id, le::PieceKind piece_kind = le::PieceKind::RECT, size_t piece_index = 0)
        {
            select_object(SelectedObject{ShapePiece{.shape_id = shape_id, .piece_kind = piece_kind, .piece_index = piece_index}});
        }

        // Whole-object selection for a kind with no piece concept
        // (no backing Shape to address a rect/polygon/path within - see
        // SelectedObject's own comment). One overload per bare-id
        // alternative rather than a single templated `select(auto)`: the
        // implicit SelectedObject construction below only works for a
        // type that's actually one of the variant's own alternatives, and
        // an explicit overload set gives a real compile error at the call
        // site for anything else, not a confusing variant-construction one.
        void select(le::RowId row_id) { select_object(SelectedObject{row_id}); }
        void select(le::PlacementId placement_id) { select_object(SelectedObject{placement_id}); }
        void select(le::RegionId region_id) { select_object(SelectedObject{region_id}); }

        void deselect(le::ShapeId shape_id, le::PieceKind piece_kind = le::PieceKind::RECT, size_t piece_index = 0)
        {
            deselect_object(SelectedObject{ShapePiece{.shape_id = shape_id, .piece_kind = piece_kind, .piece_index = piece_index}});
        }
        void deselect(le::RowId row_id) { deselect_object(SelectedObject{row_id}); }
        void deselect(le::PlacementId placement_id) { deselect_object(SelectedObject{placement_id}); }
        void deselect(le::RegionId region_id) { deselect_object(SelectedObject{region_id}); }

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
        bool is_selected(le::ShapeId shape_id) const
        {
            return std::ranges::any_of(selection_, [&](const SelectedObject &selected)
                                        {
                const auto *piece = std::get_if<ShapePiece>(&selected);
                return piece && piece->shape_id == shape_id; });
        }

        // Whole-object selected query, one overload per bare-id
        // alternative, same reasoning as the select() overload set above.
        bool is_selected(le::RowId row_id) const { return selected_keys_.contains(SelectedObject{row_id}); }
        bool is_selected(le::PlacementId placement_id) const { return selected_keys_.contains(SelectedObject{placement_id}); }
        bool is_selected(le::RegionId region_id) const { return selected_keys_.contains(SelectedObject{region_id}); }

        const std::vector<SelectedObject> &selection() const { return selection_; }
        uint64_t selection_version() const { return selection_version_; }

        // Adds one already-built SelectedObject of any kind - for a caller
        // holding a mixed list of candidates (api.cpp's click cycling).
        void select_any(const SelectedObject &object) { select_object(object); }

    private:
        std::set<SelectedObject> selected_keys_;

        // Shared core of every select()/deselect() overload above - one
        // place that maintains selected_keys_/selection_/selection_version_
        // together, so every public overload (ShapeId+piece, or a bare
        // RowId/PlacementId/RegionId) goes through the identical dedup/
        // bump logic instead of re-deriving it per overload.
        void select_object(SelectedObject object)
        {
            if (!selected_keys_.insert(object).second)
                return; // duplicate, no-op

            selection_.push_back(object);
            ++selection_version_;
        }

        void deselect_object(const SelectedObject &object)
        {
            if (selected_keys_.erase(object) == 0)
                return;

            std::erase(selection_, object);
            ++selection_version_;
        }
        le::AbstractId current_abstract_;
        le::LayoutId current_layout_id_;
        int hierarchy_depth_ = 0;
        uint64_t hierarchy_version_ = 0;
        le::Point pan_{0, 0};
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
        ResizeState resize_;
        int flightline_max_fanout_ = 10;
        std::array<le::ShapeSnapMode, 4> shape_snap_modes_{le::ShapeSnapMode::USER_GRID, le::ShapeSnapMode::USER_GRID, le::ShapeSnapMode::USER_GRID, le::ShapeSnapMode::USER_GRID}; // RECT, POLYGON, PATH (+ VIA, VIA_ITERATE), unused VIA slot
        le::PlacementSnapMode placement_snap_mode_ = le::PlacementSnapMode::SITE;
        double ruler_label_size_px_ = 11.0;
        double label_min_size_px_ = 12.0; // draw_helpers.hpp's kMinLabelPixelSize
        double label_max_size_px_ = 24.0; // draw_helpers.hpp's kMaxLabelPixelSize
        std::map<std::string, le::Color> layer_color_overrides_;

    public:
        // What "saved" means for the exit
        // confirmation. The design is unsaved once root's mutation version
        // moves past saved_mutation_version (set at creation and by a
        // successful write_db or read_db - only a .led file holds the whole
        // database, so a design read from LEF/DEF/Verilog, or written only to
        // them, is unsaved). Settings are unsaved once
        // settings_to_json no longer matches saved_settings_json (set at
        // creation, by save_settings/load_settings, and by a clean read,
        // which can fill in grid spacing in um). Maintained by api.cpp.
        uint64_t saved_mutation_version = 0;
        // The native database file last read or written (read_db/write_db),
        // the GUI's File > Save target; empty until there is one.
        std::string db_path;
        // Whether the GUI asks before a Save overwrites a design file (a
        // setting, saved in settings.json).
        bool confirm_overwrite = true;
        // Bumped by ExtensionContext::request_redraw (from any thread): the
        // render's extension_overlay_version, so Compose redraws.
        std::atomic<uint64_t> extension_overlay_version{0};
        std::string saved_settings_json;
        // The last loaded settings file's top-level keys this version
        // doesn't know (a JSON object, or "" for none) - written back by
        // save_settings so a newer layout_engine's settings survive a save
        // from this one.
        std::string unknown_settings_json;
        // settings.json's "extensions" sections whose extension this build
        // doesn't have (a JSON object, or empty): kept and written back, so
        // a build without an extension doesn't lose its settings.
        std::string unknown_extension_settings_json;

    private:
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
        std::unordered_map<std::string, bool> layer_name_visible_;
        // {purpose, false} for each purpose whose `flag` is false in the
        // generated registry.
        static std::unordered_map<le::ViewLayerPurpose, bool> purposes_off_by_default(bool le::ViewLayerPurposeInfo::*flag)
        {
            std::unordered_map<le::ViewLayerPurpose, bool> off;
            for (const le::ViewLayerPurposeInfo &info : le::kViewLayerPurposes)
                if (!(info.*flag))
                    off.emplace(info.purpose, false);
            return off;
        }
        // Pre-seeded with every purpose the registry hides by default; the
        // rest fall back to is_purpose_visible()'s "unknown key -> visible",
        // and the pipeline reads this map the same way.
        std::unordered_map<le::ViewLayerPurpose, bool> purpose_visible_ = purposes_off_by_default(&le::ViewLayerPurposeInfo::visible_by_default);
        uint64_t visibility_version_ = 0;
        std::unordered_map<std::string, bool> layer_name_selectable_;
        // Pre-seeded the same way from selectable_by_default.
        std::unordered_map<le::ViewLayerPurpose, bool> purpose_selectable_ = purposes_off_by_default(&le::ViewLayerPurposeInfo::selectable_by_default);
        le::ObjectFilterSets hidden_objects_;
        le::ObjectFilterSets unselectable_objects_;
        mutable std::mutex placement_type_values_mutex_;
        mutable std::vector<std::string> placement_type_values_;
        mutable uint64_t placement_type_values_version_ = std::numeric_limits<uint64_t>::max();

        // `sets`' value set for an LeObjectFilter; nullptr for an unknown one.
        template <typename Sets>
        static auto filter_values(Sets &sets, int32_t filter) -> decltype(&sets.placement_types)
        {
            if (filter == LE_OBJECT_FILTER_PLACEMENT_TYPE)
                return &sets.placement_types;
            if (filter == LE_OBJECT_FILTER_ROUTE_USE)
                return &sets.route_uses;
            return nullptr;
        }
        static void set_membership(std::set<std::string> &values, std::string value, bool member)
        {
            if (member)
                values.insert(std::move(value));
            else
                values.erase(value);
        }

        std::vector<SelectedObject> selection_;
        // signature (piece_signature) -> index into selection_ - see
        // select()'s own comment for why this exists.
        std::unordered_multimap<size_t, size_t> selection_index_;
        uint64_t selection_version_ = 0;

    public:
        // The Shapes the most recent le_shape_* call created, read back via
        // le_shape_op_result_at - same "count now, _at later" shape as the
        // generated search-result caches below.
        std::vector<le::ShapeId> shape_op_results;

        // Generated TCL property-reading cache - one cached_X_property_id/
        // cached_X_properties pair per TCL-readable class not already covered
        // by hand-written code above. Never edit generated/api/
        // handle_fields.inc directly - the build regenerates it.
#include "generated/api/handle_fields.inc"
};

// Every genuine mutation of a LeHandle goes through exactly this pattern
// (mutex_'s own doc comment above: a std::unique_lock<std::shared_mutex>
// is *only* ever taken by a real write) - wrapping that acquisition in
// this one RAII type, in place of a bare std::unique_lock, is what makes
// notify_render_needed() (LeHandle's own doc comment above) fire on
// every mutating call site mechanically, without maintaining a hand-
// picked list of them. Construct as `HandleWriteLock lock(handle);` -
// same call-site shape as the std::unique_lock it replaces, so the bulk
// conversion across api.cpp (and the codegen fork's own generated
// create/update/delete bodies, schema.py) is a pure find-and-replace.
class HandleWriteLock
{
public:
    explicit HandleWriteLock(LeHandle *handle) : handle_(handle), lock_(handle->mutex_) {}
    ~HandleWriteLock() { handle_->notify_render_needed(); }

    HandleWriteLock(const HandleWriteLock &) = delete;
    HandleWriteLock &operator=(const HandleWriteLock &) = delete;

private:
    LeHandle *handle_;
    std::unique_lock<std::shared_mutex> lock_;
};
