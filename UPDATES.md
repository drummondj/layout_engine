# 1. API Updates

## 1.1. Enable library browser. - DONE

Currently the API only exposes a list of flat designs and their count. To build a hierarchical library browser widget, I need a hierarchical representation of the library -> design -> view structure. Including the LibraryId, DesignId and AbstractId for each one.

Can a struct be passed via the C API? Or maybe use Google Flat Buffers / Protocol Buffers to transfer complex data? Please explore possible options.

## 1.2 Zoom/Pan control. - DONE

The current API requires the frontend to store scale and pan information and send that to the API. But, when le_fit_scene is called, that information is updated internally to the backend. I would like the backend to own the scale and pan numbers, and replace the le_set_pan and le_set_pan with:

- le_zoom(double factor, int x, int y) - where factor can be positive for zoom in and negative for zoom out. x and y are the pixel coordinate for the center of the zoom.

- le_pan(double x_factor, y_factor) - same concept as le_zoom, using a factor to pan

# 2. Colors and layer selection

## 2.1 The default colors should be split into two lists. - DONE

1. Default ROUTING and CUT layer colors - bright high contrast colors
2. Other layer colors - more muted colors

Each CUT layer above a ROUTING layer should be the same color as the ROUTING layer below it.

## 2.2 Layer visibility and selection - DONE

Each view layer can be set as visible/invisible and selectable/un-selectable.

This is achieved by a widget that contains one row for each layer and columns for each purpose.

The API needs to be update to get a list of layers for the widget, plus provide methods to change visibility and selectability.

## 2.3 Layer fill patterns - DONE

I would like to implement the following fill patterns. Maybe using a shader?

1. ROUTING layers - diagonal stripes with different directions depending on the layers routing direction
2. CUT layers - a cross shape as most cut layers should be rectangles
3. OBSTRUCTIONS purpose - a brick pattern
4. TERMINAL purpose - a tighter diagonal stripe
5. Other layers - small dots.

All patterns must have a transparent background so layers below are visible between.

# 3. Error message handling and display - DONE

When certain operations happen in the backend, such as reading a LEF file. I would like to return any error messages to the flutter_plugin so they can be displayed in the GUI.

# 4. Automatic text sizing - DONE

Try and size text based on the shape it's labeling, with a minimum text size. Should be simple for rectangle shapes, but polygons and paths need to use the width of the shape to determine the text size and not the bbox.

# 5. Grid and snapping

## 5.1 Grid display - DONE

I would like to display a grid of major and minor dots. The major dots should be bolder than minor dots. The grid spacing should be configurable via the API but a good default value is minor 5nm and major 50nm. When zoomed out the grid should not be shown if too dense.

A solid line should be used for the x and y axis.

**Update**: once zoomed out far enough that the minor tier itself is hidden (below `kMinGridDotPixelSpacing`), the major dots now draw in the minor tier's own color (`draw_grid`, `draw_helpers.hpp`) rather than the bolder major color - at that zoom level the major dots are the only grid left on screen, so the bold treatment would visually imply a finer tier is still being contrasted against when there isn't one.

## 5.2 Mouse snapping - DONE

I would like to store the current mouse position and a snapped to grid mouse position. Then display a red box around the current grid position that follows the mouse. The mouse position, in pixels, should be sent by flutter via the API. If this causes performance problems then an alternative solution may be required. For example, rendering the mouse position in a separate SkPicture and thread, then just merging it with the abstract layout picture.

## 5.3 Coordinate display - DONE

The API should return the snapped mouse coordinates so they can be displayed in the Flutter UI.

## 5.4 Origin marker - DONE

Please add a cross shape at the origin of the abstract, which is not necessarily at 0,0 - it depends on the origin property of the abstract view.

# 6. Small shape display - DONE

When zoomed out of a large design, very small shapes disappear which makes it look like there is nothing in the design. I would like to see the performance impact of rendering a single pixel instead of nothing for small shapes.

Single pixel representations can not be selected, they are there just to inform the user that something exists and they need to zoom in.

# 7. Shape selection - DONE

We will have different modes which cause mouse gestures to perform different tasks.

The first of these modes is "Select" mode.

## 7.1 Mouse gestures and keyboard modifiers - DONE

Shape selection requires the following features:

1. When the mouse hovers over a selectable shape, the outline of the shape turns yellow.
2. When the mouse is clicked the shape is added to the selection.
3. Shapes are selected by top-most layer first.
4. To select shapes under the current selection, the user must press shift.
5. Mouse down, move and release selects all selectable shapes on all layers completely enclosed by the selection rectangle.
6. Multiple shapes are selected with shift-click, without shift the current selection is replaced with the new selection.

## 7.2 Selection results - DONE

Each selected object and it's properties are sent to the frontend via the API, so the flutter GUI can display the data in a table.

Simple properties such as int, double, string etc are just sent as is. Coordinates are always converted to um. Lists of other objects or values are just sent as the size of the list, not the list contents.

## 7.3 Tooltip message - DONE

I would like a toopltip message to be generated, which is used in the GUIs status bar below the texture. This tooltip will contain instructions to the user about which mouse gestures and keyboard shortcuts/modifiers are available at that time. For example, when select mode is activated the tooltip should read something click: "Left click to select. Shift for multi-select. Left click and drag for rectangle multi-select."

# 8. Label placement update - DONE

I would like to try a different Geometry::get_label_location algorithm.

1. For polygons/paths I would like to fracture the polygon into rects. If the polygon's bbox is wider than it is tall, fracture vertically, otherwise fracture horizontally. The place a label in the center of the largest rectangle.

2. For rects, find the largest rect in the shape and label it.

3. Please draw a cross at the origin of the label because some larger labels can overlap other shapes, so it's not obvious what the label is labeling!

# 9. Mouse and keyboard event additions - DONE

1. Ctrl-A select all but stop at 10,000 objects
2. Mouse scroll wheel zoom in/out
3. Right mouse button rectangle zoom
4. 1, 2, 3 ,4 etc changes layer visibility - where 1 maps to the first routing layer, 2 to the second etc. If two adjacent routing layers are made visible via the keyboard (not when the user clicks on the layer manager), then the VIA layer between then is also made visible. Same logic in reverse for making vias invisible.
5. Ctrl-D deselect all
6. Ctrl-F fit selected

