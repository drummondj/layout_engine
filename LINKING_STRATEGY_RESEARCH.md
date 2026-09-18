# Schematic↔Layout Linking: Research Notes

Research for `LINKING_STRATEGY.md`. This document proposes no code changes by itself —
it lays out the solution space (2–3 concrete options per sub-problem, with pros/cons and
a recommendation) so a direction can be picked before implementation starts.

All file:line references are to `backend/` unless stated otherwise.

## 0. Ground truth this document leans on

- `Design` owns both `Schematic` and `Layout` as direct children
  (`src/database/schema.py:1748`), so any `Placement`/`Route` reaches its sibling
  `Schematic` in exactly two hops: `Placement.layout → Layout.design → Design.schematic`.
  No separate global walk is ever needed for that hop.
- `Instance`/`Net`/`Port` names are `unique_per_parent`, scoped to their own
  `Schematic`, each with a generated O(1) index: `Root::get_instance_by_name`
  (`src/database/generated/root.hpp:5347`), `get_net_by_name` (`:5755`),
  `get_port_by_name` (`:5555`). `Placement`/`Route` names are the equivalent, scoped
  per-`Layout`.
- DEF names are stored as **flat, unsplit strings** — `Placement.name = component->id()`
  (`src/io/def_reader.cpp:591`), `Route.name = net->name()` (`:952`) — no `/` hierarchy
  splitting happens anywhere in the codebase today.
- `Instance.name` itself is only ever one Schematic-local segment (e.g. `"u_inv"`, or a
  generate-disambiguated `"gen_inv[0].u_inv"` using `.` — `src/sv/sv_reader.cpp:106`).
  There is no stored full top-to-leaf path anywhere; reaching a nested Instance N levels
  down means walking level-by-level: `get_instance_by_name(schematic_id, segment) →
  Instance.reference_design → Root::get_design_schematic(design_id)` → repeat.
- The only existing lazy-resolution precedent is `SVReader::link_unresolved_instances`
  (`src/sv/sv_reader.cpp:704`, exposed as the TCL `link` command,
  `src/tcl/le_tcl_procs.tcl:1318`): re-runnable, collects ids into a vector before
  mutating (avoids iterator invalidation), matches by name via a global `Design.name`
  index.
- By contrast, `Placement.reference_design` is **required**, resolved *eagerly* at
  DEF-read time — `DEFReader` errors and skips creating the Placement if the macro name
  doesn't resolve (`src/io/def_reader.cpp` comment at the component callback: this
  replaced a former "store the name, resolve later" fallback, deliberately removed).
  This is a real precedent *against* extending the lazy model to the physical side —
  weighed explicitly in §1/§2 below.
- `Route.name`'s own schema comment already anticipates this task: "not resolved to a
  (future) Net, same deferred-connectivity convention" (`schema.py`, Route klass).
  `PhysicalPort.net_name` has the identical shape and comment. Like `Placement.name`,
  both are stored as verbatim, unsplit strings — DEF's `NETS`/`SPECIALNETS`/`PINS`
  syntax allows the same hierarchical `"a/b/c/n1"` form component names use, so
  resolving either needs the identical N-level hierarchical walk as Instance/Placement
  matching, not a flat lookup (see §2/§3).
- `Pin.net`/`Port.net` are plain optional reference fields (forward-only); `Net` has no
  back-list of connected Pins/Ports at all — finding "every Pin/Port connected to this
  Net" today needs a linear scan.
- The delete cascade (`Klass.delete_api_body()`,
  `codegen/codegen/schema.py:1699`) is 100% ownership-structural (`is_child`/`parent`
  edges only, planned at Python codegen time). There is **no** declarative
  `on_delete`/cascade/policy mechanism anywhere in the schema DSL — confirmed by grep,
  zero matches.
