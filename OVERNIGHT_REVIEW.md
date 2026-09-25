# Overnight review — 2026-09-25/26

NEW_FEATURES_SEPT_2026.md items 8–14, worked unattended with the
overnight-review pattern: research -> implement -> test -> commit+push per
item. Judgment calls (ambiguities decided without anyone to ask) are marked
**Judgment call:** under each item so they're easy to find and override.

The previous night's review (2026-09-24/25) was replaced rather than
explicitly approved for deletion this time - it's committed (26b3529), so
`git show 26b3529:OVERNIGHT_REVIEW.md` recovers it.

Order worked: 11 (builds directly on the day's placement purpose fixes),
then the small GUI items (8, 10, 14), then the interaction items (12, 13),
and the settings window (9) last.

## Item 11 — Merge placementName/placementBoundary into one `placement` purpose

**What was needed:** earlier the same day `PLACEMENT_NAME` (label) and
`PLACEMENT_BOUNDARY` (outline) were made independently toggleable, and
placement selection was gated on `PLACEMENT_BOUNDARY`. The request is now
one purpose for both.

**Fix:**
- `ViewLayerPurpose::PLACEMENT` replaces both (`view_style.hpp`), one
  `PLACEMENT` row/ViewLayer, `placement_style()` (the same solid light
  gray, 220), accessor `placement_view_layer()`.
- `HierarchyResolverStage` emits one batched `RenderShape` per Layout with
  index-parallel rects (outline + label reference box) and texts, instead
  of two shapes.
- `RasterizeBlend2DStage`: the PLACEMENT layer strokes its rects again
  (the skip added for the split earlier today is gone) and draws labels.
- `placements_selectable` (`api.cpp`) gates click/drag selection on
  `PLACEMENT`.

**Judgment call — ordinals:** `ViewLayerPurpose`'s raw ordinal crosses
the C API. `PLACEMENT` keeps `PLACEMENT_NAME`'s ordinal 11;
`CUSTOM_SHAPE`/`DEBUG`/`FLIGHTLINE` shift down to 12/13/14 rather than
leaving a dead slot at 12. Every mirror of the ordinals is in-repo now
that the Flutter frontend is gone, and all were updated together:
`api.hpp`'s `le_purpose_at` comment, `layer_manager.cpp`'s
`kPurposeNames`, `le_tcl_procs.tcl`'s `::purpose_names` (Tcl keyword is
now `placement`; `placementName`/`placementBoundary` are rejected as
unknown). A saved script using the old keywords will need updating.

**Tests:** view_style/resolver/api/gui/layer-generation tests updated for
one fewer purpose and row. The earlier render test became
`PlacementPurposeDrawsTheOutlineAndLabelAndHidingItHidesBoth`; the
selection test became
`PlacementsAreSelectableOnlyWhilePlacementPurposeIsVisibleAndSelectable`.
`TCL_COMMANDS.md` regenerated. Full suite: 842/842.

## Item 8 — No selectable checkbox for objects that can't be selected

**What was needed:** every purpose row in the Layers panel had an S
checkbox, but most purposes (row, boundary, tracks, gcellgrid, blockages,
region, customShape, debug, flightline) have nothing hit-testing ever
selects, so the toggle did nothing.

