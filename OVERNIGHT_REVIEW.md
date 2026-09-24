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