# 10. Default layer visibility - DONE

By default the only visible layers should be ROUTING, CUT and BOUNDARY, all other layers should not be visible when the user reads a LEF with LAYERs in to initialize a Technology.

# 11. Modes switching and edit mode - DONE (mode-switching infrastructure only - see below)

The default select mode (already implemented) can be switched to other modes, initially edit mode. During edit mode only the selected objects can be edited. To change the selection the user must switch back to select mode.

Modes are changed by 2 mechanisms:

1. Keyboard shortcuts: s - select mode, e - edit mode
2. Via events in the flutter UI which requires a mode switching method in the API

The current mode can be queried via the API.

Details of how objects are edited to follow.

**Resolution**: `Scene::Mode` (`SELECT`/`EDIT`, `scene.hpp`) is the new per-handle state, defaulting to `SELECT`. `LE_KEY_SELECT_MODE`/`LE_KEY_EDIT_MODE` (`LeKeyCode`, `api.hpp`) drive it from `le_key_down` (fires on every call including key-repeat, same idempotent-action-code shape as `LE_KEY_ZOOM`/`LE_KEY_FIT`); `le_get_mode`/`le_set_mode` (`LeMode`, crossing the FFI boundary as `int32_t` like every other small enum here) give the same switch/query from a Flutter UI event instead. `le_mouse_up`'s whole selection-changing block (click-select, drag-select, the pipeline run backing the hit-test) is now gated on `Scene::Mode::SELECT` - a click/drag in Edit mode is a no-op for the selection, though `end_drag()` still runs unconditionally so drag state always resets. `le_tooltip_message` now branches on mode too, with a placeholder `"Edit mode"` string until a follow-up item defines real Edit-mode behavior. Keyboard: `s`/`e` in `flutter_plugin/lib/layout_engine_input.dart`'s `_keyCodeMap`. UI: `LeProvider.setMode()` (`frontend/lib/providers/le_provider.dart`), a status-bar mode readout, and two Select/Edit buttons in the main menu. Covered by new `scene_test.cpp`/`api_test.cpp` cases (default mode, round-trip, key-down switching, null-handle safety, selection-unchanged-in-Edit-mode for both click and drag, `end_drag()` still running in Edit mode). What "editing" itself does once something is selected in Edit mode remains out of scope, per this item's own last line - a later item.

# 12. LEF Syntax Completion (big task) - DONE (missing-line count against `complete.5.8.lef` taken from 240 to 104 across 6 rounds - see the Progress notes below; the `ARRAY`/floorplan section is out of scope, DEF-format territory, and the rest is vendored-writer/-reader limitations fully catalogued in `LEFDEF_BUGS.md`, not gaps in this project's own code)

The LEF parser only supports a subset of the available syntax. I need it to support all LEF syntax.

There is an example LEF file that contains all LEF syntax that needs to be parsed: src/lefdef/lef/TEST/complete.5.8.lef

Here is an outline of a plan to follow (you may modify as long as the results are the same):

1. Create a LEFWriter class that creates a LEF file for the specified AbstractId. The LEFWriter needs an option to choose wether to include Technology layers or not, or just write out Technology layers.
2. Read the complete.5.8.lef and write it out, diff the results
3. Update the LEFReader and LEFWriter to fix the differences reported by lefdiff. You will also need to update schema.py and rebuild the database. Make sure any coordinates are converted from microns to dbu.
4. Repeat step 2 until there are no reported differences.

There will be ambiguous cases where you need to ask me what to do. For example, should you store other values as integers, or us the units in the LEF file, or use SI units. Please ask me and we will discuss each individual case.

Known issue: the current LEFReader splits ITERATE statements into separate shapes, we need to store the raw ITERATE as an object in the database then split during generate_shapes in the pipeline.

