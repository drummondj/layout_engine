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

## Item 22 — Rectangle select/zoom stuck after switching windows; Esc to cancel

**What was wrong:** switching to another app mid-drag makes ImGui (1.93)
clear its mouse-button state on focus loss (`ImGuiIO::ClearInputMouse`)
*without* reporting a release. `forward_mouse_input` (`le_gui.cpp`) only
ended a gesture on `IsMouseReleased`, so its `gesture` stayed active and the
backend stayed `is_dragging()`: the next press was ignored (a gesture was
"already in progress") and that press's release committed a rectangle from
the long-gone start point. Escape did nothing to a drag either.

**Fix:**
- `le_cancel_drag` (new, `api.cpp`/`api.hpp`) ends a drag with no selection
  or zoom.
- Escape (`LE_KEY_FINISH_RULER`) now cancels a drag in progress first, and
  only that - one gesture per press (so Tcl/other frontends get it too).
- `forward_mouse_input`: while a gesture is active, Escape (hovered or not)
  cancels it via `le_cancel_drag`; so does finding its button no longer down
  with no release seen - the release went to another window. Escape spent
  that way isn't also forwarded as `LE_KEY_FINISH_RULER` that frame
  (`forward_keyboard_input`'s new `skip_escape`), so it doesn't also finish
  a ruler or cancel an armed Move.

**Judgment call:** a drag whose release went to another window is
*cancelled* (the moment focus loss clears the button), not committed at
wherever the mouse was last seen - the user left mid-gesture, and a
selection/zoom they didn't finish would be the surprise.

**Tests:** `ApiFixture.CancellingADragSelectsNothing` - cancel via
`le_cancel_drag`, then via Escape, each followed by a stray release,
selects nothing; the same drag uncancelled selects both pins. Fails with
the Escape change reverted. The focus-loss path is GUI-only and wasn't
exercised live (synthetic input doesn't reach the window under WSLg).
Full suite: 869/869.

## Item 21 — "CPUs" setting

**Fix:**
- Settings panel (`settings_panel.cpp`): a new **Performance** section
  with a "CPUs" field - the same commit-when-done `-`/`+` integer field as
  Hierarchy Depth - driving `set_max_concurrency` through a Tcl command
  (`GuiProvider::set_max_concurrency`), like the panel's other settings.
- Saved in `settings.json` as `max_concurrency` and applied on load
  (`le_set_max_concurrency`'s body moved into `set_max_concurrency_unlocked`,
  `api.cpp`, so `apply_settings_json` shares it - same clamp to >= 2). Being
  in the settings JSON, it also counts toward item 18's unsaved-settings
  check.

**Judgment call:** also persisted in the settings file (the request only
asks for the panel entry) - every other Settings-panel value is saved, and
a thread cap is exactly the kind of per-machine preference worth keeping.

**Tests:** `ApiFixture.SettingsSaveThenLoadRoundTripsEverySetting` now
covers `max_concurrency`. The panel field itself wasn't clicked live
(synthetic input doesn't reach the window under WSLg). Full suite:
869/869.

## Item 25 — Saving the window (docking) state

**What was needed:** `le_gui.cpp` set `io.IniFilename = nullptr` and rebuilt
the default left/center/right dock split every time a window opened, so
any rearranging was lost.

**Fix:**
- The layout is ImGui's own ini file at
  `~/.layout_engine/window_layout.ini` (`window_layout_path()`, beside
  `settings.json`; the directory is created if needed). ImGui loads it on
  the first frame and saves it itself - a few seconds after a change, and
  when the window closes.
- The default split is built only when that file has no `DockSpace` line
  (`has_saved_dock_layout`); a restored layout's first frame still gets the
  same one-frame viewport-size distrust as a freshly built one.
- The GLFW window's size is saved too - ImGui doesn't, and the dock layout's
  node sizes are in pixels, so they only fit the window they came from - as
  a `[LayoutEngine][Window]` section in the same file
  (`add_window_size_settings_handler`, an `ImGuiSettingsHandler`).
- Settings panel -> Settings file: **Reset window layout** rebuilds the
  default split (a GUI-only request through `GuiProvider`).

**Judgment calls:**
- A separate file, not part of `settings.json` (the item offered either):
  settings are saved explicitly (item 9) and count toward item 18's
  "unsaved settings" prompt, while a panel layout should just be remembered
  - it would otherwise make the exit dialog nag after every panel drag, or
  need its own exception there.
- `~/.layout_engine`, not the item's `~/.layout_editor` - read as a typo for
  the directory the settings file already uses.
- Window *position* isn't saved, only size - a saved position can land
  off-screen when the monitor setup changes.

**Tests:** GUI-only. Verified live with a scratch `HOME` (so nothing touched
the real `~/.layout_engine`): the first run wrote the file on close;
editing its Browser node to 450px and the window to 1100x700 and reopening
showed exactly that (window geometry 1100x700, Browser ~450px). The Reset
button wasn't clicked live (synthetic input doesn't reach the window under
WSLg). Full suite: 869/869.