- A `Transaction`-batching mechanism already exists at the TCL-command level:
  `le_repl_eval` wraps every whole typed command in `begin_command`/`end_command`
  (`le_tcl_procs.tcl:88` → `LeHandle::command_history.begin/end`). One hand-written
  precedent for a single API call recording multiple mutations into one transaction
  already exists: `move_click_unlocked` (`src/api/api.cpp:772`), which loops
  `update_shape` once per selected shape inside one `begin`/`end` pair.

---

## 1. Instance ↔ Placement linking (scenarios 1 & 3)

**Option A — recommended.** Add `Placement.instance: Optional[Instance]` (a plain
reference field, the same convention as `Shape.layer`) plus
`Placement.physical_only: bool`. Populate both during `link`.

- *Scenario 1* (Placement with no matching Instance): create the Placement as today,
  leave `.instance` unset, set `physical_only = True`, log one WARNING per Placement —
  and only on Placements not already resolved, so re-running `link` doesn't re-warn.
- *Scenario 3* (Instance with no matching Placement): during `link`, for every Instance
  whose `reference_design` is already resolved and which has no linked Placement yet,
  create a default `Placement` — `location = (0,0)`, `orientation = R0`,
  `reference_design = Instance.reference_design`, `instance` = the Instance itself.
  **Naming (confirmed):** `Placement.name` = the full `/`-joined hierarchical path from
  the top Schematic down through every ancestor Instance segment to the Instance itself
  — exactly the string a real DEF file would have used had this instance actually been
  placed. This is guaranteed unique within the Layout by DEF's own hierarchical-naming
  convention (two distinct instances can never produce the same full path), so no extra
  dedup logic is needed. The top Layout/Design's own root Schematic contributes no
  segment of its own — only descendant Instance names do, matching how DEFReader never
  emits a name for "the design being placed," only for instances inside it. This only
  makes sense when the Instance's Design already *has* a Layout at all; when it doesn't
  (no DEF was ever read for that Design), skip — do not fabricate a Layout.

Pros: matches the existing plain-reference-field convention exactly (same shape as
`Shape.layer`/`Instance.reference_design`); id-based, so a later rename of the Instance
or Placement doesn't break the link; drives filter-expression hops
(`.instance.name`, `.instance.reference_design...`) for free through the existing
generated hop machinery (`match_hop`, no new codegen needed); the optional field cleanly
represents "unlinked" for scenario 1.

Cons: scenario 3's auto-create is a new mutation path during `link`, not just a
resolve — needs its own idempotency discipline (see §6).

**Option B — rejected.** No stored field; resolve Instance↔Placement on demand each
time it's queried (walk the name-matching logic live instead of caching a link).
Cons: can't drive filter-expression hops without new codegen support for a computed
(non-stored) hop; can't cleanly do the one-time warning/auto-create bookkeeping `link`
needs to do exactly once; repeats the §3 hierarchical walk on every single query
instead of once at `link` time.

**Option C — rejected.** A separate join klass between Instance and Placement.
Cons: pure overhead for what is structurally a 1:1 relationship, already directly
expressible as a plain reference field — no M:N need exists here.

---

## 2. Net/Route and Port/PhysicalPort linking (scenarios 2 & 4)

**Correction from an earlier draft of this document:** Route/PhysicalPort resolution
does **not** reduce to a flat 2-hop lookup. `Route.name`/`PhysicalPort.net_name` are
stored exactly as verbatim, unsplit strings the same way `Placement.name` is
(`def_reader.cpp:952` for Route's own name; the same shape for PhysicalPort's
`net_name`) — and DEF's `NETS`/`SPECIALNETS`/`PINS` syntax allows the identical
hierarchical `"a/b/c/n1"` form component names use, whenever the net being routed is
local to a nested instance rather than a true top-level net. Nothing in DEF keeps net
names flat while component names go hierarchical. So resolving `Route.net`/
`PhysicalPort.net` needs the **exact same N-level hierarchical walk** as Instance/
Placement matching (§3) — the only difference is the *leaf* lookup at the end of the
walk targets `Net.name` instead of `Instance.name`. This is exactly the shared-resolver
design §3 now describes.

