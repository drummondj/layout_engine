# TCL support exploration

Design-decision document and build log for UPDATES.md item 15. Started as
research only (round 1, before any code existed) to answer item 15's two
flagged open problems and correct two assumptions in the original request;
round 2 locked in the architecture decisions; the "Phased roadmap" section
below has since tracked real implementation through all 7 phases —
Terminal/TerminalPort/Obstruction/Shape CRUD + filter-search, wrapped into
Tcl via SWIG, plus a batch/interactive shell entry point. `show_gui` (real
Flutter GUI integration) is the one deliberately deferred piece — see
Phase 6's own entry for why.

## What item 15 asked for

- TCL commands to create/get/update/delete Terminal, Obstruction, Boundary
  objects and their shapes (rect/poly/path), e.g. `create_terminal`,
  `create_terminal_port -shapes {...} -layer M4`, `get_terminal -filter
{...}`, `update_terminal_port`, `delete_terminal`.
- A batch mode: run a TCL shell from the terminal, then a command like
  `show_gui` opens the Flutter GUI on the already-loaded database.
- Flagged open problems: (1) how to connect the GUI to the Root database
  already read in batch mode, (2) how to refresh the GUI when TCL commands
  are entered in the shell.
- Suggested SWIG to wrap the C API into TCL, and noted Shape objects "may
  need to be added to a pool... because they can [have] multiple parent
  types."
- Suggested "cmg could be used to generate the C API."

## Findings (round 1)

### Current C API is read/interact-only

`src/api/api.hpp`/`api.cpp` is a pure-C `extern "C"` API around an opaque
`LeHandle*` — one `Root` + `ViewLayerSet` + `Scene` + `Pipeline` +
`Renderer` + `std::mutex` per handle, created via `le_create()`/
`le_destroy()`. Naming is `le_<verb>_<noun>`; ids are `{index, generation}`
structs; errors go through a polled message queue
(`le_message_count`/`le_message_at`), not exceptions or a single
last-error. Today it covers LEF loading, library/design/layer browsing,
viewport control (zoom/pan/fit), mouse-driven selection, and rendering to a
pixel buffer.

**There is no create/update/delete for Terminal, Obstruction, Boundary, or
Shape anywhere in this API.** Reading a selected object's data goes through
generic name/type/value property rows (`LeProperty`), not per-field
getters, and there is no mutation path at all today. The whole CRUD surface
item 15 wants would be new work.

### cmg cannot generate a C API or SWIG bindings today — but will be

enhanced to generate generic C++ CRUD/search (round 2 decision)

`cmg` (`/Volumes/Docking/Projects/synthosilicon/cmg`) has exactly two export
styles, `SMART_POINTERS` and `INDEXED_POOLS` (this repo uses
`INDEXED_POOLS`) — both are pure C++ database codegen: `XxxData` struct,
`XxxId` handle, `Pool<XxxData, XxxId>`, and a `Root` with
`create_x`/`get_x`/`get_x_ids`/`clear_x`/`get_x_size`/`for_each_x_id`. As of
round 1, there was no C-API-layer generator and no SWIG/binding generator,
and no generated `delete_x`/`update_x`.

**Round 2 decision: cmg is enhanced to generate `update_x`/`delete_x` plus
filter-expression search, at the C++ `Root` layer only** (superseded by
round 3 below: `update_x` turned out to be the wrong shape and was
replaced by narrower `set_x_<field>` setters — see Phase 1's "DONE" entry
in the roadmap for the actual outcome) — not a flat C API and not SWIG
bindings. `backend/src/api/` keeps hand-writing thin
per-class `extern "C"` wrappers over whatever cmg generates on `Root`,
same as it does today for `create_x`/`get_x`. This keeps one deliberate
boundary: cmg only ever emits C++ against the schema; deciding what's
exposed to TCL/Dart stays a hand-written, reviewed decision in `api.cpp`,
not an automatic consequence of a class being pooled. See "cmg codegen
design" below for what this actually requires in cmg's generator.

### Shape needs to become a real pooled class; Boundary stays a field

(round 2 decision)

`Shape` (`schema.py`, `has_pool=False`) is an embedded-by-value struct
duplicated inside both `TerminalPort.shapes` and `Obstruction.shapes` — it
has no stable id today. This confirms item 15's own observation: to
`get`/`update`/`delete` an individual shape via TCL, `Shape` needs to
become a real pooled `Klass` (its own `ShapeId`, `has_pool=True`)
referenced by id from both parent types, rather than embedded by value.
This is a schema change (`schema.py` + `regen-database`), not a small
tweak — it touches every place `TerminalPort`/`Obstruction` shapes are read
today (pipeline, geometry, tests).

**Round 2 decision: `Boundary` stays a field, not a Klass.**
`Abstract.boundary` remains `list[Polygon]`; TCL exposes it via
`update_abstract -boundary {x y x y ...}` rather than standalone
`create_boundary`/`get_boundary`/`delete_boundary` commands — an Abstract
always has at most one boundary, so a dedicated pooled class and CRUD
surface would be pure overhead. This also settles item 14's separate
"make the boundary layer selectable" note without needing a new Klass
there either.

### No existing TCL/SWIG code — greenfield

Neither this repo nor the `cmg` checkout has any `.i` SWIG files, `Tcl_*`
calls, or scripting/REPL code. The vendored LEF/DEF parser Makefiles have
no Tcl dependency either. This is a clean start.

### Closest existing precedent: `render_preview.cpp`

`src/pipeline/benchmarks/render_preview.cpp` is the only place in the repo
that constructs a `Root`/`Scene`/`Pipeline`/`Renderer` directly from the
command line without the Flutter plugin. It's a throwaway, non-interactive
batch tool (not a REPL, not wired into `ctest`), but it's the right
reference for how a TCL batch-mode entry point would load a `Root` from
LEF files at startup.

## External research: SWIG + Tcl, and OpenROAD as prior art

SWIG's Tcl backend is actively maintained — fixes as recent as mid-2025
(`-external-runtime` header generation, Tcl 8.x compatibility). It compiles
a `%module` interface file into a shared library loadable via `load` (or
linkable into a custom `Tcl_AppInit`), wrapping global C functions as new
Tcl commands automatically — a natural fit for `api.hpp`'s existing
`le_<verb>_<noun>` flat function surface. **Round 2 decision: SWIG needs
custom typemaps for coordinate arrays** (shape rect/poly/path coordinates,
`-boundary {x y x y ...}`) rather than relying on SWIG's default array
handling — these are exactly the kind of hand-tuned typemap SWIG expects
users to write for anything beyond scalars/strings.

**OpenROAD** is close, well-established prior art for exactly this shape of
problem: a C++ EDA core with a single shared database singleton, wrapped by
SWIG into Tcl commands, with a Qt GUI that is just another consumer of the
same singleton (not a separate process). It exposes a `gui_start`-style Tcl
command that opens the GUI on whatever is already loaded — this directly
validates item 15's `show_gui` idea and its batch-mode-then-GUI premise.

Tcl's own threading model (one interpreter per thread, event loop via
`Tcl_DoOneEvent`) is documented as interleavable with another toolkit's
event loop (see prior art on Tcl+Qt integration) — relevant only if the GUI
ever needs its own event loop running concurrently with the Tcl shell's
stdin loop; the single-process model below avoids needing this initially.

## Recommended direction

- **Open problem 1 (connect GUI to batch-mode Root)**: adopt the OpenROAD
  model — one process, one `LeHandle`-equivalent piece of state shared
  between the Tcl interpreter and the GUI. There is no "batch process"
  handing off to a "GUI process" to bridge; `show_gui` just starts
  rendering from the state that's already there.
