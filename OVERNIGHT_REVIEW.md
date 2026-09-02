# Overnight session review (2026-09-02)

Working through BUGS_AND_ENHANCEMENTS.md's remaining open items, one at a
time (research -> implement -> test -> commit+push per item), B6 saved
for last since it needs live testing. Each entry below is added as an
item is finished (or if I get stuck/need to make a judgment call without
being able to ask). Please review the corresponding commits alongside
this file.

## B3. Via arrays are not being rendered.

**Fixed.** Root cause (found via a research agent, then verified
directly): LEF via geometry comes in two shapes, and only one was
supported. A plain `VIA name LAYER ... RECT ...` (explicit rects, however
many) already worked fully end-to-end - reader and renderer both loop
over every rect on every layer, just untested. The actual gap was the
LEF 5.6 "VIARULE-inside-VIA" form (`VIA name VIARULE rule CUTSIZE ...
LAYERS ... CUTSPACING ... ENCLOSURE ... ROWCOL rows cols`) - a real via
*array* is exactly a ROWCOL clause with rows/cols > 1. The reader parsed
everything except `hasRowCol()/numCutRows()/numCutCols()`, and the
renderer (`via_shapes.hpp`) explicitly skipped this whole via shape with
no explicit `ViaLayer` rects, by design (its own doc comment called this
out as deferred) - so *no* VIARULE-inside-VIA reference ever drew
anything at all, array or not.

Fix: added `ViaRuleReference.num_cut_rows`/`num_cut_cols` to the schema
(bumped to 0.40.0), read them in both `lef_reader.cpp` (`hasRowCol()`)
and `def_reader.cpp` (DEF VIAS VIARULE has the same ROWCOL clause,
`defiVia::hasRowCol()/rowCol()`), and taught `via_shapes.hpp` to
synthesize a real rows x cols grid of cut rects (centered on the via
placement point) plus one bottom/top metal enclosure rect sized to the
whole array - not one enclosure rect per cut, matching how a real
multi-cut via's own metal coverage actually looks. A ViaRuleReference
with *no* ROWCOL clause at all now also renders (as a single cut) -
LEF's own meaning for that case, previously skipped too, not just the
array case.

**Judgment call (not asked, seemed clearly in scope):** ORIGIN/OFFSET/
PATTERN (rarer LEF sub-clauses - a caller-supplied grid origin override
and a sparse cut-presence bitmap) are still not modeled - the array is
always centered on the via placement point with no gaps. Documented as a
deliberate deferral in the schema/via_shapes.hpp comments, same as this
codebase's existing `Field.create_excluded` convention elsewhere. A
top-level `VIARULE ... GENERATE` (deriving cut geometry from a *named
rule's* enclosure/spacing ranges, rather than one via's own explicit
CUTSIZE/CUTSPACING) is a substantially different, still-unimplemented
feature - not attempted, left as a clearly separate follow-up.

New tests: `lef_reader_test.cpp` (`ReadsAViaWithARowColClauseAsARealArray`,
plus a new assertion on the existing VIA2 test confirming no-ROWCOL stays
unset), `pipelines_test.cpp` (`RunSynthesizesAViaRuleReferencesRowColIntoARealCutArray`,
`RunSynthesizesASingleCutForAViaRuleReferenceWithNoRowCol`). Full 665-test
suite passes; both `build`/`build_release` rebuilt.

## B4. Running a zoom command before show_gui causes the layout viewer to hang.

**Could not reproduce - marked done, please re-verify.** Tried 4 different
repro shapes via a real PTY session (le_shell, matching how you'd hit
this interactively): `zoom` before/after `open_design` then `show_gui`;
multiple `zoom`/`zoom -factor -0.5` calls with *no* design loaded at all
before `show_gui`; `zoom_area` (a different code path, `le_fit_rect`, not
`le_zoom`) plus an extreme zoom factor before `show_gui`. Every variant:
window opened, console stayed responsive throughout, `dump_png` returned
a real, correctly-sized PNG afterward - no hang, no delay.

My working theory: this was already fixed as a side effect of two
architecture changes made earlier this same session (before this
overnight pass started) - both are exactly the class of bug this report
describes ("do something before the GUI/viewport is really ready, then
things go wrong once it opens"):
1. The render thread used to start *immediately* on window-open with a
   guessed bootstrap viewport size, before the real (dock-aware) size was
   known - if a `zoom` had already left a scale/pan in place before that
   guess, the first real render could hit a slow/degenerate case. The
   render thread's own startup is now deferred until the first frame with
   a *real*, trustworthy size is available - no more guessed first call.
2. A separate, now-fixed bug (`HierarchyResolver`'s stale-node pruning
   window) could permanently wipe a freshly-built node graph within 2-3
   polling cycles for continuous-polling callers like this GUI - a
   plausible source of "renders forever, never recovers" symptoms
   generally, not proven specific to this report.

No code change made here since I have nothing to point a fix at without a
reproducible case. If you still see this hang, the exact sequence of
commands (and whether it's every time or intermittent) would help a lot -
please reopen B4 (leave the checkbox unchecked again) if so, with
whatever repro you can capture.

## B5. Resizing a sidebar should wait until the resize is finished before rendering a new frame in the layout window.

**Fixed.** Dragging a dock splitter changes the "Layout" panel's own
content region *every single frame* for the whole drag - previously this
called `le_set_viewport_size` (and so a full synchronous rasterize -
potentially seconds for a real design, see BENCHMARKS.md) on every one
of those frames, which would visibly stall the drag itself rather than
just following it smoothly.

`le_gui.cpp`'s main loop now debounces: a still-changing size resets a
150ms timer every frame it changes, and is only actually applied once
it's held steady for that long - i.e. once the drag has actually
stopped, not on every intermediate frame of it. The very first size
application (right after the dock layout settles, not a user drag at
all) still applies immediately, unchanged - it's also what starts the
render thread, so debouncing it would delay every single window open by
150ms for no benefit.

**Judgment call:** picked 150ms for the debounce window - long enough to
comfortably outlast a frame-to-frame render/resize hitch during a drag,
short enough that the design view still updates promptly (~1/7th of a
second) once you actually stop dragging. No existing precedent in this
codebase to match against; happy to tune if it feels off in practice.

Couldn't verify visually (no way to simulate a real mouse-drag on a dock
splitter in this sandbox) - the logic itself is straightforward and
doesn't change behavior for the non-resizing steady-state case (verified
via a PTY smoke test: window still opens and renders correctly). Worth a
quick manual drag-a-sidebar check on your end.