**Option A — recommended.** Add `Route.net: Optional[Net]` and
`PhysicalPort.net: Optional[Net]` (plain reference fields, same convention as §1).
Both resolve during `link` by calling §3's shared resolver in exact-match mode with
`leaf_kind = Net`, anchored at the same top Schematic reached via
`Layout → Design → Schematic`.

- Unresolved `Route.net` → **ERROR** (scenario 2, user-specified exactly).
- Unresolved `PhysicalPort.net` → **WARNING**, not ERROR. Reasoning: chip-boundary
  power/ground pins legitimately lack a netlist-level Net far more often than a real
  signal Route does, and PhysicalPorts are typically two to three orders of magnitude
  fewer than Routes — a hard ERROR gate on every unresolved boundary pin risks blocking
  `link` entirely over what's frequently cosmetic. Route, being both far more numerous
  and far more likely to indicate a genuine connectivity bug when unresolved, keeps the
  harder gate.

Pros: identical benefits to §1 Option A — id-based, hop-friendly, minimal, matches
existing convention; reuses 100% of §3's resolver, no separate resolution code.

Cons: none material beyond what's already listed.

**Option B — rejected.** No stored field, name-match on demand. Same rejection
reasoning as §1 Option B.

Scenario 4 (Net without Route: fine) needs no active handling at all — it's simply the
case where `Route.net` never gets set for that Net's side because no Route exists.
Nothing to build for it.

---

## 3. The shared hierarchical resolver (used by §1, §2, and §4)

This is genuinely one piece of infrastructure, not three: §1's Instance/Placement
matching, §2's Net/Route/PhysicalPort matching, and §4's TCL search syntax all need the
identical capability — walk a `/`-delimited path down the Instance hierarchy starting
from a Schematic, and resolve the final segment against some target klass's per-Schematic
name index. Building it once and having every caller be a thin wrapper around it (per
the reuse requirement below) is the right shape, not an accident of convenience.

### Shape of the resolver

Conceptually (no code yet — this is the algorithm, not an implementation):

```
resolve_path(root_schematic, segments[0..n-1], leaf_kind) -> [LeafId]:
    frontier = { root_schematic }
    for i in 0 .. n-2:                      # every segment but the last always
                                             # descends through an Instance
        next_frontier = {}
        for schematic in frontier:
            if segments[i] is a plain literal (no glob metacharacters):
                inst = get_instance_by_name(schematic, segments[i])      # O(1)
                if inst valid:
                    next_frontier.add(get_design_schematic(inst.reference_design))
            else:
                for inst in get_schematic_instances(schematic):          # O(children)
                    if glob_match(segments[i], inst.name):
                        next_frontier.add(get_design_schematic(inst.reference_design))
        frontier = next_frontier
    # leaf segment: resolved against whichever klass the caller wants
    results = []
    for schematic in frontier:
        if segments[n-1] is a plain literal:
            id = get_<leaf_kind>_by_name(schematic, segments[n-1])       # O(1)
            if valid: results.add(id)
        else:
            for candidate in get_schematic_<leaf_kind>s(schematic):
                if glob_match(segments[n-1], candidate.name):
                    results.add(candidate)
    return results
```

`leaf_kind` is the only thing that varies per caller: `link`'s Placement-matching pass
calls this with `leaf_kind = Instance`; its Route/PhysicalPort-matching pass calls it
with `leaf_kind = Net`; §4's `get_instances`/`get_nets`/`get_ports` each pass their own
`leaf_kind` too. **A path with no glob metacharacters anywhere degenerates exactly to
the plain O(1)-per-segment walk** — `frontier` never exceeds size 1, so `link`'s
exact-match usage (§1, §2) pays zero cost for the generality §4 needs. A glob only
costs extra at the specific segment(s) where one actually appears — this is what makes
mid-path globs (`a/*/c/n1`) fall out of the design for free rather than needing special
handling, addressing the "please implement mid-path glob support" request directly:
the fan-out at a wildcarded segment is exactly the same mechanism whether the wildcard
is in the middle or at the end of the path.