- **Open problem 2 (refresh GUI on Tcl input)**: no push/observer mechanism
  needed initially. The GUI already re-renders from live `Root`/`Scene`
  state on every Flutter texture refresh (poll-based, not event-pushed) —
  Tcl commands mutating the same `Root` "just work" the next time Flutter
  asks for a frame. Revisit only if a benchmark shows staleness/latency is
  a real problem (matches this repo's own "benchmark first" rule).
- **SWIG should wrap the same `api.hpp` C functions Dart FFI already
  binds to**, not a second parallel API. One C surface, two consumers.
  This also means new CRUD functions land in `src/api/` following existing
  conventions (opaque handle, `le_` naming, message-queue errors,
  null-check-and-degrade), not a new module.
- **The natural first build step** is a minimal SWIG toolchain spike
  (see Phase 0 below) before committing to the full CRUD/schema/cmg work.

## cmg codegen design (round 2)

Investigated `cmg`'s generator in depth to turn "cmg is enhanced" from an
aspiration into a concrete plan:

- **cmg emits via Jinja2** (`cmg/generator.py`; templates under
  `cmg/templates/indexed_pools/*_j2.py`, Python modules holding template
  strings rendered against `Schema`/`Klass`/`Field` objects). Adding
  `update_x`/`delete_x` is a direct extension of the existing
  `root_hpp_j2.py` loop that already emits `create_x`/`get_x` per pooled
  class — same abstraction level as existing code, no new machinery.
- **`Field`/`Klass` metadata is relationship-aware but not
  predicate-aware**: it already knows parent vs. child, list vs. scalar,
  and which Klass a reference/child points to (this is what today's
  single-hop `index=True` lookup-map generation and parent→children list
  generation already use). It has no concept of multi-hop paths,
  comparison operators, or predicate composition — new helper methods on
  `Field`/`Klass` in `cmg/schema.py`, following the existing pattern of
  `get_ordered_fields()`/`is_reference()`/`get_cpp_type()`.
- **Parent links are already O(1)**: a child struct stores its parent's id
  directly as a field (e.g. `TerminalPortData::terminal` is `TerminalId`,
  `ObstructionData::abstract` is `AbstractId`), so `.terminal.name` from a
  `TerminalPort` resolves via one direct `Pool` lookup, no search needed.
  Note: `Root::index_` also declares reverse/child→parent maps, but
  they're dead code — never populated by any generated `create_x` — so
  they should not be relied on or extended for this; the stored-id-on-child
  field is the real, reliable mechanism.
- **Nuance found**: `TerminalPort` and `Obstruction` have **no scalar
  fields of their own** — the only filterable content (`layer_name`) lives
  inside their nested `shapes: List[Shape]` field. So item 15's own
  example filter, `-filter {.terminal.name =~ IN* && .layer_name == M4}`
  against `TerminalPort`, actually mixes two different traversal kinds: a
  single-object parent walk (`.terminal.name`) and a one-to-many "does any
  list element match" walk (`.layer_name`, really `.shapes[*].layer_name`).
  Item 15 itself says its example isn't an exact spec, so this is a design
  input, not a contradiction to resolve with the user.

### Filter-expression architecture

Filter strings are typed by a TCL user at runtime; cmg only runs at
schema-build time, so it cannot pre-compile arbitrary filter expressions —
it can only generate the **metadata a runtime evaluator needs**:

- **Generated by cmg** (mechanical, non-domain-specific, lands in
  `Root`/generated headers): per-class field-accessor metadata (name, C++
  type tag, scalar vs. list, and — for reference/child-list fields — which
  Klass/field the hop lands on); `Root::delete_x`/`set_x_<field>`; a
  generated `Root::search_x` entry point running a predicate over
  `for_each_x_id`.
- **Hand-written once, shared** (not generated — one implementation serves
  every class because it only depends on the generated metadata): the
  filter-expression parser (`.path.expr op value (&& / || ...)` → small
  AST) and its evaluator, including path resolution with an **explicit-hop
  grammar** — `.shapes.layer_name`, not an implicit/magic `.layer_name` —
  so both traversal kinds above are handled by one general, N-hop-capable
  resolver with no per-class special-casing: parent/reference-scalar hops
  walk directly, list hops are implicitly existential (any element
  satisfying the rest of the path makes the predicate true). Fully general
  at any hop depth for free, not hardcoded to today's 1–2 hop examples.
- **Performance**: linear scan (`for_each_x_id` + native predicate
  evaluation, no callbacks into Tcl per object) first, matching this
  repo's benchmark-first rule — no index-fast-path optimization (reusing
  `index=True` equality maps for exact-match sub-predicates) until a
  benchmark shows scan cost actually matters at realistic object counts.
  A dedicated benchmark, mirroring
  `src/pipeline/benchmarks/stress_data.hpp`'s millions-of-shapes generator
  and `pipeline_benchmark.cpp`'s pattern, is part of validating this
  feature, not a prerequisite to building it.

Comparison operator set: `==`, `!=`, `<`, `>`, `<=`, `>=`, `=~` (glob),
`&&`, `||` — covers item 15's example plus the obvious numeric/enum
comparisons its scalar fields need. Decided here as a low-stakes,
adjustable default rather than something worth blocking on.

## Tcl ergonomics layer (round 3)

Building the Phase 0 spike surfaced a gap: a straight SWIG wrap of
`api.hpp` produces `le_<verb>_<noun>` Tcl commands that all take an
explicit handle as their first argument (`le_read_lef $handle $path`) —
correct for what it wraps (`api.hpp` is deliberately handle-per-call, for
Dart FFI, where Flutter owns and threads the handle itself), but not what
item 15 actually shows: `read_lef <filename>`, `create_terminal -name IN0
-direction IN`, `get_terminal -filter {...}` — no visible handle
(an implicit "current session", OpenROAD-singleton-style), domain-verb
command names, and `-flag value` arguments. SWIG doesn't produce any of
that on its own; it's a deliberate layer that has to be built, separate
from the typemap work in Phase 5 below.

**Decision: the session and each command's logic live in a thin C++ shim
between `api.hpp` and the SWIG-wrapped surface** (`src/tcl/le_tcl_shim.hpp`/
`.cpp`, proved out in Phase 0): a process-global `LeHandle*`, lazily
created, that a set of bare, domain-named functions (`read_lef`,
`design_count`, ...) close over and call into `api.hpp` through — SWIG
wraps _this_ shim, not `api.hpp` directly. This keeps session/business
logic in C++ (reviewable, testable, consistent with how the rest of this
backend is built) rather than in hand-written Tcl.

One consequence of that choice worth calling out explicitly rather than
leaving implicit: SWIG-wrapped C++ functions are always **positional** —
Tcl's own `-flag value` calling convention isn't something SWIG generates
for you, it has to be parsed in Tcl itself before a positional call is
made. So any command item 15 shows with flags (`create_terminal -name ...
-direction ...`, `get_terminal -filter ...`) is exposed by the C++ shim
under an internal `*_cmd` name taking plain positional arguments (e.g.
`set_viewport_size_cmd(width, height)`), and a matching thin Tcl proc
(`src/tcl/le_tcl_procs.tcl`) does the `-flag` parsing and then calls the
`*_cmd` form. This isn't extra hand-written business logic creeping into
Tcl — it's boilerplate flag-parsing only, mechanical per command, and
matches how other SWIG+Tcl EDA tools (OpenROAD included) split this same
concern.

Phase 0 was extended to prove this end to end using calls `api.hpp`
already supports (nothing CRUD-shaped exists yet to demonstrate on):
`read_lef`/`design_count`/`design_name`/`message_count`/`message_at` as
direct bare-named wraps, plus `set_viewport_size -width W -height H` as a
full `-flag`-parsing round trip (`set_viewport_size_cmd` → `le_set_viewport_size`,
verified by reading the resulting size back through
`le_render_pixel_buffer`). `src/tcl/tests/smoke_test.tcl` (`ctest`'s
`le_tcl_smoke`) exercises both, including that an unknown flag is
rejected. All future CRUD commands (Phase 4 onward) follow this same
shim-plus-procs shape rather than being wrapped ad hoc.

## Phased roadmap