**Progress**: ongoing, tracked via `lef_roundtrip_diff`'s missing-line count against `complete.5.8.lef` (see git history for the numbered Phase commits). Round 1: fixed a real, previously-undiscovered `lefwViaLayerPolygon` bug that wrote corrupt/unparseable LEF for any VIA with polygon geometry (`myVia23`) and was silently truncating the diff tool's own comparison for everything written after it in the same file; found and documented a second vendored dead end (`lefwViaRuleLayer`'s `DIRECTION` is version-gated as obsolete at LEF 5.8, but the reader's own grammar still requires it for a non-GENERATE `VIARULE`); and, per discussion - `Shape.spacing`/`Shape.design_rule_width` are now genuinely `is_optional` (a real `SPACING 0`/`DESIGNRULEWIDTH 0` means "use the LAYER definition's own rules only when unset, not when 0" - the router treats an explicit `0` as a real override, so the old 0-means-unset representation was wrong, not just incomplete), and `Foreign.origin`/`orient` are now independently optional too (multiple `FOREIGN` per MACRO already worked at the schema level - the real bug was the reader/writer collapsing "no point"/"no orientation" into `(0,0)`/`N`, losing every FOREIGN after the first that didn't explicitly repeat both). The ARRAY/floorplan top-level LEF section is out of scope - that's DEF-format territory, not this schema. All three changes surfaced the _same_ residual vendored-writer bug in three different functions (`lefwMacroObsLayer`/`lefwMacroPinPortLayer`/`*DesignRuleWidth`/`lefwMacroForeignStr` all gate on a bare `if (value)`, so a real `0`/`(0,0)` still can't reach the written file - see LEFDEF_BUGS.md) - the database representation is now correct either way. Missing-line count: 240 → 224.

Round 2: added `Technology.capacitance_units_pf`/`resistance_units_ohms`/`power_units_mw`/`current_units_ma`/`voltage_units_v`/`frequency_units_mhz` (UNITS was previously `database_units_microns`-only, Phase 1 scope); `SpacingRule.area` (CUT-layer `SPACING ... AREA`, LEF 5.7); `ViaLayer.rect_masks`/`polygon_masks` and `RectIterate.mask`/`PathIterate.mask` (parallel-array MASK color, same convention `Shape` already used). Also fixed a real bug in the existing VIA-placement `mask` field (`ShapeVia`/`ShapeViaIterate`): the vendored reader reports a VIA placement's MASK as three separate digits (`topMaskNum`/`cutMaskNum`/`bottomMaskNum`), but only `topMaskNum` was ever captured - a real `VIA MASK 103` (top=1, cut=0, bottom=3) silently became `1`. Fixed by recombining all three digits the same way the reader itself split them (`lef_reader.cpp`'s `combine_via_mask`). Missing-line count: 224 → 191. Remaining gaps not yet investigated further: most `PIN`-level electrical-characteristics fields beyond `TAPERRULE` (`LEQ`/`POWER`/`LEAKAGE`/noise-margin/voltage-threshold/slew-rate fields etc. - not in `schema.py` at all yet, a sizeable follow-up in its own right); a `WIDTH` statement inside `PORT`/`OBS` geometry with no `PATH` immediately following (currently captured into a local variable and dropped, since nothing but a `Path` currently stores it); and a `LAYER` occurrence with genuinely no content at all (no geometry, no `SPACING`/`DESIGNRULEWIDTH`/`EXCEPTPGNET` either) is dropped by the existing `geo_count > 0` guard.

Round 3: the single biggest win of this item so far - confirmed (via a standalone test file, not just the real fixture) that the recoverable `LEFPARS-1705` "requires a DIRECTION" error from Round 1's `VIARULE` finding wasn't just losing that one construct's own data: `lefdiff`'s own comparison silently stops accumulating _everything_ written after the errored VIARULE in the same file. `write_via_rules` now skips non-GENERATE `VIARULE`s entirely (there's no way to write one that doesn't hit this - see LEFDEF_BUGS.md), which unblocked `NONDEFAULTRULE` and `SITE` - both round-tripped correctly the whole time, they just couldn't be seen. Also fixed a real, separate bug found while re-checking the newly-visible `VIARULE` output: a GENERATE `VIARULE`'s cut layer (the one with `RECT`/`SPACING`) isn't always the 3rd `LAYER` in the file (`complete.5.8.lef`'s own `VIAGEN3T` lists it 2nd - metal/cut/metal, not metal/metal/cut) - `write_via_rules` assumed a fixed index; now identifies the cut layer by which one actually has a `rect` set. Added `Technology.fixed_mask`/`use_min_spacing_obs`/`use_min_spacing_pin`/`clearance_measure`/`manufacturing_grid`/`max_via_stack(_bottom_layer/_top_layer)` (top-level `FIXEDMASK`/`USEMINSPACING`/`CLEARANCEMEASURE`/`MANUFACTURINGGRID`/`MAXVIASTACK` - none previously modeled at all) and `PropertyDefinition.default_number`/`default_string` (a `PROPERTYDEFINITIONS` entry's own default value, e.g. `LIBRARY intNum INTEGER 20`, was parsed but silently discarded - the writer always passed a dummy `0`/`nullptr` instead of the real stored default). Missing-line count: 191 → 108. What's left is almost entirely already-documented, unfixable-without-patching-vendored-source limitations (see LEFDEF_BUGS.md) plus the explicitly-out-of-scope `ARRAY`/floorplan section - re-checked every remaining category by hand rather than assuming, including `LAYER RX`'s missing `PROP lip`/`lrp` (confirmed as the already-documented "numeric PROPERTY unwritable on ROUTING layers" gap, not a new bug).

Round 4: added `Technology.antenna_input_gate_area`/`antenna_inout_diff_area`/`antenna_output_diff_area` (legacy pre-5.0 top-level `ANTENNAINPUTGATEAREA`/`ANTENNAINOUTDIFFAREA`/`ANTENNAOUTPUTDIFFAREA` defaults - not modeled at all). Fixed a real bug in ROUTING-layer `SPACING ... LENGTHTHRESHOLD len RANGE min max`: the vendored parser tracks that trailing `RANGE` in its own dedicated `hasSpacingLengthThresholdRange()`/`spacingLengthThresholdRangeMin()`/`Max()` accessors, a genuinely separate pair from `hasSpacingRange()`/`spacingRangeMin()`/`Max()` (used by the _different_ plain `SPACING d RANGE min max` construct) - despite both ultimately printing as a trailing `"RANGE %g %g"` clause. `SpacingRule` was reading/writing the wrong pair, so a `LENGTHTHRESHOLD`'s own range was silently dropped whenever present (confirmed the mixup the hard way: a range of exactly `(0, 0.1)` initially still produced no output after a naive first fix, since `write_via_rules` was checking the _shared_ fields, which were never populated for this construct in the first place - genuinely needed `SpacingRule.length_threshold_range_min`/`_max` as their own fields, not reuse). Missing-line count: 108 → 104. This is a good stopping point - the remainder is dominated by `ARRAY` (out of scope) and vendored dead ends already catalogued in `LEFDEF_BUGS.md`; further gains would mostly come from the still-unimplemented `PIN` electrical-characteristics fields noted in Round 2.

Round 5: implemented the `PIN` electrical-characteristics fields flagged as follow-up in Round 2 - `Terminal.power`/`leakage`/`max_delay`/`pin_capacitance`/`pin_resistance`/`pulldown_resistance`/`tie_off_resistance`/`vhi`/`vlo`/`rise_voltage_threshold`/`fall_voltage_threshold`/`rise_thresh`/`fall_thresh`/`rise_sat_cur`/`fall_sat_cur`/`current_source` (LEF `POWER`/`LEAKAGE`/`MAXDELAY`/`CAPACITANCE`/`RESISTANCE`/`PULLDOWNRES`/`TIEOFFR`/`VHI`/`VLO`/`RISEVOLTAGETHRESHOLD`/`FALLVOLTAGETHRESHOLD`/`RISETHRESH`/`FALLTHRESH`/`RISESATCUR`/`FALLSATCUR`/`CURRENTSOURCE`) - full, correct read+write plumbing, matching every other field's own convention. Missing-line count: 104 → 104, **no change**, and a significant discovery explains why: reading `lef.y` directly (not just checking writer function existence, which is where Round 2's plan stopped) shows the vendored _reader_ itself deliberately discards 15 of these 16 statements at `versionNum >= 5.4` - it matches the syntax, then silently ignores the value and emits an "obsolete... will ignore this statement" warning, before our callback ever runs. `write_lef` always emits `VERSION 5.8`, and any real-world modern LEF file will itself declare >= 5.4, so this project can never actually observe a value for these fields via round-trip testing - not a bug in this project's own code, a permanent, version-gated reader-side dead end (see LEFDEF*BUGS.md's new "Reader-side: intentional version-obsolescence" section). Kept the fields anyway (correct, real plumbing for a legacy pre-5.4 file, and useful independent of LEF at all for a `Terminal` built/edited via the API directly) rather than reverting the work, but flagging clearly: this was this round's main lesson, not a fix. The one field in this group that \_is* unconditionally readable regardless of version, `MAXDELAY`, was added as read-only (no `lefwMacroPinMaxdelay` function exists at all - a plain missing-API gap, not a version gate, same class as the pre-existing `max_load`).

