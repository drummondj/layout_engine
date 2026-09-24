# Overnight review — 2026-09-24/25

NEW_FEATURES_SEPT_2026.md items 3, 4 and 5, worked unattended with the
overnight-review pattern: research -> implement -> test -> commit+push per
item. Judgment calls (ambiguities decided without anyone to ask) are marked
**Judgment call:** under each item so they're easy to find and override.

Order worked: 4 (custom library naming), 5 (flightlines), 3 (shape resizing)
— smallest/most self-contained first, the large interactive one last.

## Item 4 — Custom library naming

**What was needed:** `read_lef`/`read_def`/`read_verilog` named their
library after the file stem (`api.cpp`'s `le_read_*` passed
`path.stem()`). The library must be a required TCL argument, created if
missing; re-reading a view a design already has must be an error, while
Abstract/Layout/Schematic views of one design can come from different reads.

**Bugs found along the way:**
- `LEFReader::lefrMacroBeginCbkFn` never looked a library up by name — it
  created a new one per `read_lef`, so two reads "into" the same name made
  two libraries.
- `DEFReader::defrDesignCbkFn` logged "already has a Layout" but returned 0,
  so the parse carried on with an invalid `layout_id_` and `read_def`
  reported success.
- `SVReader`'s `get_or_create_schematic` silently reused an existing
  Schematic, merging a second read's ports/nets/instances into it.

**Fix:**
- `le_read_lef`/`le_read_def`/`le_read_verilog` (`api.hpp`) take a required
  `library_name` (null/empty -> error, nothing read). TCL: `read_lef
  -library <name> <path>`, `read_def -library <name> <path>`, `read_verilog
  -netlist|-rtl -library <name> <path>...` (`_take_library_flag`,
  `le_tcl_procs.tcl`).
- New `src/database/library_helpers.hpp`: `get_or_create_library`, and
  `get_or_create_design` (global name lookup, new designs into the named
  library, warns when an existing design lives in a different library).
- LEF: existing Abstract -> error (read fails). DEF: existing Layout ->
  callback returns 1, `read_def` returns nonzero. Verilog: every module
  declared in the files is checked up front (`check_no_existing_schematics`,
  `sv_reader.cpp`); any existing Schematic fails the read before it creates
  anything, including the library.
- `_file_positional_name` (tab completion of file arguments) now counts
  only positional options, so `read_lef`'s new `-library` flag doesn't
  switch its path completion off.

**Judgment calls:**
- **Design identity stays global by name.** `Design.name` is one flat index
  (`create_design` overwrites it), DEF placements/Verilog instances resolve
  references by name alone, and a netlist read's auto-generated stub modules
  must attach to LEF cells in *another* library. So the library name
  decides where *new* designs go; an existing design of the same name keeps
  its library and gains the view (with a warning if the named library
  differs — suppressed for Verilog because of the stubs). Making designs
  library-scoped would mean a unique-per-library `Design.name` index and
  library-qualified reference resolution everywhere — worth a separate item.
- **The C API requires the name too**, not just TCL — one rule everywhere.
  All 162 C++ test/benchmark call sites were rewritten mechanically to pass
  the fixture's stem, i.e. exactly the name they used to get implicitly.
- **LEF creates its library lazily at the first MACRO**, so a tech-only LEF
  (still required to name a library) doesn't leave an empty one behind.
- **A failed LEF/DEF read is not rolled back** — macros before the
  conflicting one stay read, as with any other mid-file LEF/DEF error today.
  Verilog, which can check up front, fails atomically.
- `backend/tcl/*.tcl` example scripts now pass `-library` (`nangate`,
  `asap7`, `aes_1`, `aes_5x5`, ...).

**Tests:** `api_test.cpp`: `ReadRequiresALibraryName`,
`ReadLefCreatesTheNamedLibraryOnceThenReusesIt` (would have made 2 libraries
before), `ReadingAViewTheDesignAlreadyHasIsAnError` (the DEF half returned 0
before), `AbstractLayoutAndSchematicViewsCombineOnOneDesign`,
`NewDesignsGoIntoTheNamedLibrary`. TCL help/session tests updated for the
new signatures. Full ctest 822/822.

## Item 5 — Flightline display

**What was needed:** light blue lines for the selected placements' net
connections, on their own layer purpose, hidden by default.