### `**` — recursive descent, needed for §5's rename propagation

A plain `*` here is a **single-segment** glob — it matches within one hierarchy level
only, the same way a shell glob matches one directory listing but never crosses a `/`
on its own. That means a path like `top/a/b/*` reaches only `b`'s *direct* children,
not everything nested arbitrarily deep below `b` — this is a real, previously-unstated
gap, surfaced by §5's own need to find every descendant of a renamed Instance.

Fix: add `**` as a second wildcard form, meaning "zero or more full hierarchy levels"
— the same convention Bash's globstar and `find -path` already use, not a novel
invention. In the resolver above, a `**` segment doesn't do one bounded fan-out step
and stop; it recurses into every matched child's own children, and *their* children,
unboundedly, collecting every Instance (or, one level further via each visited
Schematic's own `nets`, every Net) reachable at any depth. `top/a/b/**` then correctly
means "every descendant of `b`, at any depth" — which is exactly the set §5's rename
propagation needs. This is still the same shared resolver, not a separate mechanism:
`**` is just a different fan-out rule at one segment, exactly like `*` already is one
fan-out rule and a literal segment is the O(1) no-fan-out case.

**Confirmed: expose `**` through §4's TCL syntax too**, not just as an internal
capability §5 happens to need. A user typing `get_instances top/a/b/**` to list a
whole subtree, or `get_nets top/a/b/**` to pull every net anywhere under a block, is a
genuine user-facing feature in its own right — recursive hierarchical queries like this
are notably absent from the TCL/Tcl-command interfaces of established EDA tools, which
is exactly why this is worth calling out as a real differentiator rather than treating
it as internal plumbing that happens to leak out. Since §4 is already a thin wrapper
over this same resolver, this costs nothing extra to expose once §5 needs it built
regardless — see §4 below for the concrete syntax notes.

### Performance options for this resolver

**Option A — recommended to start.** Exactly the walk above, no caching. Cost for `link`
(exact-match, single-candidate frontier throughout): **O(N·D)** — N = number of DEF
objects being resolved, D = hierarchy depth. Cost for a glob query (§4) depends on how
many segments actually carry wildcards and how wide the matching fan-out is at each —
worth stating plainly that a heavily-wildcarded path (e.g. `*/*/*/n1`) can be
expensive, since it's no longer bounded by O(D) lookups.

The real inefficiency to flag for the exact-match (`link`) case: siblings sharing a
long common prefix (e.g. 1,000 instances all under the same 5-deep hierarchical parent)
each redo all 5 hops redundantly — this is exactly what Option B targets.

Pros: zero new state or caching to maintain; always correct/live against the current
database; minimal code, built entirely on existing generated Root API plus the existing
`glob_match` helper already used by `get_<type>` search commands.

Cons: redundant prefix work at scale for `link`'s own exact-match usage, quantified
above; no protection against a pathologically wide glob query from §4.

**Option B — documented follow-up, benchmark-gated.** Same walk, plus a
**request-scoped** prefix cache (path-prefix string → resolved `SchematicId` set),
built fresh per top-level call (one `link` invocation, or one TCL search call) and
discarded when it returns. A *persistent* cross-call cache is explicitly rejected: it
would need invalidation on every Instance/Design mutation, and no such invalidation
infrastructure exists anywhere in this codebase today — a request-scoped cache
sidesteps that problem by construction.

For `link`'s exact-match usage this turns O(N·D) into roughly **O(N + U·D)** where
U = number of unique prefixes actually seen — a large win when U≪N (true of typical
real, prefix-grouped DEF output), close to zero benefit if names arrive unsorted.

**Option C — flagged by the user as likely eventually necessary; try without it
first.** Pre-sort all DEF names and do a single combined DFS/merge-join against the
Schematic tree, rather than resolving each name independently. This is the option to
reach for if Option A's (or B's) per-object resolution cost proves too high once
measured against the real stress fixtures — the user's own expectation, based on this
project's scale (millions of instances/placements in the existing benchmark fixtures),
is that this will likely be needed eventually. Per this project's own explicit rule —
"Performance decisions must be backed by a benchmark, not intuition"
(`backend/CLAUDE.md`) — the plan is still to implement and measure Option A (and B if
needed) first, with Option C as the pre-identified next step the moment profiling shows
it's warranted, rather than building it speculatively now.

