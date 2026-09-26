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

## Item 20 — Minor grid hard to see, changes color under a drag box

**What was wrong:** a real bug, not eyesight. `ComposeStage` builds the
frame on transparent black (`clear_all()`, deliberately - `dump_png` writes
a PNG with a transparent background), so every pixel covered only by
translucent content - the minor grid (`kMinorGridColor = {128,128,128,120}`)
and every shape fill (alpha 100) - is still translucent premultiplied RGBA.
The GUI uploads that as a texture and ImGui draws it with straight-alpha
blending, darkening those pixels a second time: the brightest minor-grid
pixel measured 14/255 on screen. The drag rectangle's own fill raised the
alpha under it, so the dots there showed at nearly their real brightness -
the color change in the report.

**Fix:** `render_thread_loop` (`le_gui.cpp`) sets every pixel's alpha to
255 when copying a frame into the GUI's mailbox. The design view is always
on black, and premultiplied color over black is the color itself, so this
is the exact composite, not an approximation. Brightest minor-grid pixel:
14 -> 43/255 (measured from screenshots of the real window). `dump_png`
and `le_render_pixel_buffer` are unchanged.

**Judgment call:** this also brightens every translucent shape fill in the
GUI to what its style asks for (about 2.5x what showed before) - they're
hatch patterns, so the look is similar, just less murky; checked on
NAND2_X1. Grid colors themselves are unchanged - with the double darkening
gone they read clearly, so I didn't also retune them.

**Tests:** GUI-only (the composite happens in the GUI's texture copy), no
automated coverage; verified by measuring screenshot pixels before/after.
Full suite: 868/868, three consecutive parallel runs.

**Also fixed (separate commit):** `DEFWriterRoundtripFixture`'s tests all
wrote and re-read one shared scratch file, so under `ctest -j` a random
`RoundTrips*` test failed now and then (it had tripped three of tonight's
and yesterday's runs). Each test now writes its own file.
