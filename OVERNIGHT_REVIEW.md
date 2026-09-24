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