**Recommendation:** implement Option A first, exactly as designed above (already
general enough to serve §1/§2's exact-match needs and §4's glob needs from one
implementation). Benchmark against the existing 1M-instance/4M-placement stress
fixtures (`benchmarks/stress_data.hpp`/`layout_stress_data.hpp`). Move to Option B,
then Option C, only as that data demands it.

---

## 4. TCL hierarchical search syntax (`get_nets a/b/c/n1`, plus `**`)

This section is intentionally thin — it is a TCL-facing wrapper around §3's resolver,
not a separate implementation, per the explicit reuse requirement: the same
hierarchical-query capability `link` needs internally should be exposed to a user
typing TCL commands, not reimplemented.

**Option A — recommended.** At the shim/TCL layer (`src/tcl/le_tcl_shim.cpp` /
`le_tcl_procs.tcl`): split the argument on `/` into segments (each segment may itself
be a glob pattern — including mid-path segments, e.g. `a/*/c/n1` or `*/b/*/n*` — and,
confirmed, `**` for recursive descent, e.g. `get_instances top/a/b/**` to list every
instance nested anywhere under `b`, or `get_nets top/a/b/**` for every net anywhere
under it), call §3's resolver directly with `leaf_kind` set to whatever the calling
command already searches for (`Instance` for `get_instances`, `Net` for `get_nets`,
`Port` for `get_ports`), anchored at `handle->current_schematic_id` (or the `-of`
schematic, same scoping `get_<type>` already uses today) as the root. A bare name with
no `/` at all is just the n=1 degenerate case of the same call — **fully backward
compatible**, existing single-segment glob behavior is preserved exactly, not
reimplemented separately.

Worth calling out as a genuine differentiator, not just an internal convenience: a
recursive hierarchical query like `get_nets top/a/b/**` — "every net anywhere under
this block, in one call" — isn't something established EDA tool TCL interfaces
typically offer directly; it falls out here for free because §3's resolver already has
to support it for §5's rename propagation, and §4 is just a thin pass-through to that
same resolver.

