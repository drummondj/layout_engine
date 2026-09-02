# Overnight session review (2026-09-02 -> 2026-09-03)

Working through BUGS_AND_ENHANCEMENTS.md's remaining open items, one at a
time (research -> implement -> test -> commit+push per item, matching the
established pattern from prior overnight passes - see git log). B6 stays
skipped (marked "SKIP FOR NOW" in the file itself). Each entry below is
added as an item is finished, or if I hit a judgment call I couldn't ask
about. Items I'm not fully confident in are left uncommitted with an
explicit request for sign-off, same convention as previous nights.

## B8. write_def always writes "WEIGHT -1" to each COMPONENT even if that wasn't in the original DEF.

**Fixed.** `DEFWriter::write_placements` passed `placement->weight.value_or(-1.0)`
to the vendored `defwComponentStr`. Its own header comment claims `-1.0` is
the correct "omit this field" sentinel ("The optional fields will be
ignored if they are set to zero (except for weight which must be set to
-1.0)"), but the real implementation (`defwWriter.cpp`) gates the `+
WEIGHT` line on a plain `if (weight)`, true for *any* non-zero value
including `-1.0` - so the documented sentinel actually always writes a
literal `WEIGHT -1`. The header comment is simply wrong. `0.0` is the only
value that check treats as false, so it's the real sentinel - changed the
call site to `weight.value_or(0.0)`. Documented in `LEFDEF_BUGS.md`'s DEF
writer section (a real vendored-library documentation bug, not something
in this project's own code, so it belongs there rather than being a silent
one-line fix).

One real, unfixable consequence noted alongside the fix: a `Placement`
whose weight genuinely *is* `0.0` (unusual but valid DEF) can never be
told apart from "no weight at all" through this vendored API - it will
always round-trip as unset. Same shape as the NONDEFAULTRULES
sub-micron-precision gap already documented there.

New tests: `DEFWriterComponentWeightFixture` (`def_writer_test.cpp`, new
`component_weight.def` fixture with one COMPONENT with no WEIGHT and one
with `+ WEIGHT 3`) - `ComponentWithNoWeightWritesNoWeightLineAtAll` checks
the raw output text never contains `"WEIGHT -1"` (the literal reported
symptom) and that the re-read Placement has no weight set;
`ComponentWithARealWeightRoundTrips` checks a real weight survives
write->reread intact. Both verified to fail against the pre-fix code
(temporarily reverted via `git checkout HEAD --`, confirmed the exact
`"WEIGHT -1"` string appearing and `weight.has_value()` being `true` when
it shouldn't be) and pass with the fix restored. Full suite passes; both
`build`/`build_release` rebuilt.

## B9. write_def -> read_def round trip loses via arrays; need a round-trip verification methodology.

**Fixed - root cause was very different from what the symptom suggested.**
Investigated by re-reading `DEFWriter::write_vias`
(`src/io/def_writer.cpp`) end to end and comparing every `defwViaViarule*`
call it makes against the vendored writer's own full function family
(`defwWriter.hpp`). `write_vias` calls `defwViaViarule(...)` (the base
CUTSIZE/LAYERS/CUTSPACING/ENCLOSURE clause) for every via with a
`ViaRuleReference`, but never calls `defwViaViaruleRowCol`/`Origin`/
`Offset` at all - three separate follow-up calls the vendored API requires
immediately after `defwViaViarule` to write the ROWCOL/ORIGIN/OFFSET
clauses (each "can only be called once... after defwViaViarule", per their
own header comments - not optional parameters on the base call). Since
`ViaRuleReference` with no ROWCOL clause means exactly "a single cut, not
an array" (the schema's own documented convention, from the original B3
fix), every via *array* silently collapsed to a single cut on write - the
real cause of "missing vias" after a round trip. This also explains the
report's own "lots of via arrays one after another... hard to tell by
eye" observation in hindsight: with ROWCOL always missing, every array via
and every genuinely-single-cut via look identical in the written text -
there's no way to tell them apart by eye anymore, which is exactly what
was reported.

**Judgment call:** while fixing this, checked whether `LEFWriter` (the LEF
side, not what B9 reported, but the same underlying B3 follow-up schema
fields) had the identical gap - it did, verbatim (`write_vias` in
`lef_writer.cpp` calls `lefwViaViarule` but never
`lefwViaViaruleRowCol`/`Origin`/`Offset` either). Fixed both in the same
pass rather than leaving a known-identical bug sitting in the sibling
writer - same root cause, same fix shape, found investigating one bug
report.

**On the "methodology" ask:** interpreted this pragmatically as real,
targeted round-trip regression coverage for the actual defect, rather than
building a generic n-way `Root`-vs-`Root` diff tool - the latter would be
substantial new infrastructure disproportionate to what actually caused
this bug (one missing set of writer calls, now covered by a direct test
that reads the exact fields it's about). If you want stronger,
production-scale confidence beyond the two hermetic fixture tests below
(e.g. running write_def/read_def on the real `aes_5x5.def` design and
diffing via/shape counts end to end), that's a reasonable follow-up but
felt like a separate, larger effort out of scope for finishing this one
tonight - happy to pick it up as its own item if you'd like it added to
`BUGS_AND_ENHANCEMENTS.md`.

New tests: `LEFWriterViaRuleReferenceRoundtripFixture` (`lef_writer_test.cpp`,
reuses the existing `via_rule_reference.lef` fixture's VIA4 - ROWCOL +
ORIGIN + OFFSET together) and `DEFWriterViaRuleReferenceRoundtripFixture`
(`def_writer_test.cpp`, reuses `via_rule_reference.def`'s VIA_ARRAY_1, the
same fixture `DEFReaderViaRuleReferenceFixture` already covers on the read
side) - both assert `num_cut_rows`/`num_cut_cols`/`origin`/`bot_offset`/
`top_offset` all survive a real write->reread round trip intact. Both
verified to fail against the pre-fix code (`git checkout HEAD --` on each
writer file in turn, confirmed `num_cut_rows.has_value()` was `false` when
it shouldn't be) and pass with the fix restored. Full suite passes; both
`build`/`build_release` rebuilt.

## E28.b. write_lef -library/-abstracts (write MACROs for a whole Library, or an explicit subset).

**Done.** Reworked `le_write_lef` (and the underlying `LEFWriter::write_lef`)
to write one MACRO per Abstract in a list, not just one, so a single call
can now cover a whole Library. Resolution order (documented in `api.hpp`'s
own doc comment):

1. An explicit Abstract list, if given (via `-abstract` or `-abstracts`) -
   always wins outright, regardless of which Library any of them belong to.
2. Else a `-library` token, if given - every Abstract in every Design of
   that Library (a Library with no Abstracts yet writes zero MACROs, not
   an error).
3. Else the current Abstract (the original E28 behavior, unchanged).
4. Else - error, same message shape as before.

**Judgment calls (not asked, both seemed like the most useful reading of
the spec without over-restricting it):**
- Kept the original single `-abstract` flag working exactly as it did
  under E28 (mutually exclusive with `-library`/`-abstracts`) rather than
  removing it in favor of always requiring `-library`/`-abstracts` -
  the item's own text reads as "add this new capability", not "replace
  the old one", and the single-Abstract case is common enough to be worth
  keeping the more convenient dedicated flag for.
- Made `-abstracts` usable *standalone*, without requiring `-library` -
  the item's own text frames it as a filter nested under `-library`
  ("-library argument, then an -abstracts argument"), but an explicit
  token list already fully determines what to write on its own, so
  requiring `-library` too would just be an arbitrary extra restriction
  with no real benefit. `-library` alone (no `-abstracts`) still means
  exactly what the item asks: every Abstract in the Library.

**Real bugs found and fixed along the way, not just the new feature
itself:**
- My own first attempt at `ApiFixture.WriteLefWithALibraryWritesAMacroForEveryAbstractInEveryDesign`
  read `testcell.lef` then `othercell.lef` into one handle and assumed
  both designs landed in the same Library - `le_read_lef`'s own Library
  name is the LEF file's stem (`api.cpp`), so they're actually two
  separate Libraries. Fixed by building two Designs from scratch under
  one real Library instead (`le_create_library`/`le_create_design`/
  `le_create_abstract` directly) - a test-only mistake, not a product bug,
  but the kind of thing worth recording since I initially had it backwards.
- The three round-trip variants of this test initially used
  `LE_LEF_LAYER_WRITE_MODE_NONE`, which never writes `UNITS`/`DATABASE
  MICRONS` - re-reading that output into a *fresh* handle then fails
  ("used geometry that required DATABASE MICRONS before it was ever
  declared"). Also a test-only mistake (the original single-Abstract E28
  tests never re-read their own output, so this gap was never exercised)
  - fixed by using `INCLUDE_WITH_ABSTRACT` for every test that re-reads.
- `crud_test.tcl`'s own new checks searched for `"MACRO SCRATCH_DESIGN "`
  (trailing space) to disambiguate from `"MACRO SCRATCH_DESIGN2 "` - the
  vendored writer's real output has a newline right after the MACRO name,
  not a space, so the search always failed to match. Fixed to search for
  `"MACRO SCRATCH_DESIGN\n"` instead. Caught immediately by actually
  running the test rather than assuming the string shape - a good
  reminder to check the *real* written bytes instead of guessing writer
  formatting from memory.

**Also regenerated `TCL_COMMANDS.md`** (`generate-tcl-docs` skill) -
it hadn't been rebuilt since before `write_lef`/`write_def` (E28) were
even added, so `write_lef`'s entry there was completely missing, not just
stale. Fixed for both E28 and E28.b's usage text in one pass.

New/updated tests: `api_test.cpp` (`WriteLefWithALibraryWritesAMacroForEveryAbstractInEveryDesign`,
`WriteLefWithAnExplicitAbstractListWritesOnlyThoseAbstracts`,
`WriteLefWithALibraryThatHasNoAbstractsWritesNoMacrosAndIsNotAnError`,
plus every existing E28 test updated for the new `le_write_lef` signature)
and `crud_test.tcl` (`-library`, `-abstracts`, and the `-abstract`/
`-library` mutual-exclusion error). Full 700-test suite passes; both
`build`/`build_release` rebuilt.

## E30. get_selection/select TCL commands.

**Done.** Added a new C API function, `le_select_object_ref(handle, ref)` -
the script-driven counterpart to a real mouse click, built on the same
generic `LeObjectRef` (kind/index/generation) type `le_selected_object_ref`
already returns for reading the current selection (E1) - so a script can
now round-trip `get_selection` -> tokens -> `select` those same tokens
back, using one consistent identity representation both directions.

Only `LE_OBJECT_KIND_SHAPE`/`_ROW`/`_PLACEMENT`/`_REGION` are supported -
the same four kinds `Scene::SelectedObject`'s own variant actually covers.
`select shape:N` selects *every* rect/polygon/path of that Shape (reusing
`select_all_unlocked`'s own per-shape piece-enumeration loop), not one
specific piece - piece-level granularity only has meaning from a real
mouse hit-test (which piece was actually clicked), a bare Shape token from
a script has no such information to narrow it down.

TCL surface: `get_selection` (no args, returns the current selection as a
list of `shape:`/`row:`/`placement:`/`region:` tokens) and `select
<tokens>` (one or more tokens, additive - same as ctrl/shift-clicking,
does not clear the existing selection first). Both are thin wrappers
around three new shim functions (`selection_count_cmd`/
`get_selection_at_cmd`/`select_cmd`, `le_tcl_shim.cpp`) dispatching by
token prefix / `LeObjectKind`.

**Judgment calls (not asked):**
- `select`'s own token-prefix dispatch uses plain literal `"shape:"`/
  `"row:"`/etc. string checks in the shim rather than the generated
  `kShapePrefix`/etc constants those literals mirror - the constants live
  in the generated file's own scope, not this hand-written one, and
  duplicating a 5-character literal locally seemed simpler/lower-risk than
  reaching across that boundary for it.
- `select_cmd`'s own return value distinguishes "unrecognized prefix" (2)
  from "recognized prefix but le_select_object_ref itself failed" (1) -
  needed because le_tcl_shim.cpp has no access to `LeHandle`'s real
  definition (it's opaque outside `api.cpp`) to push its own ERROR message
  for the first case the way every other shim function pushes messages
  through the handle it's given; `select` (the Tcl proc) raises its own
  clear error using the token text it already has for that case instead,
  and only reads `handle->messages` (via `message_count`/`message_at`,
  the same idiom `write_lef`/`write_def` already use) for the second.

Also regenerated `TCL_COMMANDS.md` again (`generate-tcl-docs` skill) for
`get_selection`/`select`'s own new entries.

New tests: `api_test.cpp` (`SelectObjectRefWithAShapeRefSelectsEveryPieceOfIt`,
`SelectObjectRefWithAnInvalidShapeFailsWithAMessage`,
`SelectObjectRefIsAdditiveNotReplacing`, `SelectObjectRefWithAPlacementSelectsIt`,
`SelectObjectRefWithAnUnsupportedKindFailsWithAMessage`,
`SelectObjectRefWithNullHandleDoesNotCrash` - all pass on the first real
attempt, no fail-before/pass-after cycle needed since this is new
functionality, not a bug fix) plus `smoke_test.tcl` (basic no-data-loaded
smoke checks) and `crud_test.tcl` (a real Shape/Row/Region round trip
through `get_selection`/`select`, including the additive and rejected-
token cases). Full 706-test suite passes; both `build`/`build_release`
rebuilt.
