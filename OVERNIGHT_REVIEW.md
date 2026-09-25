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