Explicitly pin down **why `/` and not `.`**: DEF/Placement/Route names use `/` as their
hierarchy delimiter (matching this feature's own motivating DEF convention), while
`Instance.name` can itself already contain `.` from generate-block disambiguation
(e.g. `"gen_inv[0].u_inv"`, `sv_reader.cpp:106`), and Verilog's own human-facing
hierarchy convention is also `.` — this is a deliberate DEF-side convention choice, not
an oversight, and shouldn't be confused with a hypothetical future Verilog-`.`-path
feature.

Pros: fully backward compatible; the *entire* hierarchical-matching logic lives in one
place (§3) and is exercised identically whether the caller is `link` or a TCL user —
a bug fix or performance improvement to §3 benefits both automatically; matches the
user's own example syntax exactly, now including mid-path globs.

Cons: none material — the earlier draft's "only a trailing glob" limitation is gone now
that §3 itself is glob-capable at every segment.

**Option B — rejected as premature.** Push this into codegen so every
`unique_per_parent`-scoped-name search command gets it uniformly. Only a few commands
(`get_nets`, `get_instances`, `get_ports`) need this today — not enough to justify new
shared codegen machinery yet; revisit if many more commands need the same capability.
(Note this is now a smaller gap than the earlier draft suggested, precisely because §3
is already one shared, non-duplicated implementation — the codegen question is really
just "should the thin per-command wrapper be hand-written or generated," not "should
the resolution logic itself be duplicated per command," which was the bigger concern
before this section's rewrite.)

**Option C — rejected.** A brand-new, separate TCL command instead of extending
`get_nets`/`get_instances`/`get_ports`. Splits the UX into two mental models
(glob-search vs. path-resolve) for what the user's own example explicitly asks the
*existing* commands to do.

---

## 5. Mutation side-effects on Net delete

`LINKING_STRATEGY.md` actually specifies **two distinct** side-effects for deleting a
Net, with two distinct correct behaviors — worth separating clearly, since they're easy
to conflate into one "clean up references" bucket:

- **(a) "its Route should also be deleted from the Layout view"** — the user states
  this outcome directly, not as an open question: a Route with no Net is orphaned
  physical routing geometry with no logical meaning, so it should be *deleted*, not
  merely unlinked. Once §2 exists (`Route.net`/`PhysicalPort.net`), this is a genuine
  **cross-klass cascade delete** — not a dangling-reference problem, since Route isn't
  owned by Net but the desired outcome is still "make it go away with its Net."
  **`PhysicalPort` is confirmed to get different treatment (not the same as Route):**
  it should *remain*, with just its `.net` reference cleared — exactly the same
  treatment a Pin gets (a Pin whose Net is deleted keeps existing, just loses its
  connection). A chip-boundary I/O pin is a real physical/structural feature of the
  design independent of whether its net still resolves; a Route is pure wire geometry
  for a specific net and has no independent meaning once that net is gone. So (a) now
  splits cleanly: Route → delete; PhysicalPort → clear-only, batched into the same
  dangling-reference step (b) below rather than the cascade-delete step.
- **(b) "if a pin or port is deleted, it should be disconnected from its Net"** —
  read literally this is already satisfied: `Pin.net`/`Port.net` are fields *on*
  Pin/Port, so deleting either already destroys its own `.net` field with it, nothing
  is left dangling on the Pin/Port side. The real, currently-unhandled gap runs the
  *other* direction (confirmed with the user): deleting a **Net** leaves every
  `Pin.net`/`Port.net` that pointed to it dangling (a stale id, silently resolving to
  "not found" on next lookup, never cleared) — this is the actual
  dangling-*reference*-clearing problem to solve, and it belongs on the Net-delete path
  alongside (a), not on Pin/Port's own delete path.

Both belong in the same orchestration, since both trigger off deleting a Net.

**Option A — recommended.** Hand-written orchestration in `api.cpp`'s
`le_delete_net`: open/reuse a `Transaction` (the mechanism already exists via
`command_history.begin/end`), then, before deleting the Net itself:
1. Find and delete every `Route` (once §2 exists) whose `.net` points at this Net —
   scanning the Net's own sibling `Layout` (via `Layout → Design → Schematic`,
   reversed), bounded to that one Layout, not global — recording each delete into the
   same transaction (reuses the generated `delete_route` body, which already records
   itself when a transaction is open).
2. Scan the Net's own sibling `Layout`'s `PhysicalPort`s (once §2 exists) and the
   Net's own `Schematic`'s Pins/Ports — both bounded, not global — for `.net ==
   this_id`, and clear each via `update_physical_port`/`update_pin`/`update_port`
   (recorded into the same transaction, same batching pattern `move_click_unlocked`
   already uses for its own per-shape loop).
3. Delete the Net itself.

Why bounded scans are fine, and a reverse index (e.g. `Root::get_net_pins`) is *not*
needed yet: each scan is bounded by one Layout's or one Schematic's own object count
(tens of thousands at most for a large block, not global), and Net-delete is a rare,
user-initiated operation — not a hot path, unlike §3's resolution which runs N times
during `link`. Concrete trigger condition for revisiting this later: if this scan
pattern ever needs to run per-Net in a loop over many Nets (i.e., becomes hot-path),
build the reverse index then.

Pros: uses 100% existing infrastructure (Transaction batching, generated cascade
recording, plain `update_x` calls); consistent with this project's stated "keep
abstractions minimal and justified by present, not hypothetical, needs" principle;
directly implements what the user actually asked for, not a weaker approximation of it.

Cons: doesn't automatically generalize to a future third cross-klass case — acceptable,
since no such case exists yet.

**Option B — documented future path, not now.** A declarative `on_delete`/cascade
field in `schema.py`, with new codegen machinery generating this uniformly for any
klass that declares it. Rejected for now: one real use case doesn't justify new
schema-DSL machinery. Revisit if a third similar cross-klass mutation-effect case
appears.

**Option C — rejected.** A generic Root-level "clear all dangling references to this
id" pass, covering every plain-reference field automatically (`Shape.layer`,
`Placement.reference_design`, `Instance.reference_design`, `Pin.net`, `Port.net`,
future `Route.net`/`PhysicalPort.net`/`Placement.instance`, all at once). This is the
largest-scope option of the three. The current "dangling id fails safe, resolves to
not-found" behavior (`Pool::get()` checks generation, returns `nullptr` for a stale id)
is already a documented, accepted, tolerated limitation elsewhere in this codebase —
not an urgent gap that needs a sweeping architectural fix today.

