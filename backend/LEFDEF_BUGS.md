# Vendored LEF/DEF Library Bugs

Confirmed defects and limitations in the vendored Si2 LEF/DEF 6.0.62-p004
C++ parser/writer (`src/lefdef/lef/`), found while implementing UPDATES.md
item 12 (LEF syntax completion) by diffing `LEFReader`→`LEFWriter` round
trips against `src/lefdef/lef/TEST/complete.5.8.lef` via the
`lef_roundtrip_diff` dev tool. This is third-party vendored source (see its
own `LICENSE.TXT`) — never hand-edited, per CLAUDE.md's own rule — so every
entry here is worked around in `src/io/lef_writer.cpp`/`.hpp`, not fixed at
the source. Each workaround site has its own `KNOWN VENDORED-LIBRARY
BUG`/`GAP`/`EDGE CASE` comment in the code; this file is the consolidated
index. Almost all are on the **writer** (`lefw*`) side — most reader
asymmetries trace back to a real writer defect below, not a parsing error.
The one exception is a real reader-side finding, not a bug but a deliberate
library behavior worth knowing about before spending time on it again - see
"Reader-side: intentional version-obsolescence" at the end of this file.

## Bugs that produce unparseable output

These write syntactically invalid LEF — a file written through the
affected path fails to re-parse, not just loses information.

- **`lefwLayerResistancePerCut`** writes the literal keyword
  `RESISTANCEPERCUT`, which does not exist anywhere in `lef.y`'s grammar —
  the real CUT-layer statement is plain `RESISTANCE <value> ;` (same
  keyword as ROUTING's own `RESISTANCE`, different `lefiLayer` accessor
  pair). Confirmed by writing it and re-reading the result: `ERROR
  (LEFPARS-1): ... on token RESISTANCEPERCUT`. Not called; CUT-layer
  resistance is read-only in `LEFWriter`.
- **`lefwViaLayerPolygon`** (non-encrypted branch): prints point 0 as
  `"%.11g %.11g"` (no trailing separator) and every later point as
  `"%.11g %.11g "` (no *leading* separator), so point 0's y and point 1's x
  land back-to-back with zero characters between them — e.g. `y0=-1,
  x1=-0.2` writes as the single unparseable token `-1-0.2`. Found via
  `complete.5.8.lef`'s `myVia23`, which has real multi-point VIA POLYGON
  geometry; made `lefdiff` choke partway through and silently truncate the
  rest of that dump, which hid the real diff for everything written after
  it in the same file (NONDEFAULTRULE/SITE weren't actually missing -
  `lefdiff` just never got that far). Every *other* polygon writer in the
  file (`lefwMacroPinPortLayerPolygon`/`lefwMacroObsLayerPolygon`) does not
  have this bug — only the VIA-specific one does, and there's no working
  alternate API for it. Not called; `ViaLayer.polygons` is read-only.
- **`lefwViaRuleLayer`/`lefwViaRuleGenLayer`** (both route through the
  shared `lefwViaRulePrtLayer` helper) hard-reject `direction`/`overhang`/
  `metalOverhang` with `LEFW_OBSOLETE` at `versionNum >= 5.6` — and
  `write_lef` always writes `VERSION 5.8`, so none of the three can ever be
  passed. But `lef.y`'s own grammar for a **non-GENERATE** `VIARULE`'s
  `LAYER` still *requires* a `DIRECTION` construct at 5.8 regardless —
  confirmed by re-parsing a round-tripped `complete.5.8.lef` (`VIALIST1`/
  `VIALIST12`, both non-GENERATE): `ERROR (LEFPARS-1705): VIARULE statement
  in a layer, requires a DIRECTION construct statement`. A genuine
  contradiction inside the vendored library itself — the writer's own
  version gate and the reader's own grammar disagree about whether
  `DIRECTION` is allowed at LEF 5.8, with no version this project's writer
  can pick that satisfies both, and a non-GENERATE `VIARULE`'s `LAYER`
  requires exactly 2 `LAYER` sub-statements per `lef.y` (an empty
  `VIARULE` is a *fatal* `LEFPARS-1`), so there's no partial-data fallback
  either. Confirmed the recoverable `LEFPARS-1705` error still has an
  outsized blast radius: `write_via_rules` skips the entire non-GENERATE
  `VIARULE` now (not just `DIRECTION`) precisely because leaving it
  half-written was worse than dropping it - `lefdiff`'s own comparison
  silently stopped accumulating *everything* written after the errored
  VIARULE in the same file, so `NONDEFAULTRULE`/`SITE` (which round-trip
  correctly on their own) looked missing purely because of this one
  upstream error, not because they were actually broken. `overhang`/
  `metal_overhang` are read-only for GENERATE VIARULE layers too
  (ENCLOSURE, LEF 5.5's replacement, still works and is written when
  present). Not called for non-GENERATE; `ViaRule`s with `is_generate ==
  false` are entirely read-only for writing.