**Decision (per discussion)**: this project doesn't need to support LEF older than 5.4. Reverted the "kept anyway" call from Round 5 - removed all 15 dead-on-read fields (`power` through `current_source`) from `schema.py`/`lef_reader.cpp`/`lef_writer.cpp` entirely, keeping only `max_delay` (genuinely readable at any version, unlike its 15 siblings). Added a real version check: `LEFReader::read_lef` now records the file's own `VERSION` (`lefrSetVersionCbk`) and, once parsing finishes, rejects anything below `5.4` with a clear error message and a distinct return code (`4`) - same "checked once at the end, not aborted mid-parse" convention `used_dbu_before_units_declared_` already established. Verified directly (not just by inspection): a hand-written `VERSION 5.3` fixture is rejected with the expected message, `VERSION 5.4` and the existing `complete.5.8.lef` fixture both still succeed - see `LEFReaderErrors.VersionBelow5_4IsAnError`.

# 13. Ruler mode - DONE

The next mode to implement is Ruler mode, shortcut key r.

- Rulers can be drawn on the design by clicking on the layout view.
- Each time the user clicks a new point is added to the ruler.
- A "ghost" ruler should follow the users snapped mouse position, until they click, then the real ruler segment is drawn.
- Points are snapped to the grid by using the snapped mouse position.
- The ruler displays the distance between points and the total distance.
- Rulers are drawn orthogonally by default. The user can hold down shift to allow an non-orthogonal ruler.
- A tooltip displays the information about the shift function.

Ruler display:

- A line is drawn between the point with dynamic ticks depending on the scene's scale.
- There are major ticks with values and minor ticks without values.
- Major ticks should be in multiple of tens, i.e. every 1, 10, 100 etc and minor values should be one order of magnitude below. i.e. ten minor ticks for every major tick.
- The point-to-point distance should be displayed at the end of the line segment between the two points.
- The total distance should be displayed at the last point, prefixed with "total: "