**Fix:**
- New `ViewLayerPurpose::FLIGHTLINE` (ordinal 15) with its own `FLIGHTLINE`
  pseudo-row (`view_style.hpp`, `flightline_style()` = (135,206,250)),
  pre-seeded invisible and non-selectable in `LeHandle`
  (`purpose_visible_`/`purpose_selectable_`), and added to the hand-synced
  name lists (`layer_manager.cpp`'s `kPurposeNames`, TCL `::purpose_names`
  -> `set_purpose_visible flightline 1`, `api.hpp`'s ordinal docs).
- `src/core/flightlines.hpp`: `NetEndpointIndex` (net -> every placed pin
  and top-level `PhysicalPort` in the Layout — Root has no reverse index for
  `Pin.net`/`PhysicalPort.net`) and `placement_flightlines` (from each
  selected placement's connected pins to every other endpoint on the net).
  Pin locations are the center of the Terminal's port geometry in the
  placed Abstract, through the placement transform (bbox center if the pin
  has no geometry).
- `ComposeStage::draw_flightline_overlay` draws them (1px, under the
  selection outline); `ViewRenderOptions::flightline_version` makes it
  redraw on a visibility toggle or `link`, which change nothing rasterized.
- api.cpp's `flightlines_for` + `LeHandle::flightline_cache`: two-level
  cache (index per Root mutation, lines per selection change), computed
  only while FLIGHTLINE is visible.

**Judgment calls:**
- **"Between selected placement pins" = from the selected placements' pins
  to everything they connect to** (other placements, selected or not, and
  top-level pins), drawn as a star from each selected pin — the usual EDA
  flightline behavior, and useful for placing a cell near its neighbours.
  A connection between two selected pins is drawn once. If you meant only
  lines *between selected placements*, it's a one-line filter in
  `placement_flightlines`.
- **Connectivity comes from `link`** (Placement.instance/Pin.net): a
  placement with no linked Instance draws nothing. No automatic `link` is
  triggered.
- **No cap on large nets.** A selected clock-buffer's fanout draws in full;
  worth revisiting if high-fanout nets turn out to be noise.
- **Pins use the Abstract's geometry even at hierarchy depth > 0**, where a
  placement may be drawn as its Layout.

**Benchmark** (`flightline_benchmark.cpp`, BENCHMARKS.md 2026-09-25, Release):
index build 3.8 ms / 116 ms at 10k / 100k placements; lines for 1 / 100 /
10k selected placements (of 100k) 2 us / 0.54 ms / 107 ms. Justifies the
two-level cache: rebuilding the index per click would add 116 ms to every
selection change on a 100k design.

**Tests:** `core/tests/flightlines_test.cpp` (star fan-out through an FN
placement's mirrored pin, dedupe of a selected-to-selected connection,
top-level pin endpoints, unlinked placements draw nothing);
`api_test.cpp` `SelectedPlacementDrawsFlightlinesOnlyWhenTheFlightlinePurposeIsVisible`
(real netlist read + `link`, render-and-sample: none by default, light blue
once visible, gone after deselect); purpose/row count tests updated.
Full ctest 826/826.

## Item 3 — Shape resizing

**What was needed:** a resize tool: drag a rectangle's edges, move a
polygon's edges, move a path's segments (dragging anywhere on the segment,
its neighbours following), with a secondary toolbar of per-kind snapping
options.

**Fix:**
- `src/core/shape_resize.hpp`: `find_resize_handle` (the rect edge,
  polygon edge or path segment nearest a point - a path segment anywhere
  within half its width), `resize_piece` (drag it by a delta and snap),
  `ShapeSnapContext`, `TrackGrid`/`layer_track_grids`, `replace_piece`.
  - A rect edge moves across its own axis; dragged past the opposite edge
    the rect renormalizes.
  - A polygon edge or path segment moves as a whole, both of its points
    together, so the adjacent edges/segments stretch; an axis-aligned one
    moves only across its own axis (rectilinear stays rectilinear).
    A polygon that repeats its first point keeps the repeat in step.
- `LeHandle::ResizeState` (armed + current grab) and per-kind
  `shape_snap_modes_`; arming Resize disarms Move and vice versa; Escape or
  leaving Edit mode disarms.
- api.cpp: `le_mouse_down` with Resize armed grabs the nearest edge/segment
  of a *selected* piece within 6px (`try_begin_resize_grab_unlocked`,
  `DragKind::RESIZE` - no rubber band); the ghost reuses the Move ghost
  overlay; `le_mouse_up` after a real drag commits one undoable "resize"
  (`commit_resize_unlocked`), a click-sized release changes nothing.
  C API: `le_arm_resize`, `le_is_resize_armed`, `le_set/get_shape_snap_mode`,
  `le_is_shape_snap_mode_available`, `le_selected_piece_kinds`. TCL:
  `arm_resize`, `set_shape_snap_mode <rect|polygon|path> <mode>`,
  `get_shape_snap_mode`, `shape_snap_mode_available`.
- GUI: a Resize button (Lucide "scaling") next to Move in the Edit toolbar
  (`draw_tool_button`, shared with Move); while armed, the secondary toolbar
  shows one snap group per kind of piece selected ("Rects:", "Polygons:",
  "Paths:"). `secondary_toolbar.cpp` was refactored so both toolbars share
  `draw_snap_group`/`Row`/`PendingChoice`.
- Fixed in passing: the Move button's optimistic "armed" highlight stayed
  on forever if arming was refused (e.g. nothing selected); a pending arm
  now expires after 30 frames (both buttons).

**Judgment calls:**
- **Resize is an Edit-mode tool like Move**, not a new mode, and uses
  press-drag-release (the spec says "dragging") where Move uses two clicks.
  Only edges of *selected* pieces can be grabbed - select first, as for Move.
- **Snap settings are per kind and persist** (defaulting to the user grid,
  which is always available): rects and polygons are listed separately in
  the spec, so each has its own setting even though they offer the same
  options.
- **FinFET grid** snaps only the coordinate across the fins (y for
  HORIZONTAL fins); the other axis snaps to the manufacturing grid - the
  same rule as Placement Move.
- **"Snap edges to manufacturing grid" (paths)** puts the centerline at
  `grid(center - width/2) + width/2`: the lower/left edge lands on the grid;
  the other edge does too whenever the width is a grid multiple (true for
  real routing widths).
- **"Snap center of path to tracks"** uses the Layout's DEF TRACKS naming
  the path's layer (horizontal segment -> TRACKS Y, vertical -> TRACKS X);
  with no such tracks (e.g. an Abstract view) it falls back to an unbounded
  grid from the layer's LEF PITCH/OFFSET (offset defaulting to pitch/2,
  LEF's own default). "Tracks" is disabled when neither exists.
- **Diagonal edges/segments** move by the full delta, each axis snapped.
- **No keyboard shortcut** for Resize (Move's is Ctrl-M); easy to add.
- **Not exercised in the real GUI** (no display available overnight) - the
  API-level test drives the same `le_mouse_down`/`le_set_mouse_position`/
  `le_mouse_up` calls `forward_mouse_input` makes, and checks the ghost's
  pixels mid-drag, but the toolbar's look is unverified.

**Tests:** `core/tests/shape_resize_test.cpp` (11: handle finding incl. a
path grabbed within its width, rect edge per snap mode incl. FinFET per
axis, renormalizing, polygon edge + neighbours, closed-polygon repeat,
path segment + neighbours, tracks/mfg-edge/user snapping, per-kind modes,
track grids from TRACKS vs LEF PITCH with clamping, `replace_piece`);
`api_test.cpp` `ResizeDragsARectEdgeSnapsItAndIsUndoable` (miss, ghost
mid-drag, snapped commit, stays armed, undo, Escape) and
`ShapeSnapModesArePerKindAndOnlyAcceptModesTheKindOffers`; TCL smoke
checks. Full ctest 839/839.

## Closing summary

**Done (all committed and pushed, one commit each):**
- Item 4, custom library naming — `d138e4b`
- Item 5, flightlines — `28a04bd`
- Item 3, shape resizing — `e57a99b`

Nothing was left uncommitted for sign-off: none of the three touches a file
writer or other hard-to-undo data path (item 4 changes the readers' error
behaviour, all of it covered by tests, and every edit in item 3 is undoable).
All three are marked DONE in NEW_FEATURES_SEPT_2026.md. Full ctest ended at
839/839; `build` and `build_release` are both current.

**Worth a look in the morning:**
- **Item 4's design identity** — designs stay matched by global name, not
  per library (see that section's first judgment call). A same-named design
  in two libraries still collides in `Design.name`'s flat index, as before.
- **Item 5's flightline scope** — lines go from the selected placements to
  *everything* they connect to, not only between selected placements.
- **Item 3 in the real GUI** — the toolbar and drag feel haven't been seen;
  the API-level tests drive the same calls the GUI makes.
- **Breaking change:** `read_lef`/`read_def`/`read_verilog` now require
  `-library <name>`; personal scripts outside `backend/tcl/` need updating.

**Possible follow-ups for BUGS_AND_ENHANCEMENTS.md:**
- Library-scoped design names: a unique-per-library `Design.name` index and
  library-qualified reference resolution (DEF COMPONENTS, Verilog instances,
  `design:NAME` tokens), if the same cell name in two libraries should be
  two designs.
- LEF/DEF reads aren't transactional: a read that fails part-way (e.g. on a
  duplicate Abstract) keeps whatever it read before the error.
- Flightlines: optionally skip high-fanout nets (clock/reset) above a
  threshold, and use a placement's Layout pins when drawn at hierarchy
  depth > 0.
- Resize: a keyboard shortcut; resizing several selected pieces' shared
  edge at once; free-form vertex dragging for polygons.