- **`lefwLayerRoutingSpacingEndOfLine`** unconditionally flushes (`;\n`)
  whatever `SPACING` statement is still open *before* writing `ENDOFLINE
  ...`, producing an orphaned top-level `ENDOFLINE` statement — but
  `lef.y` only ever accepts `K_ENDOFLINE` nested inside a `SPACING`
  statement's own option grammar (one grammar occurrence). Not called;
  `SpacingRule.end_of_line_*`/`parallel_edge_*`/`two_edges` are read-only.
- **`lefwLayerRoutingSpacingNotchLength`** / **`SpacingEndOfNotchWidth`**:
  same root cause as the `ENDOFLINE` bug above — both flush the open
  `SPACING` statement and emit `NOTCHLENGTH`/`ENDOFNOTCHWIDTH` as a
  separate top-level statement, but `lef.y` only accepts them nested
  inside `SPACING`'s own option grammar. Not called; `SpacingRule.
  notch_length`/`end_of_notch_*` are read-only.
- **`lefwStartMacroDensity(layerName)`** prints `DENSITY <layerName>\n`
  directly with no `LAYER` keyword, but `lef.y`'s `macro_density` rule
  requires bare `DENSITY` followed by one `LAYER name ;` statement per
  layer group — the written text can never be re-parsed as a `DENSITY`
  statement, for any layer count. It also flatly refuses a second call in
  the same macro (an internal `lefwIsMacroDensity` guard), so even a
  syntax-correct workaround couldn't cover more than one layer group. Not
  called; `Abstract.densities` is read-only.

## Bugs/gaps that silently reject or drop valid data

These don't corrupt output — they just refuse a call (`LEFW_BAD_ORDER`/
`LEFW_BAD_DATA`) or silently omit a token for input the real LEF grammar
and the vendored *reader* both accept.

- **`lefwIntPropDef`/`RealPropDef`/`StringPropDef`** validate the owner
  keyword (`objType`) against uppercase LEF keywords (`"LAYER"`, `"VIA"`,
  ...) and reject anything else — but `lefiProp::propType()` (the read
  side, set by `lef.y`'s own `PROPERTYDEFINITIONS` grammar) always returns
  it **lowercase** (`"layer"`, `"via"`, ...). Worked around with a local
  `to_upper()` helper before calling the PropDef writers — a real
  reader/writer case mismatch within the same library, not something a
  caller should need to patch.
- **`lefwRealProperty`/`lefwIntProperty`** are missing
  `LEFW_LAYERROUTING`/`LEFW_LAYERROUTING_START` from their accepted-state
  list (`lefwStringProperty` has it). Numeric `PROPERTY` values are
  therefore writable on CUT layers but not ROUTING layers — string
  properties work on both. Worked around by passing `include_numeric =
  false` at the ROUTING-layer call site.
- The three generic property writers (`lefwStringProperty`/
  `lefwRealProperty`/`lefwIntProperty`) never accept `LEFW_SITE`,
  `LEFW_NONDEFAULTRULE(_START)`, or `LEFW_VIARULEGEN(_START)` at all.
  `PROPERTY` on SITE, NONDEFAULTRULE, or a `GENERATE` VIARULE is fully
  readable but never writable via this API — the states simply aren't in
  any of the three functions' accepted lists.
- **`lefwLayerSpacingCenterToCenter`** (the ROUTING-layer `CENTERTOCENTER`
  writer) is documented as "obsoleted in 5.7" with no replacement — only
  `lefwLayerCutSpacingCenterToCenter` (CUT layers) still exists. A
  ROUTING `SpacingRule.center_to_center` is read-only.
- Seven antenna "SideArea"-family writers —
  `lefwLayerAntennaSideAreaRatio`, `DiffSideAreaRatio`(+`Pwl`),
  `CumSideAreaRatio`, `CumDiffSideAreaRatio`(+`Pwl`), `SideAreaFactor` —
  all check `!lefwIsRouting` and reject CUT layers outright
  (`LEFW_BAD_DATA`), unlike `AntennaModel`/`AreaRatio`/`DiffAreaRatio`/
  `CumAreaRatio`/`AreaFactor`, which all accept `!lefwIsRouting &&
  !lefwIsCut` (both layer kinds). Some of these functions' own doc
  comments in `lefwWriter.hpp` even claim "valid ... if the layer type is
  either ROUTING or CUT," contradicting their actual `.cpp`
  implementation. `lefiAntennaModel` has no such restriction on the read
  side. `AntennaModel.side_area_*` fields are read-only on CUT layers.
- **`lefwLayerArraySpacing`** requires `lefwIsCut` and rejects any
  ROUTING-typed layer — but `complete.5.8.lef`'s own `LAYER cut24` (`TYPE
  ROUTING`, despite the name) legitimately uses `ARRAYSPACING`. Unwritable
  for a ROUTING-typed layer; `Layer.array_cuts`/`array_spacing` are
  read-only there.
- **`lefwLayerRoutingPitchXYDistance`**, **`DiagPitch`**,
  **`DiagPitchXYDistance`**, **`OffsetXYDistance`**, and
  **`lefwLayerRoutingStartSpacingtableTwoWidths`** all require
  `lefwIsRouting` and reject CUT layers — but `complete.5.8.lef`'s own
  `LAYER CUT01` (`TYPE CUT`) legitimately uses two-value `PITCH`/`OFFSET`
  and `DIAGPITCH`. Unwritable for a CUT layer; `Layer.pitch_xy`/
  `offset_xy`/`diag_pitch(_xy)`/`diag_spacing`/`diag_width` are read-only
  there.
- **`lefwLayerACCurrentDensity`/`DCCurrentDensity`** dispatch on `if
  (value)`: a real plain-scalar value of exactly `0.0` would be
  misinterpreted as "open table form" (with no closing `TableEntries`
  call ever made), rather than written as `ACCURRENTDENSITY <type> 0 ;`.
  Not hit by any value in `complete.5.8.lef`; not worked around.
- **`lefwMacroPinPortLayer`/`lefwMacroObsLayer`** (`SPACING`) and
  **`lefwMacroPinPortDesignRuleWidth`/`lefwMacroObsDesignRuleWidth`**
  (`DESIGNRULEWIDTH`) both gate on a bare `if (spacing)`/`if (width)` — a
  real value of exactly `0.0` is written as if it were never passed at
  all. Unlike the AC/DC CURRENTDENSITY entry below, this one *is* hit by
  `complete.5.8.lef` (`LAYER a1sig DESIGNRULEWIDTH 0`/an OBS `SPACING 0`),
  and it's the same reason a real `Shape.spacing`/`design_rule_width` of
  `0` (UPDATES.md item 12 — the router falls back to the LAYER
  definition's own rules only when genuinely *unset*, not when it's `0`)
  now round-trips correctly as far as the in-memory database goes
  (`is_optional`, no longer a 0-means-unset sentinel) but still can't
  reach the written file when the real value happens to be `0`.
  **`lefwMacroForeignStr`**'s point has the identical `if (xl || yl)` gate
  (see `lefwViaForeignStr`/`lefwMacroForeignStr`'s own doc comments,
  "optional(0)") — `complete.5.8.lef`'s `FWHSQCN690V15` has a real
  `FOREIGN FWHSQCN690 0.00 0.00 ;`, indistinguishable at write time from
  no point at all. All three: not called with a value that would trigger
  the bug (0.0 is passed straight through, since that's what "omit" also
  looks like to the caller); `Shape.spacing`/`design_rule_width` and
  `Foreign.origin` are correctly optional in the database, but a literal
  `0`/`(0,0)` is unwritable through these four vendored functions.
- **`lefwLayerRoutingSpacingtableTwoWidthsWidth`** checks `if
  (runLength)` to decide whether to emit `PRL ...` at all — a real PRL of
  exactly `0.0` (present in `complete.5.8.lef`'s own `WIDTH 0.25 PRL 0.0
  ...`) is indistinguishable from "no PRL" and gets silently dropped.
- **`lefwNonDefaultRuleLayer`** has no `diag_width` parameter at all
  (confirmed against `lefwWriter.hpp` — only width/minSpacing/
  wireExtension/resistance/capacitance/edgeCap) — a NONDEFAULTRULE LAYER's
  `DIAGWIDTH` is readable but has nowhere to go on write.
- **`lefwNonDefaultRuleLayer`** also writes `WIDTH`/`SPACING` *unconditionally*
  (`fprintf(..., "WIDTH %.11g ;\n", width)`/`"SPACING %.11g ;\n"`, no `if`
  guard at all - unlike every other numeric parameter in the same
  function, which all check truthiness first) - a rule whose LAYER only
  ever specified `WIDTH` (`complete.5.8.lef`'s own `clock`/`clock1`/
  `clock2`/`wide1_5x`/`wide3x`) gets a spurious `SPACING 0 ;` it never
  asked for. `NonDefaultRuleLayer.spacing` stays correctly `is_optional`
  in the database (the reader only sets it when `hasSpacing()` is real),
  but there's no way to tell this writer function "omit SPACING
  entirely" - passing `0.0` for "unset" and a real `0.0` look identical to
  it either way.
- **`lefwMacroExceptPGNet`** only accepts `!lefwIsMacroObs` — it cannot be
  called from a PIN PORT context at all, even though `lef.y`'s own
  `layer_exceptpgnet` grammar rule is shared by both PORT and OBS geometry
  (`complete.5.8.lef`'s own OBS-only usage happens to never exercise the
  PORT side). It also guards on an internal `lefwSpacingVal` flag that's
  reset only once per OBS section (via `lefwStartMacroObs`/
  `lefwStartMacroPinPort`), not per LAYER — once any LAYER-with-SPACING has
  been written anywhere earlier in that OBS section, `EXCEPTPGNET` is
  permanently blocked for the rest of it. `Shape.except_pg_net` is
  writable on OBS only, and callers must avoid mixing a SPACING-layer
  before an EXCEPTPGNET-layer in the same OBS section.

## Naming quirk (not a functional bug)

- **`lefwLayerRoutineEndSpacingtable`** — the header really does spell it
  "Routine", not "Routing". It's the only call that resets `lefwState`
  from `LEFW_LAYERROUTINGWIDTH` back to `LEFW_LAYERROUTING` after a
  `SPACINGTABLE` (every other layer-routing writer function rejects
  `LEFW_LAYERROUTINGWIDTH` outright) — skipping it silently breaks every
  statement written after a `SPACINGTABLE`. Confirmed against the
  vendored sample driver `src/lefdef/lef/lefwrite/lefwrite.cpp`, the only
  other place this misspelled name turns up.

## Reader-side: intentional version-obsolescence

Not a bug — the vendored *reader*'s own grammar (`lef.y`) deliberately
discards these statements at LEF `versionNum >= 5.4`, emitting an
"obsolete ... will ignore this statement" warning instead of ever calling
the corresponding `lefiPin::set*()`. Every writer function for these
*does* exist and work (confirmed) - the block is entirely on the read
side, and it's unconditional on version, not on whether a real value was
present. This project only reads/writes LEF `>= 5.4` (`read_lef` rejects
anything older - see `LEFReaderErrors.VersionBelow5_4IsAnError`), so a
caller can never observe `hasPower()`/etc. return true for any file this
project would accept at all - confirmed by reading `lef.y` directly
(search each field's own `K_*` token), not inferred from behavior alone;
a naive test (checking `hasPower()` after reading a real `POWER 2.0 ;`
PIN statement, expecting it to round-trip) is what surfaced this, after
first suspecting a bug in this project's own reader/writer code.

- **`POWER`/`LEAKAGE`/`CAPACITANCE`/`RESISTANCE`/`PULLDOWNRES`/`TIEOFFR`/
  `VHI`/`VLO`/`RISEVOLTAGETHRESHOLD`/`FALLVOLTAGETHRESHOLD`/`RISETHRESH`/
  `FALLTHRESH`/`RISESATCUR`/`FALLSATCUR`/`CURRENTSOURCE`** (PIN-scoped)
  all gate on `lefData->versionNum < 5.4` in `lef.y`. Not modeled in
  `schema.py` at all (removed after initially being added and fully
  wired up, both directions - real, correct plumbing, it just could never
  observe a value) - there's no point carrying fields that can never be
  populated for any file this project will ever accept.
- **`INPUTNOISEMARGIN`/`OUTPUTNOISEMARGIN`/`OUTPUTRESISTANCE`/`IV_TABLES`**
  (PIN-scoped) have the exact same `versionNum < 5.4` gate - never added
  to `schema.py` in the first place, same reasoning.
- The one genuine exception in this whole PIN-electrical-fields family:
  **`MAXDELAY`**/**`MAXLOAD`** have *no* version gate in `lef.y` at
  all - always read, any version - which is exactly why
  `Terminal.max_load`/`max_delay` are kept, genuinely readable, just
  read-only for writing (no `lefwMacroPinMaxdelay`/`*Maxload` function
  exists at all, a plain missing-API gap, not a version gate).