1. **Phase 0 — SWIG toolchain spike, plus the Tcl ergonomics layer proof.
   DONE.** `src/tcl/le_tcl_shim.hpp`/`.cpp` — a hidden process-global
   `LeHandle*` behind bare, domain-named functions
   (`read_lef`/`design_count`/`design_name`/`message_count`/`message_at`,
   plus `set_viewport_size_cmd`/`viewport_width`/`viewport_height`)
   calling into `api.hpp`; `src/tcl/le_api.i` wraps the shim (not
   `api.hpp` directly) into a loadable Tcl extension via CMake's `UseSWIG`
   (target `le_tcl`, guarded on `find_package(SWIG)`/`find_package(TCL)`
   both succeeding, so a machine without them still configures/builds
   everything else), pinned to Homebrew's `tcl-tk@8` (Tcl 8.6 — SWIG
   4.5's Tcl support is battle-tested there; Tcl 9's ABI is new and not
   yet a safe default). `src/tcl/le_tcl_procs.tcl` supplies the
   `-flag`-parsing `set_viewport_size` proc over `set_viewport_size_cmd`
   (see "Tcl ergonomics layer" above for why that split exists).
   `src/tcl/tests/smoke_test.tcl` (`ctest`'s `le_tcl_smoke`) exercises the
   whole thing — no visible handle, domain verb names, a real `-flag`
   round trip, and unknown-flag rejection — against the same
   `testcell.lef` fixture `api_test.cpp` already uses. Verified: full
   build + `ctest` (441/441, including `le_tcl_smoke`) pass. Toolchain
   _and_ the representative command shape both confirmed — proceed to
   Phase 1.
2. **Phase 1 — cmg: generic CRUD + filter-search codegen. DONE.** All in
   the `cmg` repo (`cmg/templates/indexed_pools/`, `cmg/schema.py`),
   regenerated into this repo's `src/database/generated/` and validated
   against the real schema, not a toy example.
   - `root_hpp_j2.py`: `delete_x` added to the existing `create_x`/`get_x`
     per-class loop, cleaning up any parent/index-map entries that
     referenced the deleted id. **No whole-record `update_x`** — an
     earlier version of this had one (replace the entire `XxxData`
     wholesale), but that was a mirror-`create_x`-for-symmetry default,
     not a considered choice, and it was wrong for two reasons: (1) most
     fields aren't indexed or parent-linked, and for those, direct
     mutation through the pointer `get_x()` already returns is exactly
     right — no update function needed at all, and always has worked this
     way; (2) for the fields that _are_ indexed/parent-linked, item 15's
     own TCL examples are single-field updates
     (`update_terminal_port -name ...`), and whole-record replace would
     force a read-modify-write on every one of them. Replaced with a
     narrow **`set_x_<field>(id, value)` generated per indexed or
     parent-linked field only** (e.g. `set_layer_name`,
     `set_layer_technology`) — each does just that field's index fixup
     (remove the stale entry, mutate, add the fresh one; a same-value set
     is a no-op) and maps directly to a single `-flag value` TCL command
     with no read-modify-write. Every other field keeps using direct
     `get_x()` pointer mutation, unchanged.
     `search_x(predicate)` added alongside `for_each_x_id` — a linear
     scan collecting every id whose data satisfies a generic
     `predicate(root, id, data)` callable; Phase 2's evaluator plugs into
     this, cmg doesn't need to know anything about filter-expression
     syntax to generate it.
   - `schema.py`: two new `Klass` helpers, `get_filterable_scalar_fields()`
     (leaf fields — struct fields minus parent back-references, lists,
     and non-enum references) and `get_filterable_hop_fields()`
     (relational fields — any reference to a non-enum class, sourced from
     the full field list since `is_child` list fields are never struct
     members).
   - `struct_hpp_j2.py`: per class (pooled and non-pooled alike — a
     non-pooled class like `Shape` can still be a hop _target_), a
     generated `get_x_field(data, name)` (reuses the exact same
     `wrap_with_to_property()` helper `to_properties()` already uses — no
     new value type invented, just a by-name lookup into the same
     per-field logic) and `match_x_hop(root, id, data, hop, matcher)` (a
     `Matcher` template parameter, since which concrete type it's invoked
     with depends on which of the five structural hop shapes matches:
     parent-scalar, child-scalar, child-list, plain-list, plain-scalar —
     see `get_filterable_hop_fields()`'s branches). Non-pooled classes get
     a simpler signature with no `root`/`id` parameters, since they can
     never have parent/child relationships (only pooled classes have ids
     to be a parent/child _of_).

   **Real finding from building this, not just designing it**: because
   `hop` is a runtime `string_view`, `match_x_hop`'s branches are ordinary
   runtime `if`s, not `if constexpr` — so for a given `Matcher` type,
   _every_ branch's body gets instantiated together, not just whichever
   one a particular call happens to take. A class with many hop fields
   (`Layer` has over a dozen) requires its `Matcher` to compile against
   every one of those target types at once. This isn't a flaw — it's
   exactly the shape Phase 2's evaluator already needed (a fully generic,
   name-dispatching matcher, not a type-specific one) — but it was caught
   empirically, by writing real tests with type-specific lambdas and
   watching them fail to compile against unrelated hop targets, not by
   reasoning about it in advance. Fixed by using
   `if constexpr (requires {...})`-guarded generic matchers in the tests
   (see `database_test.cpp`'s `FilterMetadata` suite) — the same pattern
   Phase 2's real evaluator will use structurally (dispatch by calling
   `get_x_field`/`match_x_hop`, which exist for every generated type, not
   by knowing concrete types up front). Also caught and fixed by this same
   testing pass: `match_x_hop`'s plain-scalar-reference branch didn't
   unwrap `is_optional` fields (e.g. `Layer.pitch_xy: optional<Point>`),
   passing the wrapper itself to `matcher` instead of the `Point`.

   A second, pre-existing bug surfaced while building the `set_x_<field>`
   setters above: `generator.py` renders `root.hpp` (and every other
   `HEADER_ONLY_CLASSES` template) via `.render(schema=schema)`, never
   passing `export_style` — so any `field.get_cpp_type(export_style=export_style)`
   call inside `root.hpp`'s own template silently receives Jinja's
   `Undefined` sentinel, which compares `False` against every real
   `ExportStyle` value, so `get_cpp_type()` falls through every
   style-specific branch to its bare-type-name default. This already
   existed (the pre-existing `get_x_by_field` accessor uses the same
   call) but was invisible: every `index=True` field so far happened to
   be a plain scalar (a `str`, mainly `name`), for which the fallen-through
   default and the correctly-styled answer are identical. The first
   parent-linked setter (`set_layer_technology`) was also the first
   `root.hpp`-generated code to need a _reference_ field's type from
   inside `root.hpp`, which is exactly where the two diverge — it
   generated `bool set_layer_technology(LayerId id, Technology value)`,
   referencing a type (`Technology`, no `Data` suffix, no `Id` type) that
   doesn't exist, a compile error. Fixed by passing `export_style` into
   that `.render()` call. Both bugs were only found because real,
   instantiated tests were written against the actual schema, not toy
   examples — compilation success alone (which every intermediate step
   here did achieve) didn't catch either one.

   `FilterMetadata.*` in `database_test.cpp` (8 tests) exercises all five
   hop-branch shapes plus `search_x`; `Database.SetLayer*`/`Delete*` (6
   tests) exercises both `set_x_<field>` kinds (by-value index, parent
   index) and `delete_x`, using `Layer`/`Technology` (parent-scalar,
   child-list), `Design` (child-scalar, the one shape nothing else
   reached), and `Shape`/`Obstruction` (plain-list, non-pooled hop target)
   — chosen to cover every structural shape at least once, not
   arbitrarily.

   **Two more refinements, made while actually building Phase 2 below
   (not anticipated up front):**
   - `get_{{klass}}_field`/`match_{{klass}}_hop` were **renamed to bare
     `get_field`/`match_hop`, overloaded once per generated class**
     (same name everywhere, distinguished by argument type) rather than
     per-class-suffixed names. A hand-written recursive evaluator can't
     call a per-class-suffixed function without already knowing the
     concrete class — the entire point of "generic" here is that it
     shouldn't need to. Overloading was the missing piece; nothing else
     about the design changed.
   - `match_hop`'s pooled-target branches now call
     `matcher(target_id, *target)` (id **and** data), not just
     `matcher(*target)`. The id was always in scope (it's either the
     field's own stored value or a loop/lookup variable) but wasn't
     being passed on — which meant a chained hop (`.abstract.design.name`,
     two hops deep) had no way to continue past the first, since nothing
     downstream would ever have an id to call `match_hop` with again.
     Every generated `match_hop` now has two call shapes depending on
     whether the resolved target is pooled (`matcher(id, data)`) or an
     embedded value (`matcher(data)`) — documented in the function's own
     doc comment. `database_test.cpp` gained a dedicated
     `MatchHopChainsTwoLevelsDeep...` test proving a real 2-hop chain
     (`Obstruction.abstract` → `Abstract.design`) now resolves, which
     wasn't possible before this fix.

   Verified: full build + `ctest` (455/455) pass after both refinements.

3. **Phase 2 — shared filter-expression engine. DONE.** `backend/src/database/filter.hpp`
   (header-only, matching `database`/`geometry`'s own convention) — a
   hand-rolled recursive-descent parser (`parse_filter_expression`,
   returning `std::expected<FilterExpr, std::string>` rather than
   throwing, matching this project's "no exceptions for expected-missing-
   data paths" convention — a malformed `-filter {...}` string is
   user-input-shaped, not a bug) plus a generic `evaluate_filter(expr,
root, id, data)` walking `FilterExpr`'s path segments through the
   Phase 1 `match_hop`/`get_field` functions. Grammar (matches item 15's
   own examples exactly):
   `expr := or_expr; or_expr := and_expr ('||' and_expr)*; and_expr :=
unary ('&&' unary)*; unary := '(' expr ')' | comparison; comparison :=
path op literal` — `&&` binds tighter than `||`, parens override it.
   Operators: `== != < <= > >= =~` (glob, Tcl `string match` semantics:
   `*`/`?`, hand-rolled, no `<regex>`). A literal's type isn't decided at
   parse time — it's kept as raw text and compared against whatever
   `PropertyValue::Type` the matched leaf field actually has at
   evaluation time (`std::from_chars`-based numeric parsing; a
   non-numeric literal against a numeric field just never matches, same
   degrade-gracefully convention as everywhere else). `evaluate_filter`
   is two overloads of an internal `walk()` (one takes an id, for a
   pooled current object; one doesn't, for a non-pooled one), each
   recursing through `match_hop`'s generic `matcher(id, data)`/`matcher(data)`
   continuation until the last path segment, then comparing via
   `get_field`. `src/database/tests/filter_test.cpp` (13 tests) covers:
   string/numeric leaves, glob, `&&`/`||`/parens precedence, a single
   parent hop, a single existential-list hop, a real 2-hop chain
   (`.abstract.design.name`), item 15's own example expression verbatim
   (`.terminal.name =~ IN* && .shapes.layer_name == M4`), integration
   with `Root::search_x`, parse-error reporting, quoted literals, and
   unknown field/hop names degrading to "no match" rather than erroring.
   Verified: full build + `ctest` (468/468) pass.
4. **Phase 3 — `Shape` pooling. Deferred in round 5 (see below), then
   DONE in round 6** once a real need appeared: updating one existing
   shape (including its layer) attached to a TerminalPort/Obstruction, by
   a stable id independent of its parent - exactly the case round 5
   flagged as the actual bar for doing this. `schema.py`: `Shape` is now
   `has_pool=True` with two parent-link fields (`terminal_port`,
   `obstruction`, mutually exclusive - a given Shape sets at most one);
   `TerminalPort.shapes`/`Obstruction.shapes` became `is_child=True`.
   Regenerated via `regen-database`.

   **Real architectural collision found while migrating, fixed cleanly**:
   `Geometry`/`Pipeline` use `Shape` as a freestanding value type
   (Pipeline synthesizes many ephemeral shapes per render call via RECT/
   PATH/POLYGON ITERATE expansion, never persisted to `Root` at all) -
   pooling renamed the generated struct to `ShapeData`, breaking every
   `Shape`-typed signature in `geometry.hpp` (`Geometry::bbox`,
   `merge_overlapping_fills`, ...) and `pipeline.hpp`
   (`RenderedShape::shape`). Fixed with one line in `database.hpp`:
   `using Shape = ShapeData;` - the two names are the exact same type,
   so every existing signature keeps compiling unchanged; only the ~9
   files that actually read/wrote `.shapes` as a field (`lef_reader.cpp`,
   `lef_writer.cpp`, `pipeline.hpp`'s two shape-collecting loops,
   `api.cpp`, and every test that constructed a `TerminalPortData`/
   `ObstructionData` directly) needed real changes, to
   `Root::get_terminal_port_shapes(id)`/`get_obstruction_shapes(id)` +
   `Root::get_shape(id)` instead of direct field access.

   **cmg bug found and fixed alongside**: `create_x`'s parent-index
   population wrote an entry even when a parent field was left at its
   default/invalid value - harmless for every existing single-parent
   field (always set by construction) but would have silently polluted
   `index_.obstruction_shapes[ObstructionId{}]` (and the reverse) for
   every shape, since exactly one of Shape's two parent fields is always
   unset. Fixed by guarding each write with `d.<field>.valid()` in
   `root_hpp_j2.py` - a strict improvement for every class, not just
   Shape's new dual-parent case.

   **Benchmarked, per this project's own rule, not assumed**:
   `BM_GenerateShapes` (1M shapes) went from 423 ms to 590 ms (+~40%,
   real and reproducible, not noise - cv 1.52% over 5 reps) -
   `generate_shapes`'s two shape loops now do a `Root::get_shape(id)`
   pool lookup per shape instead of iterating an embedded vector
   directly. Accepted: this is the cost of a capability that didn't
   exist at all before (addressing one shape independently), and
   `generate_shapes`'s result is already cached per-`AbstractId` - paid
   once per structural Design change, not per frame/pan/zoom. Full
   writeup in `BENCHMARKS.md`'s 2026-08-10 entry, including a partly
   offsetting effect: `ObstructionData` no longer has a `shapes` field
   at all, so `to_properties(ObstructionData)` (a _different_ hot path,
   `api.cpp`'s `build_selected_object_properties`) can never again
   regress into the exact "deep-copies 900K embedded shapes" bug
   `BENCHMARKS.md`'s 2026-08-07 entry already fixed once - there's no
   `shapes` field left to accidentally copy.

   491 tests passing after the migration itself (before Phase 4's new
   shape API below added more).

5. **Phase 4 — hand-written C API in `backend/src/api/`. DONE.** Thin
   `le_*` wrappers over the Phase 1/2/3 `Root` methods for
   Terminal/TerminalPort/Obstruction/Shape, plus
   `le_update_abstract_boundary` (no separate Boundary object).

   **Terminal slice**: `le_create_terminal`, `le_terminal_property_count`/
   `_at` (by-id, not selection-scoped - reuses `to_properties()` plus a
   derived `port_count` row, same shape as the existing selection-scoped
   property table), `le_set_terminal_name`/`_direction` (`name` isn't
   indexed, so these are direct field mutations through `get_terminal()`,
   not a generated `Root::set_terminal_name`), `le_delete_terminal`
   (cascades to delete every owned `TerminalPort` first - a deliberate
   API-layer exception to `Root::delete_terminal`'s generic no-cascade
   default, since a `TerminalPort` is only ever reachable through its
   parent Terminal and would otherwise become permanently-unreachable
   garbage, not just a dangling reference that degrades gracefully),
   `le_search_terminal`/`le_search_result_terminal_at` (parses via
   `filter.hpp`, evaluates via `Root::search_terminal`, caches results on
   the handle - same "valid until the next call" convention as
   `le_selected_object_property_at`; a parse error returns -1 and pushes
   an `ERROR:` message, same channel `le_read_lef` already uses). New
   `LeTerminalId`/`LeTerminalPortId`/`LeObstructionId`/`LeSignalDirection`
   types in `api.hpp`. One real bug caught while writing the tests: the
   first draft's `LeTerminalId invalid{}` zero-initialized to `{0, 0}`,
   not this API's actual invalid sentinel (`{UINT32_MAX, 0}`, per
   `LeLibraryId`/`LeDesignId`'s own established convention) - a real id
   could have index 0, so this would have made a genuine failure look
   like success. 10 new `ApiFixture` tests in `api_test.cpp`.

   **TerminalPort/Obstruction slice**: `le_create_terminal_port`/
   `le_create_obstruction` create an empty parent only - no layer, no
   geometry (see the Shape slice's own round-7 redesign below for why).
   Property/delete/search functions follow the exact Terminal shape.
   `SearchTerminalPortFindsMatchesUsingUpdatesMdItem15SExampleExpression`
   runs item 15's exact two-part filter
   (`.terminal.name =~ IN* && .shapes.layer_name == M4`) against real
   created data and gets exactly the right match.

   **Abstract boundary**: `le_update_abstract_boundary` takes a flat
   `const double* coords_um` (x/y pairs, at least 3 points) and replaces
   `Abstract.boundary` wholesale with one polygon — direct field mutation
   through `get_abstract()`, not a generated setter (boundary isn't
   indexed/parent-linked). No getter exists yet to read it back through
   this API (only tested via return code, not a full round-trip) — a real
   gap if a future caller needs to verify a boundary it didn't just set
   itself; add `le_abstract_property_at` (mirroring the other classes'
   by-id property accessor) if/when that's needed.

   **Shape slice, redesigned in round 7** (asked directly: "what about
   paths, polygons and texts?" — the first pass only ever handled rects).
   Two real findings from that question, not just "add more coverage":
   - **Texts aren't a gap at all.** `Shape.texts` is never populated by
     `lef_reader.cpp` - it's only ever pushed onto the ephemeral,
     Pipeline-computed copy of a shape at render time
     (`pipeline.hpp`'s `generate_shapes`, for auto-placed terminal
     labels), never onto the stored `ShapeData`. LEF's PIN/OBS syntax has
     no `TEXT` geometry statement - there's nothing for a caller to
     author here, so texts are correctly excluded, not missing.
   - **Rects, polygons, and paths are equally real LEF geometry** -
     `lef_reader.cpp` parses all three from PIN/OBS the same way. Baking
     rects (and only rects) into `le_create_terminal_port`/
     `le_create_obstruction` was an asymmetry, not a simplification -
     user feedback on this exact point: "Either le*create*_ handles all
     shape members, or le*create*_ does not handle any shape members and
     they are added with separate calls... rects is not more important
     than polygons or paths." Chose the latter globally:
     - `le_create_terminal_port`/`le_create_obstruction` now take **no**
       layer/geometry parameters - just the parent id.
     - `le_create_terminal_port_shape`/`le_create_obstruction_shape`
       create an empty Shape (just `layer_name`) on a given parent.
     - Every geometry kind gets the _same_ shape thereafter: count/read
       - `le_add_shape_<kind>` + `le_remove_shape_<kind>` for
         rects (`le_add_shape_rect` takes 4 plain doubles - simpler than an
         array, since a rect is always exactly 4 numbers),
         polygons (`le_add_shape_polygon`, flat microns array, ≥3 points),
         and paths (`le_add_shape_path`, width + flat microns centerline,
         ≥2 points). `le_update_shape` (rects-only, replace-wholesale) is
         **removed** - replaced by `le_set_shape_layer_name` (renames only)
         plus the symmetric add/remove primitives.
     - Rects/polygons/paths are addressed by plain 0-based position
       within their own Shape's list, not a further stable id (unlike a
       Shape itself) - removing one shifts later ones down, which is
       fine here since nothing needs to hold a rect/polygon/path
       reference across other mutations the way a Shape's own id does
       across its parent's mutations.
   - **Real bug caught by this pass, unrelated to the redesign's own
     purpose**: `le_delete_terminal_port`/`le_delete_obstruction` never
     got updated after Phase 3 pooled `Shape` - they still deleted only
     the parent, leaving its shapes as permanently unreachable garbage
     (exactly the hazard `le_delete_terminal`'s own cascade-to-ports
     already exists to avoid, just not applied here yet). Fixed: both
     now cascade-delete their shapes first, same reasoning, and a
     dedicated test (`DeleteTerminalPortCascadesToItsShapesAndIs...`)
     confirms the shape is actually gone afterward, not just the parent.

   10 tests for the Terminal slice, plus 21 for
   TerminalPort/Obstruction/boundary/Shape (31 total for Phase 4).
   Verified: full build + `ctest` (500/500) pass.

6. **Phase 5 — SWIG interface + typemaps. DONE.** `le_tcl_shim.hpp`/`.cpp`
   and `le_api.i` now cover the full Phase 4 CRUD/search surface —
   Terminal, TerminalPort, Obstruction, Shape (rect/polygon/path), and
   Abstract boundary — following the same shim-plus-`*_cmd`-plus-
   `le_tcl_procs.tcl` shape Phase 0 proved out.
   - **Ids cross the shim packed into a plain `long long`, not as a
     wrapped C struct — for `AbstractId`/`DesignId`. Terminal/
     TerminalPort/Obstruction/Shape were later changed to type-prefixed
     friendly strings (see UPDATES.md's Terminal-friendly-id item,
     below the Phase 6 section) — this bullet describes the original,
     still-accurate-for-two-types convention.** Every `LeXxxId` in
     `api.hpp` is `{uint32_t
index, generation}` — wrapping each of the
     distinct id types (`LeAbstractId`/`LeTerminalId`/`LeTerminalPortId`/
     `LeObstructionId`/`LeShapeId`/...) would mean a custom SWIG struct
     typemap pair (in + out each) per type. Instead `le_tcl_shim.cpp` has
     one generic `pack<IdT>`/`unpack<IdT>` pair (`generation << 32 |
     index`) and `design_abstract_id`/`design_by_name`/
     `set_current_design_cmd`/`update_abstract_boundary_cmd`'s
     `abstract_id` argument still take/return `long long` — a fundamental
     type SWIG already marshals to/from a plain Tcl integer via `%include
<stdint.i>`, zero custom typemap code needed. `kInvalidId`
     (`0xFFFFFFFF`, _not_ `-1`) is a plain Tcl variable set in
     `le_tcl_procs.tcl`, matching what `pack()` produces for every
     api.hpp failure path (`index == UINT32_MAX`, `generation == 0`,
     never left uninitialized — see the "zero-init bug" note above) - now
     only meaningful for these two types, since Terminal/TerminalPort/
     Obstruction/Shape use an empty string `""` as their own invalid
     sentinel instead (`pack`/`unpack` are still used *internally* for
     TerminalPort/Obstruction/Shape's own numeric friendly-id suffix, just
     no longer surfaced to Tcl directly as a bare integer - see
     `le_tcl_shim.hpp`'s own "IDs" comment for the current, full
     contract).
   - **The one real typemap this phase exists for**: a `%typemap(in)`
     in `le_api.i` converting a Tcl list of doubles directly into the
     `(const double *points_um, int32_t point_coord_count)` pairs
     `add_shape_polygon_cmd`/`add_shape_path_cmd`/
     `update_abstract_boundary_cmd` all take, via a `std::vector<double>`
     declared as the typemap's own local variable (so it lives for the
     whole wrapper function, not just the typemap's own code block) and
     `Tcl_ListObjGetElements`/`Tcl_GetDoubleFromObj` for the actual
     conversion, `SWIG_fail` on either failing. Applied once via
     `%apply` to the one parameter-name pair every one of those
     functions was normalized to use (`api.hpp`'s own `coords_um`/
     `coord_count` boundary-specific wording was renamed to match in the
     shim) — a Tcl caller writes `add_shape_polygon -shape $s -points {0
0 10 0 10 10}`, never a pre-flattened C array.
   - **Property tables and search-result lists are built in Tcl, not
     C++.** `le_tcl_shim.cpp` only exposes plain `count`+by-index
     accessors (`terminal_property_count`/`_name`/`_value`, mirroring
     `api.hpp`'s own shape exactly) and, for most search results and
     parent→children shape lists, a space-joined string of friendly ids
     (already a well-formed Tcl list, *provided* every token is
     guaranteed whitespace/brace-free — true of `get_obstructions`/
     `get_terminal_ports`/`terminal_port_shapes`/`obstruction_shapes`,
     whose tokens are always purely numeric `"obstruction:N"`/`"terminal_port:N"`/
     `"shape:N"`, but *not* true of Terminal's own friendly id, which
     embeds its LEF-authored name — `get_terminals` is therefore built as
     `get_terminals_cmd`/`get_terminals_at` count+by-index instead, same
     as a property table, with the Tcl-side `get_terminals` proc doing
     the joining via `lappend` for correct quoting; see UPDATES.md's
     Terminal-friendly-id item and `le_tcl_shim.hpp`'s own "IDs" comment).
     `le_tcl_procs.tcl` loops over these with `dict set`/`lappend` to
     build the ergonomic dict/list a caller actually wants
     (`terminal_properties`, `shape_rects`, `shape_polygons`,
     `shape_paths`, ...) — correct Tcl-list quoting by construction,
     which hand-rolling the same aggregation in C++ is not. Every
     property value is pre-stringified (no `LePropertyType` tag crossing
     the boundary) since Tcl is "everything is a string" by design
     anyway (`expr {$v + 1}` works identically on a numeric string).
   - Every `-flag value` command's shim-side accessor uses a **shared
     `thread_local` scratch `std::string`** for building a formatted
     `const char*` return value on the fly (property values, packed-id
     lists, rect/point-as-string rows) — safe despite the project's
     usual "valid until the next call" pointer convention, since SWIG's
     Tcl `const char*` typemap copies the bytes into a new `Tcl_Obj`
     immediately on return, before the next shim call (a separate Tcl
     statement) can ever run.
   - `set_terminal_direction`/`create_terminal`'s `-direction` argument
     both take a name (`INPUT`/`OUTPUT`/`INOUT`/`NONE`/
     `OUTPUT_TRISTATE`/`FEEDTHRU`, matching `LeSignalDirection`'s LEF-
     derived vocabulary, not item 15's own abbreviated `IN`/`OUT` — see
     this doc's earlier note that item 15's example isn't an exact
     spec) via a hand-maintained `direction_codes` array in
     `le_tcl_procs.tcl`; `set_terminal_direction_cmd` (the shim's raw-int
     form) is what SWIG actually wraps.
   - **Known, deliberate scope limit (resolved by UPDATES.md item 17)**:
     no "current library/design/view" context existed yet at this point
     (item 15's own `current_library`/`current_design`/`current_view`,
     which it flagged as needing a concrete specification, not something
     this round invented unprompted) — `create_terminal`/
     `create_obstruction`/`update_abstract_boundary` all take an explicit
     `-abstract` id argument instead of operating on an implicit
     "current" one, and still do. A new `design_abstract_id
     <design_index>` shim function (scoped to the first Library only, via
     `le_library_design_at`'s `library_index = 0`) was the only way a
     script got one — correct as long as only one LEF file/library is in
     play, the same assumption this project's single-shared-Technology
     convention already makes elsewhere. Item 17 later added
     `open_design <name>` (backed by a new `Root::get_design_by_name`
     wrapper and `Scene::current_abstract()`, both of which already
     existed for the render path) as the "current view" this note called
     out as unbuilt, and scoped the read-only `get_terminals`/
     `get_obstructions`/`get_terminal_ports` commands (replacing
     `search_terminal`/`search_obstruction`/`search_terminal_port`) to
     it — the CRUD commands above remain explicit-`-abstract`, unchanged;
     item 17 was query scoping only, not a full context stack. UPDATES.md
     item 19.1 later generalized `get_terminals`/`get_obstructions`/
     `get_terminal_ports` (plus new `get_libraries`/`get_designs`/
     `get_abstracts`/`get_shapes`) into one unified `get_<type> [<name-
     expr>...] [-of <parent-token>...] [-filter <expr>] [-help]` pattern —
     `-of` reintroduces explicit, opt-in cross-view addressing (e.g.
     `get_terminals -of abstract:5` from a script currently viewing a
     different Abstract), with the current-view default preserved when
     `-of` is omitted. See item 19.1's own Resolution for the full
     command surface and friendly-id extension to Library/Design/Abstract.

   `src/tcl/tests/crud_test.tcl` (`ctest`'s `le_tcl_crud`) exercises the
   whole surface end to end against the same `testcell.lef` fixture:
   create/rename/redirect/delete a Terminal, `&&`/`||`/glob filter
   searches (including one exercising `.terminal.name`/`.shapes.layer_name`
   relational + list-hop traversal on `TerminalPort`, straight from item
   15's own example), a TerminalPort/Shape with a rect _and_ a polygon
   _and_ a path all added through the coordinate-list typemap and read
   back, an Obstruction with its own shape, an Abstract boundary update,
   and cascade-delete (deleting a Terminal takes its TerminalPort and
   Shape with it, verified by a follow-up search finding nothing).
   Verified: full build + `ctest` (501/501, including `le_tcl_smoke` and
   `le_tcl_crud`) pass; `build_release`'s `api`/`render`/`io` targets
   confirmed unaffected (this phase only adds a new `src/tcl/` target).

7. **Phase 6 — Batch shell + `show_gui`. DONE (batch shell only —
   `show_gui` deliberately deferred, see below).** `src/tcl/le_shell.cpp`
   is a small custom Tcl shell binary, built the same way `tclsh`/`wish`/
   OpenROAD's own shell are: a `Tcl_AppInitProc` (`app_init`) handed to
   `Tcl_Main()`, not a hand-rolled REPL. `Tcl_Main` already supplies both
   modes item 15 asks for, for free: no script argument (interactive
   terminal) drops into a real REPL with a `% ` prompt; a script argument
   runs it non-interactively then exits (batch mode) — this binary's own
   code never implements either mode itself, only bootstraps. `app_init`
   `load`s the SWIG-built `le_tcl` module and sources
   `le_tcl_procs.tcl`, so every CRUD/search command is ready to type (or
   already available to a batch script) without either path sourcing
   anything itself. `-module`/`-procs` are `le_shell`'s own leading
   flags (also settable via `LE_TCL_MODULE`/`LE_TCL_PROCS_PATH` env vars),
   consumed before the remaining `argv` reaches `Tcl_Main` unchanged —
   `le_shell` doesn't link against `le_tcl` at all; it dynamically
   `load`s the built shared module at startup exactly like an unmodified
   `tclsh` would, keeping the same build/test artifact Phase 0's spike
   already proved out as the thing being loaded.

   **`show_gui` is a deliberate, documented stub, not a silently missing
   feature** (`le_tcl_procs.tcl`): it prints an explanation and returns,
   rather than faking a connection. Asked directly rather than assumed:
   the recommended-direction section above (round 1) proposed a single-
   process/shared-`LeHandle` model matching OpenROAD's own `gui_start`,
   but that section predates this project's Flutter-based GUI actually
   existing — OpenROAD's Qt GUI is a C++ library in the _same_ process as
   its Tcl interpreter, while this project's only GUI is a separate Dart/
   Flutter runtime that consumes `api`/`render`/`io` via FFI, not
   something a C++/Tcl process can "just start rendering" in-process
   without embedding a full `FlutterEngine` alongside this shell's own
   Tcl event loop — a substantial, separate native-host build (a new
   embedding target, likely macOS's C++ `FlutterEngine` embedder API),
   not a mechanical follow-on to the rest of Phase 5/6. Deferred as its
   own future pass rather than built speculatively here; the batch shell
   itself doesn't depend on it being solved (a Tcl script that never
   calls `show_gui` gets full CRUD/search functionality today).

   `src/tcl/tests/shell_test.tcl` (`ctest`'s `le_tcl_shell`) runs as a
   batch script _under the real compiled `le_shell` binary_ (not `tclsh`
   directly loading `le_tcl`, which `le_tcl_smoke`/`le_tcl_crud` already
   cover) — this is what actually exercises `le_shell.cpp`'s own
   bootstrap (`-module`/`-procs` argv handling, `load`, sourcing
   `le_tcl_procs.tcl`): if any of that were broken, every command in the
   script would fail with "invalid command name" instead of running.
   Also manually verified `le_shell`'s stdin-REPL path (piped, non-tty
   input) runs `read_lef`/`design_count`/`exit` without error — true
   TTY-interactive testing would need a pty and wasn't automated.
   Verified: full build + `ctest` (502/502, including the new
   `le_tcl_shell`) pass; `build_release` unaffected (this phase only adds
   to `src/tcl/`, not anything it links).

   **`show_gui` follow-up — implemented, inverted embedding direction.**
   The embed-a-FlutterEngine-in-Tcl direction above was researched
   concretely (not just estimated) and confirmed genuinely high-risk:
   Flutter's macOS embedding API is Objective-C only, a bare `Tcl_Main`
   executable has no `.app` bundle for `FlutterDartProject` to find its
   Dart AOT snapshot/`icudtl.dat` in, and — the real blocker —
   `Tcl_Main`'s own blocking event loop and Cocoa's `NSApplication` run
   loop both want to own the main thread (Tk solves exactly this for
   `wish` via a custom `Tcl_NotifierProcs`, but that technique is
   unverified for Flutter's engine and would be multi-day, speculative
   work to find out). The user proposed inverting the direction instead:
   keep `le_shell` and the Flutter app as two separate binaries, and
   embed **Tcl as a library** (`Tcl_CreateInterp`/`Tcl_Eval` per command,
   not `Tcl_Main`) inside the Flutter app's own already-running native
   host, exposed as an in-app console widget — no second event loop to
   reconcile, no bundle-discovery problem, no `FlutterTexture`
   reimplementation, since the Flutter app's process/run loop/rendering
   pipeline are all untouched.
   - `src/tcl/le_tcl_shim.{hpp,cpp}` gained one new function,
     `set_session_handle(long long)`, backward-compatibly overriding
     `session()`'s lazy self-create with an externally-owned handle when
     one has been injected (`le_shell`/`le_tcl` used standalone are
     unaffected — nothing calls it there). A dedicated GTest
     (`src/tcl/tests/session_handle_test.cpp`, `ctest`'s
     `SessionHandle.InjectedHandleIsSharedNotFresh`) proves this two
     ways: a handle pre-loaded directly via `api.hpp` (no Tcl involved)
     is visible through an injected Tcl interpreter, and — the stronger
     check — a Terminal created _via a Tcl command_ is visible through a
     _direct_ `api.hpp` call on the same raw pointer afterward, ruling
     out "two handles that happen to agree on one read."
   - `flutter_plugin/macos/Classes/LeTclBridge.h`/`.mm` (new): owns one
     `Tcl_Interp*`, created lazily, `load`s the same `le_tcl.so`
     `le_shell` already builds and tests, injects the Dart-owned
     `LeHandle*` via `set_session_handle`, sources `le_tcl_procs.tcl`,
     and exposes `-evalTcl:` for synchronous one-command-at-a-time
     evaluation — mirrors `LeApiBridge`'s existing shape exactly.
     Deliberately links backend's **Debug** tree (`build/le_tcl.so`), not
     Release like `libapi.a`/`librender.a`/`libio.a` — interpreting a
     handful of typed commands per keystroke doesn't need optimized
     build performance, and it avoids adding `le_tcl` to the Release
     tree at all. `layout_engine_plugin.podspec` links Tcl (same
     `tcl-tk@8` keg backend's own CMake pins) and injects the
     `le_tcl.so`/`le_tcl_procs.tcl` absolute paths via
     `GCC_PREPROCESSOR_DEFINITIONS` — dev-machine-only, same explicitly
     accepted scope limit as every other backend path this podspec
     already hardcodes.
   - `LayoutEnginePlugin.swift` gained `createTclConsole`/`evalTclCommand`/
     `disposeTclConsole` method-channel cases (mirroring the existing
     texture ones); `lib/layout_engine_plugin.dart` gained
     `LeEditor.createTclConsole()` and an `LeTclConsole` class (mirroring
     `LeTexture`'s shape) wrapping them.
   - `frontend/lib/components/tcl_console.dart` (new): a collapsible
     panel next to the existing `MessageConsole`, with its own scrollback
     and command-line `TextField`. `LeProvider.runTclCommand()` (new)
     lazily creates one `LeTclConsole` per provider (reused across
     commands — a fresh interpreter per command would lose Tcl variables/
     state between them) and calls `refreshAndNotify()` after every
     command, same as any other mutation — the texture is pull-based,
     not auto-refreshing, so without this a Tcl-driven edit would stay
     invisible until an unrelated interaction happened to trigger a
     redraw.

   Verified: `le_tcl_session_test` (new) passes, full backend `ctest`
   stays green (503/503) confirming `le_shell`/`le_tcl` standalone
   behavior is unaffected; `flutter_plugin`/`frontend` `dart analyze`
   clean; both the plugin's `example` app and the real `frontend` app
   build and link successfully (`nm -gU` confirms all 97 `le_*` symbols
   still exported); `frontend` launches with no crash and no Tcl-bridge
   error in the log, and a screenshot confirms the new console panel
   renders correctly, collapsed, below `MessageConsole`. **Not verified**:
   actually typing a command into the running console and watching it
   take effect — simulating clicks/keystrokes needs Accessibility
   permission this environment doesn't have (Screen Recording was
   available, which is why the screenshot worked; System Events `click`
   returned "not allowed assistive access"). That step needs a human
   running the app.

   **Real crash found and fixed by that human running the app**: `read_lef`
   through the console segfaulted immediately (`EXC_BAD_ACCESS` in
   `LefDefParser::lefGetKeyword`'s internal keyword-table lookup, called
   from `le_tcl.so`'s own copy of the vendored LEF parser). Root cause:
   `le_tcl.so` (dynamically `load`ed by `LeTclBridge`, statically linking
   its own copy of `api`/`io`/`liblef.a`) and `layout_engine_plugin.framework`
   (which _also_ statically links the same code, for Dart FFI) are two
   independently-compiled Mach-O images loaded into the _same_ process.
   `lefGetKeyword`'s keyword-table `std::map` is a function-local
   `static` — ordinary, safe within one image — but with both images
   exporting a symbol of the same mangled name at default visibility,
   dyld bound `le_tcl.so`'s own internal call to the _other_ image's
   (uninitialized, in this call order) copy instead of its own,
   dereferencing a garbage pointer. Reproduced headlessly and
   deterministically (no need for the GUI or Accessibility permission)
   via `SessionHandle.ReadLefThroughTclFirstDoesNotCrash`
   (`src/tcl/tests/session_handle_test.cpp`): a plain GTest binary that
   itself links `api` directly (mirroring the framework's own static
   link) _and_ dynamically `load`s `le_tcl.so` — the same two-copies-
   one-process shape, no Flutter needed.
   - **Fix**: `le_tcl`'s CMake target now links with
     `-Wl,-unexported_symbols_list,src/tcl/le_tcl_unexported_symbols.txt`,
     hiding every symbol under the `LefDefParser::` namespace (the
     confirmed culprit — 1575 symbols, `nm`-verified) from `le_tcl.so`'s
     exported symbol table, so dyld can never bind another image's call
     (or `le_tcl.so`'s own calls, from another image's perspective) to
     the wrong copy.
   - **Two wrong turns on the way there, both instructive**:
     `CXX_VISIBILITY_PRESET hidden` on the `le_tcl` target alone did
     _nothing_ — it only affects compile flags for the target's own two
     listed sources; `api`/`io`/`render`/`lef_lib` are separate CMake
     targets already compiled into `.a` archives with default-visible
     symbols, and no downstream target's visibility preset can
     retroactively hide symbols already baked into linked-in object
     code. A blanket `-exported_symbols_list` allowlist (hide
     everything except the two SWIG entry points) _did_ stop the
     segfault, but broke something worse: it also hides libc++ typeinfo
     that's supposed to stay coalescable across dylib boundaries, so
     `std::system_error` thrown deep in the parser's own
     `std::vector`/`std::map` usage stopped RTTI-matching a
     `catch (const std::exception&)` in the calling image — "unknown
     C++ exception" instead of a clean `TCL_ERROR`
     (`abi::__cxa_current_exception_type()->name()` confirmed
     `NSt3__112system_errorE`, uncaught). The denylist scoped to exactly
     `LefDefParser::` avoids this: everything else, including `std::`
     templates instantiated with a `LefDefParser::` type as a parameter
     (e.g. exception guards for `vector<lefrOBSSpacing>`), keeps normal
     default visibility and stays coalescable.
   - **A third scenario looked broken but wasn't real**: chaining
     `ReadLefThroughTclFirstDoesNotCrash` directly after
     `InjectedHandleIsSharedNotFresh` in one raw binary invocation (no
     `--gtest_filter`) reintroduced the same `std::system_error`
     symptom — a _second_, independent `Tcl_Interp` re-loading the
     already-resident `le_tcl.so`. Isolated with a dedicated test
     (`DirectReadThenSingleTclInterpReadBothSucceed`, one `Tcl_Interp`
     only) that passes cleanly, confirming this is specific to two
     _sequential, separate_ `Tcl_Interp`s each loading the same `.so` —
     not how `LeTclBridge` (one `Tcl_Interp`, created once, reused for
     the session) or `le_shell` (`Tcl_Main`, also exactly one
     `Tcl_Interp` per process) actually work, and not reachable through
     `ctest` either (`gtest_discover_tests` runs each `TEST()` as its
     own process). Not chased further.
   - Verified: full backend `ctest` (505/505) passes, including three
     `SessionHandle.*` tests covering the crash's exact repro, the real
     app's actual "GUI-imports-then-console-reads" ordering, and the
     original injected-handle proof. Also confirmed against the exact
     file from the crash report
     (`test_data/stripe_15layer.lef`), not just the `testcell.lef`
     fixture — same result, since the bug is structural (which image's
     copy of a symbol gets used), not dependent on LEF file content.

   **Second real bug found and fixed, same testing session**: a Shape
   created via the console (`create_terminal_port_shape`/
   `add_shape_rect`) never appeared on screen, no matter how many times
   the caller re-rendered. Root cause had nothing to do with frame
   requests: `Pipeline::generate_shapes`'s cache key was `(AbstractId,
ViewLayerSet::generation())` - neither changes on a CRUD mutation (no
   LEF re-read, no Abstract switch), so the pipeline kept returning its
   pre-mutation cached result forever. `LeProvider.runTclCommand()`
   already called `refreshTexture()`/`markFrameAvailable()` after every
   command (that part was never broken) - the bug was one layer deeper,
   in a cache that had no way to know database _content_ had changed at
   all, only that the viewport/selection/layer-visibility might have.
   - **Fix**: `Root` gained a `mutation_version()`/`bump_mutation_version()`
     monotonic counter (added via cmg - `cmg/templates/indexed_pools/root_hpp_j2.py`,
     Root-level, not per-class - mirroring `Scene::selection_version()`'s
     existing pattern). `Pipeline`'s three shape-producing stages
     (`generate_shapes`, `filter_by_viewport_and_size`,
     `filter_by_layer_visibility` - plus the parallel tiny-shape-dot
     pair) all fold it into their own cache keys, cascading the same way
     they already cascade `ViewLayerSet::generation()`. All 19 mutating
     functions in `api.cpp` (every `le_create_*`/`le_delete_*`/
     `le_set_*_*`/`le_add_shape_*`/`le_remove_shape_*`) call
     `handle->root.bump_mutation_version()` on their success path -
     audited and counted by hand (`grep -c bump_mutation_version
api.cpp` == 19), not generated automatically, since cmg's own
     generated `create_x`/`delete_x`/`set_x_<field>` don't cover every
     mutation path either (Shape's `rects`/`polygons`/`paths` are plain
     list fields, mutated directly through a `get_shape()` pointer in
     `le_add_shape_rect` etc. - never through a generated setter at
     all - so an auto-bump inside cmg's own codegen would have missed
     exactly the function the original bug report actually called).
   - Regression test: `PipelineFixture.GenerateShapesRecomputesAfterACrudMutationEvenForTheSameAbstractIdAndViewLayerSet`
     (`pipeline_test.cpp`) - mutates via the same `Root` API `api.cpp`
     itself calls, confirms `generate_shapes` both cache-hits when
     nothing changed and correctly recomputes (via `generate_calls()`,
     not just checking the output) after a mutation, same AbstractId and
     ViewLayerSet throughout.
   - Verified: full backend `ctest` (506/506) passes; both `build` and
     `build_release` rebuilt. Unlike the first crash fix (`le_tcl.so`,
     loaded dynamically at runtime - a relaunch alone picked it up),
     this fix touches `libapi.a` itself, which `layout_engine_plugin`
     links **statically** at build time - the Flutter app needs an
     actual rebuild (`flutter build macos`/`flutter run -d macos`), not
     just a relaunch, to pick this one up.

   **Second-and-a-half bug, same fix, one layer higher**: after the
   `Pipeline` fix above, a Tcl-created shape _still_ didn't appear until
   an unrelated action (e.g. zooming) forced a real recompute - reported
   directly from the running app. Root cause: `Renderer` (`render.hpp`)
   sits on top of `Pipeline` and has its _own_ independent `CachedStage`
   per stage (`transform_to_pixels`, `build_picture`,
   `build_tiny_shapes_picture`, `rasterize_frame`,
   `rasterize_tiny_shapes_frame`, `compose_with_overlays`), every one of
   which was keyed on some subset of `{AbstractId, viewport_version,
visibility_version, selection_version, mouse_version}` - never
   `Root::mutation_version()`. So even once `Pipeline::generate_shapes`
   started correctly recomputing, `Renderer`'s own stages kept handing
   back their pre-mutation cached `SkPicture`/`PixelBuffer`, exactly the
   failure mode `compose_with_overlays`'s own pre-existing doc comment
   already warned about in the abstract ("`CachedStage`'s key comparison
   ... never inspects the picture arguments themselves, so a key that
   doesn't change would silently return a stale composited buffer even
   though a _different_ picture was passed in") - just not yet connected
   to this specific trigger.
   - **Fix**: every shape-content-dependent `Renderer` stage now folds
     `root.mutation_version()` into its key (a `const Root&` parameter
     added where one wasn't already present) - `transform_to_pixels`,
     `transform_tiny_shapes_to_pixels`, `build_picture` (already took
     `root`, just needed the key updated), `build_tiny_shapes_picture`,
     `rasterize`/`rasterize_frame`, `rasterize_tiny_shapes_frame`, and
     `compose_with_overlays` (which also threads `root` into its own
     internal `rasterize_frame`/`rasterize_tiny_shapes_frame` calls).
     `build_overlay_picture`/`build_selection_overlay_picture`/
     `rasterize_selection_overlay_frame` are deliberately untouched -
     pure mouse/selection chrome, no design content, so they don't need
     it. Every call site across `api.cpp`, `render_test.cpp`,
     `pipeline_benchmark.cpp`, and `render_preview.cpp` updated to pass
     `root` through (~70 call sites, mostly mechanical).
   - Regression test:
     `RenderFixture.ComposeWithOverlaysShowsAShapeAddedAfterACrudMutationWithNoOtherSceneChange`
     (`render_test.cpp`) - runs the _entire_ chain `le_render_pixel_buffer`
     itself calls, twice, with the identical `Scene` both times (no pan/
     scale/viewport/selection/mouse change at all) and only a CRUD
     mutation in between; confirms the new shape's own layer color is
     genuinely absent before and present after, in the final composited
     buffer - not just Pipeline's intermediate output, which the earlier
     `pipeline_test.cpp` regression test already covered on its own and
     wasn't sufficient by itself.
   - Verified: full backend `ctest` (507/507) passes; both `build` and
     `build_release` rebuilt.
