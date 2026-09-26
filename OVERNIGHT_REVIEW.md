# Overnight review — 2026-09-26/27

NEW_FEATURES_SEPT_2026.md items 20–25, worked unattended with the
overnight-review pattern: research -> implement -> test -> commit+push per
item. Judgment calls (ambiguities decided without anyone to ask) are marked
**Judgment call:** under each item so they're easy to find and override.

The previous night's review (2026-09-25/26) was replaced rather than
explicitly approved for deletion - it's committed (d425161), so
`git show d425161:OVERNIGHT_REVIEW.md` recovers it.

Order worked: 23 (status bar padding) and 20 (grid color) first - small,
both in the design view's drawing; then 22 (Esc cancels a drag), 21 (CPUs
setting), 25 (saving the dock layout), and 24 (the TCL_COMMANDS.md
rewrite, the largest) last.

## Item 23 — Status bar bottom padding

**What was wrong:** `le_gui.cpp` reserved a fixed `kStatusBarHeight = 36`
for the status bar, but what's actually drawn below the design image is
the `ItemSpacing.y` (10) gap after the image, the 1px Separator, another
10px gap, then the text row - about 37px, so the text sat ~1px off the
window's bottom edge against ~13px above it.

**Fix:** `status_bar_height()` (`le_gui.cpp`, replacing the constant)
computes the space from the live style - `3 * ItemSpacing.y + 1 +
TextLineHeight + 2 * CellPadding.y` - the third `ItemSpacing.y` being the
new bottom margin, equal to the separator-to-text gap. The design view
gives up ~13px of height for it.

**Tests:** GUI layout only, no automated coverage. Verified with a
screenshot of the real window: ~15px separator-to-text, ~14px below the
text. Full suite: 868/868.