**Fix:**
- `purpose_has_selectable_objects(ViewLayerPurpose)` (`view_style.hpp`)
  — true for TERMINAL, OBSTRUCTION, ROUTE and PLACEMENT only, which is
  exactly what `placement_geometry.hpp`'s hit-tests and `api.cpp`'s
  `placements_selectable` walk (vias follow their owning Shape's purpose).
  Its doc comment says to extend it when a new kind becomes selectable.
- C API `le_purpose_has_selectable_objects(purpose)` (the GUI only
  depends on `api`), carried into `GuiProvider::PurposeRow::has_selectable_objects`.
- `layer_manager.cpp`: `draw_toggle_row` leaves the S cell empty when the
  purpose has nothing selectable; the "All"/"Purposes" aggregate S
  checkboxes ignore those purposes both when computing their checked state
  and when toggling.

**Judgment call:** layer rows keep their S checkbox - every technology
layer can carry terminal/obstruction/route shapes. CUSTOM_SHAPE free
shapes aren't hit-tested yet (CLAUDE.md), so customShape loses its
checkbox until they are. The Tcl `set_purpose_selectable` still accepts
any purpose (harmless, and keeps scripts working).

**Tests:** `GuiProviderFixture.OnlyPurposesWithSelectableObjectsOfferASelectableToggle`.
Full suite: 843/843.

## Item 10 — Library browser collapsed by default

**Fix:** `library_browser.cpp` library nodes used `ImGuiTreeNodeFlags_DefaultOpen`;
now a plain `TreeNode` (design nodes were already collapsed).

**Judgment call:** collapsing by default would hide filter matches, so on
the frame the filter text changes every still-shown library is opened
(or, when the filter is cleared, collapsed again) via
`SetNextItemOpen(..., ImGuiCond_Always)`. It only fires on the change, so
nodes can still be toggled by hand while a filter is active.

**Verified:** real GUI (`le_shell` + `show_gui`, AES_1 + Nangate) — both
libraries start collapsed. No automated test: there's no headless ImGui
harness in `src/gui/tests`, only `GuiProvider` state tests.

## Item 14 — Resize disabled while placements are selected

**Fix:**
- Backend: `arm_resize_unlocked` (`api.cpp`) refuses to arm when the
  selection holds any `PlacementId`, so Ctrl-R and the Tcl `arm_resize`
  are covered too, not just the button. `le_arm_resize`'s doc updated.
- GUI: `mode_toolbar.cpp`'s `draw_tool_button` takes an optional
  `disabled_reason` — the Resize button is drawn with `BeginDisabled`
  while `placement_move.selected_count > 0`, and its tooltip (shown with
  `ImGuiHoveredFlags_AllowWhenDisabled`) says why.

**Judgment call:** a placement selected *alongside* resizable shapes also
disables Resize, rather than resizing just the shapes - resizing a mixed
selection would silently ignore part of it.

**Tests:** `ApiFixture.ResizeDoesNotArmWhileAPlacementIsSelected` (fails
without the guard). Verified in the real GUI: greyed icon plus the
tooltip. Full suite: 844/844.

## Item 12 — Via arrays selectable and movable

**What was needed:** item 6 made plain vias (`Shape.vias`) pieces;
arrays (`Shape.via_iterates` — LEF `VIA ITERATE ... DO n BY m STEP x y`,
DEF routed `VIA DO n BY m STEP x y`) were explicitly left out.

**Fix:** a new `PieceKind::VIA_ITERATE` (appended, so no existing value
shifts), threaded through every place `VIA` was:
- `geometry.hpp` `extract_piece`/`piece_in_range`/`transform_piece_in_place`
  (moving an array moves its origin, so every instance moves together);
  `shape_resize.hpp` treats it like a via (no edges).
- `api.cpp`: `expanded_via_geometry` expands either kind through the same
  `append_via_shapes` the renderer uses (selection outline / Move ghost
  trace every instance); `ViaHitBoxes::bbox(ShapeViaIterate)` stretches the
  first instance's cached box over `(num-1)*space` on each axis;
  `hit_test_via_point_all`/`hit_test_via_rect`, select-all and
  select-by-shape include arrays; `apply_shape_snapshot_with_vias` also
  restores `via_iterates` (undo); Resize ignores arrays like vias.

**Judgment call:** an array is selected and moved as *one* unit, not per
instance - the request says "via arrays", and per-instance selection would
mean splitting a DEF/LEF array record on edit. A click anywhere inside the
array's bounding box (including between instances) picks it, the same
bbox rule placements use; a single via or wire stacked inside it still
wins because hits are ordered smallest-area first.

**Tests:** new fixture `via_array_cell.lef` (2x2 VIA12 array);
`ClickingAViaArraySelectsTheWholeArray` (click between instances, outline
at both far edges, rubber-band gives one piece) and
`MovingASelectedViaArrayMovesEveryInstanceAndIsUndoable`. Both fail
without the api.cpp change. The Layout-view (DEF route) path shares the
same code via `for_each_via_owner_shape` and isn't separately tested.
Full suite: 846/846.

## Item 13 — Move snaps routes and vias like Resize

**What was needed:** Move shifted every shape piece by one shared,
user-grid-snapped mouse offset - a piece that started off-grid stayed
off-grid, and there was no track/manufacturing-grid option. Resize
(item 3) already had per-kind snap modes.

**Fix:**
- `snap_moved_piece_delta` (`core/shape_resize.hpp`): from the raw
  (unsnapped, axis-constrained) mouse offset, the delta that lands a
  path's first centerline point, or a via's / via array's origin, on the
  snap target. It reuses `ShapeSnapContext::snap_path_center`, so
  MANUFACTURING_GRID puts a path's *edges* on the grid and TRACKS its
  centerline on a track, exactly as Resize does. An axis the move doesn't
  change isn't snapped.
- `api.cpp` `moving_piece_deltas_unlocked`: one delta per moving piece,
  shared by the ghost and the commit (same "plan once" pattern as
  placements and Resize). Paths/vias/via arrays snap individually; other
  pieces keep the shared user-grid delta. Snap contexts are cached per
  (kind, layer) so TRACKS doesn't re-resolve track grids per piece.
- Snap settings: `LE_PIECE_KIND_VIA`/`VIA_ITERATE` added to the C enum;
  vias and via arrays share one slot (`shape_snap_slot`), offering
  NONE/USER_GRID/MANUFACTURING_GRID/TRACKS. New
  `le_selected_move_snap_piece_kinds`. Tcl `set_shape_snap_mode via ...`.
- GUI: while Move is armed with paths/vias selected (and no placements -
  the placement toolbar keeps precedence), the secondary toolbar shows
  "Paths:" and "Vias:" snap groups. The toolbar now wraps a group onto a
  second line when it won't fit (the overlay auto-sizes its height) -
  at the default 1280px window, Paths + Vias overflowed and clipped
  (this also fixes the same latent clipping for Resize with rects +
  polygons + paths selected).

**Judgment calls:**
- Paths share *one* snap setting between Move and Resize ("the same
  options as resize"), rather than separate Move/Resize settings.
- Vias get Tracks/Mfg grid/User grid/None - no FinFET option (via
  positions relate to routing tracks, not fins).
- Snapping is now *absolute* for paths and vias: the reference point
  lands on the grid, where before the piece moved by a whole number of
  grid steps from wherever it was. Default mode is still User grid.
- A path's reference point is its first centerline point - for a
  multi-segment route that's one end; other points move rigidly with it.
- Rects/polygons (e.g. a DEF route RECT) keep the old shared-delta
  behaviour - the item names routes and vias only.

**Tests:** `ShapeMoveSnap.*` (core: path user/mfg/none/tracks, via user
grid with an untouched axis, via array mfg grid, other kinds untouched),
`SnapModesOfferedPerKind` extended, and end-to-end
`MovingAViaSnapsItsOriginToTracksWhenAsked`,
`MovingAViaSnapsItsOriginToTheUserGridByDefault`,
`MovingARoutesPathSnapsItsCentrelineToTracksWhenAsked` (the two track
tests fail with per-piece snapping switched off). Tcl smoke test covers
`via tracks` round-trip and `via fin` rejection. Verified in the real GUI:
both groups shown and wrapped, and clicking Vias -> Tracks sets the mode.
`TCL_COMMANDS.md` regenerated. Full suite: 852/852.