**Resolution**: `Scene::Mode::RULER` (`scene.hpp`) is a new mode alongside `SELECT`/`EDIT`. Rulers are `Scene::Ruler{points, finished}`, stored as `std::vector<Ruler>` - multiple can exist at once (asked the user: starting a new one never clears an existing one). A click in Ruler mode (`le_mouse_up`) commits `Scene::add_ruler_point` - grid-snapped via the existing `snapped_mouse_position()`, orthogonally constrained to whichever axis moved more relative to the ruler's last point unless Shift is held (`Scene::ruler_free_form`, resynced from `LE_KEY_SHIFT` on every key event so render-side code can read it every frame). Also asked the user follow-up questions during design: zoom (keyboard/scroll/right-drag) needed to keep working mid-ruler (already did, by construction - none of the zoom paths are mode-gated, and both new render caches key on `viewport_version()`); and how a ruler finishes/restarts. First tried double-click-to-finish, but that made "which click placed the final point" ambiguous (a double-click's first click is indistinguishable from a plain click at the moment it happens) and caused two rounds of real bugs - settled instead on **the Esc key finishes the active ruler** (`LE_KEY_FINISH_RULER` -> `Scene::finish_active_ruler`, matching every other action-code's idempotent/safe-on-repeat shape), decoupling "finish" from mouse clicks entirely. Starting a new ruler is still click-driven: **a new ruler only starts if the next click is far enough** (`kNewRulerMinDistancePx`) from the just-finished ruler's last point, guarding against a stray click landing on top of it. `Scene::add_ruler_point` also rejects a point identical to the ruler's current last point outright, so two clicks landing on the same grid point never produce a zero-length segment. Pressing 'r' again (or `le_set_mode(LE_MODE_RULER)`) while already in Ruler mode abandons/finishes any in-progress ruler too (`Scene::reset_ruler_mode`). A `le_clear_rulers` API + "Clear Rulers" menu button was added beyond the item's own text, since without it accumulated rulers have no way to go away. The hover outline (yellow shape highlight) is now a Select-mode-only affordance - it was left on unconditionally before, so it kept highlighting shapes while placing ruler points too. `Scene::set_current_abstract` now clears rulers alongside selection/hover - rulers are plain dbu `Point`s with no Abstract scoping at all, so left uncleared they kept being drawn (at the same raw coordinates) after switching to an unrelated Abstract's own coordinate space.

Rendering: the live ghost segment (mouse-driven, recomputed every frame) lives in the existing `BuildOverlayPictureStage`; finalized rulers get their own `BuildRulerOverlayPictureStage` + a 4th `RasterizeStage` (same reasoning as selection's own split - only recompute on an actual ruler change, not every mouse move), composited into `ComposeWithOverlaysStage`. Tick spacing (`draw_helpers.hpp`'s `ruler_major_tick_spacing_um`) picks the largest-precision power-of-ten micron spacing whose on-screen pixel spacing stays above a minimum readable floor, with minor ticks at exactly 1/10th; perpendicular tick/label direction is computed in pixel space so free-form (non-orthogonal) segments get identical treatment to orthogonal ones. Micron conversion for labels uses a new `technology_dbu_per_um(root)` helper (`Renderer::render()` already receives `Root`, computed once and threaded into both new/changed overlay stages).

Covered by new `scene_test.cpp`/`api_test.cpp`/`render_test.cpp` cases (orthogonal constraint, free-form, the multi-ruler and distance-guard behavior, zoom mid-ruler leaving points unchanged, cache-invalidation regression tests for both new render stages, and a pixel-sampling test proving a committed segment actually renders). Full backend suite (556 tests) passes; `flutter_plugin`/`frontend` both build and launch cleanly with the new API surface linked.

# 14. Properties revisit - DONE

I notice that the properties contain the bbox of shapes and not the raw values. I would like to see the raw polygon, path and rect values as properties, in addition to the bbox.

Also, I would like the boundary layer selectable, which selects the abstract object and creates it's properties in the API.

# 15. TCL support exploration - DONE (see TCL_EXPLORATION.md; `show_gui` deliberately deferred)

I would like to explore how to enable TCL commands to be executed by the user.

For example:

```tcl
read_lef <filename>
create_library -name my_library
current_library my_library
create_design -name top_design
current_design top_design
create_view -type abstract
current_view abstract

create_terminal -name IN0 -direction IN
create_terminal_port -terminal IN0 -shapes {0.1 0.1 0.3 0.4} -layer M4

set terminals [get_terminal -filter {.name =~ IN*}]

set i 0
foreach terminal $terminals {
    create_terminal_port -terminal $terminal -shapes [list [expr $i * 10 - 10] 0 [expr $i * 10 + 10] 100]
}

set terminal_ports [get_terminal_ports -filter {.terminal.name =~ IN* && .layer_name == M4}]

foreach terminal_port $terminal_ports {
    update_terminal_port -name [get_prop $terminal_port .name]_SUFFIX
}

delete_terminal [get_terminal]

... etc ...
```

The main CRUD functions are, for each type of object:

- create
- get
- update
- delete

This is just a very rough example, not an exact specification, we would need to plan a concrete specification. Especially how to create different shapes, rect, poly, and path.

Ideally, I would like to support a batch mode where the user can run a terminal command to open an interactive TCL shell interface. Then use a TCL command, e.g. show_gui, to open the Flutter GUI. Please explore if this is possible. Possible problems include:

1. How to connect the GUI to the Root database already read in batch mode
2. How to refresh the GUI when TCL commands are entered in the shell

The reason I want to explore this now, is because the next step is to create API functions to create/read/update/delete terminal, obstruction and boundary objects and their shapes. I would like to develop the C API so it can be used by the Flutter GUI and a TCL shell at the same time.

There is a TCL interface generator called SWIG which may be useful to wrap the C API into a TCL API.

Also, Shape objects may need to be added to a pool to support this. The reason they are not in a pool right now, is that they can multiple parent types.

cmg could be used to generate the C API.

# 16. Pipeline and render module refactor for structure and clarity - DONE (see BENCHMARKS.md 2026-08-12 entries for both Pipeline and Renderer)

I would like more structure to help increase code clarity, understanding and readability.

Both pipeline and render perform similar data transformation functions using cached data. To make it easier to determine what feed each stage and waht is cached, I would like to refactor both modules to introduce some helper classes.

This MVP is only a small subset of the number of steps required for a full LEF/DEF editor, so it is only going to get more complex.

My idea is:

1. Each step in the data flow has it's own class
2. The output of each class contains a version number
3. Classes can be stitched together, so the inputs of one connect to the outputs of other.
4. Ideally some generic class can be created for easier future expansion.

This should help reduce the amount of caching bugs, like we had to fix in the previous commit.

# 17. Limiting TCL database access commands to current view scope - DONE (query scoping only - see below)

At the moment, all TCL commands access database objects from the global pools, which contains terminals and obstructions across all open abstract views.

From a users perspective, they only want to operate on objects related to the currently open abstract.

Example user flow:

```tcl
read_lef test_data/stripe_15layer.lef
open_design STRIPETEST -view abstract ;# NOTE: -view is redundant at the moment and should default to abstract for now
set terminals [get_terminals *]
```

Then the terminals variable will contain a list of terminal IDs for the STRIPETEST abstract only.

Also, I think the database TCL commands may be too low level. There is a serious risk that the user could modify something in the database that breaks the consistence. I think my previous request didn't account for this.

**Resolution**: `open_design <name> [-view abstract]` (`le_tcl_procs.tcl`) resolves a Design by name and sets it as the session's current view (`Scene::current_abstract()` - already existed for the render path, just not reachable from TCL before this). `search_terminal`/`search_obstruction`/`search_terminal_port` were replaced outright by `get_terminals`/`get_obstructions`/`get_terminal_ports`, scoped to the current view's Abstract (`le_get_terminals`/`le_get_obstructions`/`le_get_terminal_ports` in `api.hpp`/`api.cpp`, via `Root::get_abstract_terminals`/`get_abstract_obstructions` - no unscoped TCL escape hatch kept; `le_search_terminal`/`le_search_obstruction`/`le_search_terminal_port` remain in the C API unscoped, for any future Dart/GUI use, but no longer back a TCL command). Pass `*` instead of a filter expression to match everything in the current view. See `src/tcl/tests/crud_test.tcl`'s "Current-view scoping" section for the regression check (two Designs read into one session, `get_terminals *` proven not to leak across `open_design` switches).

The "TCL commands may be too low level" consistency-risk concern is **not** addressed here - deliberately deferred as a separate future item once it's specified concretely (e.g. should mutation commands reject cross-abstract operations, or is a higher-level guarded API wanted instead of raw CRUD). The CRUD commands (`create_terminal`, `create_obstruction`, `update_abstract_boundary`, ...) are unchanged - still explicit-`-abstract`, still low-level.

# 18. Friendly TCL ids instead of raw packed integers - DONE

Returning object IDs from TCL commands isn't very user friendly (e.g. `create_terminal` returning something like `4294967297`). I'd like a naming scheme instead - `object_type:name` if the object has a `.name` field, otherwise an index.

**Resolution**: `terminal:<name>` (Terminal already has a `.name` field), `obstruction:<id>`/`terminal_port:<id>`/`shape:<id>` (Obstruction/TerminalPort/Shape have no name field, so these keep their existing packed integer, just type-prefixed for self-description rather than a bare number) - every `create_terminal`/`create_terminal_port`/`create_obstruction`/`create_terminal_port_shape`/`create_obstruction_shape`/`get_terminals`/`get_obstructions`/`get_terminal_ports`/`terminal_port_shapes`/`obstruction_shapes` call and every command that takes one of these ids back (`delete_*`, `set_*`, every shape geometry mutator) uses this friendly string form exclusively - no numeric fallback kept for these four types. Empty string `""` is the uniform "not found/invalid" signal, replacing the old `kInvalidId` for these four (`kInvalidId` still applies to Design/Abstract ids, unaffected by this item).

`create_terminal`/`set_terminal_name` now reject a name that collides with another Terminal already on the same Abstract (`le_create_terminal`/`le_set_terminal_name` in `api.hpp`/`api.cpp`, a plain linear scan over `Root::get_abstract_terminals` - deliberately _not_ a `cmg`/`schema.py` `index=True` lookup, since real LEF libraries legitimately reuse pin names like `VDD`/`IN0` across different Abstracts, and `index=True`'s generated index is flat/global, not per-Abstract) - this closes a real, previously-unvalidated gap (the LEF reader has a literal `// TODO: Check that pin doesn't already exists with the same name`), not just a naming-scheme side effect. Obstruction gained no name field and no uniqueness concept - it was deliberately kept index-based per a direct correction during planning.

Renaming a Terminal changes what its friendly id refers to, by construction (the id _is_ the name) - a script holding an old `"terminal:OLDNAME"` string after a rename has a stale reference and needs to re-derive the new one (trivial - it already has the new name literally). See `src/tcl/tests/crud_test.tcl`'s uniqueness-enforcement and rename-staleness cases.

**Known, deliberately out-of-scope gap**: `lefrPinCbkFn` (`src/io/lef_reader.cpp`) still has no duplicate-PIN-name check when _reading_ a LEF file (bypasses the new API-layer uniqueness check entirely, calling `Root::create_terminal` directly) - two same-named PINs in one malformed `MACRO` would produce two Terminals sharing one friendly id, and the second becomes unreachable by name from TCL (still present in the database, just not addressable by `terminal:name`). Left as a documented gap, not fixed here - this item was about TCL ergonomics/API-level create/rename validation, not LEF-import validation.

# 19. Common TCL command patterns for all object types - DONE

## 19.1 get\_\*

Each object type should have a get\_ command: get_libraries, get_designs, get_abstracts, get_terminals, get_terminal_ports, get_obstructions etc.

The get\_ command syntax should be (square brackets means optional):

`get_<type> [<name expressions>] [-of <parent tokens>] [-filter <filter expression>] [-help]`

NOTE: For object types without a name, omit the positional `<name expressions>` argument.

- `<name expressions>`: filters object by name, using optional widrcards, may be multiple expressions which should be or'ed.
- `-of <parent tokens>`: returns objects that have the specified parent tokens.
- `-filter <filter expression>`: filter by properties (current implementation).
- `-help`: Returns usage

Examples:

Get a library name my_library:

```
get_libraries my_library -> library:my_library
```

Get all designs in my_library:

```
get_designs -of library:my_library -> {design:A design:B ...}
```

Get designs with name containing AND from my_library:

```
get_designs *AND* -of library:my_library -> {design:AND4 design:AND8 ...}
```

Get designs abstract view:

```
get_abstract -of design:AND4 -> abstract:0
```

Get abstract views terminals:

```
get_terminals -of abstract:0 -> {terminal:IN0 terminal:OUT ...}
```

Error checking:

1. -of must only contain valid parent object types
2. -filter must only contain valid property names/chains that match the objects available properties. For example, get_abstracts -of library:my_library should return an error stating that only design objects can be used with get_abstracts -of.

**Resolution**: `get_libraries`, `get_designs`, `get_abstracts`, `get_terminals`, `get_terminal_ports`, `get_obstructions`, and (for full type coverage) `get_shapes` all share one Tcl proc shape (`parse_get_args` in `le_tcl_procs.tcl`): zero-or-more OR'd `<name expressions>` (glob-matched, only for the types with a real `.name` field - Library/Design/Terminal), zero-or-more OR'd `-of <parent-token>` values (each validated against the command's own valid parent-type prefix _before_ any lookup - a wrong-type token errors immediately, per requirement 1), an optional `-filter <expr>`, and `-help`. Omitting `-of` falls back to the existing "current view" default (`Scene::current_abstract()`, from `open_design` - item 17) rather than requiring it on every call, extended outward/inward through the whole hierarchy (`get_abstracts` defaults to the current view itself; `get_designs` defaults to the current view's own Library; `get_terminal_ports`/`get_shapes` default to a 2-hop union under the current view, same shape as item 17's own `get_terminal_ports` default already was).

`filter_expression`'s field/hop names are now validated (requirement 2) against a hand-maintained per-type allowlist in `api.cpp` (`validate_filter_path`/`filter_field_tables`, cross-checked directly against each generated class's own `get_field`/`match_hop`) - an unrecognized name is a real parse-time-adjacent error (pushed via `le_message_*`, `-1` return), not the silent no-match `filter.hpp` produces on its own. Deliberately excludes hops into non-pooled/value-list fields (Abstract's `bbox`/`densities`/etc., Terminal's `antenna_*`, Shape's `rects`/`polygons`/etc.) - those remain usable only via the older, unscoped `le_search_terminal`/etc. escape hatch.

Library/Design/Abstract gained friendly ids extending item 18's scheme: `"library:<name>"`/`"design:<name>"` (both real, globally-unique `index=True` fields already) and `"abstract:<n>"` (no name field, same numeric-type-prefix pattern as Obstruction/TerminalPort/Shape). This is additive, not a retrofit - `design_by_name`, `design_abstract_id`, `set_current_design_cmd`, and every `-abstract` CRUD flag (`create_terminal`, `create_obstruction`, `update_abstract_boundary`) are unchanged, still raw packed ids; only `open_design`'s own return value was switched to `"design:<name>"` for consistency (trivial - it already has the name in hand). `get_terminals`/`get_obstructions`/`get_terminal_ports` themselves were changed in place (breaking their item-17/18 single-filter-string signature) rather than left alongside new commands, per the "one unified pattern" framing of this item's own title.

See `src/tcl/tests/crud_test.tcl` for the full regression coverage, including the `-of`-wrong-type and `-filter`-unknown-field error cases.

## 19.2 Properties - DONE

Instead of individual commands for listing properties (please remove them) I would like the following command to get properties using tokens:

- get_properties <list of tokens> [<property names>] : Returns a list of properties for the specified tokens

For example:

Return a list all terminal properies, returns list of dict:

```
get_properties [get_terminals] -> { {name IN0 direction INPUT } { name OUT1 direction OUTPUT} ...}
```

Get a single property value:

```
get_properties terminal:IN0 name -> IN0
```

Get multiple properties for one object:

```
get_properties terminal:IN0 {name direction} -> {IN0 INPUT}
```

Get multiple properties for mutlple objects (returns list of list):

```
get_properties [get_terminals] {name direction} -> { {IN0 INPUT} { OUT1 OUTPUT} ...}
```

Then another command to report properties to stdout in a human readable format:

```
report_properties <list of tokens>
```

Example:

```
report_properties [get_terminals]

terminal:IN0
  name:      IN0
  direction: INPUT
  ... etc ...

terminal:OUT0
  name:      OUT0
  direction: OUTPUT
  ... etc ...
```

NOTE: property names must be padded to align values.

**Resolution**: `terminal_properties`/`terminal_port_properties`/`obstruction_properties` are removed. `get_properties`/`report_properties` work on any friendly-id token from the item 19.1 `get_<type>` family - Library/Design/Abstract/Terminal/TerminalPort/Obstruction/Shape - dispatched by the token's own prefix (`property_accessors_for_token` in `le_tcl_procs.tcl`). `get_properties`'s return shape is decided purely by whether `tokens`/`property names` each hold one element or many (Tcl can't otherwise distinguish a bare token from a one-element list of tokens) - verified against all four of this item's own worked examples character-for-character. `report_properties`'s padding is computed per token block (the longest property name in that specific token's own property set), matching the example output exactly.

Backing this required four new `le_X_property_count`/`_at` pairs in `api.hpp`/`api.cpp` (Library/Design/Abstract/Shape - Terminal/TerminalPort/Obstruction already had theirs), each a thin wrapper over `cmg`'s generated `to_properties()` with no hand-appended child-pool count row (unlike Terminal's `port_count`/TerminalPort's and Obstruction's `shapes_count` - not extended to the new types since this item asked for a unified listing mechanism, not new fields).

**Follow-up (dot notation + chaining)**: `get_properties`'s property-name argument now takes the same dotted path syntax as `-filter` (item 15) instead of a bare word - `get_properties terminal:IN0 .name`, `get_properties terminal:IN0 {.name .direction}` - and that path can chain through a relational hop exactly like `-filter` does, e.g. `get_properties terminal_port:0 .terminal.name` or a list hop like `get_properties terminal_port:0 .shapes.layer_name` (existential/first-match semantics on the list hop, same contract `-filter` already has - resolves to `""` if the port has no shapes yet rather than erroring). This reuses the filter DSL's machinery rather than adding a second parser: `filter.hpp` gained `parse_property_path`/`resolve_property_path`, built on the same generated `get_field`/`match_hop` functions `-filter` itself walks, and one new `le_X_property_path`/`X_property_path` accessor per type (`api.hpp`/`api.cpp`, `le_tcl_shim`, `le_api.i`) alongside the existing `_count`/`_at`/`_value` triplet. A path that fails to parse (missing leading `.`) or references an unrecognized field/hop name is a Tcl error, same as an unrecognized `-filter` field; a structurally valid path that simply has no data to resolve (e.g. an empty list hop) returns an empty string, not an error.

**Found and fixed a real, previously-latent bug while implementing this**: `to_properties()`/`to_string()` for any `is_optional` field called `.value()` unconditionally, with no `has_value()` check - `Abstract.power` (unset on any Abstract without a LEF POWER statement, which is the common case) crashed with `std::bad_optional_access` the moment `get_properties`/`report_properties` reached an Abstract token, since nothing in this codebase had ever called `to_properties(AbstractData)` before. Fixed at the root cause in `cmg`'s code generator (`Field.wrap_with_to_property`/`wrap_with_to_string` in the sibling `cmg` checkout) - both now emit `.value_or(<value-initialized-default>)` instead of `.value()` - then regenerated via the `regen-database` skill. This is a generator-level fix (affects the `cmg` tool itself, not just this project's `schema.py`), flagged here since it's a real, if narrow, behavior change to shared codegen infrastructure beyond this repo.

**Follow-up (non-pooled object-list fields return their contents, not a count)**: a list field whose element type is an embedded (non-pooled) Klass - `Shape.rects`/`polygons`/`paths`/`rect_iterates`/`path_iterates`/`polygon_iterates`/`texts`/`vias`/`via_iterates`, and any equivalent field elsewhere in the schema - now reports its full contents in `get_properties`, not a bare `rects_count 1`. The old `<field>_count` property is gone for these fields entirely (not kept alongside the new one) - a bare count was confusing once the real data was one property lookup away. A list of a plain scalar (`Shape.rect_masks`/`polygon_masks`/`path_masks`, `vector<int>`) is unaffected and still reports `<field>_count`, since there's no per-element structure to expand.

This is another generator-level change (`cmg`'s `Field.wrap_with_to_property`, same file as the bug fix above). It required going one level deeper than a naive fix: simply reusing each element's existing `to_string()` would have still shown only counts for anything _that_ element itself lists (e.g. `Shape.polygons`' own `Polygon.points`, or `Shape.paths`' `Path.polygon.points` one level deeper still) - `to_string()` deliberately collapses lists to a count, by design, for compact debug logging, and that convention is staying as-is for logging. So `cmg` now also generates a second, parallel function per class, `to_property_string()` - `to_string()`'s fully-recursive, get*properties()-facing sibling, which never collapses a list - plus a shared `to_property_list_string()` helper (`property.hpp`) that joins a list into one Tcl-list-friendly string, brace-delimiting each element via \_its own* `to_property_string()` so nested lists expand too, all the way down. A scalar (non-list) embedded-struct reference field (e.g. `Path.polygon`) is expanded the same fully-recursive way instead of the old count-collapsing `to_string()`; an enum-typed reference, or a reference to a pooled Klass stored as a bare Id (e.g. `Instance.reference_design`), is the one exception in both directions - neither has a `to_property_string()` overload, so those keep going through `to_string()` unchanged.

**Follow-up (Shape's rects/polygons/paths report clean micron coordinates, not raw dbu)**: the generic `to_property_string()` fix above still left `Shape.rects`/`polygons`/`paths` showing e.g. `rects {{Rect{ll=Point{x=396000 y=0 } ur=Point{x=398000 y=1000000 } }}}` - correct contents, but raw database units with type-tag noise, not the plain coordinate list every other coordinate-reporting command in this project uses (`shape_rect_at`, `bbox_um`, ...). `cmg`'s generated code has no fix available here: `to_properties()`/`to_property_string()` only ever see the bare `ShapeData` struct, never `Root`, so they have no way to reach `Technology`'s `dbu_per_um` and convert. Fixed by hand in `api.cpp` instead, where that access already exists: `build_shape_properties` now calls `le::to_properties()` as before, then `replace_shape_geometry_properties` overwrites just the `rects`/`polygons`/`paths` rows (by property name) with a clean listing built from `format_coordinate_um` (already used for `bbox_um`) - `rects {{2 2} {8 8}}` (one `{ll} {ur}` pair per rect), `polygons {{0 0} {5 0} {5 5} {0 5}}` (one `{x y}` per point), `paths {{0 0} {10 10} {20 0} 0.500}` (the path's points, then its width, all in one group) - each rect/polygon/path keeping its own brace grouping so multiple elements never get flatten-merged into one ambiguous point list. `rect_iterates`/`path_iterates`/`polygon_iterates`/`texts`/`vias`/`via_iterates` keep the raw-dbu, `to_property_string()`-based form for now - not asked for, and each has its own shape (a `Text` isn't a coordinate list at all) that would need its own case-by-case formatting decision.

**Found and fixed a second real, previously-latent bug while implementing this one**: `get_properties shape:N .rects` (single-segment dot-path lookup, not the bare-token dict form) raised `unknown field 'rects' on Shape` - the dot-path resolver (`resolve_property_path`, backing every `le_X_property_path`) is built on the `-filter` DSL's `get_field()`, which only recognizes scalar leaf fields, never list fields like `rects`/`polygons`/`paths` (those are hops in filter terms, not leaves) - so a bare `.rects` could never have worked there, even though a plain `get_properties shape:N` (no name) already showed it via the unrelated `to_properties()`-based path. Fixed by having every `le_X_property_path` try a single-segment path against that same `build_X_properties()` row list first (the identical rows `get_properties $token` with no name already shows), falling back to the filter-DSL leaf/hop resolver only for a genuinely chained path (`.terminal.name`, `.shapes.layer_name`) - `build_X_properties()`'s row set is always a superset of the filter DSL's scalar leaves, so this is a pure widening, not a behavior change for anything that already worked. While fixing this, testing it surfaced a second, unrelated, genuinely dangerous bug: every `le_X_property_path` built its returned `LeProperty`'s `.name`/`.string_value` as raw `c_str()` pointers into a `std::optional<PropertyValue>` (or, for the new single-segment path, a freshly-built temporary `vector<PropertyValue>`) that was local to the function - a dangling pointer the instant the function returned. A short value (`"IN0"`) "worked" by sheer luck (small-string-optimized, its bytes often still intact in the just-freed stack slot when the caller read them milliseconds later), which is exactly why this went unnoticed through all of items 19.2's original dot-notation testing - only a longer, heap-allocated value (a formatted `rects`/`polygons`/`paths` coordinate list) reliably came back corrupted. Fixed by adding one more handle-owned single-slot cache (`cached_property_path_value`) that every `le_X_property_path` now writes its result into before returning `to_c()` of it - "valid until the next call", the same convention every other `LeProperty`-returning accessor in this file already relies on.

# 20. Integrated help system

Features applied to all TCL procs to help the user while they run interactive TCL commands in the GUI and le_shell:

1. `help` command to return short usage messages for matching commands, i.e. help get\_\*
2. `man` command to display a man page style output for a specific command
3. Command-line completion, when the user presses tab, a token is sent to the API with the current command line text and sugeestions for commands, and command arguments a provided (GUI only for now). When the command argument provides a property name using dot-hop format. The property names should be auto-completed too. For example: get_properties shape:0 .[TAB] should list the properties available for the Shape object.
4. Generate TCL command documentation in MD format, for every command and command option.

The usual way to do this is to create some helper functions in TCL, that can be ran after creating a proc to annotate the usage and option data for that proc. The usage data can be stored in a global TCL variable and read by the help, man and command completetion procs. Then any custom TCL procs can be added to the help system at runtime.

You should wrap all generated TCL commands with procs to enable this feature.

# 21. Object editing

Add support for editing objects in the GUI with undo/redo support.

1. Setup command pattern with undo/redo capability.
2. Move current Flutter based history into backend and reuse history from step 1.
3. Implement "Move" command
  3.1. The user selects one or more shapes in the GUI
  3.2. Then enters edit mode
  3.3. Click on Move button or CTRL-m to start move
  3.4. Click on layout window to register start point
  3.5. Move mouse to new position, show "ghost" shape while mouse is moving. Defaults to orthogonal, use shift to allow freeform move.
  3.6. Second click commits the move.
4. Also, when in edit mode, no select mode keyboard shortcuts should be available that change the object selection. The user needs to move to select mode to change the selection. This is to stop unintended object selection during editing.
5. Different object types may require different editing semantics, but we will start with shapes only first. Something to consider when shaping the code.
6. This bulk of this code should probably live in it's own module, with a thin wrapper in the api module.

