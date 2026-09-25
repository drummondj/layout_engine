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