### Rename propagation (confirmed requirement, not previously covered)

The user has confirmed: renaming a `Net` must rename its linked `Route` to match, and
renaming an `Instance` must rename its linked `Placement` to match. This is a new
mutation side-effect, distinct from delete: it triggers on `update_net`/
`update_instance` when the `name` field actually changes.

**Net → Route is the simple case.** A `Net` has no children, so its rename can only
ever affect the *one* directly-linked `Route`. If `Route.name` is stored as a full
`/`-joined path (§1's naming rule extended to Route/PhysicalPort — the trailing segment
is the Net's own local name), renaming the Net means recomputing just that trailing
segment on the linked `Route`'s name and calling `update_route`, batched into the same
transaction as the `update_net` call itself.

**Instance → Placement is the expensive case, and needs to be stated plainly as such.**
Because `Placement.name`/`Route.name` are *full hierarchical paths* (§1's confirmed
naming rule) rather than single local segments, renaming an Instance that has
*descendants* — i.e. anything but a leaf — means every `Placement`/`Route`/
`PhysicalPort` linked anywhere under it needs its own name recomputed too, not just the
one Placement directly linked to the renamed Instance itself. Example: renaming
instance `top/a/b` to `top/a/b2` must also turn `top/a/b/u_inv`'s Placement name into
`top/a/b2/u_inv`, and do the same for every other Placement/Route nested under `b`,
however many levels deep.

**Recommended approach — reuses §3's resolver, not a string-matching scan.** An
earlier draft of this design proposed finding affected rows by scanning the Layout for
a stored name with the *old* full path as a string prefix — that has a real
correctness bug: `startswith("top/a/b2/u_inv", "top/a/b")` is true, so renaming
`top/a/b` would incorrectly also rewrite the unrelated sibling `top/a/b2`'s
descendants. Fixed design, using §3's new `**` recursive-descent segment instead of
any string matching:

1. Call §3's resolver with `leaf_kind = Instance` on `<renamed instance's own resolved
   path>/**` to get the exact set of descendant `InstanceId`s (this is real graph
   traversal over ids via `Instance.reference_design → Design.schematic →
   Schematic.instances`, never a name comparison, so the sibling-prefix bug above
   can't occur by construction) — and, at each visited Schematic along the way, also
   collect that Schematic's own `Net` ids the same way, for the Route/PhysicalPort
   side.
2. Build one **request-scoped** reverse map — `InstanceId → PlacementId` and
   `NetId → {RouteId, PhysicalPortId}` — via a single pass over the renamed Instance's
   own sibling `Layout`'s Placements/Routes/PhysicalPorts (same "bounded to one
   Layout, built fresh per call, no persistent-cache invalidation needed" pattern as
   §3 Option B's prefix cache). This is the one place a real scan still happens, and
   it's the same shape/cost as every other bounded scan already justified in this
   document.
3. Walk the descendant set from step 1 (the **same** recursive descent, so the
   traversal itself is done once, not twice): at each descendant Instance/Net, look up
   the reverse map from step 2 in O(1); if a linked Placement/Route/PhysicalPort
   exists, compute its new full path by simple string concatenation of the
   already-known new parent path plus that descendant's own unchanged local name (no
   string surgery on the old name needed at all — every descendant's own local segment
   name is untouched by the rename, only the renamed ancestor's segment changes), and
   call `update_placement`/`update_route`/`update_physical_port`, batched into the same
   transaction as the `update_instance` call.

Pros over the string-prefix approach: no sibling-name-collision risk, since nothing is
ever compared by substring; reuses §3's shared resolver rather than inventing a second
traversal mechanism, directly answering the "should this reuse §3" question; the
descendant walk (step 1/3) is bounded by the actual number of descendants, not by
Layout size — only step 2's reverse-map build is Layout-bounded, and that's a single
pass, not one-scan-per-descendant.

**Caveat, unchanged from the earlier draft:** renaming an Instance near the *root* of a
very large, deeply-placed hierarchy still touches every real descendant Placement/
Route/PhysicalPort — that blast radius is inherent to the requirement itself (the user
asked for the rename to propagate), not an artifact of a particular implementation.
Worth a concrete note in the tool's own docs/warnings; revisit with a real reverse
index only if profiling shows step 2's per-call reverse-map build is itself the
bottleneck (same benchmark-gated posture as §3).

---

## 6. Open questions / risks

- **Idempotency of `link`.** Re-running it must not create duplicate default
  Placements (§1 scenario 3) or re-emit duplicate warnings — mirror
  `link_unresolved_instances`'s own "only touch what's still unresolved" pattern
  exactly (check `Placement.instance`/`.physical_only` state before acting).
- **Ordering dependency within one `link` call.** §1 and §2's resolution both depend
  on `Instance.reference_design` already being resolved (itself lazy, via the existing
  `link_unresolved_instances` step). A single `link` invocation must run instance
  resolution to a fixed point *before* attempting Placement/Route/PhysicalPort
  resolution, or resolution could spuriously fail partway down a hierarchy branch.
- **Leaf recursion stopping condition (§3).** A path segment resolving to an Instance
  whose Design has no Schematic at all (a hard macro / pure physical leaf cell) is the
  *normal*, expected way a hierarchical walk terminates before exhausting every DEF
  path segment — this must be treated as a normal stop, not an error.
- **Per-Schematic scoping discipline.** Every resolution in §1/§2/§3 must use the
  correct *sibling* Schematic, reached via `Layout → Design → Schematic` — never a
  global name lookup. Easy to get wrong by accident since `get_instance_by_name`/etc.
  take a `SchematicId` parameter that must be exactly the right one at each hierarchy
  level, not e.g. always the top-level Schematic.
- **Rename-propagation blast radius (§5).** Already flagged concretely in §5's own
  rename-propagation subsection, restated here because it's the single biggest
  performance risk in this whole document: renaming a near-root Instance genuinely
  does touch every real descendant Placement/Route/PhysicalPort — that's inherent to
  the requirement, not fixable by algorithm choice. §5's corrected design (§3's `**`
  plus a request-scoped reverse map, not string-prefix matching) keeps the cost
  proportional to real descendant count plus one Layout-bounded scan, and is also now
  bug-free against sibling-name collisions (an earlier draft's string-prefix approach
  was not — see §5's own note). Still worth benchmarking alongside §3 once built.
- **Black-box support — explicitly out of scope.** `LINKING_STRATEGY.md`'s own side
  note says black-box handling for missing reference data (an EDA-tool-common feature)
  will **not** be supported now, deferred to a possible future enhancement. Every
  option above assumes an unresolved reference stays unresolved (logged, not silently
  stubbed with a placeholder black-box object) — consistent with that note, called out
  here so it isn't accidentally reintroduced as a side effect of some other design
  choice.
- **"Something I haven't thought of" (LINKING_STRATEGY.md item 5).** This list is this
  document's answer to that prompt — flag anything still missing.
